#include "AudioEngine.h"
#include <iostream>
#include <windows.h>
#include <chrono>
#include <cmath>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

AudioEngine::AudioEngine(const Config& cfg) 
    : config(cfg), running(false) {
    
    ipcClient = std::make_unique<rime::IpcAudioClient>();
    vstHost = std::make_unique<VSTHost>();
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    std::cout << "Initializing IPC Client...\n";
    if (!ipcClient->initialize(config.sampleRate, 2)) return false;
    
    if (!config.virtualCableName.empty()) {
        int bufferFrames = config.sampleRate * 2; // 2 seconds buffer
        virtualBuffer = std::make_unique<RingBuffer>(bufferFrames, config.virtualCableChannels);
        
        std::wstring wDeviceName(config.virtualCableName.begin(), config.virtualCableName.end());
        virtualCapture = std::make_unique<WASAPICapture>(*virtualBuffer, wDeviceName, config.virtualCableChannels);
        
        std::cout << "Initializing Virtual Capture (" << config.virtualCableName << ")...\n";
        if (virtualCapture->initialize()) {
            std::cout << "Virtual Capture initialized successfully.\n";
            virtualCapture->start();
        } else {
            std::cerr << "Failed to initialize Virtual Capture. Falling back to native Stereo.\n";
            virtualCapture.reset();
            virtualBuffer.reset();
        }
    }

    if (!config.keepAliveDeviceName.empty()) {
        std::wstring wDeviceName(config.keepAliveDeviceName.begin(), config.keepAliveDeviceName.end());
        keepAlive = std::make_unique<WASAPIKeepAlive>(wDeviceName);
        std::cout << "Initializing Keep-Alive stream (" << config.keepAliveDeviceName << ")...\n";
        if (keepAlive->initialize()) {
            std::cout << "Keep-Alive stream initialized successfully.\n";
            keepAlive->start();
        } else {
            std::cerr << "Failed to initialize Keep-Alive stream.\n";
            keepAlive.reset();
        }
    }

    int inChannels = virtualCapture ? config.virtualCableChannels : 2;
    int outChannels = 2; // Output to headphones is always stereo

    std::cout << "Initializing VST3 Host...\n";
    vstHost = std::make_unique<VSTHost>();

    // Determine state path
    char* appdata = nullptr;
    size_t sz = 0;
    _dupenv_s(&appdata, &sz, "APPDATA");
    std::string path = "";
    if (appdata) {
        path = std::string(appdata) + "\\RIME Standalone\\state.bin";
        free(appdata);
    }

    if (!vstHost->loadPlugin(config.pluginPath, inChannels, 2, path)) {
        std::cerr << "Failed to load Neumann RIME plugin!\n";
        return false;
    }

    return true;
}

void AudioEngine::start() {
    running = true;
    processingThread = std::thread(&AudioEngine::processingLoop, this);
}

void AudioEngine::shutdown() {
    running = false;
    if (processingThread.joinable()) {
        processingThread.join();
    }
    if (virtualCapture) {
        virtualCapture->stop();
    }
    if (keepAlive) {
        keepAlive->stop();
    }
    if (ipcClient) {
        ipcClient->shutdown();
    }
}

bool AudioEngine::isRunning() const {
    return running;
}

bool AudioEngine::showUI() {
    if (vstHost) {
        return vstHost->showUI();
    }
    return false;
}

bool AudioEngine::loadPluginState(const std::string& path) {
    if (vstHost) return vstHost->loadState(path);
    return false;
}

bool AudioEngine::savePluginState(const std::string& path) {
    if (vstHost) return vstHost->saveState(path);
    return false;
}

void AudioEngine::processingLoop() {
    const int MAX_BLOCK = 480; // Match WASAPI 10ms period at 48kHz
    int inChannels = virtualCapture ? config.virtualCableChannels : 2;
    int outChannels = 2;

    std::vector<float> apoBlock(MAX_BLOCK * 2, 0.0f);
    std::vector<float> virtualBlock(MAX_BLOCK * inChannels, 0.0f);
    std::vector<float> inputBlock(MAX_BLOCK * inChannels, 0.0f);
    std::vector<float> outputBlock(MAX_BLOCK * outChannels, 0.0f);

    // Register with MMCSS for real-time thread priority
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hTask) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
        std::cout << "[DIAG] MMCSS registered: Pro Audio (CRITICAL priority)\n";
    } else {
        std::cout << "[DIAG] WARNING: MMCSS registration failed (err=" << GetLastError() << "), running at normal priority\n";
    }

    // --- Diagnostics ---
    uint64_t framesProcessed = 0;
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t totalProcessingTimeUs = 0;
    uint64_t processCallCount = 0;
    float virtualPeak = 0.0f;

    // Compute output gain multiplier from dB
    const float outputGain = std::pow(10.0f, config.outputGainDb / 20.0f);
    std::cout << "[DIAG] Output gain: " << config.outputGainDb << " dB (x" << outputGain << ")\n";

    std::cout << "[DIAG] IPC Processing thread started (MAX_BLOCK=" << MAX_BLOCK << ", event-driven)\n";

    while (running) {
        // Wait for the APO to signal that new data is available (5ms timeout as safety net)
        ipcClient->waitForData(5);
        
        if (!running) break;

        // Drain all available frames from the IPC ring buffer
        while (running) {
            auto t0 = std::chrono::steady_clock::now();
            
            // Read up to MAX_BLOCK frames (whatever is available) from APO
            int readCount = ipcClient->readFromApo(apoBlock.data(), MAX_BLOCK);
            if (readCount == 0) {
                break; // No data from APO
            }
            
            // Read from virtual cable
            if (virtualCapture) {
                int available = virtualBuffer->availableRead();
                
                // --- LATENCY SYNC ---
                // If the buffer accumulates more than 3 blocks of data (30ms), 
                // we fast-forward to the newest data to prevent permanent desync.
                if (available > readCount * 3) {
                    int excess = available - (readCount * 2);
                    virtualBuffer->skip(excess);
                    available = virtualBuffer->availableRead();
                }

                int toRead = (available < readCount) ? available : readCount;
                if (toRead > 0) {
                    virtualBuffer->read(virtualBlock.data(), toRead);
                }
                if (toRead < readCount) {
                    // pad remaining with silence to prevent desync
                    std::memset(&virtualBlock[toRead * inChannels], 0, (readCount - toRead) * inChannels * sizeof(float));
                }
                
                // Mix APO (stereo) into Front Left/Right (Channels 0 and 1) of inputBlock
                // And copy virtualBlock into inputBlock
                for (int i = 0; i < readCount; ++i) {
                    for (int ch = 0; ch < inChannels; ++ch) {
                        float v = virtualBlock[i * inChannels + ch];
                        if (std::abs(v) > virtualPeak) virtualPeak = std::abs(v);
                        if (ch < 2) {
                            v += apoBlock[i * 2 + ch]; // additive mix
                        }
                        inputBlock[i * inChannels + ch] = v;
                    }
                }
            } else {
                std::memcpy(inputBlock.data(), apoBlock.data(), readCount * 2 * sizeof(float));
            }

            // Process the block through VST3
            vstHost->process(inputBlock.data(), outputBlock.data(), readCount, inChannels, outChannels);
            
            // Apply output gain compensation and hard-clip to prevent wrap-around distortion
            int totalSamples = readCount * outChannels;
            for (int i = 0; i < totalSamples; ++i) {
                if (outputGain != 1.0f) {
                    outputBlock[i] *= outputGain;
                }
                // Prevent digital wrap-around when Windows converts float to integer for the DAC
                if (outputBlock[i] > 1.0f) outputBlock[i] = 1.0f;
                if (outputBlock[i] < -1.0f) outputBlock[i] = -1.0f;
            }
            
            // Send back to APO
            ipcClient->writeToApo(outputBlock.data(), readCount);
            framesProcessed += readCount;
            
            auto t1 = std::chrono::steady_clock::now();
            totalProcessingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            processCallCount++;
        }

        // Report every second
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count();
        if (elapsed >= 1) {
            double avgUs = processCallCount > 0 ? (double)totalProcessingTimeUs / processCallCount : 0;
            std::cout << "[DIAG] t=" << elapsed << "s | frames=" << framesProcessed 
                      << " | avg_process=" << (int)avgUs << "us"
                      << " | calls=" << processCallCount 
                      << " | virt_peak=" << virtualPeak << "\n";
            framesProcessed = 0;
            totalProcessingTimeUs = 0;
            processCallCount = 0;
            virtualPeak = 0.0f;
            lastReport = now;
        }
    }

    // Unregister from MMCSS
    if (hTask) AvRevertMmThreadCharacteristics(hTask);

    std::cout << "[DIAG] IPC Processing thread stopped.\n";
}

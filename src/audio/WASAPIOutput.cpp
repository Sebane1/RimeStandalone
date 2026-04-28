#include "WASAPIOutput.h"
#include <iostream>

#pragma comment(lib, "Mmdevapi.lib")

WASAPIOutput::WASAPIOutput(RingBuffer& rb) : 
    ringBuffer(rb), isRunning(false), device(nullptr), 
    audioClient(nullptr), renderClient(nullptr), waveFormat(nullptr), eventHandle(nullptr) {}

WASAPIOutput::~WASAPIOutput() {
    stop();
    if (renderClient) renderClient->Release();
    if (waveFormat) CoTaskMemFree(waveFormat);
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (eventHandle) CloseHandle(eventHandle);
}

bool WASAPIOutput::initialize() {
    HRESULT hr;
    IMMDeviceEnumerator* enumerator = nullptr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return false;

    // Default render device
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) return false;

    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr)) return false;

    // Check for 48kHz Stereo requirement
    if (waveFormat->nSamplesPerSec != 48000 || waveFormat->nChannels != 2) {
        std::cerr << "[ERROR] WASAPI Output endpoint MUST be 48000Hz 2-channel. Current: "
                  << waveFormat->nSamplesPerSec << "Hz " << waveFormat->nChannels << "ch\n";
        return false;
    }

    constexpr REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC / 10; // 100ms buffer

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        0,
        waveFormat,
        nullptr);

    if (FAILED(hr)) {
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsRequestedDuration,
            0,
            waveFormat,
            nullptr);
        if (FAILED(hr)) return false;
    } else {
        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        audioClient->SetEventHandle(eventHandle);
    }

    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) return false;

    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) return false;

    return true;
}

void WASAPIOutput::start() {
    if (isRunning) return;
    isRunning = true;
    renderThread = std::thread(&WASAPIOutput::renderLoop, this);
}

void WASAPIOutput::stop() {
    if (!isRunning) return;
    isRunning = false;
    if (renderThread.joinable()) {
        renderThread.join();
    }
}

void WASAPIOutput::renderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // Pre-fill buffer with silence
    BYTE* pData;
    HRESULT hr = renderClient->GetBuffer(bufferFrameCount, &pData);
    if (SUCCEEDED(hr)) {
        memset(pData, 0, bufferFrameCount * waveFormat->nBlockAlign);
        renderClient->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    
    audioClient->Start();

    while (isRunning) {
        UINT32 numFramesPadding;
        hr = audioClient->GetCurrentPadding(&numFramesPadding);
        
        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
        
        if (numFramesAvailable > 0) {
            hr = renderClient->GetBuffer(numFramesAvailable, &pData);
            if (SUCCEEDED(hr)) {
                std::vector<float> floatData(numFramesAvailable * 2, 0.0f);
                int framesRead = ringBuffer.read(floatData.data(), numFramesAvailable);

                // If underflow, framesRead will be less than numFramesAvailable.
                // It's naturally padded with 0.0f (silence) since we initialized with 0.0f.

                if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)waveFormat;
                    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                        memcpy(pData, floatData.data(), numFramesAvailable * 2 * sizeof(float));
                    } else if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                        if (waveFormat->wBitsPerSample == 16) {
                            short* shortData = (short*)pData;
                            for (size_t i = 0; i < numFramesAvailable * 2; ++i) {
                                float val = floatData[i];
                                if (val > 1.0f) val = 1.0f;
                                if (val < -1.0f) val = -1.0f;
                                shortData[i] = (short)(val * 32767.0f);
                            }
                        }
                    }
                }
                
                renderClient->ReleaseBuffer(numFramesAvailable, 0);
            }
        }

        if (eventHandle) {
            WaitForSingleObject(eventHandle, 2000);
        } else {
            Sleep(bufferFrameCount * 1000 / waveFormat->nSamplesPerSec / 2);
        }
    }

    audioClient->Stop();
    CoUninitialize();
}

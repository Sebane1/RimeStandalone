#include "WASAPICapture.h"
#include <iostream>

#pragma comment(lib, "Mmdevapi.lib")

// Reference: https://docs.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream

#include <Functiondiscoverykeys_devpkey.h>

WASAPICapture::WASAPICapture(RingBuffer& rb, const std::wstring& deviceName, uint32_t targetChannels) : 
    ringBuffer(rb), targetDeviceName(deviceName), targetChannels(targetChannels), isRunning(false), device(nullptr), 
    audioClient(nullptr), captureClient(nullptr), waveFormat(nullptr), eventHandle(nullptr) {}

WASAPICapture::~WASAPICapture() {
    stop();
    if (captureClient) captureClient->Release();
    if (waveFormat) CoTaskMemFree(waveFormat);
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (eventHandle) CloseHandle(eventHandle);
}

bool WASAPICapture::initialize() {
    HRESULT hr;
    IMMDeviceEnumerator* enumerator = nullptr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return false;

    if (targetDeviceName.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        IMMDeviceCollection* collection = nullptr;
        enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDevice = nullptr;
            collection->Item(i, &pDevice);
            IPropertyStore* pProps = nullptr;
            pDevice->OpenPropertyStore(STGM_READ, &pProps);
            PROPVARIANT varName;
            PropVariantInit(&varName);
            pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            std::wstring friendlyName(varName.pwszVal);
            PropVariantClear(&varName);
            pProps->Release();
            
            if (friendlyName.find(targetDeviceName) != std::wstring::npos) {
                device = pDevice;
                break;
            } else {
                pDevice->Release();
            }
        }
        collection->Release();
        
        if (!device) {
            std::wcerr << L"[ERROR] WASAPI Capture could not find device: " << targetDeviceName << L"\n";
            enumerator->Release();
            return false;
        }
    }
    enumerator->Release();
    if (!device) return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) return false;

    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr)) return false;

    // Allow testing against the required channels
    if (waveFormat->nSamplesPerSec != 48000 || waveFormat->nChannels != targetChannels) {
        std::cerr << "[ERROR] WASAPI Capture endpoint MUST be 48000Hz " << targetChannels << "-channel. Current: "
                  << waveFormat->nSamplesPerSec << "Hz " << waveFormat->nChannels << "ch\n";
        return false;
    }

    constexpr REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC; 

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        0,
        waveFormat,
        nullptr);

    std::cout << "[WASAPI] Capture initialized: " 
              << waveFormat->nSamplesPerSec << "Hz, " 
              << waveFormat->nChannels << "ch, "
              << waveFormat->wBitsPerSample << "bps, "
              << "FormatTag=" << waveFormat->wFormatTag << "\n";
              
    if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)waveFormat;
        std::cout << "[WASAPI] SubFormat: ";
        if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) std::cout << "FLOAT\n";
        else if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) std::cout << "PCM\n";
        else std::cout << "UNKNOWN\n";
    }

    if (FAILED(hr)) {
        // Fallback without event callback
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

    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) return false;

    return true;
}

void WASAPICapture::start() {
    if (isRunning) return;
    isRunning = true;
    captureThread = std::thread(&WASAPICapture::captureLoop, this);
}

void WASAPICapture::stop() {
    if (!isRunning) return;
    isRunning = false;
    if (captureThread.joinable()) {
        captureThread.join();
    }
}

void WASAPICapture::captureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audioClient->Start();

    int captureCalls = 0;
    float capturePeak = 0.0f;
    auto lastReport = std::chrono::steady_clock::now();

    while (isRunning) {
        if (eventHandle) {
            WaitForSingleObject(eventHandle, 2000);
        } else {
            Sleep(bufferFrameCount * 1000 / waveFormat->nSamplesPerSec / 2);
        }

        UINT32 packetLength = 0;
        HRESULT hr = captureClient->GetNextPacketSize(&packetLength);

        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;

            hr = captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // write silence
                std::vector<float> silence(numFramesAvailable * targetChannels, 0.0f);
                ringBuffer.write(silence.data(), numFramesAvailable);
            } else {
                // Convert to float and write
                if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    WAVEFORMATEXTENSIBLE* pEx = (WAVEFORMATEXTENSIBLE*)waveFormat;
                    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                        float* floatData = (float*)pData;
                        for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                            if (std::abs(floatData[i]) > capturePeak) capturePeak = std::abs(floatData[i]);
                        }
                        ringBuffer.write(floatData, numFramesAvailable);
                    } else if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                        if (waveFormat->wBitsPerSample == 16) {
                            std::vector<float> floatData(numFramesAvailable * targetChannels);
                            short* shortData = (short*)pData;
                            for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                                floatData[i] = shortData[i] / 32768.0f;
                            }
                            ringBuffer.write(floatData.data(), numFramesAvailable);
                        } else if (waveFormat->wBitsPerSample == 24) {
                            std::vector<float> floatData(numFramesAvailable * targetChannels);
                            BYTE* byteData = pData;
                            for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                                int32_t val = (byteData[i*3] << 8) | (byteData[i*3+1] << 16) | (byteData[i*3+2] << 24);
                                floatData[i] = (val >> 8) / 8388608.0f;
                            }
                            ringBuffer.write(floatData.data(), numFramesAvailable);
                        } else if (waveFormat->wBitsPerSample == 32) {
                            std::vector<float> floatData(numFramesAvailable * targetChannels);
                            int32_t* intData = (int32_t*)pData;
                            for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                                floatData[i] = intData[i] / 2147483648.0f;
                            }
                            ringBuffer.write(floatData.data(), numFramesAvailable);
                        }
                    }
                } else if (waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
                    if (waveFormat->wBitsPerSample == 16) {
                        std::vector<float> floatData(numFramesAvailable * targetChannels);
                        short* shortData = (short*)pData;
                        for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                            floatData[i] = shortData[i] / 32768.0f;
                        }
                        ringBuffer.write(floatData.data(), numFramesAvailable);
                    } else if (waveFormat->wBitsPerSample == 24) {
                        std::vector<float> floatData(numFramesAvailable * targetChannels);
                        BYTE* byteData = pData;
                        for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                            int32_t val = (byteData[i*3] << 8) | (byteData[i*3+1] << 16) | (byteData[i*3+2] << 24);
                            floatData[i] = (val >> 8) / 8388608.0f;
                        }
                        ringBuffer.write(floatData.data(), numFramesAvailable);
                    } else if (waveFormat->wBitsPerSample == 32) {
                        std::vector<float> floatData(numFramesAvailable * targetChannels);
                        int32_t* intData = (int32_t*)pData;
                        for (size_t i = 0; i < numFramesAvailable * targetChannels; ++i) {
                            floatData[i] = intData[i] / 2147483648.0f;
                        }
                        ringBuffer.write(floatData.data(), numFramesAvailable);
                    }
                }
            }

            captureClient->ReleaseBuffer(numFramesAvailable);
            captureClient->GetNextPacketSize(&packetLength);
            captureCalls++;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 1) {
            std::cout << "[CAPTURE] calls=" << captureCalls << " | peak=" << capturePeak << "\n";
            captureCalls = 0;
            capturePeak = 0.0f;
            lastReport = now;
        }

        if (onCapture) {
            onCapture();
        }
    }

    audioClient->Stop();
    CoUninitialize();
}

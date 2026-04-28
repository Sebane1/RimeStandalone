#include "WASAPIKeepAlive.h"
#include <iostream>
#include <vector>
#include <functiondiscoverykeys_devpkey.h>

WASAPIKeepAlive::WASAPIKeepAlive(const std::wstring& targetDeviceName) 
    : targetDeviceName(targetDeviceName) {}

WASAPIKeepAlive::~WASAPIKeepAlive() {
    stop();
}

bool WASAPIKeepAlive::initialize() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return false;

    if (targetDeviceName.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    } else {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr)) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(collection->Item(i, &dev))) {
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                            std::wstring friendlyName = varName.pwszVal;
                            if (friendlyName.find(targetDeviceName) != std::wstring::npos) {
                                device = dev;
                                PropVariantClear(&varName);
                                props->Release();
                                break;
                            }
                        }
                        PropVariantClear(&varName);
                        props->Release();
                    }
                    if (device == nullptr) dev->Release();
                }
            }
            collection->Release();
        }
    }

    if (!device) {
        enumerator->Release();
        enumerator = nullptr;
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) return false;

    WAVEFORMATEX* waveFormat = nullptr;
    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr)) return false;

    hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, // 0 for shared mode default latency
        0,
        waveFormat,
        nullptr);

    if (FAILED(hr)) {
        CoTaskMemFree(waveFormat);
        return false;
    }

    hr = audioClient->SetEventHandle(hEvent);
    if (FAILED(hr)) {
        CoTaskMemFree(waveFormat);
        return false;
    }

    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    CoTaskMemFree(waveFormat);

    return SUCCEEDED(hr);
}

void WASAPIKeepAlive::start() {
    if (running || !audioClient || !renderClient) return;
    
    running = true;
    HRESULT hr = audioClient->Start();
    if (SUCCEEDED(hr)) {
        renderThread = std::thread(&WASAPIKeepAlive::renderLoop, this);
    } else {
        running = false;
    }
}

void WASAPIKeepAlive::stop() {
    if (!running) return;
    running = false;
    
    if (hEvent) SetEvent(hEvent);
    
    if (renderThread.joinable()) {
        renderThread.join();
    }
    
    if (audioClient) {
        audioClient->Stop();
    }
}

void WASAPIKeepAlive::renderLoop() {
    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);

    // Initial fill with silence
    BYTE* data = nullptr;
    if (SUCCEEDED(renderClient->GetBuffer(bufferFrameCount, &data))) {
        renderClient->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    while (running) {
        DWORD waitResult = WaitForSingleObject(hEvent, 100);
        if (!running) break;
        if (waitResult == WAIT_OBJECT_0) {
            UINT32 numFramesPadding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&numFramesPadding))) continue;

            UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
            if (numFramesAvailable > 0) {
                if (SUCCEEDED(renderClient->GetBuffer(numFramesAvailable, &data))) {
                    renderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }
    }
}

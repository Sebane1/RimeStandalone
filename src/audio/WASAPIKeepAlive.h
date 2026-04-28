#pragma once
#include <windows.h>
#include <string>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>

class WASAPIKeepAlive {
public:
    WASAPIKeepAlive(const std::wstring& targetDeviceName = L"");
    ~WASAPIKeepAlive();

    bool initialize();
    void start();
    void stop();

private:
    std::wstring targetDeviceName;
    
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    HANDLE hEvent = NULL;

    std::thread renderThread;
    std::atomic<bool> running{false};

    void renderLoop();
};

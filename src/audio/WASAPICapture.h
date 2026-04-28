#pragma once
#include "RingBuffer.h"
#include <windows.h>
#include <string>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>

#include <functional>

class WASAPICapture {
public:
    WASAPICapture(RingBuffer& ringBuffer, const std::wstring& deviceName = L"", uint32_t targetChannels = 8);
    ~WASAPICapture();

    bool initialize();
    void start();
    void stop();

    std::function<void()> onCapture;

private:
    void captureLoop();

    RingBuffer& ringBuffer;
    std::wstring targetDeviceName;
    uint32_t targetChannels;
    std::thread captureThread;
    std::atomic<bool> isRunning;

    IMMDevice* device;
    IAudioClient* audioClient;
    IAudioCaptureClient* captureClient;
    WAVEFORMATEX* waveFormat;
    UINT32 bufferFrameCount;
    HANDLE eventHandle;
};

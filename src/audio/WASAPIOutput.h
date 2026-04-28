#pragma once
#include "RingBuffer.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>

class WASAPIOutput {
public:
    WASAPIOutput(RingBuffer& ringBuffer);
    ~WASAPIOutput();

    bool initialize();
    void start();
    void stop();

private:
    void renderLoop();

    RingBuffer& ringBuffer;
    std::thread renderThread;
    std::atomic<bool> isRunning;

    IMMDevice* device;
    IAudioClient* audioClient;
    IAudioRenderClient* renderClient;
    WAVEFORMATEX* waveFormat;
    UINT32 bufferFrameCount;
    HANDLE eventHandle;
};

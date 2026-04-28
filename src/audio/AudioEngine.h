#pragma once
#include "../config/Config.h"
#include "IpcAudioClient.h"
#include "../vst/VSTHost.h"
#include "WASAPICapture.h"
#include "WASAPIKeepAlive.h"
#include "RingBuffer.h"
#include <memory>
#include <thread>
#include <atomic>

class AudioEngine {
public:
    AudioEngine(const Config& config);
    ~AudioEngine();

    bool initialize();
    void start();
    void shutdown();
    bool isRunning() const;
    
    bool showUI();
    
    bool loadPluginState(const std::string& path);
    bool savePluginState(const std::string& path);

private:
    void processingLoop();

    Config config;
    std::atomic<bool> running;
    
    std::thread processingThread;
    
    std::unique_ptr<rime::IpcAudioClient> ipcClient;
    std::unique_ptr<VSTHost> vstHost;
    
    std::unique_ptr<RingBuffer> virtualBuffer;
    std::unique_ptr<WASAPICapture> virtualCapture;
    std::unique_ptr<WASAPIKeepAlive> keepAlive;
};

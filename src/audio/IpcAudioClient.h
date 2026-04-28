#pragma once
#include "IpcSharedMemory.h"
#include <windows.h>
#include <string>

namespace rime {

class IpcAudioClient {
public:
    IpcAudioClient();
    ~IpcAudioClient();

    bool initialize(uint32_t sampleRate, uint32_t channels);
    void shutdown();

    // Host calls this to read data sent by the APO
    int readFromApo(float* outData, int maxFrames);
    
    // Host calls this to write processed data back to the APO
    int writeToApo(const float* data, int numFrames);
    
    // Block until the APO signals new data is available, or timeout expires
    bool waitForData(DWORD timeoutMs);
    
    // Pre-fill fromHost with silence to prevent cold-start underruns
    void prefillSilence(int numFrames);

    bool isInitialized() const { return sharedMem != nullptr; }

private:
    HANDLE hMapFile = NULL;
    HANDLE hDataEvent = NULL;
    IpcMemoryLayout* sharedMem = nullptr;
    
    // Local copy of format
    uint32_t m_sampleRate = 48000;
    uint32_t m_channels = 2;
    uint32_t m_lastFlushCount[4] = {0};

    int getAvailableRead(SharedRingBuffer& ring) const;
    int getAvailableWrite(SharedRingBuffer& ring) const;
};

} // namespace rime

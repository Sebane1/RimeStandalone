#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace rime {

// 10ms of audio at 48000Hz = 480 frames. 1 second = 48000.
// Let's reserve 1 second of buffer per ring just to be safe.
constexpr uint32_t MAX_CAPACITY_FRAMES = 48000;
constexpr uint32_t MAX_CHANNELS = 2; // Stereo

struct SharedRingBuffer {
    std::atomic<uint32_t> writePos;
    std::atomic<uint32_t> readPos;
    uint32_t capacityFrames;
    uint32_t padding; // Ensure 16-byte alignment
    
    // Fixed size buffer to keep the memory map predictable
    float data[MAX_CAPACITY_FRAMES * MAX_CHANNELS];
};


constexpr uint32_t MAX_IPC_SLOTS = 4;

struct IpcSlot {
    std::atomic<uint32_t> instanceId;      // 0 means slot is free
    std::atomic<uint32_t> lastProcessTime; // GetTickCount()
    std::atomic<uint32_t> flushRequestCount;
    SharedRingBuffer toHost;
    SharedRingBuffer fromHost;
};

struct IpcMemoryLayout {
    // Audio Format Information
    uint32_t sampleRate;
    uint32_t channels;
    
    // Status flags
    std::atomic<bool> hostIsRunning;
    std::atomic<bool> apoIsRunning;
    
    // Master Instance Tracking (the instance that will output the final mixed audio)
    std::atomic<uint32_t> masterInstanceId;
    std::atomic<uint32_t> masterLastProcessTime;
    
    // Diagnostics
    std::atomic<uint32_t> apoUnderrunCount;
    
    // Parallel processing slots for Hardware Offload architectures (multiple EFX pins)
    IpcSlot slots[MAX_IPC_SLOTS];
};

constexpr const wchar_t* IPC_SHARED_MEM_NAME = L"Global\\RimeAudioSharedMem";
constexpr const wchar_t* IPC_EVENT_NAME = L"Global\\RimeApoDataReady";
constexpr size_t IPC_SHARED_MEM_SIZE = sizeof(IpcMemoryLayout);

} // namespace rime

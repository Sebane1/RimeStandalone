#include "IpcAudioClient.h"
#include <iostream>
#include <vector>
#include <sddl.h>

namespace rime {

IpcAudioClient::IpcAudioClient() {
}

IpcAudioClient::~IpcAudioClient() {
    shutdown();
}

bool IpcAudioClient::initialize(uint32_t sampleRate, uint32_t channels) {
    m_sampleRate = sampleRate;
    m_channels = channels;

    PSECURITY_DESCRIPTOR pSD = NULL;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &pSD, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };

    // Create or open the named shared memory map
    hMapFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,    // use paging file
        &sa,                     // permissive security
        PAGE_READWRITE,          // read/write access
        0,                       // maximum object size (high-order DWORD)
        IPC_SHARED_MEM_SIZE,     // maximum object size (low-order DWORD)
        IPC_SHARED_MEM_NAME);    // name of mapping object
        
    DWORD err = GetLastError();
    if (pSD) LocalFree(pSD);

    if (hMapFile == NULL && err == ERROR_ACCESS_DENIED) {
        hMapFile = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, IPC_SHARED_MEM_NAME);
        err = GetLastError();
    }

    if (hMapFile == NULL) {
        std::cerr << "Could not create file mapping object: " << err << std::endl;
        return false;
    }

    // Map the view into our process address space
    sharedMem = (IpcMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, IPC_SHARED_MEM_SIZE);

    if (sharedMem == NULL) {
        std::cerr << "Could not map view of file: " << GetLastError() << std::endl;
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    // If we are the first to create it, OR if the APO created it but left it zeroed out, initialize the layout
    if (err != ERROR_ALREADY_EXISTS || sharedMem->slots[0].toHost.capacityFrames == 0) {
        // Only memset if it wasn't already mapped, to avoid erasing the apoIsRunning flag
        if (err != ERROR_ALREADY_EXISTS) {
            memset(sharedMem, 0, IPC_SHARED_MEM_SIZE);
        }
        
        sharedMem->sampleRate = sampleRate;
        sharedMem->channels = channels;
        
        for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
            sharedMem->slots[i].toHost.capacityFrames = MAX_CAPACITY_FRAMES;
            sharedMem->slots[i].toHost.writePos = 0;
            sharedMem->slots[i].toHost.readPos = 0;
            
            sharedMem->slots[i].fromHost.capacityFrames = MAX_CAPACITY_FRAMES;
            sharedMem->slots[i].fromHost.writePos = 0;
            sharedMem->slots[i].fromHost.readPos = 0;
        }
        
        sharedMem->apoUnderrunCount = 0;
    }

    // Create the named event for APO -> Host signaling (auto-reset)
    {
        PSECURITY_DESCRIPTOR pSD2 = NULL;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &pSD2, NULL);
        SECURITY_ATTRIBUTES saEvt = { sizeof(saEvt), pSD2, FALSE };
        hDataEvent = CreateEventW(&saEvt, FALSE, FALSE, IPC_EVENT_NAME); // auto-reset
        if (!hDataEvent) {
            hDataEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, IPC_EVENT_NAME);
        }
        if (pSD2) LocalFree(pSD2);
        if (!hDataEvent) {
            std::cerr << "Could not create/open IPC event: " << GetLastError() << std::endl;
            // Non-fatal: fall back to polling if event fails
        }
    }

    sharedMem->hostIsRunning = true;
    
    // Pre-fill fromHost with silence so the APO has data on its first read
    prefillSilence(48); // 1ms at 48kHz — minimal startup buffer
    
    return true;
}

void IpcAudioClient::shutdown() {
    if (sharedMem) {
        sharedMem->hostIsRunning = false;
        UnmapViewOfFile(sharedMem);
        sharedMem = nullptr;
    }
    if (hDataEvent) {
        CloseHandle(hDataEvent);
        hDataEvent = NULL;
    }
    if (hMapFile) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
}

int IpcAudioClient::getAvailableRead(SharedRingBuffer& ring) const {
    uint32_t w = ring.writePos.load(std::memory_order_acquire);
    uint32_t r = ring.readPos.load(std::memory_order_acquire);
    
    if (w >= r) {
        return w - r;
    } else {
        return ring.capacityFrames - r + w;
    }
}

int IpcAudioClient::getAvailableWrite(SharedRingBuffer& ring) const {
    uint32_t w = ring.writePos.load(std::memory_order_acquire);
    uint32_t r = ring.readPos.load(std::memory_order_acquire);
    
    if (r > w) {
        return r - w - 1; // 1 frame reserved to distinguish full from empty
    } else {
        return ring.capacityFrames - w + r - 1;
    }
}

int IpcAudioClient::readFromApo(float* outData, int maxFrames) {
    if (!sharedMem) return 0;
    
    // Synchronize multi-slot reads: we must wait until ALL active slots have data
    // to prevent interleaved "hard swapping" output to the master instance.
    uint32_t now = GetTickCount();
    int activeSlots = 0;
    int readySlots = 0;
    
    for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
        if (sharedMem->slots[i].instanceId.load(std::memory_order_acquire) == 0) continue;
        
        uint32_t lastProcessTime = sharedMem->slots[i].lastProcessTime.load(std::memory_order_acquire);
        // If the slot has been processed within the last 100ms, consider it actively running
        if ((now - lastProcessTime) <= 100) {
            activeSlots++;
            SharedRingBuffer& ring = sharedMem->slots[i].toHost;
            if (getAvailableRead(ring) >= maxFrames) {
                readySlots++;
            }
        }
    }
    
    // If there are active slots, but not all of them have data yet, we must wait!
    // Returning 0 causes the bridge to loop and wait for the remaining slots to fire their events.
    if (activeSlots > 0 && readySlots < activeSlots) {
        return 0;
    }
    
    memset(outData, 0, maxFrames * m_channels * sizeof(float));
    int maxRead = 0;
    
    // We need a temporary buffer to read each slot's data
    float* tempBuf = new float[maxFrames * m_channels];
    
    for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
        if (sharedMem->slots[i].instanceId.load(std::memory_order_acquire) == 0) continue;
        
        SharedRingBuffer& ring = sharedMem->slots[i].toHost;
        
        uint32_t currentFlush = sharedMem->slots[i].flushRequestCount.load(std::memory_order_acquire);
        if (currentFlush != m_lastFlushCount[i]) {
            m_lastFlushCount[i] = currentFlush;
            uint32_t w = ring.writePos.load(std::memory_order_acquire);
            ring.readPos.store(w, std::memory_order_release);
        }
        
        int available = getAvailableRead(ring);
        if (available == 0) continue;
        
        int maxAllowedLatency = 4800; 
        if (available > maxAllowedLatency) {
            uint32_t skipFrames = available - maxFrames;
            uint32_t r = ring.readPos.load(std::memory_order_acquire);
            r = (r + skipFrames) % ring.capacityFrames;
            ring.readPos.store(r, std::memory_order_release);
            available = maxFrames;
        }
        
        int toRead = (available < maxFrames) ? available : maxFrames;
        if (toRead > maxRead) maxRead = toRead;
        
        uint32_t r = ring.readPos.load(std::memory_order_acquire);
        uint32_t endSpace = ring.capacityFrames - r;
        
        if (toRead <= endSpace) {
            memcpy(tempBuf, &ring.data[r * m_channels], toRead * m_channels * sizeof(float));
        } else {
            memcpy(tempBuf, &ring.data[r * m_channels], endSpace * m_channels * sizeof(float));
            memcpy(tempBuf + (endSpace * m_channels), &ring.data[0], (toRead - endSpace) * m_channels * sizeof(float));
        }
        
        // Mix into outData
        for (int j = 0; j < toRead * m_channels; ++j) {
            outData[j] += tempBuf[j];
        }
        
        ring.readPos.store((r + toRead) % ring.capacityFrames, std::memory_order_release);
    }
    
    delete[] tempBuf;
    return maxRead;
}

int IpcAudioClient::writeToApo(const float* data, int numFrames) {
    if (!sharedMem) return 0;
    
    uint32_t masterId = sharedMem->masterInstanceId.load(std::memory_order_acquire);
    int masterSlot = -1;
    
    for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
        if (sharedMem->slots[i].instanceId.load(std::memory_order_acquire) == masterId) {
            masterSlot = i;
            break;
        }
    }
    
    if (masterSlot == -1) return 0; // Master not found, drop data
    
    SharedRingBuffer& ring = sharedMem->slots[masterSlot].fromHost;
    
    int available = getAvailableWrite(ring);
    int toWrite = (available < numFrames) ? available : numFrames;
    if (toWrite == 0) return 0;
    
    uint32_t w = ring.writePos.load(std::memory_order_acquire);
    uint32_t endSpace = ring.capacityFrames - w;
    
    if (toWrite <= endSpace) {
        memcpy(&ring.data[w * m_channels], data, toWrite * m_channels * sizeof(float));
    } else {
        memcpy(&ring.data[w * m_channels], data, endSpace * m_channels * sizeof(float));
        memcpy(&ring.data[0], data + (endSpace * m_channels), (toWrite - endSpace) * m_channels * sizeof(float));
    }
    
    ring.writePos.store((w + toWrite) % ring.capacityFrames, std::memory_order_release);
    return toWrite;
}

bool IpcAudioClient::waitForData(DWORD timeoutMs) {
    if (hDataEvent) {
        return WaitForSingleObject(hDataEvent, timeoutMs) == WAIT_OBJECT_0;
    }
    // Fallback to Sleep if event is unavailable
    Sleep(1);
    return false;
}

void IpcAudioClient::prefillSilence(int numFrames) {
    if (!sharedMem) return;
    std::vector<float> silence(numFrames * m_channels, 0.0f);
    
    for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
        SharedRingBuffer& ring = sharedMem->slots[i].fromHost;
        int available = getAvailableWrite(ring);
        int toWrite = (available < numFrames) ? available : numFrames;
        
        if (toWrite > 0) {
            uint32_t w = ring.writePos.load(std::memory_order_acquire);
            uint32_t endSpace = ring.capacityFrames - w;
            
            if (toWrite <= endSpace) {
                memcpy(&ring.data[w * m_channels], silence.data(), toWrite * m_channels * sizeof(float));
            } else {
                memcpy(&ring.data[w * m_channels], silence.data(), endSpace * m_channels * sizeof(float));
                memcpy(&ring.data[0], silence.data() + (endSpace * m_channels), (toWrite - endSpace) * m_channels * sizeof(float));
            }
            ring.writePos.store((w + toWrite) % ring.capacityFrames, std::memory_order_release);
        }
    }
    std::cout << "[IPC] Pre-filled all fromHost slots with " << numFrames << " frames of silence\n";
}

} // namespace rime

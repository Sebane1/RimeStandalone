#include "RimeApo.h"
#include <sddl.h>
#include <new>
#include <stdio.h>

extern void PipeLog(const char* format, ...);

static std::atomic<uint32_t> s_nextInstanceId{1};

RimeApo::RimeApo(IUnknown* pUnkOuter) : m_refCount(1), m_format(nullptr), m_channels(2), m_hMapFile(NULL), m_hDataEvent(NULL), m_sharedMem(nullptr), m_retryCounter(0), m_instanceId(s_nextInstanceId.fetch_add(1)), m_mySlot(-1), m_hasSlot(false), m_silentFrames(0), m_NonDelegatingUnknown(this), m_pUnkOuter(pUnkOuter) {
    if (!m_pUnkOuter) {
        m_pUnkOuter = &m_NonDelegatingUnknown;
    }
    PipeLog("RimeApo Constructor called! pUnkOuter = %p, instanceId = %u", pUnkOuter, m_instanceId);
}

RimeApo::~RimeApo() {
    PipeLog("RimeApo Destructor called!");
    unmapSharedMemory();
    if (m_format) {
        CoTaskMemFree(m_format);
    }
}

HRESULT RimeApo::CNonDelegatingUnknown::QueryInterface(REFIID riid, void** ppv) { return m_pApo->NonDelegatingQueryInterface(riid, ppv); }
ULONG RimeApo::CNonDelegatingUnknown::AddRef() { return m_pApo->NonDelegatingAddRef(); }
ULONG RimeApo::CNonDelegatingUnknown::Release() { return m_pApo->NonDelegatingRelease(); }

HRESULT RimeApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(&m_NonDelegatingUnknown);
        PipeLog("RimeApo NonDelegatingQueryInterface: IUnknown");
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
        PipeLog("RimeApo NonDelegatingQueryInterface: IAudioProcessingObject");
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(static_cast<IAudioSystemEffects2*>(this));
        PipeLog("RimeApo NonDelegatingQueryInterface: IAudioSystemEffects");
    } else if (riid == __uuidof(IAudioSystemEffects2)) {
        *ppv = static_cast<IAudioSystemEffects2*>(this);
        PipeLog("RimeApo NonDelegatingQueryInterface: IAudioSystemEffects2");
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        PipeLog("RimeApo NonDelegatingQueryInterface: IAudioProcessingObjectRT");
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        PipeLog("RimeApo NonDelegatingQueryInterface: IAudioProcessingObjectConfiguration");
    } else if (riid == __uuidof(IApoAuxiliaryInputConfiguration)) {
        *ppv = static_cast<IApoAuxiliaryInputConfiguration*>(this);
        PipeLog("RimeApo NonDelegatingQueryInterface: IApoAuxiliaryInputConfiguration");
    } else {
        PipeLog("RimeApo NonDelegatingQueryInterface: E_NOINTERFACE for %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", 
                riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        return E_NOINTERFACE;
    }

    if (riid == IID_IUnknown) {
        NonDelegatingAddRef();
    } else {
        ((IUnknown*)*ppv)->AddRef(); // Delegate to outer object!
    }
    
    return S_OK;
}

ULONG RimeApo::NonDelegatingAddRef() {
    ULONG count = ++m_refCount;
    PipeLog("RimeApo NonDelegatingAddRef: %u", count);
    return count;
}

ULONG RimeApo::NonDelegatingRelease() {
    ULONG count = --m_refCount;
    PipeLog("RimeApo NonDelegatingRelease: %u", count);
    if (count == 0) {
        PipeLog("RimeApo DELETING SELF!");
        delete this;
    }
    return count;
}

IFACEMETHODIMP RimeApo::QueryInterface(REFIID riid, void** ppv) {
    return m_pUnkOuter->QueryInterface(riid, ppv);
}

IFACEMETHODIMP_(ULONG) RimeApo::AddRef() {
    return m_pUnkOuter->AddRef();
}

IFACEMETHODIMP_(ULONG) RimeApo::Release() {
    return m_pUnkOuter->Release();
}

IFACEMETHODIMP RimeApo::Reset() {
    PipeLog("RimeApo Reset called!");
    return S_OK;
}

IFACEMETHODIMP RimeApo::GetLatency(HNSTIME* pTime) {
    PipeLog("RimeApo GetLatency called!");
    if (!pTime) return E_POINTER;
    *pTime = 0; // We report 0 latency to avoid adding delay to the audio graph, though practically the host adds some delay
    return S_OK;
}

IFACEMETHODIMP RimeApo::GetEffectsList(LPGUID *ppEffectsIds, UINT *pcEffects, HANDLE Event) {
    PipeLog("RimeApo GetEffectsList called!");
    if (!ppEffectsIds || !pcEffects) return E_POINTER;
    *pcEffects = 0; // We don't report internal hardware effects here, just process the stream.
    return S_OK;
}

IFACEMETHODIMP RimeApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) {
    PipeLog("RimeApo GetRegistrationProperties called!");
    if (!ppRegProps) return E_POINTER;
    *ppRegProps = (APO_REG_PROPERTIES*)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
    if (!*ppRegProps) return E_OUTOFMEMORY;
    
    (*ppRegProps)->clsid = CLSID_RimeApo;
    (*ppRegProps)->Flags = (APO_FLAG)7; // APO_FLAG_INPLACE (1) | APO_FLAG_SAMPLESPERSEC_MUST_MATCH (2) | APO_FLAG_BITSPERSAMPLE_MUST_MATCH (4)
    wcsncpy_s((*ppRegProps)->szFriendlyName, 256, L"Rime APO", _TRUNCATE);
    wcsncpy_s((*ppRegProps)->szCopyrightInfo, 256, L"Neumann", _TRUNCATE);
    (*ppRegProps)->u32MajorVersion = 1;
    (*ppRegProps)->u32MinorVersion = 1;
    (*ppRegProps)->u32MinInputConnections = 1;
    (*ppRegProps)->u32MaxInputConnections = 1;
    (*ppRegProps)->u32MinOutputConnections = 1;
    (*ppRegProps)->u32MaxOutputConnections = 1;
    (*ppRegProps)->u32MaxInstances = 0xffffffff;
    (*ppRegProps)->u32NumAPOInterfaces = 1;
    (*ppRegProps)->iidAPOInterfaceList[0] = __uuidof(IAudioSystemEffects);
    return S_OK;
}

IFACEMETHODIMP RimeApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    PipeLog("RimeApo Initialize called! cbDataSize: %u", cbDataSize);
    
    if (cbDataSize == sizeof(APOInitSystemEffects2) && pbyData) {
        APOInitSystemEffects2* pInit = (APOInitSystemEffects2*)pbyData;
        PipeLog("RimeApo Initialize: InitializeForDiscoveryOnly = %d", pInit->InitializeForDiscoveryOnly);
        PipeLog("RimeApo Initialize: AudioProcessingMode = %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                pInit->AudioProcessingMode.Data1, pInit->AudioProcessingMode.Data2, pInit->AudioProcessingMode.Data3,
                pInit->AudioProcessingMode.Data4[0], pInit->AudioProcessingMode.Data4[1], pInit->AudioProcessingMode.Data4[2],
                pInit->AudioProcessingMode.Data4[3], pInit->AudioProcessingMode.Data4[4], pInit->AudioProcessingMode.Data4[5],
                pInit->AudioProcessingMode.Data4[6], pInit->AudioProcessingMode.Data4[7]);
    }
    
    mapSharedMemory();
    return S_OK;
}

IFACEMETHODIMP RimeApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) {
    if (!pRequestedInputFormat || !ppSupportedInputFormat) return E_POINTER;
    
    const WAVEFORMATEX* pWfx = pRequestedInputFormat->GetAudioFormat();
    if (pWfx) {
        PipeLog("RimeApo IsInputFormatSupported: wFormatTag=%u, nChannels=%u, nSamplesPerSec=%u, wBitsPerSample=%u",
                pWfx->wFormatTag, pWfx->nChannels, pWfx->nSamplesPerSec, pWfx->wBitsPerSample);
    } else {
        PipeLog("RimeApo IsInputFormatSupported: FORMAT IS NULL?");
    }
    
    *ppSupportedInputFormat = pRequestedInputFormat;
    (*ppSupportedInputFormat)->AddRef();
    return S_OK;
}

IFACEMETHODIMP RimeApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat) {
    PipeLog("RimeApo IsOutputFormatSupported called!");
    if (!pRequestedOutputFormat || !ppSupportedOutputFormat) return E_POINTER;
    
    const WAVEFORMATEX* pWfx = pRequestedOutputFormat->GetAudioFormat();
    
    *ppSupportedOutputFormat = pRequestedOutputFormat;
    (*ppSupportedOutputFormat)->AddRef();
    return S_OK;
}

IFACEMETHODIMP RimeApo::GetInputChannelCount(UINT32* pu32ChannelCount) {
    PipeLog("RimeApo GetInputChannelCount called!");
    if (!pu32ChannelCount) return E_POINTER;
    *pu32ChannelCount = 2;
    return S_OK;
}

// IApoAuxiliaryInputConfiguration Implementation
IFACEMETHODIMP RimeApo::AddAuxiliaryInput(DWORD dwInputId, UINT32 cbDataSize, BYTE *pbyData, APO_CONNECTION_DESCRIPTOR *pInputConnection) {
    PipeLog("RimeApo AddAuxiliaryInput called! dwInputId=%u", dwInputId);
    return S_OK;
}

IFACEMETHODIMP RimeApo::RemoveAuxiliaryInput(DWORD dwInputId) {
    PipeLog("RimeApo RemoveAuxiliaryInput called! dwInputId=%u", dwInputId);
    return S_OK;
}

IFACEMETHODIMP RimeApo::IsInputFormatSupported(IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat) {
    PipeLog("RimeApo IApoAuxiliaryInputConfiguration::IsInputFormatSupported called!");
    return this->IsInputFormatSupported(nullptr, pRequestedInputFormat, ppSupportedInputFormat);
}

bool RimeApo::mapSharedMemory() {
    PSECURITY_DESCRIPTOR pSD = NULL;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &pSD, NULL);
    
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    m_hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, rime::IPC_SHARED_MEM_SIZE, rime::IPC_SHARED_MEM_NAME);
    if (!m_hMapFile && GetLastError() == ERROR_ACCESS_DENIED) {
        // If it already exists and we don't have permission to create it with this SD, try opening it.
        m_hMapFile = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, rime::IPC_SHARED_MEM_NAME);
    }
    
    if (m_hMapFile) {
        m_sharedMem = (rime::IpcMemoryLayout*)MapViewOfFile(m_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, rime::IPC_SHARED_MEM_SIZE);
        if (m_sharedMem) {
            m_sharedMem->apoIsRunning = true;
            PipeLog("RimeApo mapSharedMemory SUCCESS!");
        } else {
            PipeLog("RimeApo MapViewOfFile FAILED: %d", GetLastError());
        }
    } else {
        PipeLog("RimeApo CreateFileMappingW FAILED: %d", GetLastError());
    }
    
    // Open/create the named event for signaling the host
    if (!m_hDataEvent) {
        m_hDataEvent = CreateEventW(&sa, FALSE, FALSE, rime::IPC_EVENT_NAME); // auto-reset
        if (!m_hDataEvent) {
            m_hDataEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, rime::IPC_EVENT_NAME);
        }
        if (m_hDataEvent) {
            PipeLog("RimeApo event handle acquired!");
        } else {
            PipeLog("RimeApo WARNING: Could not open event, host may poll. err=%d", GetLastError());
        }
    }
    
    if (pSD) LocalFree(pSD);
    return m_sharedMem != nullptr;
}

void RimeApo::unmapSharedMemory() {
    releaseSlot();
    if (m_sharedMem) {
        m_sharedMem->apoIsRunning = false;
        UnmapViewOfFile(m_sharedMem);
        m_sharedMem = nullptr;
    }
    if (m_hDataEvent) {
        CloseHandle(m_hDataEvent);
        m_hDataEvent = NULL;
    }
    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
    }
}

bool RimeApo::tryClaimSlot() {
    if (!m_sharedMem || m_hasSlot) return m_hasSlot;
    
    uint32_t now = GetTickCount();
    
    for (int i = 0; i < rime::MAX_IPC_SLOTS; ++i) {
        uint32_t owner = m_sharedMem->slots[i].instanceId.load(std::memory_order_acquire);
        uint32_t lastProcessTime = m_sharedMem->slots[i].lastProcessTime.load(std::memory_order_acquire);
        
        if (owner == 0 || (now - lastProcessTime) > 100) {
            if (m_sharedMem->slots[i].instanceId.compare_exchange_strong(owner, m_instanceId, std::memory_order_acq_rel)) {
                m_mySlot = i;
                m_hasSlot = true;
                m_sharedMem->slots[i].lastProcessTime.store(now, std::memory_order_release);
                m_sharedMem->slots[i].flushRequestCount.fetch_add(1, std::memory_order_release);
                
                if (m_sharedMem->slots[i].fromHost.capacityFrames > 0) {
                    uint32_t w = m_sharedMem->slots[i].fromHost.writePos.load(std::memory_order_acquire);
                    uint32_t margin = 480;
                    uint32_t cap = m_sharedMem->slots[i].fromHost.capacityFrames;
                    uint32_t r = (w >= margin) ? (w - margin) : (cap - margin + w);
                    m_sharedMem->slots[i].fromHost.readPos.store(r, std::memory_order_release);
                }
                
                PipeLog("RimeApo instance %u CLAIMED IPC slot %d (buffers flushed)", m_instanceId, i);
                return true;
            }
        }
    }
    
    static uint32_t lastLogTime = 0;
    if (now - lastLogTime > 1000) {
        PipeLog("RimeApo instance %u FAILED to find an empty IPC slot!", m_instanceId);
        lastLogTime = now;
    }
    return false;
}

void RimeApo::releaseSlot() {
    if (m_hasSlot && m_mySlot >= 0 && m_sharedMem) {
        uint32_t expected = m_instanceId;
        if (m_sharedMem->slots[m_mySlot].instanceId.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            PipeLog("RimeApo instance %u RELEASED IPC slot %d", m_instanceId, m_mySlot);
        }
        m_hasSlot = false;
        m_mySlot = -1;
    }
}

IFACEMETHODIMP_(void) RimeApo::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections) {
    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0) return;

    APO_CONNECTION_PROPERTY* pInput = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* pOutput = ppOutputConnections[0];

    float* inData = reinterpret_cast<float*>(pInput->pBuffer);
    float* outData = reinterpret_cast<float*>(pOutput->pBuffer);
    UINT32 numFrames = pInput->u32ValidFrameCount;
    UINT32 channels = m_channels;

    // If shared memory is not mapped, try mapping it every 100 frames (~1 second)
    if (!m_sharedMem) {
        if (++m_retryCounter % 100 == 0) {
            PipeLog("RimeApo APOProcess trying to map memory... m_retryCounter=%u", m_retryCounter);
            if (!mapSharedMemory()) {
                PipeLog("RimeApo mapSharedMemory failed! GetLastError()=%d", GetLastError());
            }
        }
    } else {
        if (++m_retryCounter % 48000 == 0) {
            PipeLog("RimeApo APOProcess running. counter=%u, hostIsRunning=%d", 
                    m_retryCounter, (int)m_sharedMem->hostIsRunning.load());
        }
    }

    // If host is not connected or shared memory not available: Passthrough!
    if (!m_sharedMem || !m_sharedMem->hostIsRunning) {
        if (inData != outData) {
            memcpy(outData, inData, numFrames * channels * sizeof(float));
        }
        pOutput->u32ValidFrameCount = numFrames;
        pOutput->u32BufferFlags = pInput->u32BufferFlags;
        return;
    }

    // Note: We no longer surrender slots on silence because the hybrid mixer (Virtual Cable)
    // might be actively feeding audio to this APO instance even if the Windows input is silent.
    // Dead instances will naturally be purged by the bridge via the lastProcessTime heartbeat.

    // If not claimed, try to claim a slot
    if (!m_hasSlot) {
        if (!tryClaimSlot()) {
            if (inData != outData) {
                memcpy(outData, inData, numFrames * channels * sizeof(float));
            }
            pOutput->u32ValidFrameCount = numFrames;
            pOutput->u32BufferFlags = pInput->u32BufferFlags;
            return;
        }
    } else {
        // Verify we still own our slot
        if (m_sharedMem->slots[m_mySlot].instanceId.load(std::memory_order_acquire) != m_instanceId) {
            m_hasSlot = false;
            m_mySlot = -1;
            if (inData != outData) {
                memcpy(outData, inData, numFrames * channels * sizeof(float));
            }
            pOutput->u32ValidFrameCount = numFrames;
            pOutput->u32BufferFlags = pInput->u32BufferFlags;
            return;
        }
    }
    
    // Prevent divide-by-zero if shared memory is not fully initialized by the bridge yet
    if (m_sharedMem->slots[m_mySlot].toHost.capacityFrames == 0 || m_sharedMem->slots[m_mySlot].fromHost.capacityFrames == 0) {
        if (inData != outData) {
            memcpy(outData, inData, numFrames * channels * sizeof(float));
        }
        pOutput->u32ValidFrameCount = numFrames;
        pOutput->u32BufferFlags = pInput->u32BufferFlags;
        return;
    }

    // Write to Host
    rime::SharedRingBuffer& toHost = m_sharedMem->slots[m_mySlot].toHost;
    uint32_t w = toHost.writePos.load(std::memory_order_acquire);
    uint32_t r = toHost.readPos.load(std::memory_order_acquire);
    uint32_t availableWrite = (r > w) ? (r - w - 1) : (toHost.capacityFrames - w + r - 1);
    
    if (availableWrite >= numFrames) {
        for (uint32_t i = 0; i < numFrames; ++i) {
            uint32_t dstIdx = ((w + i) % toHost.capacityFrames) * rime::MAX_CHANNELS;
            uint32_t srcIdx = i * channels;
            toHost.data[dstIdx] = inData[srcIdx];         // Front Left
            toHost.data[dstIdx + 1] = inData[srcIdx + 1]; // Front Right
        }
        toHost.writePos.store((w + numFrames) % toHost.capacityFrames, std::memory_order_release);
        
        // Signal the host that new data is available
        if (m_hDataEvent) {
            SetEvent(m_hDataEvent);
        }
    }

    // Master Selection logic
    uint32_t currentMaster = m_sharedMem->masterInstanceId.load(std::memory_order_acquire);
    uint32_t masterLastTime = m_sharedMem->masterLastProcessTime.load(std::memory_order_acquire);
    uint32_t now = GetTickCount();
    
    if (currentMaster == 0 || currentMaster == m_instanceId || (now - masterLastTime) > 100) {
        m_sharedMem->masterInstanceId.store(m_instanceId, std::memory_order_release);
        m_sharedMem->masterLastProcessTime.store(now, std::memory_order_release);
        currentMaster = m_instanceId;
    }
    
    if (currentMaster == m_instanceId) {
        // We are the Master! Read mixed output from Host.
        rime::SharedRingBuffer& fromHost = m_sharedMem->slots[m_mySlot].fromHost;
        uint32_t fw = fromHost.writePos.load(std::memory_order_acquire);
        uint32_t fr = fromHost.readPos.load(std::memory_order_acquire);
        uint32_t availableRead = (fw >= fr) ? (fw - fr) : (fromHost.capacityFrames - fr + fw);

        if (availableRead >= numFrames) {
            // Keep at most numFrames of cushion to prevent permanent latency buildup
            if (availableRead > numFrames * 2) {
                uint32_t skipFrames = availableRead - numFrames;
                fr = (fr + skipFrames) % fromHost.capacityFrames;
                fromHost.readPos.store(fr, std::memory_order_release);
            }
            
            for (uint32_t i = 0; i < numFrames; ++i) {
                uint32_t srcIdx = ((fr + i) % fromHost.capacityFrames) * rime::MAX_CHANNELS;
                uint32_t dstIdx = i * channels;
                
                // Passthrough all channels first (if out-of-place)
                if (inData != outData) {
                    for (uint32_t c = 0; c < channels; ++c) {
                        outData[dstIdx + c] = inData[dstIdx + c];
                    }
                }
                
                // Overwrite Front Left and Front Right with processed audio
                outData[dstIdx] = fromHost.data[srcIdx];
                outData[dstIdx + 1] = fromHost.data[srcIdx + 1];
            }
            fromHost.readPos.store((fr + numFrames) % fromHost.capacityFrames, std::memory_order_release);
        } else {
            // Host latency spike: fallback to passthrough
            if (inData != outData) {
                memcpy(outData, inData, numFrames * channels * sizeof(float));
            }
            m_sharedMem->apoUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // We are a Slave instance!
        // The bridge is actively processing audio through the Master instance.
        // We MUST output pure silence so we don't cause the raw audio to double-mix 
        // with the spatialized audio in the Hardware Audio Engine!
        memset(outData, 0, numFrames * channels * sizeof(float));
    }

    // Record the fact that we are actively processing audio!
    m_sharedMem->slots[m_mySlot].lastProcessTime.store(GetTickCount(), std::memory_order_release);
    
    pOutput->u32ValidFrameCount = numFrames;
    pOutput->u32BufferFlags = pInput->u32BufferFlags;
}

IFACEMETHODIMP_(UINT32) RimeApo::CalcInputFrames(UINT32 u32OutputFrameCount) {
    return u32OutputFrameCount;
}

IFACEMETHODIMP_(UINT32) RimeApo::CalcOutputFrames(UINT32 u32InputFrameCount) {
    return u32InputFrameCount;
}

IFACEMETHODIMP RimeApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    if (u32NumInputConnections > 0 && ppInputConnections[0] && ppInputConnections[0]->pFormat) {
        const WAVEFORMATEX* pWfx = ppInputConnections[0]->pFormat->GetAudioFormat();
        if (pWfx) {
            m_channels = pWfx->nChannels;
        }
    }
    
    PipeLog("RimeApo LockForProcess called! Inputs: %u, Outputs: %u, instanceId: %u, channels: %u", u32NumInputConnections, u32NumOutputConnections, m_instanceId, m_channels);
    mapSharedMemory();
    tryClaimSlot();
    return S_OK;
}

IFACEMETHODIMP RimeApo::UnlockForProcess() {
    return S_OK;
}

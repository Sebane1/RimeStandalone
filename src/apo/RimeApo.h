#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <atomic>
#include "../audio/IpcSharedMemory.h"

// Generate a random GUID for the Rime APO Class ID
// {A1B2C3D4-E5F6-7890-1234-56789ABCDEF0} - Example
// We should use a real GUID.
// Let's use {1A348F28-0904-4F9A-A5AB-5BAF6B5C58C3}
const CLSID CLSID_RimeApo = {0x1a348f28, 0x0904, 0x4f9a, {0xa5, 0xab, 0x5b, 0xaf, 0x6b, 0x5c, 0x58, 0xc3}};

class RimeApo : public IAudioProcessingObject, public IAudioProcessingObjectRT, public IAudioProcessingObjectConfiguration, public IAudioSystemEffects2, public IApoAuxiliaryInputConfiguration {
public:
    RimeApo(IUnknown* pUnkOuter = nullptr);
    virtual ~RimeApo();

    class CNonDelegatingUnknown : public IUnknown {
    public:
        CNonDelegatingUnknown(RimeApo* pApo) : m_pApo(pApo) {}
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv);
        IFACEMETHODIMP_(ULONG) AddRef();
        IFACEMETHODIMP_(ULONG) Release();
    private:
        RimeApo* m_pApo;
    } m_NonDelegatingUnknown;

    IUnknown* m_pUnkOuter;

    HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppv);
    ULONG NonDelegatingAddRef();
    ULONG NonDelegatingRelease();

    // IUnknown (Delegating)
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IAudioProcessingObject
    IFACEMETHODIMP Reset();
    IFACEMETHODIMP GetLatency(HNSTIME* pTime);
    IFACEMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps);
    IFACEMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData);
    IFACEMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat);
    IFACEMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat);
    IFACEMETHODIMP GetInputChannelCount(UINT32* pu32ChannelCount);
    // IApoAuxiliaryInputConfiguration
    IFACEMETHODIMP AddAuxiliaryInput(DWORD dwInputId, UINT32 cbDataSize, BYTE *pbyData, APO_CONNECTION_DESCRIPTOR *pInputConnection);
    IFACEMETHODIMP RemoveAuxiliaryInput(DWORD dwInputId);
    IFACEMETHODIMP IsInputFormatSupported(IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat);

    // IAudioProcessingObjectRT
    IFACEMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections);
    IFACEMETHODIMP_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount);
    IFACEMETHODIMP_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount);

    // IAudioProcessingObjectConfiguration
    IFACEMETHODIMP LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections);
    IFACEMETHODIMP UnlockForProcess();

    // IAudioSystemEffects
    IFACEMETHODIMP GetEffectsList(LPGUID *ppEffectsIds, UINT *pcEffects, HANDLE Event);

private:
    std::atomic<ULONG> m_refCount;
    
    WAVEFORMATEX* m_format;
    uint32_t m_channels;
    
    // IPC
    HANDLE m_hMapFile;
    HANDLE m_hDataEvent;
    rime::IpcMemoryLayout* m_sharedMem;
    uint32_t m_retryCounter;
    uint32_t m_instanceId;   // Unique ID for this APO instance
    int m_mySlot;            // Which slot this instance owns (-1 if none)
    bool m_hasSlot;          // Whether this instance owns an IPC slot
    uint32_t m_silentFrames; // Counter for contiguous silent frames
    
    bool mapSharedMemory();
    void unmapSharedMemory();
    bool tryClaimSlot();
    void releaseSlot();
};

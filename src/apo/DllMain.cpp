#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include "RimeApo.h"

void PipeLog(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    HANDLE hPipe;
    while (1) {
        hPipe = CreateFileW(L"\\\\.\\pipe\\RimeApoLog", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return;
        if (!WaitNamedPipeW(L"\\\\.\\pipe\\RimeApoLog", 1000)) return;
    }
    
    DWORD written;
    WriteFile(hPipe, buffer, strlen(buffer), &written, NULL);
    CloseHandle(hPipe);
}

long g_lServerLocks = 0;
long g_lComponents = 0;

class CClassFactory : public IClassFactory {
public:
    CClassFactory() : m_refCount(1) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) Release() {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
        PipeLog("CClassFactory::CreateInstance started! pUnkOuter=%p", pUnkOuter);
        
        if (pUnkOuter && riid != IID_IUnknown) {
            PipeLog("CClassFactory::CreateInstance AGGREGATION FAILED: riid must be IUnknown when pUnkOuter is present");
            return CLASS_E_NOAGGREGATION;
        }

        RimeApo* pApo = new (std::nothrow) RimeApo(pUnkOuter);
        if (!pApo) return E_OUTOFMEMORY;
        
        PipeLog("CClassFactory::CreateInstance calling NonDelegatingQueryInterface!");
        HRESULT hr = pApo->NonDelegatingQueryInterface(riid, ppv);
        PipeLog("CClassFactory::CreateInstance QueryInterface returned %x", hr);
        pApo->NonDelegatingRelease();
        return hr;
    }


    IFACEMETHODIMP LockServer(BOOL fLock) {
        if (fLock) {
            InterlockedIncrement(&g_lServerLocks);
        } else {
            InterlockedDecrement(&g_lServerLocks);
        }
        return S_OK;
    }

private:
    long m_refCount;
};

// DLL Exports
STDAPI DllCanUnloadNow() {
    if (g_lComponents == 0 && g_lServerLocks == 0) {
        return S_OK;
    }
    return S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
        PipeLog("DllGetClassObject called!");
    if (rclsid != CLSID_RimeApo) {
        PipeLog("DllGetClassObject CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    CClassFactory* pFactory = new (std::nothrow) CClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            PipeLog("DllMain DLL_PROCESS_ATTACH!");
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}

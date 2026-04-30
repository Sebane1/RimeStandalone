// RIME Standalone Native Installer
// Zero-dependency Win32 installer that handles APO registration, device selection, and startup.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---- Constants ----
static const wchar_t* APO_CLSID        = L"{1A348F28-0904-4F9A-A5AB-5BAF6B5C58C3}";
static const wchar_t* APO_IFACE        = L"{5CA5EA24-13B5-4D6D-A263-D13CE52D5392}";
static const wchar_t* INSTALL_DIR_NAME = L"RIME Standalone";
static const wchar_t* TASK_NAME        = L"RIME Standalone Bridge";
static const wchar_t* FX_PKEY          = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";

// ---- Globals ----
static HWND g_hWnd, g_hList, g_hStatus, g_hInstallBtn, g_hUninstallBtn;
static bool g_isUninstalling = false;

struct AudioDevice {
    std::wstring id, name, fxPath;
    bool installed;
};
static std::vector<AudioDevice> g_devices;

// ---- Helpers ----
static std::wstring GetInstallDir() {
    wchar_t pf[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf);
    return std::wstring(pf) + L"\\" + INSTALL_DIR_NAME;
}

static std::wstring GetConfigDir() {
    wchar_t ap[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ap);
    return std::wstring(ap) + L"\\" + INSTALL_DIR_NAME;
}

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L'\\'));
}

static void SetStatus(const wchar_t* msg) {
    SetWindowTextW(g_hStatus, msg);
    UpdateWindow(g_hStatus);
}

static bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void InstallVBCable() {
    std::wstring exeDir = GetExeDir();
    std::wstring vbSetup = exeDir + L"\\VBCABLE\\VBCABLE_Setup_x64.exe";
    if (FileExists(vbSetup)) {
        SetStatus(L"Installing VB-Audio Virtual Cable (Please wait)...");
        
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = g_hWnd;
        sei.lpVerb = L"runas";
        sei.lpFile = vbSetup.c_str();
        sei.lpParameters = L"-i -h";
        sei.nShow = SW_HIDE;
        
        if (ShellExecuteExW(&sei)) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }
    }
}

static void RestartAudioService() {
    SetStatus(L"Restarting Windows Audio Service...");
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"Audiosrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS ss;
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        // Wait for stop
        for (int i = 0; i < 50; i++) {
            QueryServiceStatus(svc, &ss);
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
        StartServiceW(svc, 0, NULL);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    Sleep(1000);
}

// ---- Registry Helpers ----
static void RegSetString(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    HKEY hk;
    RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
    RegSetValueExW(hk, name, 0, REG_SZ, (BYTE*)value, (DWORD)(wcslen(value) + 1) * sizeof(wchar_t));
    RegCloseKey(hk);
}

static void RegSetDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
    HKEY hk;
    RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
    RegSetValueExW(hk, name, 0, REG_DWORD, (BYTE*)&value, sizeof(DWORD));
    RegCloseKey(hk);
}

static void RegDeleteTree(HKEY root, const wchar_t* subkey) {
    RegDeleteTreeW(root, subkey);
    RegDeleteKeyW(root, subkey);
}

// ---- TrustedInstaller Bypass ----
static bool UnlockFxProperties(const wchar_t* deviceId) {
    std::wstring path = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\") + deviceId + L"\\FxProperties";
    
    // Enable privileges
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        auto enablePriv = [&](const wchar_t* priv) {
            TOKEN_PRIVILEGES tp;
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            LookupPrivilegeValueW(NULL, priv, &tp.Privileges[0].Luid);
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
        };
        enablePriv(L"SeTakeOwnershipPrivilege");
        enablePriv(L"SeRestorePrivilege");
        CloseHandle(hToken);
    }

    // Take ownership
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, WRITE_OWNER, &hk) == ERROR_SUCCESS) {
        PSID adminSid = NULL;
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid);
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorOwner(&sd, adminSid, FALSE);
        RegSetKeySecurity(hk, OWNER_SECURITY_INFORMATION, &sd);
        FreeSid(adminSid);
        RegCloseKey(hk);
    }

    // Grant full control
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, WRITE_DAC, &hk) == ERROR_SUCCESS) {
        PSID adminSid = NULL;
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid);
        
        EXPLICIT_ACCESS_W ea = {};
        ea.grfAccessPermissions = KEY_ALL_ACCESS;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)adminSid;
        
        PACL pNewDacl = NULL;
        SetEntriesInAclW(1, &ea, NULL, &pNewDacl);
        if (pNewDacl) {
            RegSetKeySecurity(hk, DACL_SECURITY_INFORMATION, NULL); // clear first
            SECURITY_DESCRIPTOR sd2;
            InitializeSecurityDescriptor(&sd2, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&sd2, TRUE, pNewDacl, FALSE);
            RegSetKeySecurity(hk, DACL_SECURITY_INFORMATION, &sd2);
            LocalFree(pNewDacl);
        }
        FreeSid(adminSid);
        RegCloseKey(hk);
    }
    return true;
}

// ---- Device Enumeration ----
static void EnumerateDevices() {
    g_devices.clear();
    HKEY hRender;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_READ, &hRender) != ERROR_SUCCESS) return;

    wchar_t subName[256];
    for (DWORD i = 0; RegEnumKeyW(hRender, i, subName, 256) == ERROR_SUCCESS; i++) {
        // Get friendly name
        std::wstring propPath = std::wstring(subName) + L"\\Properties";
        HKEY hProp;
        if (RegOpenKeyExW(hRender, propPath.c_str(), 0, KEY_READ, &hProp) != ERROR_SUCCESS) continue;
        
        wchar_t friendlyName[512] = {};
        DWORD nameSize = sizeof(friendlyName);
        DWORD type;
        if (RegQueryValueExW(hProp, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", NULL, &type, (BYTE*)friendlyName, &nameSize) != ERROR_SUCCESS) {
            RegCloseKey(hProp);
            continue;
        }
        RegCloseKey(hProp);

        // Check if APO installed
        std::wstring fxPath = std::wstring(subName) + L"\\FxProperties";
        bool installed = false;
        HKEY hFx;
        if (RegOpenKeyExW(hRender, fxPath.c_str(), 0, KEY_READ, &hFx) == ERROR_SUCCESS) {
            wchar_t clsid[128] = {};
            DWORD clsidSize = sizeof(clsid);
            if (RegQueryValueExW(hFx, FX_PKEY, NULL, &type, (BYTE*)clsid, &clsidSize) == ERROR_SUCCESS) {
                if (_wcsicmp(clsid, APO_CLSID) == 0) installed = true;
            }
            RegCloseKey(hFx);
        }

        std::wstring fullFxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + fxPath;
        g_devices.push_back({ subName, friendlyName, fullFxPath, installed });
    }
    RegCloseKey(hRender);
}

static void PopulateDeviceList() {
    SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_devices.size(); i++) {
        std::wstring display = g_devices[i].name;
        if (g_devices[i].installed) display += L"  [INSTALLED]";
        SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)display.c_str());
    }
}

// ---- Install Logic ----
static void DoInstall(int deviceIndex) {
    std::wstring exeDir = GetExeDir();
    std::wstring installDir = GetInstallDir();
    std::wstring dllSrc = exeDir + L"\\rime_apo.dll";
    std::wstring bridgeSrc = exeDir + L"\\rime_bridge.exe";
    std::wstring dllDst = L"C:\\Windows\\System32\\rime_apo.dll";
    std::wstring bridgeDst = installDir + L"\\rime_bridge.exe";

    if (!FileExists(dllSrc) || !FileExists(bridgeSrc)) {
        SetStatus(L"ERROR: rime_apo.dll and rime_bridge.exe must be in the same folder as this setup.");
        return;
    }

    // 1. Stop service & copy files
    SetStatus(L"[1/7] Copying files...");
    RestartAudioService(); // stop first
    CreateDirectoryW(installDir.c_str(), NULL);
    CopyFileW(dllSrc.c_str(), dllDst.c_str(), FALSE);
    CopyFileW(bridgeSrc.c_str(), bridgeDst.c_str(), FALSE);

    // 2. Install VB-Cable
    SetStatus(L"[2/7] Installing VB-Audio Virtual Cable...");
    InstallVBCable();

    // 3. APO signing bypass
    SetStatus(L"[3/7] Disabling APO signing enforcement...");
    RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", L"DisableProtectedAudioDG", 1);

    // 4. Register COM
    SetStatus(L"[4/7] Registering APO COM server...");
    std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + APO_CLSID;
    std::wstring inprocPath = clsidPath + L"\\InProcServer32";
    RegSetString(HKEY_LOCAL_MACHINE, clsidPath.c_str(), NULL, L"Rime Audio Processing Object");
    RegSetString(HKEY_LOCAL_MACHINE, inprocPath.c_str(), NULL, dllDst.c_str());
    RegSetString(HKEY_LOCAL_MACHINE, inprocPath.c_str(), L"ThreadingModel", L"Both");

    const wchar_t* apoPaths[] = {
        L"SOFTWARE\\Classes\\AudioProcessingObject\\",
        L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\"
    };
    for (auto prefix : apoPaths) {
        std::wstring apoPath = std::wstring(prefix) + APO_CLSID;
        RegSetString(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"Name", L"Rime APO");
        RegSetString(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"FriendlyName", L"Rime APO");
        RegSetString(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"Copyright", L"Neumann");
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MajorVersion", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MinorVersion", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"Flags", 0x0000000f);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MinInputConnections", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MaxInputConnections", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MinOutputConnections", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MaxOutputConnections", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"MaxInstances", 0xffffffff);
        RegSetDword(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"NumAPOInterfaces", 1);
        RegSetString(HKEY_LOCAL_MACHINE, apoPath.c_str(), L"APOInterface0", APO_IFACE);
    }

    // 5. Install on selected device
    if (deviceIndex >= 0 && deviceIndex < (int)g_devices.size()) {
        SetStatus(L"[5/7] Installing APO on device...");
        auto& dev = g_devices[deviceIndex];
        UnlockFxProperties(dev.id.c_str());

        // Create FxProperties if needed
        HKEY hk;
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, dev.fxPath.c_str(), 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
        RegSetValueExW(hk, FX_PKEY, 0, REG_SZ, (BYTE*)APO_CLSID, (DWORD)(wcslen(APO_CLSID) + 1) * sizeof(wchar_t));
        RegCloseKey(hk);
    }

    // 6. Create config
    SetStatus(L"[6/7] Creating configuration...");
    std::wstring configDir = GetConfigDir();
    CreateDirectoryW(configDir.c_str(), NULL);
    std::wstring configFile = configDir + L"\\config.json";
    if (!FileExists(configFile)) {
        FILE* f = _wfopen(configFile.c_str(), L"w");
        if (f) {
            fprintf(f, "{\n");
            fprintf(f, "  \"pluginPath\": \"C:\\\\Program Files\\\\Common Files\\\\VST3\\\\Neumann RIME.vst3\\\\Contents\\\\x86_64-win\\\\Neumann RIME.vst3\",\n");
            fprintf(f, "  \"sampleRate\": 48000,\n");
            fprintf(f, "  \"outputGainDb\": 3.0\n");
            fprintf(f, "}\n");
            fclose(f);
        }
    }

    // 7. Register startup
    SetStatus(L"[7/7] Registering startup...");
    std::wstring startupVal = L"\"" + bridgeDst + L"\"";
    RegSetString(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", TASK_NAME, startupVal.c_str());

    // Restart audio
    RestartAudioService();

    // Launch bridge
    ShellExecuteW(NULL, L"open", bridgeDst.c_str(), NULL, NULL, SW_SHOW);

    SetStatus(L"Installation complete! RIME bridge launched.");
    EnableWindow(g_hInstallBtn, FALSE);
    EnumerateDevices();
    PopulateDeviceList();
}

// ---- Uninstall Logic ----
static void DoUninstall() {
    std::wstring installDir = GetInstallDir();
    std::wstring dllDst = L"C:\\Windows\\System32\\rime_apo.dll";
    std::wstring bridgeDst = installDir + L"\\rime_bridge.exe";

    // 1. Kill bridge
    SetStatus(L"[1/5] Stopping bridge...");
    system("taskkill /F /IM rime_bridge.exe >nul 2>&1");

    // 2. Remove startup
    SetStatus(L"[2/5] Removing startup entry...");
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, TASK_NAME);
        RegCloseKey(hk);
    }

    // 3. Remove APO from all devices
    SetStatus(L"[3/5] Removing APO from devices...");
    EnumerateDevices();
    for (auto& dev : g_devices) {
        if (dev.installed) {
            UnlockFxProperties(dev.id.c_str());
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dev.fxPath.c_str(), 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
                RegDeleteValueW(hk, FX_PKEY);
                RegCloseKey(hk);
            }
        }
    }

    // 4. Unregister COM & restore signing
    SetStatus(L"[4/5] Removing COM registration...");
    std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + APO_CLSID;
    RegDeleteTree(HKEY_LOCAL_MACHINE, clsidPath.c_str());
    RegDeleteTree(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\AudioProcessingObject\\") + APO_CLSID).c_str());
    RegDeleteTree(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + APO_CLSID).c_str());
    RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", L"DisableProtectedAudioDG", 0);

    // 5. Remove files & restart
    SetStatus(L"[5/5] Removing files...");
    RestartAudioService(); // stop to release DLL
    DeleteFileW(dllDst.c_str());
    DeleteFileW(bridgeDst.c_str());
    RemoveDirectoryW(installDir.c_str());
    RestartAudioService();

    SetStatus(L"Uninstall complete.");
    EnableWindow(g_hUninstallBtn, FALSE);
    EnumerateDevices();
    PopulateDeviceList();
}

// ---- Window Procedure ----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        g_hStatus = CreateWindowW(L"STATIC", L"Select an audio device and click Install.", WS_CHILD | WS_VISIBLE, 15, 12, 450, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        CreateWindowW(L"STATIC", L"Output Devices:", WS_CHILD | WS_VISIBLE, 15, 40, 140, 20, hwnd, NULL, NULL, NULL);

        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 15, 62, 450, 240, hwnd, (HMENU)100, NULL, NULL);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hInstallBtn = CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 15, 310, 140, 35, hwnd, (HMENU)101, NULL, NULL);
        SendMessageW(g_hInstallBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hUninstallBtn = CreateWindowW(L"BUTTON", L"Uninstall", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170, 310, 140, 35, hwnd, (HMENU)102, NULL, NULL);
        SendMessageW(g_hUninstallBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hClose = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 325, 310, 140, 35, hwnd, (HMENU)103, NULL, NULL);
        SendMessageW(hClose, WM_SETFONT, (WPARAM)hFont, TRUE);

        EnumerateDevices();
        PopulateDeviceList();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 101: { // Install
            int sel = (int)SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) {
                SetStatus(L"Please select an audio device first.");
                return 0;
            }
            DoInstall(sel);
            return 0;
        }
        case 102: // Uninstall
            DoUninstall();
            return 0;
        case 103: // Close
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- Entry Point ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    // Check for /uninstall flag
    if (cmdLine && wcsstr(cmdLine, L"/uninstall")) {
        g_isUninstalling = true;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RIMESetup";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, L"RIMESetup", L"RIME Standalone Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 395,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hWnd, SW_SHOW);

    if (g_isUninstalling) {
        DoUninstall();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

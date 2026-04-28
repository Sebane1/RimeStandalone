#include <iostream>
#include <winsock2.h>
#include <windows.h>
#include "config/Config.h"
#include "audio/AudioEngine.h"
#include "audio/OscProxy.h"
#include "audio/SolarXrClient.h"
#include <sddl.h>
#include <timeapi.h>

static AudioEngine* g_engine = nullptr;
static std::string g_statePath = "";

void SaveStateIfPossible() {
    if (g_engine && !g_statePath.empty()) {
        g_engine->savePluginState(g_statePath);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        if (g_engine) g_engine->shutdown();
        SaveStateIfPossible();
        PostQuitMessage(0);
        Sleep(1000); // Give the OS a moment to flush the disk before hard-kill
        return TRUE;
    }
    return FALSE;
}

DWORD WINAPI PipeServerThread(LPVOID) {
    PSECURITY_DESCRIPTOR pSD = NULL;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &pSD, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };
    HANDLE hPipe = CreateNamedPipeW(L"\\\\.\\pipe\\RimeApoLog", PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024, 1024, 0, &sa);
    if (pSD) LocalFree(pSD);
    while (hPipe != INVALID_HANDLE_VALUE) {
        if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
            char buffer[1024];
            DWORD read;
            while (ReadFile(hPipe, buffer, sizeof(buffer)-1, &read, NULL) && read > 0) {
                buffer[read] = 0;
                printf("[APO LOG] %s\n", buffer);
            }
            DisconnectNamedPipe(hPipe);
        }
    }
    return 0;
}

int main() {
    std::setvbuf(stdout, NULL, _IONBF, 0);
    timeBeginPeriod(1); // Set timer resolution to 1ms for accurate waits
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);

    Config config;
    config.loadFromFile(); // Load from %APPDATA%\RIME Standalone\config.json (falls back to defaults)
    
    AudioEngine engine(config);
    g_engine = &engine;

    // Determine state path
    char* appdata = nullptr;
    size_t sz = 0;
    _dupenv_s(&appdata, &sz, "APPDATA");
    if (appdata) {
        g_statePath = std::string(appdata) + "\\RIME Standalone\\state.bin";
        free(appdata);
    }

    if (!engine.initialize()) {
        std::cerr << "Failed to initialize AudioEngine\n";
        return 1;
    }

    engine.start();
    std::cout << "Phase 1 AudioBridge is running. Close the UI window or press Ctrl+C to stop.\n";

    OscProxy oscProxy(7000, 7001);
    if (!oscProxy.start()) {
        std::cerr << "WARNING: Failed to start native OSC Drift Corrector on port 7000.\n";
    }

    SolarXrClient solarXrClient(&oscProxy);
    if (!solarXrClient.start()) {
        std::cerr << "WARNING: Failed to start native SolarXR WebSocket Client.\n";
    }

    if (engine.showUI()) {
        std::cout << "RIME UI Opened.\n";
    } else {
        std::cerr << "Failed to open RIME UI.\n";
    }

    // Auto-save every 5 seconds to prevent data loss on ungraceful exits
    SetTimer(NULL, 1, 5000, [](HWND, UINT, UINT_PTR, DWORD) {
        SaveStateIfPossible();
    });

    // Standard Windows Message Loop (required for the GUI to render and respond)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    std::cout << "Shutting down...\n";
    SaveStateIfPossible();
    
    g_engine = nullptr;
    engine.shutdown();
    timeEndPeriod(1);
    return 0;
}

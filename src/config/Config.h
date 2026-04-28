#pragma once
#include <string>

struct Config {
    std::string pluginPath = "C:\\Program Files\\Common Files\\VST3\\Neumann RIME.vst3\\Contents\\x86_64-win\\Neumann RIME.vst3";
    int sampleRate = 48000;
    int bufferSize = 128;
    int inputDeviceIndex = 0;
    int outputDeviceIndex = 0;
    float outputGainDb = 4.0f; // Output gain compensation in dB (applied after VST3 processing)
    
    std::string virtualCableName = ""; // e.g. "VB-Audio Virtual Cable"
    int virtualCableChannels = 8; // 8 for 7.1 surround
    std::string keepAliveDeviceName = "";

    bool loadFromFile(); // Loads config from %APPDATA%\RIME Standalone\config.json
};

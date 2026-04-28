#include "Config.h"
#include <fstream>
#include <iostream>
#include <windows.h>

// Minimal JSON parser — no dependencies needed for a simple flat config
static std::string trimQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

bool Config::loadFromFile() {
    // Build path: %APPDATA%\RIME Standalone\config.json
    char appdata[MAX_PATH];
    if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) return false;
    
    std::string path = std::string(appdata) + "\\RIME Standalone\\config.json";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "[CONFIG] No config file found at " << path << ", using defaults\n";
        return false;
    }

    std::cout << "[CONFIG] Loading " << path << "\n";
    
    // Simple line-by-line JSON parsing (handles flat objects only)
    std::string line;
    while (std::getline(file, line)) {
        // Find key-value pairs like "key": value or "key": "value"
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);
        
        // Strip whitespace and commas
        auto stripWS = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == ',' || s.back() == '\r' || s.back() == '\n'))
                s.pop_back();
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.erase(s.begin());
        };
        stripWS(key);
        stripWS(value);
        key = trimQuotes(key);
        
        if (key == "pluginPath") {
            pluginPath = trimQuotes(value);
            // Unescape JSON backslashes
            std::string unescaped;
            for (size_t i = 0; i < pluginPath.size(); ++i) {
                if (pluginPath[i] == '\\' && i + 1 < pluginPath.size() && pluginPath[i + 1] == '\\') {
                    unescaped += '\\';
                    ++i;
                } else {
                    unescaped += pluginPath[i];
                }
            }
            pluginPath = unescaped;
            std::cout << "[CONFIG] pluginPath = " << pluginPath << "\n";
        }
        else if (key == "sampleRate") {
            sampleRate = std::stoi(value);
            std::cout << "[CONFIG] sampleRate = " << sampleRate << "\n";
        }
        else if (key == "outputGainDb") {
            outputGainDb = std::stof(value);
            std::cout << "[CONFIG] outputGainDb = " << outputGainDb << "\n";
        }
        else if (key == "virtualCableName") {
            virtualCableName = trimQuotes(value);
            std::cout << "[CONFIG] virtualCableName = " << virtualCableName << "\n";
        }
        else if (key == "virtualCableChannels") {
            virtualCableChannels = std::stoi(value);
            std::cout << "[CONFIG] virtualCableChannels = " << virtualCableChannels << "\n";
        }
        else if (key == "keepAliveDeviceName") {
            keepAliveDeviceName = trimQuotes(value);
            std::cout << "[CONFIG] keepAliveDeviceName = " << keepAliveDeviceName << "\n";
        }
    }

    return true;
}

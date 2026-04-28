#pragma once
#include <string>

class PluginLoader {
public:
    static void* load(const std::string& pluginPath);
    static void unload(void* handle);
};

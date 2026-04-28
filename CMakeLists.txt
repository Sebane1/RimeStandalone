cmake_minimum_required(VERSION 3.20)
project(rime_system_bridge)

set(CMAKE_CXX_STANDARD 20)

set(SMTG_ADD_VST3_HOSTING_SAMPLES OFF CACHE BOOL "Disable VST3 Hosting Samples")
set(SMTG_ADD_VST3_PLUGINS_SAMPLES OFF CACHE BOOL "Disable VST3 Plugin Samples")
set(SMTG_BUILD_DOC OFF CACHE BOOL "Disable Documentation")
set(SMTG_ENABLE_VST3_HOSTING_SUPPORT ON CACHE BOOL "Enable Hosting Support")
add_subdirectory(third_party/vst3sdk)

add_executable(rime_bridge
    src/main.cpp
    src/audio/AudioEngine.cpp
    src/audio/IpcAudioClient.cpp
    src/audio/WASAPICapture.cpp
    src/audio/WASAPIKeepAlive.cpp
    src/audio/RingBuffer.cpp
    src/audio/OscProxy.cpp
    src/vst/VSTHost.cpp
    src/vst/PluginLoader.cpp
    src/config/Config.cpp
    src/audio/SolarXrClient.cpp
    third_party/vst3sdk/public.sdk/source/vst/hosting/module.cpp
    third_party/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp
)

# Fix C++20 char8_t errors in VST3 SDK
target_compile_options(rime_bridge PRIVATE /Zc:char8_t-)

# Prevent windows.h from polluting the namespace and colliding with SolarXR schemas
target_compile_definitions(rime_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_include_directories(rime_bridge PRIVATE
    src
    third_party/vst3_sdk
    third_party/flatbuffers/include
    third_party/solarxr_protocol/cpp/include
)

target_link_libraries(rime_bridge PRIVATE
    sdk_hosting
    ksuser
    mfplat
    mfuuid
    wmcodecdspuuid
    winmm
    winhttp
)

# --- APO DLL Target ---
add_library(rime_apo SHARED
    src/apo/RimeApo.cpp
    src/apo/DllMain.cpp
    src/apo/RimeApo.def
)

set_property(TARGET rime_apo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(rime_apo PRIVATE src/audio src/apo)

target_link_libraries(rime_apo PRIVATE
    uuid
    ole32
    oleaut32
    advapi32
)

# --- Native Installer ---
add_executable(rime_setup WIN32
    src/setup/Setup.cpp
)

target_link_libraries(rime_setup PRIVATE
    advapi32
    shell32
    ole32
)

# Require admin elevation via UAC
set_target_properties(rime_setup PROPERTIES
    LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

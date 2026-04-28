#pragma once
#include <string>
#include <vector>
#include <memory>
#include <windows.h>
#include <mutex>
#include <atomic>
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

// Forward declarations for VST3 SDK to avoid polluting headers
namespace Steinberg {
    class IPluginFactory;
    namespace Vst {
        class IComponent;
        class IAudioProcessor;
    }
}
namespace VST3 {
    namespace Hosting {
        class Module;
    }
}

// Minimal IPlugFrame implementation so the plugin can request resize
class PlugFrame : public Steinberg::IPlugFrame {
public:
    PlugFrame(HWND hwnd) : m_hwnd(hwnd), m_refCount(1) {}

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
        if (!newSize || !m_hwnd) return Steinberg::kInvalidArgument;
        RECT winRect = { 0, 0, newSize->right - newSize->left, newSize->bottom - newSize->top };
        AdjustWindowRect(&winRect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(m_hwnd, nullptr, 0, 0, winRect.right - winRect.left, winRect.bottom - winRect.top, SWP_NOMOVE | SWP_NOZORDER);
        if (view) view->onSize(newSize);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, IPlugFrame::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    Steinberg::uint32 PLUGIN_API release() override {
        auto count = --m_refCount;
        if (count == 0) delete this;
        return count;
    }

private:
    HWND m_hwnd;
    std::atomic<Steinberg::uint32> m_refCount;
};

class VSTHost : public Steinberg::Vst::IComponentHandler {
public:
    // IComponentHandler
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override { return Steinberg::kResultOk; }

    // FUnknown
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            // Don't AddRef because we are not reference counted in VSTHost yet
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

public:
    VSTHost();
    ~VSTHost();

    bool loadPlugin(const std::string& path, int inChannels = 2, int outChannels = 2, const std::string& statePath = "");
    void process(const float* input, float* output, int numSamples, int inChannels, int outChannels);
    
    bool saveState(const std::string& path);
    bool loadState(const std::string& path);
    
    bool showUI();
    void hideUI();

private:
    bool setupAudioBus(int inChannels, int outChannels);
    
    std::string m_statePath;

    std::shared_ptr<VST3::Hosting::Module> module;
    Steinberg::Vst::IComponent* component;
    Steinberg::Vst::IAudioProcessor* processor;
    Steinberg::Vst::IEditController* controller;

    // Parameter changes queue
    struct ParamChange {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    std::vector<ParamChange> pendingParamChanges;
    std::mutex paramMutex;
    Steinberg::IPlugView* plugView;
    PlugFrame* plugFrame;
    HWND hwndUI;

    bool isLoaded;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};

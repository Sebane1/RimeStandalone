#include "VSTHost.h"
#include "VSTStream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include <iostream>
#include <fstream>
#include <vector>
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/base/ierrorcontext.h"
#include "pluginterfaces/base/ibstream.h"
#include <mutex>

#include "pluginterfaces/gui/iplugview.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

class MyHostContext : public Steinberg::Vst::HostApplication {
public:
    VSTHost* host;
    MyHostContext(VSTHost* h) : host(h) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(host);
            host->addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::Vst::HostApplication::queryInterface(iid, obj);
    }
};

Steinberg::tresult PLUGIN_API VSTHost::performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) {
    // Acknowledge the edit so the controller updates its state internally.
    std::lock_guard<std::mutex> lock(paramMutex);
    pendingParamChanges.push_back({id, valueNormalized});
    return Steinberg::kResultOk;
}

// We use Steinberg::Vst::HostApplication provided by the SDK

VSTHost::VSTHost() 
    : module(nullptr), component(nullptr), processor(nullptr), controller(nullptr), plugView(nullptr), plugFrame(nullptr), hwndUI(NULL), isLoaded(false) {}

VSTHost::~VSTHost() {
    hideUI();
    if (processor) processor->release();
    if (component) component->release();
    if (controller) controller->release();
    if (module) {
        module = nullptr;
    }
}

bool VSTHost::loadPlugin(const std::string& path, int inChannels, int outChannels, const std::string& statePath) {
    m_statePath = statePath;
    std::string error;
    auto modulePtr = VST3::Hosting::Module::create(path, error);
    if (!modulePtr) {
        std::cerr << "Failed to load VST3 module: " << error << "\n";
        return false;
    }

    auto factory = modulePtr->getFactory();
    
    // Find first Audio Module Class
    VST3::UID uid;
    bool found = false;
    for (auto& info : factory.classInfos()) {
        if (info.category() == kVstAudioEffectClass) {
            uid = info.ID();
            found = true;
            break;
        }
    }

    if (!found) return false;

    // Instantiate component
    FUnknown* hostContext = new MyHostContext(this);
    
    // createInstance returns IPtr<IComponent> but we need the raw pointer or to manage it.
    auto compPtr = factory.createInstance<IComponent>(uid);
    if (!compPtr) {
        hostContext->release();
        return false;
    }
    
    component = compPtr.get();
    component->addRef(); // We manually manage it in this implementation for now


    component->setIoMode(Vst::kRealtime);

    // Get Processor
    component->queryInterface(IAudioProcessor::iid, (void**)&processor);
    
    // Get or Create Controller
    if (component->queryInterface(IEditController::iid, (void**)&controller) != kResultOk || !controller) {
        TUID controllerUID;
        if (component->getControllerClassId(controllerUID) == kResultOk) {
            auto ctrlPtr = factory.createInstance<IEditController>(VST3::UID(controllerUID));
            if (ctrlPtr) {
                controller = ctrlPtr.get();
                controller->addRef(); // Take ownership
            }
        }
    }

    if (controller) {
        controller->initialize(hostContext);
        
        // Connect them if they are separate
        IConnectionPoint* icpComponent = nullptr;
        IConnectionPoint* icpController = nullptr;
        if (component->queryInterface(IConnectionPoint::iid, (void**)&icpComponent) == kResultOk &&
            controller->queryInterface(IConnectionPoint::iid, (void**)&icpController) == kResultOk) {
            icpComponent->connect(icpController);
            icpController->connect(icpComponent);
        }
        if (icpComponent) icpComponent->release();
        if (icpController) icpController->release();
    }

    // Initialize
    if (component->initialize(hostContext) != kResultOk) {
        hostContext->release();
        return false;
    }
    
    hostContext->release();

    if (!processor) return false;

    if (!setupAudioBus(inChannels, outChannels)) {
        std::cerr << "Failed to setup audio bus\n";
        return false;
    }

    isLoaded = true;
    
    // Take ownership
    module = modulePtr;

    return true;
}

bool VSTHost::setupAudioBus(int inChannels, int outChannels) {
    // 1. Configure channel arrangements
    SpeakerArrangement inArr = (inChannels == 8) ? SpeakerArr::k71Cine : SpeakerArr::kStereo;
    SpeakerArrangement outArr = (outChannels == 8) ? SpeakerArr::k71Cine : SpeakerArr::kStereo;
    processor->setBusArrangements(&inArr, 1, &outArr, 1);

    // 2. Activate Input and Output Busses
    component->activateBus(kAudio, kInput, 0, true);
    component->activateBus(kAudio, kOutput, 0, true);

    // 3. Setup Processing (48kHz, up to 480 samples per block = 10ms WASAPI period)
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 480;
    setup.sampleRate = 48000;
    processor->setupProcessing(setup);

    // LOAD STATE HERE before activating the component!
    if (!m_statePath.empty()) {
        loadState(m_statePath);
    }

    component->setActive(true);
    processor->setProcessing(true);

    return true;
}

void VSTHost::process(const float* input, float* output, int numSamples, int inChannels, int outChannels) {
    if (!isLoaded || !processor) {
        // Passthrough if not loaded (assuming outChannels <= inChannels)
        for (int i = 0; i < numSamples; ++i) {
            for (int ch = 0; ch < outChannels; ++ch) {
                output[i * outChannels + ch] = input[i * inChannels + ch];
            }
        }
        return;
    }

    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numSamples;
    data.numInputs = 1;
    data.numOutputs = 1;

    Steinberg::Vst::ParameterChanges inputParamChanges;
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        if (!pendingParamChanges.empty()) {
            for (const auto& change : pendingParamChanges) {
                Steinberg::int32 index;
                Steinberg::Vst::IParamValueQueue* queue = inputParamChanges.addParameterData(change.id, index);
                if (queue) {
                    Steinberg::int32 pointIndex;
                    queue->addPoint(0, change.value, pointIndex);
                }
            }
            pendingParamChanges.clear();
        }
    }
    
    if (inputParamChanges.getParameterCount() > 0) {
        data.inputParameterChanges = &inputParamChanges;
    } else {
        data.inputParameterChanges = nullptr;
    }
    data.outputParameterChanges = nullptr;

    // Allocate planar buffers dynamically (max 480 samples per block)
    std::vector<std::vector<float>> inPlanar(inChannels, std::vector<float>(numSamples));
    std::vector<std::vector<float>> outPlanar(outChannels, std::vector<float>(numSamples));

    std::vector<float*> inChannelsPtr(inChannels);
    std::vector<float*> outChannelsPtr(outChannels);

    for (int ch = 0; ch < inChannels; ++ch) {
        inChannelsPtr[ch] = inPlanar[ch].data();
    }
    for (int ch = 0; ch < outChannels; ++ch) {
        outChannelsPtr[ch] = outPlanar[ch].data();
    }

    // Deinterleave
    for (int i = 0; i < numSamples; ++i) {
        for (int ch = 0; ch < inChannels; ++ch) {
            inPlanar[ch][i] = input[i * inChannels + ch];
        }
    }

    AudioBusBuffers inBus;
    inBus.numChannels = inChannels;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inChannelsPtr.data();

    AudioBusBuffers outBus;
    outBus.numChannels = outChannels;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outChannelsPtr.data();

    data.inputs = &inBus;
    data.outputs = &outBus;

    processor->process(data);

    // Interleave
    for (int i = 0; i < numSamples; ++i) {
        for (int ch = 0; ch < outChannels; ++ch) {
            output[i * outChannels + ch] = outPlanar[ch][i];
        }
    }
}

bool VSTHost::saveState(const std::string& path) {
    if (!component) return false;
    MemoryStream* stream = new MemoryStream();
    bool success = false;
    if (component->getState(stream) == Steinberg::kResultOk) {
        std::ofstream file(path, std::ios::binary);
        if (file) {
            file.write(stream->getData().data(), stream->getData().size());
            file.close();
            success = true;
            std::cout << "[VST3] State saved to " << path << " (" << stream->getData().size() << " bytes)\n";
        }
    }
    stream->release();
    
    // Save controller state
    if (controller) {
        MemoryStream* ctrlStream = new MemoryStream();
        if (controller->getState(ctrlStream) == Steinberg::kResultOk) {
            std::ofstream cfile(path + ".ctrl", std::ios::binary);
            if (cfile) {
                cfile.write(ctrlStream->getData().data(), ctrlStream->getData().size());
                cfile.close();
            }
        }
        ctrlStream->release();
    }
    
    return success;
}

bool VSTHost::loadState(const std::string& path) {
    if (!component) return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<char> buffer(size);
    if (file.read(buffer.data(), size)) {
        MemoryStream* stream = new MemoryStream(buffer);

        bool compSuccess = false;
        Steinberg::tresult compRes = -1;
        if (component) {
            stream->seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            compRes = component->setState(stream);
            compSuccess = (compRes == Steinberg::kResultOk);
        }
        
        bool ctrlSuccess = false;
        if (controller) {
            stream->seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            Steinberg::tresult ctrlRes = controller->setComponentState(stream);
            ctrlSuccess = (ctrlRes == Steinberg::kResultOk);
            
            // Sync the Audio Component with the loaded Controller parameters!
            // Neumann RIME requires this explicit hydration via process() otherwise it defaults to Stereo passthrough!
            if (ctrlSuccess) {
                Steinberg::int32 paramCount = controller->getParameterCount();
                std::lock_guard<std::mutex> lock(paramMutex);
                for (Steinberg::int32 i = 0; i < paramCount; ++i) {
                    Steinberg::Vst::ParameterInfo info;
                    if (controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
                        Steinberg::Vst::ParamValue val = controller->getParamNormalized(info.id);
                        pendingParamChanges.push_back({info.id, val});
                    }
                }
            }
        }
        
        stream->release();
        
        // Load controller state
        if (controller) {
            std::ifstream cfile(path + ".ctrl", std::ios::binary | std::ios::ate);
            if (cfile) {
                std::streamsize csize = cfile.tellg();
                cfile.seekg(0, std::ios::beg);
                std::vector<char> cbuffer(csize);
                if (cfile.read(cbuffer.data(), csize)) {
                    MemoryStream* ctrlStream = new MemoryStream(cbuffer);
                    controller->setState(ctrlStream);
                    ctrlStream->release();
                }
            }
        }
        
        if (compSuccess || ctrlSuccess) {
            std::cout << "[VST3] State loaded from " << path << " (" << size << " bytes)\n";
            return true;
        }
    }
    return false;
}

bool VSTHost::showUI() {
    if (!controller) return false;
    if (hwndUI) {
        ShowWindow(hwndUI, SW_SHOW);
        return true;
    }

    plugView = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!plugView) return false;

    // Register Window Class
    WNDCLASSW wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"RimeVST3HostUI";
    RegisterClassW(&wc);

    // Create Window
    hwndUI = CreateWindowExW(
        0, L"RimeVST3HostUI", L"Neumann RIME Configuration",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!hwndUI) {
        plugView->release();
        plugView = nullptr;
        return false;
    }

    // Store VSTHost pointer in HWND for windowProc to use
    SetWindowLongPtrW(hwndUI, GWLP_USERDATA, (LONG_PTR)this);

    // Attach VST3 View to the Window
    if (plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == kResultTrue) {
        // Set the plug frame BEFORE attachment so the plugin can negotiate its size
        plugFrame = new PlugFrame(hwndUI);
        plugView->setFrame(plugFrame);
        
        plugView->attached(hwndUI, Steinberg::kPlatformTypeHWND);
        
        // Resize Window to fit plugin
        Steinberg::ViewRect rect;
        if (plugView->getSize(&rect) == kResultOk) {
            int w = rect.right - rect.left;
            int h = rect.bottom - rect.top;
            // Fallback if plugin reports zero size
            if (w < 100) w = 800;
            if (h < 100) h = 600;
            RECT winRect = { 0, 0, w, h };
            AdjustWindowRect(&winRect, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwndUI, nullptr, 0, 0, winRect.right - winRect.left, winRect.bottom - winRect.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        
        ShowWindow(hwndUI, SW_SHOW);
        return true;
    }

    // If unsupported platform type
    DestroyWindow(hwndUI);
    hwndUI = NULL;
    plugView->release();
    plugView = nullptr;
    return false;
}

void VSTHost::hideUI() {
    if (plugView) {
        plugView->setFrame(nullptr);
        plugView->removed();
        plugView->release();
        plugView = nullptr;
    }
    if (plugFrame) {
        plugFrame->release();
        plugFrame = nullptr;
    }
    if (hwndUI) {
        DestroyWindow(hwndUI);
        hwndUI = NULL;
    }
}

LRESULT CALLBACK VSTHost::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    VSTHost* host = (VSTHost*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    
    switch (uMsg) {
    case WM_SIZE:
        if (host && host->plugView) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            Steinberg::ViewRect viewRect(0, 0, rc.right, rc.bottom);
            host->plugView->onSize(&viewRect);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

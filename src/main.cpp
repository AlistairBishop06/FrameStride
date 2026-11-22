#include "core.hpp"
#include "dashboard.hpp"
#include "ipc.hpp"
#include "legacy_apps.hpp"

#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using namespace stride;
using namespace vr;
namespace fs = std::filesystem;
constexpr char appKey[] = "org.framestride.dashboard";
constexpr char probeKey[] = "org.framestride.input-test";
volatile std::sig_atomic_t stopSignal = 0;

void stop(int) { stopSignal = 1; }

struct Applications {
    IVRApplications* current = nullptr;
    void* prior = nullptr;
    explicit operator bool() const { return current || prior; }
    int manifest(const char* path, bool remove = false) const {
        if (!current) return legacy::manifest(prior, path, remove);
        return remove ? current->RemoveApplicationManifest(path) : current->AddApplicationManifest(path, true);
    }
    bool installed(const char* key) const { return current ? current->IsApplicationInstalled(key) : legacy::installed(prior, key); }
    int identify(uint32_t pid, const char* key) const {
        return current ? current->IdentifyApplication(pid, key) : legacy::identify(prior, pid, key);
    }
    const char* errorName(int error) const {
        return current ? current->GetApplicationsErrorNameFromEnum(static_cast<EVRApplicationError>(error)) : legacy::errorName(prior, error);
    }
    uint32_t scene() const { return current ? current->GetCurrentSceneProcessId() : (prior ? legacy::scene(prior) : 0); }
};

class Runtime {
public:
    explicit Runtime(EVRApplicationType type) {
        if (const char* path = std::getenv("FRAMESTRIDE_OPENVR_LIB"); path && *path) {
            lib_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        } else {
            for (const char* path : {"/opt/steamvr/bin/linuxarm64/libopenvr_api.so", "libopenvr_api.so"}) {
                lib_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                if (lib_) break;
            }
        }
        if (!lib_) { message = "Cannot load the native libopenvr_api.so"; return; }
        auto init = symbol<decltype(&VR_InitInternal2)>("VR_InitInternal2");
        shutdown_ = symbol<decltype(&VR_ShutdownInternal)>("VR_ShutdownInternal");
        interface_ = symbol<decltype(&VR_GetGenericInterface)>("VR_GetGenericInterface");
        if (!init || !shutdown_ || !interface_) { message = "OpenVR library lacks required entry points"; return; }
        EVRInitError error = VRInitError_None;
        init(&error, type, nullptr);
        if (error != VRInitError_None) { message = "VR_Init error " + std::to_string(error); return; }
        initialized_ = true;
        system = get<IVRSystem>(IVRSystem_Version);
        overlay = get<IVROverlay>(IVROverlay_Version);
        applications.current = get<IVRApplications>(IVRApplications_Version);
        if (!applications.current) applications.prior = get<void>("IVRApplications_007");
        input = get<IVRInput>(IVRInput_Version);
        if (!input) input = get<IVRInput>("IVRInput_010");
        if (!system) message = std::string("Runtime does not expose ") + IVRSystem_Version;
    }

    ~Runtime() {
        if (initialized_) shutdown_();
        if (lib_) dlclose(lib_);
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    IVRSystem* system = nullptr;
    IVROverlay* overlay = nullptr;
    Applications applications;
    IVRInput* input = nullptr;
    std::string message;

private:
    template<class T> T symbol(const char* name) { return reinterpret_cast<T>(dlsym(lib_, name)); }
    template<class T> T* get(const char* name) {
        EVRInitError error = VRInitError_None;
        return static_cast<T*>(interface_(name, &error));
    }
    void* lib_ = nullptr;
    decltype(&VR_ShutdownInternal) shutdown_ = nullptr;
    decltype(&VR_GetGenericInterface) interface_ = nullptr;
    bool initialized_ = false;
};

class MotionTracker {
public:
    Pose sample(IVRSystem& system, double now) {
        TrackedDevicePose_t pose{};
        system.GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid || pose.eTrackingResult != TrackingResult_Running_OK) {
            previous_ = false;
            return {now, 0, 0, false};
        }
        const auto& m = pose.mDeviceToAbsoluteTracking.m;
        const bool jump = previous_ && std::hypot(m[0][3] - x_, m[2][3] - z_) > 0.13;
        previous_ = true;
        x_ = m[0][3];
        z_ = m[2][3];
        return {now, m[1][3], std::asin(std::clamp(m[1][2], -1.0f, 1.0f)), !jump};
    }

    bool leftStick(IVRSystem& system, float& y, bool& shortcut, std::uint64_t now) {
        const auto left = system.GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_LeftHand);
        if (left == k_unTrackedDeviceIndexInvalid) return false;
        if (left != left_ || now >= axisCheck_) {
            left_ = left;
            axisCheck_ = now + 2000;
            axis_ = -1;
            for (int i = 0; i < k_unControllerStateAxisCount; ++i) {
                const auto property = static_cast<ETrackedDeviceProperty>(Prop_Axis0Type_Int32 + i);
                if (system.GetInt32TrackedDeviceProperty(left, property) == k_eControllerAxis_Joystick) { axis_ = i; break; }
            }
        }
        VRControllerState_t state{};
        if (axis_ < 0 || !system.GetControllerState(left, &state, sizeof(state))) return false;
        y = state.rAxis[axis_].y;
        const auto combo = ButtonMaskFromId(k_EButton_Grip) | ButtonMaskFromId(static_cast<EVRButtonId>(k_EButton_Axis0 + axis_));
        shortcut = (state.ulButtonPressed & combo) == combo;
        return std::isfinite(y);
    }

private:
    std::uint64_t axisCheck_ = 0;
    TrackedDeviceIndex_t left_ = k_unTrackedDeviceIndexInvalid;
    int axis_ = -1;
    float x_ = 0, z_ = 0;
    bool previous_ = false;
};

struct Shared {
    std::mutex mutex;
    PanelState state;
    std::atomic<bool> end{false};
    bool step = false, runtimeQuit = false;
};

class InputBackend {
public:
    virtual ~InputBackend() = default;
    virtual Packet status(bool openxr, std::uint64_t now) = 0;
    virtual bool send(float forward, const Settings&, unsigned flags, std::uint64_t now) = 0;
};

class NativeBackend final : public InputBackend {
public:
    bool connect() { return motion_.connect(clockMilliseconds()); }
    Packet status(bool openxr, std::uint64_t now) override {
        return (openxr ? xr_ : driver_).read(now, statusTimeoutMs);
    }
    bool send(float forward, const Settings& settings, unsigned flags, std::uint64_t now) override {
        if (settings.openxr) flags |= XrSelected;
        if (settings.physicalOverride) flags |= Override;
        return motion_.write(forward, static_cast<float>(settings.overrideThreshold), flags, now);
    }
private:
    Mailbox motion_{Channel::Motion, true}, driver_{Channel::Driver, false}, xr_{Channel::OpenXR, false};
};

void track(Runtime& vr, Shared& shared, const fs::path& config, InputBackend& backend) {
    Detector detector;
    MotionTracker tracking;
    auto previousStage = Calibration::Stage::Idle;
    bool startArmed, wasEnabled = false;
    {
        std::lock_guard hold(shared.mutex);
        startArmed = shared.state.settings.startup;
    }
    double saveAt = 0, holdUntil = 0, shortcutSince = 0;
    uint32_t scene = 0;
    auto next = std::chrono::steady_clock::now();
    unsigned tick = 0;
    while (!stopSignal && !shared.end) {
        const auto now = clockMilliseconds();
        const double seconds = clockSeconds();
        VREvent_t event{};
        for (int i = 0; i < 40 && vr.system->PollNextEvent(&event, sizeof(event)); ++i) {
            if (event.eventType == VREvent_Quit) {
                vr.system->AcknowledgeQuit_Exiting();
                shared.runtimeQuit = true;
                shared.end = true;
                break;
            }
            if (event.eventType == VREvent_StandingZeroPoseReset || event.eventType == VREvent_SeatedZeroPoseReset ||
                event.eventType == VREvent_ChaperoneUniverseHasChanged || event.eventType == VREvent_ChaperoneRoomSetupCommitted) {
                holdUntil = seconds + 0.5;
            }
        }
        if (tick++ % 4 == 0) {
            const auto current = vr.applications.scene();
            if (current != scene) holdUntil = seconds + 0.35;
            scene = current;
        }
        const bool dashboard = vr.overlay->IsDashboardVisible();
        auto pose = tracking.sample(*vr.system, seconds);
        if (seconds < holdUntil) pose.valid = false;
        Settings settings;
        bool save = false;
        {
            std::lock_guard hold(shared.mutex);
            auto& state = shared.state;
            auto& view = state.view;
            settings = state.settings;
            const auto status = backend.status(settings.openxr, now);
            const bool treadmillSuppressed = !settings.openxr && !state.driver.availableToConnect();
            const bool connected = !treadmillSuppressed && status.has(Ready) && status.has(Submitted);
            if (state.stop) { shared.end = true; }
            if (!settings.startup || state.cancelStartup || state.calibration.active() || state.stop) startArmed = false;
            if (startArmed && connected) { view.armed = true; startArmed = false; }
            if (!connected) view.armed = false;
            const auto stage = state.calibration.stage();
            if (wasEnabled != view.armed || stage != previousStage) detector.reset();
            wasEnabled = view.armed;
            previousStage = stage;
            const auto motion = state.calibration.measure(detector, pose, settings);
            state.calibration.update(seconds, motion, state.settings);
            if (stage != Calibration::Stage::Complete && state.calibration.stage() == Calibration::Stage::Complete)
                state.changed = true;
            float physicalY = 0;
            bool shortcut = false;
            const bool checkController = settings.controllerStop || (!settings.openxr && settings.physicalOverride);
            const bool controller = checkController && tracking.leftStick(*vr.system, physicalY, shortcut, now);
            const bool physical = settings.openxr || !settings.physicalOverride || controller;
            if (!settings.controllerStop || !controller || !shortcut) shortcutSince = 0;
            else if (shortcutSince == 0) shortcutSince = seconds;
            else if (seconds - shortcutSince >= 0.6) {
                view.armed = false;
                startArmed = false;
            }
            const bool manual = !settings.openxr && settings.physicalOverride && physical &&
                                std::abs(physicalY) >= settings.overrideThreshold;
            const bool allowed = !shared.end && !stopSignal && view.armed && connected && motion.tracking &&
                                 !state.calibration.active() && (!settings.pauseDashboard || !dashboard) &&
                                 (!settings.gameOnly || scene != 0) && physical;
            const float forward = allowed && !manual ? motion.stick : 0;
            const unsigned flags = (allowed ? Ready : 0) | (motion.tracking ? Tracking : 0) |
                                   (treadmillSuppressed ? TreadmillSuppressed : 0);
            const bool written = backend.send(forward, settings, flags, now);
            view = {motion, status, view.armed, allowed, physical, manual, dashboard, scene != 0,
                    settings.openxr && status.has(Physical), written ? forward : 0};
            shared.step = shared.step || motion.step;
            if (state.changed) { saveAt = seconds + 0.30; state.changed = false; }
            save = saveAt > 0 && seconds >= saveAt;
            if (save) { settings = state.settings; saveAt = 0; }
        }
        if (save) {
            std::string error;
            saveSettings(config, settings, error);
            std::lock_guard hold(shared.mutex);
            shared.state.error = std::move(error);
        }
        next += 12500us;
        const auto clock = std::chrono::steady_clock::now();
        if (next < clock) next = clock + 12500us;
        std::this_thread::sleep_until(next);
    }
    backend.send(0, {}, 0, 0);
    {
        std::lock_guard hold(shared.mutex);
        if (saveAt > 0 || shared.state.changed) saveSettings(config, shared.state.settings, shared.state.error);
    }
    shared.end = true;
}

int registerApp(const fs::path& manifest, bool remove) {
    Runtime vr(VRApplication_Utility);
    if (!vr.applications) { std::cerr << "SteamVR application registration unavailable. " << vr.message << '\n'; return 2; }
    auto& apps = vr.applications;
    if (remove && !apps.installed(appKey) && !apps.installed(probeKey)) return 0;
    const auto result = apps.manifest(manifest.c_str(), remove);
    if (result != VRApplicationError_None) { std::cerr << apps.errorName(result) << '\n'; return 2; }
    return 0;
}

int doctor() {
    Runtime vr(VRApplication_Background);
    const auto now = clockMilliseconds();
    Mailbox driver(Channel::Driver, false), xr(Channel::OpenXR, false), motion(Channel::Motion, false);
    const auto source = motion.read(now);
    const auto status = (source.has(XrSelected) ? xr : driver).read(now, statusTimeoutMs);
    MotionTracker tracker;
    const auto pose = vr.system ? tracker.sample(*vr.system, clockSeconds()) : Pose{};
    float physicalY = 0;
    bool shortcut = false;
    const bool physical = vr.system && tracker.leftStick(*vr.system, physicalY, shortcut, now);
    std::cout << std::boolalpha << "OpenVR connected: " << (vr.system != nullptr) << "   " << vr.message
              << "\nHeadset tracking: " << pose.valid << "   Height (m): " << pose.height
              << "\nMotion process heartbeat: " << (source.timestamp != 0)
              << "\nPhysical left joystick readable: " << physical << "   Y: " << physicalY
              << "\nSelected backend: " << (source.has(XrSelected) ? "OpenXR" : "SteamVR treadmill")
              << "\nInput target registered: " << status.has(Ready)
              << "\nBackend accepted output: " << status.has(Submitted) << "   Y: " << status.value
              << "\nGameplay reception: requires observation in a game\n";
    return vr.system && pose.valid && source.timestamp && status.has(Submitted) ? 0 : 1;
}

int probe(const fs::path& root) {
    Runtime vr(VRApplication_Overlay);
    if (!vr.input || !vr.applications || !vr.system) { std::cerr << "SteamVR Input unavailable. " << vr.message << '\n'; return 2; }
    if (vr.applications.manifest((root / "framestride.vrmanifest").c_str()) != VRApplicationError_None ||
        vr.applications.identify(static_cast<uint32_t>(getpid()), probeKey) != VRApplicationError_None) {
        std::cerr << "Could not register the FrameStride Input Test application.\n";
        return 2;
    }
    auto& input = *vr.input;
    VRActionSetHandle_t set = 0;
    VRActionHandle_t move = 0;
    VRInputValueHandle_t treadmill = 0;
    if (input.SetActionManifestPath((root / "probe/actions.json").c_str()) != VRInputError_None ||
        input.GetActionSetHandle("/actions/test", &set) != VRInputError_None ||
        input.GetActionHandle("/actions/test/in/move", &move) != VRInputError_None ||
        input.GetInputSourceHandle("/user/treadmill", &treadmill) != VRInputError_None) {
        std::cerr << "Could not load the FrameStride Input Test action/binding.\n";
        return 2;
    }
    VRActiveActionSet_t active{};
    active.ulActionSet = set;
    std::cout << "Reading real SteamVR treadmill actions for 25 seconds. Walk, then stop.\n" << std::flush;
    const auto start = clockSeconds();
    bool moved = false, stopped = false;
    int previous = -1;
    while (!stopSignal && clockSeconds() - start < 25) {
        InputAnalogActionData_t data{};
        const bool read = input.UpdateActionState(&active, sizeof(active), 1) == VRInputError_None &&
                          input.GetAnalogActionData(move, &data, sizeof(data), treadmill) == VRInputError_None && data.bActive;
        const int value = read ? static_cast<int>(std::round(data.y * 100)) : -1;
        if (value != previous) {
            if (read) std::cout << "SteamVR action X=" << data.x << " Y=" << data.y << '\n';
            else std::cout << "SteamVR action inactive. Check the treadmill binding and input focus.\n";
            previous = value;
        }
        moved = moved || value >= 10;
        stopped = stopped || (moved && value == 0);
        if (stopped) break;
        std::this_thread::sleep_for(25ms);
    }
    std::cout << (stopped ? "PASS: Input Test received forward input followed by neutral.\n" :
                             "FAIL: Input Test did not observe both forward input and neutral.\n");
    return stopped ? 0 : 1;
}

int run(const fs::path& root, bool showDashboard) {
    Runtime vr(VRApplication_Overlay);
    if (!vr.system || !vr.overlay) { std::cerr << "SteamVR not ready. " << vr.message << '\n'; return 75; }
    NativeBackend backend;
    if (!backend.connect()) { std::cerr << "Motion mailbox unavailable (already running or no private runtime directory).\n"; return 75; }
    Dashboard dashboard(root / "assets/InterVariable.ttf");
    DriverSetup driverSetup(root / "treadmill.sh");
    VROverlayHandle_t main = 0, thumb = 0;
    auto& api = *vr.overlay;
    const auto result = api.CreateDashboardOverlay(appKey, "FrameStride", &main, &thumb);
    if (result != VROverlayError_None) { std::cerr << api.GetOverlayErrorNameFromEnum(result) << '\n'; return 75; }
    api.SetOverlayFromFile(thumb, (root / "assets/icon.png").c_str());
    api.SetOverlayWidthInMeters(main, 1.8f);
    api.SetOverlayInputMethod(main, VROverlayInputMethod_Mouse);
    api.SetOverlayFlag(main, VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    const HmdVector2_t mouseScale{{static_cast<float>(Dashboard::width), static_cast<float>(Dashboard::height)}};
    api.SetOverlayMouseScale(main, &mouseScale);
    Shared shared;
    const char* home = std::getenv("HOME");
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const auto config = (xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config") / "framestride/settings.json";
    shared.state.settings = loadSettings(config, shared.state.error);
    if (vr.applications) {
        const auto registered = vr.applications.manifest((root / "framestride.vrmanifest").c_str());
        if (registered != VRApplicationError_None) std::cerr << "SteamVR manifest registration: " << registered << '\n';
    }
    std::cout << "FrameStride " << FRAMESTRIDE_VERSION << " dashboard running\n" << std::flush;
    std::thread engine(track, std::ref(vr), std::ref(shared), std::cref(config), std::ref(backend));
    auto lastFrame = std::chrono::steady_clock::time_point{};
    auto lastSample = lastFrame;
    bool initial = true, visible = false, errorLogged = false;
    try {
        while (!stopSignal && !shared.end) {
            VREvent_t event{};
            for (int i = 0; i < 64 && api.PollNextOverlayEvent(main, &event, sizeof(event)); ++i) {
                if (event.eventType == VREvent_OverlayClosed) { shared.end = true; break; }
                if (event.eventType != VREvent_MouseMove && event.eventType != VREvent_MouseButtonDown &&
                    event.eventType != VREvent_MouseButtonUp) continue;
                if (event.eventType != VREvent_MouseMove && event.data.mouse.button != VRMouseButton_Left) continue;
                std::lock_guard hold(shared.mutex);
                dashboard.pointer(event.data.mouse.x, Dashboard::height - event.data.mouse.y,
                                  event.eventType == VREvent_MouseButtonDown, event.eventType == VREvent_MouseButtonUp, shared.state);
            }
            {
                std::lock_guard hold(shared.mutex);
                driverSetup.poll(shared.state.driver);
            }
            const bool displayed = api.IsOverlayVisible(main);
            if (visible && !displayed) {
                std::lock_guard hold(shared.mutex);
                dashboard.pointer(0, 0, false, true, shared.state);
            }
            const auto time = std::chrono::steady_clock::now();
            const bool redraw = initial || (displayed && (!visible || time - lastFrame > 83ms));
            if (redraw || time - lastSample > 83ms) {
                PanelState copy;
                bool step;
                {
                    std::lock_guard hold(shared.mutex);
                    if (redraw) copy = shared.state;
                    else copy.view.motion = shared.state.view.motion;
                    step = shared.step;
                    shared.step = false;
                }
                dashboard.sample(copy.view.motion, step);
                lastSample = time;
                if (redraw) {
                    dashboard.draw(copy);
                    const auto error = api.SetOverlayRaw(main, dashboard.pixels(), Dashboard::width, Dashboard::height, 4);
                    if (error != VROverlayError_None && !errorLogged) {
                        std::cerr << "Dashboard texture submission: " << api.GetOverlayErrorNameFromEnum(error) << '\n';
                        errorLogged = true;
                    }
                    lastFrame = time;
                    if (initial && showDashboard && error == VROverlayError_None) api.ShowDashboard(appKey);
                    initial = false;
                }
            }
            visible = displayed;
            std::this_thread::sleep_for(visible ? 20ms : 100ms);
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        shared.end = true;
    }
    shared.end = true;
    engine.join();
    api.DestroyOverlay(thumb);
    api.DestroyOverlay(main);
    return shared.runtimeQuit ? 75 : 0;
}
}

int main(int argc, char** argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        const std::string_view arg(argc > 1 ? argv[1] : "");
        if (arg == "--version") { std::cout << "FrameStride " << FRAMESTRIDE_VERSION << '\n'; return 0; }
        const auto root = std::filesystem::canonical("/proc/self/exe").parent_path();
        if (arg == "--doctor") return doctor();
        if (arg == "--probe") return probe(root);
        if (arg == "--register" || arg == "--unregister") return registerApp(root / "framestride.vrmanifest", arg == "--unregister");
        if (arg.empty() || arg == "--open-dashboard") return run(root, arg == "--open-dashboard");
        std::cerr << "Usage: framestride [--open-dashboard|--version|--doctor|--probe|--register|--unregister]\n";
    } catch (const std::exception& ex) { std::cerr << "FrameStride: " << ex.what() << '\n'; }
    return 2;
}

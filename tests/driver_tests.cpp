#include "ipc.hpp"

#include <openvr_driver.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vr;
using namespace stride;
using namespace std::chrono_literals;

void check(bool success, const char* failure) { if (!success) throw std::runtime_error(failure); }

struct Input final : IVRDriverInput {
    float x = 0, y = 0;
    bool touch = false, reject = false;
    EVRInputError CreateBooleanComponent(PropertyContainerHandle_t, const char* name, VRInputComponentHandle_t* handle) override {
        check(std::strcmp(name, "/input/joystick/touch") == 0, "incorrect touch component");
        *handle = 3;
        return VRInputError_None;
    }
    EVRInputError UpdateBooleanComponent(VRInputComponentHandle_t handle, bool value, double) override {
        check(handle == 3, "incorrect touch handle");
        touch = value;
        return VRInputError_None;
    }
    EVRInputError CreateScalarComponent(PropertyContainerHandle_t, const char* name, VRInputComponentHandle_t* handle,
                                       EVRScalarType type, EVRScalarUnits units) override {
        check(type == VRScalarType_Absolute && units == VRScalarUnits_NormalizedTwoSided, "incorrect scalar range");
        if (std::strcmp(name, "/input/joystick/x") == 0) *handle = 1;
        else if (std::strcmp(name, "/input/joystick/y") == 0) *handle = 2;
        else throw std::runtime_error("unexpected scalar path");
        return VRInputError_None;
    }
    EVRInputError UpdateScalarComponent(VRInputComponentHandle_t handle, float value, double) override {
        if (handle == 1) x = value;
        else if (handle == 2) {
            if (reject && value != 0) return VRInputError_InvalidHandle;
            y = value;
        } else return VRInputError_InvalidHandle;
        return VRInputError_None;
    }
    EVRInputError CreateHapticComponent(PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*) override { return VRInputError_InvalidParam; }
    EVRInputError CreateSkeletonComponent(PropertyContainerHandle_t, const char*, const char*, const char*, EVRSkeletalTrackingLevel,
                                         const VRBoneTransform_t*, uint32_t, VRInputComponentHandle_t*) override { return VRInputError_InvalidParam; }
    EVRInputError UpdateSkeletonComponent(VRInputComponentHandle_t, EVRSkeletalMotionRange, const VRBoneTransform_t*, uint32_t) override { return VRInputError_InvalidParam; }
    EVRInputError CreatePoseComponent(PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*) override { return VRInputError_InvalidParam; }
    EVRInputError UpdatePoseComponent(VRInputComponentHandle_t, const HmdMatrix34_t*, double) override { return VRInputError_InvalidParam; }
    EVRInputError CreateEyeTrackingComponent(PropertyContainerHandle_t, const char*, VRInputComponentHandle_t*) override { return VRInputError_InvalidParam; }
    EVRInputError UpdateEyeTrackingComponent(VRInputComponentHandle_t, const VREyeTrackingData_t*, double) override { return VRInputError_InvalidParam; }
};

struct Properties final : IVRProperties {
    int role = 0;
    ETrackedPropertyError ReadPropertyBatch(PropertyContainerHandle_t, PropertyRead_t*, uint32_t) override { return TrackedProp_Success; }
    ETrackedPropertyError WritePropertyBatch(PropertyContainerHandle_t handle, PropertyWrite_t* write, uint32_t count) override {
        check(handle == 68, "unexpected property container");
        for (uint32_t i = 0; i < count; ++i) {
            if (write[i].prop == Prop_ControllerRoleHint_Int32) std::memcpy(&role, write[i].pvBuffer, sizeof(role));
        }
        return TrackedProp_Success;
    }
    const char* GetPropErrorNameFromEnum(ETrackedPropertyError) override { return "mock"; }
    PropertyContainerHandle_t TrackedDeviceToPropertyContainer(TrackedDeviceIndex_t) override { return 68; }
};

struct Host final : IVRServerDriverHost {
    bool connected = false;
    bool TrackedDeviceAdded(const char*, ETrackedDeviceClass deviceClass, ITrackedDeviceServerDriver* device) override {
        return deviceClass == TrackedDeviceClass_Controller && device->Activate(5) == VRInitError_None;
    }
    void TrackedDevicePoseUpdated(uint32_t, const DriverPose_t& pose, uint32_t) override { connected = pose.deviceIsConnected; }
    void VsyncEvent(double) override {}
    void VendorSpecificEvent(uint32_t, EVREventType, const VREvent_Data_t&, double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(VREvent_t*, uint32_t) override { return false; }
    void GetRawTrackedDevicePoses(float, TrackedDevicePose_t*, uint32_t) override {}
    void RequestRestart(const char*, const char*, const char*, const char*) override {}
    uint32_t GetFrameTimings(Compositor_FrameTiming*, uint32_t) override { return 0; }
    void SetDisplayEyeToHead(uint32_t, const HmdMatrix34_t&, const HmdMatrix34_t&) override {}
    void SetDisplayProjectionRaw(uint32_t, const HmdRect2_t&, const HmdRect2_t&) override {}
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
};

struct DriverLog final : IVRDriverLog { void Log(const char* text) override { std::cout << text << '\n'; } };
struct Context final : IVRDriverContext {
    Input input;
    Host host;
    Properties properties;
    DriverLog log;
    void* GetGenericInterface(const char* name, EVRInitError* code) override {
        if (code) *code = VRInitError_None;
        if (std::strcmp(name, IVRDriverInput_Version) == 0) return &input;
        if (std::strcmp(name, IVRServerDriverHost_Version) == 0) return &host;
        if (std::strcmp(name, IVRProperties_Version) == 0) return &properties;
        if (std::strcmp(name, IVRDriverLog_Version) == 0) return &log;
        // These interfaces are presence-checked by the SDK helper and unused by the driver.
        if (std::strcmp(name, IVRSettings_Version) == 0 || std::strcmp(name, IVRDriverManager_Version) == 0 ||
            std::strcmp(name, IVRResources_Version) == 0) return this;
        if (code) *code = VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    DriverHandle_t GetDriverHandle() override { return 1; }
};

void run(const char* library) {
    void* module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!module) throw std::runtime_error(dlerror());
    const auto factory = reinterpret_cast<void* (*)(const char*, int*)>(dlsym(module, "HmdDriverFactory"));
    check(factory, "missing OpenVR factory export");
    int code = -1;
    check(factory("wrong", &code) == nullptr && code == VRInitError_Init_InterfaceNotFound, "factory version check failed");
    auto* provider = static_cast<IServerTrackedDeviceProvider*>(factory(IServerTrackedDeviceProvider_Version, &code));
    check(provider && code == VRInitError_None, "missing OpenVR server provider");
    Context context;
    check(provider->Init(&context) == VRInitError_None && context.host.connected, "treadmill device did not connect");
    check(context.properties.role == TrackedControllerRole_Treadmill && !provider->ShouldBlockStandbyMode(), "incorrect controller role or standby behavior");
    Mailbox writer(Channel::Motion, true), status(Channel::Driver, false);
    auto publish = [&](float value, unsigned flags) {
        check(writer.write(value, .13f, flags, clockMilliseconds()), "failed to supply driver test input");
        provider->RunFrame();
    };
    auto pushed = [&](float expected) {
        check(context.input.x == 0 && std::abs(context.input.y - expected) < .002f, "incorrect SteamVR scalar submitted");
        check(context.input.touch == (expected > 0), "incorrect synthetic touch state");
    };
    publish(.6f, Ready | Tracking);
    pushed(.6f);
    check(status.read(clockMilliseconds()).has(Submitted), "successful SteamVR submission was not reported");
    publish(.6f, Tracking);
    pushed(0);
    publish(.6f, Ready | Tracking | XrSelected);
    pushed(0);
    publish(.6f, Ready);
    pushed(0);
    publish(.9f, Ready | Tracking);
    pushed(.9f);
    context.input.reject = true;
    publish(.6f, Ready | Tracking);
    pushed(0);
    check(!status.read(clockMilliseconds()).has(Submitted), "failed SteamVR submission was reported as accepted");
    context.input.reject = false;
    publish(.9f, Ready | Tracking);
    pushed(.9f);
    provider->EnterStandby();
    pushed(0);
    provider->LeaveStandby();
    provider->RunFrame();
    pushed(0);
    std::this_thread::sleep_for(3ms);
    publish(.9f, Ready | Tracking);
    pushed(.9f);
    std::this_thread::sleep_for(130ms);
    provider->RunFrame();
    pushed(0);
    publish(.6f, Ready | Tracking);
    provider->Cleanup();
    pushed(0);
    check(!status.read(clockMilliseconds()).has(Ready), "cleanup left device reported as ready");
    dlclose(module);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto runtime = (std::filesystem::temp_directory_path() / "framestride-driver.XXXXXX").string();
    if (!mkdtemp(runtime.data())) return 2;
    setenv("FRAMESTRIDE_RUNTIME_DIR", runtime.c_str(), 1);
    try { run(argv[1]); }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        std::filesystem::remove_all(runtime);
        return 1;
    }
    std::filesystem::remove_all(runtime);
    std::cout << "PASS: OpenVR treadmill registration, scalar submission, errors, standby, watchdog and unload\n";
}

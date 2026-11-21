#include "ipc.hpp"

#include <openvr_driver.h>

#include <cstring>

namespace {
using namespace vr;
using namespace stride;
constexpr char serial[] = "FrameStride-Treadmill-01";

class Treadmill final : public ITrackedDeviceServerDriver {
public:
    EVRInitError Activate(TrackedDeviceIndex_t index) override {
        device_ = index;
        const auto properties = VRProperties()->TrackedDeviceToPropertyContainer(index);
        VRProperties()->SetStringProperty(properties, Prop_ModelNumber_String, "FrameStride Treadmill");
        VRProperties()->SetStringProperty(properties, Prop_ManufacturerName_String, "FrameStride");
        VRProperties()->SetStringProperty(properties, Prop_ControllerType_String, "framestride_treadmill");
        VRProperties()->SetStringProperty(properties, Prop_InputProfilePath_String,
                                           "{framestride}/input/framestride_profile.json");
        VRProperties()->SetInt32Property(properties, Prop_ControllerRoleHint_Int32, TrackedControllerRole_Treadmill);
        VRProperties()->SetBoolProperty(properties, Prop_NeverTracked_Bool, true);
        auto component = [properties](const char* name, VRInputComponentHandle_t& handle) {
            return VRDriverInput()->CreateScalarComponent(properties, name, &handle, VRScalarType_Absolute,
                                                           VRScalarUnits_NormalizedTwoSided) == VRInputError_None;
        };
        ready_ = component("/input/joystick/x", x_) && component("/input/joystick/y", y_) &&
                 VRDriverInput()->CreateBooleanComponent(properties, "/input/joystick/touch", &touch_) == VRInputError_None;
        if (!ready_) {
            VRDriverLog()->Log("FrameStride: could not create treadmill input components");
            return VRInitError_Driver_Failed;
        }
        VRServerDriverHost()->TrackedDevicePoseUpdated(device_, GetPose(), sizeof(DriverPose_t));
        VRDriverLog()->Log("FrameStride: treadmill input components registered");
        publish(0);
        return VRInitError_None;
    }

    void Deactivate() override {
        release();
        ready_ = false;
        device_ = k_unTrackedDeviceIndexInvalid;
        status_.write(0, 0, 0, clockMilliseconds());
    }
    void EnterStandby() override { standby_ = true; release(); }
    void wake() { standby_ = false; resumeAfter_ = clockMilliseconds(); }
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override { if (size && response) *response = '\0'; }
    DriverPose_t GetPose() override {
        DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1;
        pose.deviceIsConnected = ready_;
        pose.result = TrackingResult_Uninitialized;
        return pose;
    }

    void run() {
        if (!ready_) return;
        const auto now = clockMilliseconds();
        const auto motion = motion_.read(now);
        const float value = !standby_ && motion.timestamp > resumeAfter_ && motion.has(Ready) &&
                            motion.has(Tracking) && !motion.has(XrSelected) ? motion.value : 0;
        if (value != lastValue_ || !submitted_ || now >= lastStatus_ + 200) {
            publish(value);
            status_.write(lastValue_, 0, Ready | (submitted_ ? Submitted : 0), now);
            lastStatus_ = now;
        }
    }

    void release() { if (ready_) publish(0); }

private:
    void publish(float y) {
        auto* input = VRDriverInput();
        if (!input) return;
        const bool successX = input->UpdateScalarComponent(x_, 0, 0) == VRInputError_None;
        const bool successY = input->UpdateScalarComponent(y_, y, 0) == VRInputError_None;
        const bool touch = input->UpdateBooleanComponent(touch_, y > 0, 0) == VRInputError_None;
        submitted_ = successX && successY && touch;
        if (!submitted_ && y > 0) {
            input->UpdateScalarComponent(y_, 0, 0);
            input->UpdateBooleanComponent(touch_, false, 0);
        }
        lastValue_ = submitted_ ? y : 0;
    }

    Mailbox motion_{Channel::Motion, false}, status_{Channel::Driver, true};
    TrackedDeviceIndex_t device_ = k_unTrackedDeviceIndexInvalid;
    VRInputComponentHandle_t x_ = 0, y_ = 0, touch_ = 0;
    std::uint64_t lastStatus_ = 0, resumeAfter_ = 0;
    float lastValue_ = 0;
    bool ready_ = false, submitted_ = false, standby_ = false;
};

class Provider final : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        return VRServerDriverHost()->TrackedDeviceAdded(serial, TrackedDeviceClass_Controller, &device_)
            ? VRInitError_None : VRInitError_Driver_Failed;
    }
    void Cleanup() override { device_.Deactivate(); VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override { device_.run(); }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override { device_.EnterStandby(); }
    void LeaveStandby() override { device_.wake(); }

private:
    Treadmill device_;
};
Provider provider;
}

extern "C" __attribute__((visibility("default"))) void* HmdDriverFactory(const char* name, int* error) {
    if (name && std::strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (error) *error = vr::VRInitError_None;
        return &provider;
    }
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

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
        auto* input = VRDriverInput();
        if (!input) {
            VRDriverLog()->Log("FrameStride: SteamVR driver input interface is unavailable");
            return VRInitError_Driver_Failed;
        }
        const auto properties = VRProperties()->TrackedDeviceToPropertyContainer(index);
        VRProperties()->SetStringProperty(properties, Prop_ModelNumber_String, "FrameStride Treadmill");
        VRProperties()->SetStringProperty(properties, Prop_ManufacturerName_String, "FrameStride");
        VRProperties()->SetStringProperty(properties, Prop_ControllerType_String, "framestride_treadmill");
        VRProperties()->SetStringProperty(properties, Prop_InputProfilePath_String,
                                           "{framestride}/input/framestride_profile.json");
        VRProperties()->SetInt32Property(properties, Prop_ControllerRoleHint_Int32, TrackedControllerRole_Treadmill);
        VRProperties()->SetBoolProperty(properties, Prop_NeverTracked_Bool, true);
        auto component = [input, properties](const char* name, VRInputComponentHandle_t& handle) {
            return input->CreateScalarComponent(properties, name, &handle, VRScalarType_Absolute,
                                                           VRScalarUnits_NormalizedTwoSided) == VRInputError_None;
        };
        if (!component("/input/joystick/x", x_) || !component("/input/joystick/y", y_) ||
            input->CreateBooleanComponent(properties, "/input/joystick/touch", &touch_) != VRInputError_None) {
            VRDriverLog()->Log("FrameStride: could not create treadmill input components");
            return VRInitError_Driver_Failed;
        }
        device_ = index;
        VRDriverLog()->Log("FrameStride: treadmill input components registered");
        return VRInitError_None;
    }

    void Deactivate() override {
        if (isActivated()) {
            publish(0);
            status_.disconnect();
        }
        connected_ = submitted_ = false;
        device_ = k_unTrackedDeviceIndexInvalid;
        standby_ = false;
        resumeAfter_ = 0;
    }
    void EnterStandby() override { standby_ = true; disconnect(); }
    void wake() { standby_ = false; resumeAfter_ = clockMilliseconds(); }
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override { if (size && response) *response = '\0'; }
    DriverPose_t GetPose() override {
        DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1;
        pose.deviceIsConnected = connected_;
        pose.result = TrackingResult_Uninitialized;
        return pose;
    }

    void run(const Packet& motion, std::uint64_t now) {
        if (!isActivated()) return;
        if (standby_ || motion.timestamp <= resumeAfter_ || motion.has(XrSelected)) {
            disconnect();
            return;
        }
        const bool newlyConnected = !connected_;
        if (newlyConnected) updateConnection(true);
        const float value = motion.has(Ready) && motion.has(Tracking) ? motion.value : 0;
        if (newlyConnected || value != lastValue_ || !submitted_ || now >= lastStatus_ + 80) {
            publish(value);
            status_.write(lastValue_, 0, Ready | (submitted_ ? Submitted : 0), now);
            lastStatus_ = now;
        }
    }

private:
    bool isActivated() const { return device_ != k_unTrackedDeviceIndexInvalid; }

    void updateConnection(bool connected) {
        connected_ = connected;
        VRServerDriverHost()->TrackedDevicePoseUpdated(device_, GetPose(), sizeof(DriverPose_t));
    }

    void disconnect() {
        if (!connected_) return;
        publish(0);
        updateConnection(false);
        status_.disconnect();
    }

    void publish(float y) {
        auto* input = VRDriverInput();
        if (!input) { submitted_ = false; return; }
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

    Mailbox status_{Channel::Driver, true};
    TrackedDeviceIndex_t device_ = k_unTrackedDeviceIndexInvalid;
    VRInputComponentHandle_t x_ = 0, y_ = 0, touch_ = 0;
    std::uint64_t lastStatus_ = 0, resumeAfter_ = 0;
    float lastValue_ = 0;
    bool connected_ = false, submitted_ = false, standby_ = false;
};

class Provider final : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        return VRInitError_None;
    }
    void Cleanup() override {
        device_.EnterStandby();
        device_.Deactivate();
        motion_.disconnect();
        registered_ = false;
        standby_ = false;
        nextRegistration_ = 0;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override {
        if (standby_ || VRServerDriverHost()->IsExiting()) return;
        const auto now = clockMilliseconds();
        const auto motion = motion_.read(now);
        if (!registered_ && motion.timestamp && !motion.has(XrSelected) && now >= nextRegistration_) {
            nextRegistration_ = now + 2000;
            if (VRDriverInput()) {
                registered_ = VRServerDriverHost()->TrackedDeviceAdded(serial, TrackedDeviceClass_Controller, &device_);
            }
        }
        if (registered_) device_.run(motion, now);
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override { standby_ = true; device_.EnterStandby(); }
    void LeaveStandby() override { standby_ = false; device_.wake(); }

private:
    Treadmill device_;
    Mailbox motion_{Channel::Motion, false};
    std::uint64_t nextRegistration_ = 0;
    bool registered_ = false, standby_ = false;
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

#include "ipc.hpp"

#define XR_NO_PROTOTYPES
#include <openxr_loader_negotiation.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
using namespace stride;
using namespace std::chrono_literals;
constexpr XrPath left = 100, joystick = 200, rightJoystick = 201;
const auto instance = reinterpret_cast<XrInstance>(0x1000);
const auto session = reinterpret_cast<XrSession>(0x2000);
const auto set = reinterpret_cast<XrActionSet>(0x3000);
const auto action = reinterpret_cast<XrAction>(0x4000);
float physicalY = 0;
bool focused = true, mixed = false;

void check(bool success, const char* failure) { if (!success) throw std::runtime_error(failure); }
template<class T, class F> T command(F resolver, const char* name) {
    PFN_xrVoidFunction fn{};
    check(resolver(instance, name, &fn) == XR_SUCCESS && fn, name);
    return reinterpret_cast<T>(fn);
}

XrResult create(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* info, XrInstance* handle) {
    if (info->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    *handle = instance;
    return XR_SUCCESS;
}
XrResult toPath(XrInstance, const char*, XrPath* path) { *path = left; return XR_SUCCESS; }
XrResult createSession(XrInstance, const XrSessionCreateInfo*, XrSession* out) { *out = session; return XR_SUCCESS; }
XrResult createSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) { *out = set; return XR_SUCCESS; }
XrResult createAction(XrActionSet, const XrActionCreateInfo*, XrAction* out) { *out = action; return XR_SUCCESS; }
XrResult destroySession(XrSession) { return XR_SUCCESS; }
XrResult destroyInstance(XrInstance) { return XR_SUCCESS; }
XrResult waitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) { state->predictedDisplayTime = 4321; return XR_SUCCESS; }
XrResult sync(XrSession, const XrActionsSyncInfo*) { return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED; }
XrResult vector(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f* state) {
    state->currentState = {0.42f, physicalY};
    state->lastChangeTime = 42;
    state->changedSinceLastSync = XR_FALSE;
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XrResult enumerate(XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t* count, XrPath* paths) {
    *count = mixed ? 2 : 1;
    paths[0] = joystick;
    if (mixed) paths[1] = rightJoystick;
    return XR_SUCCESS;
}
XrResult pathString(XrInstance, XrPath path, uint32_t capacity, uint32_t* count, char* buffer) {
    const char* value = path == joystick ? "/user/hand/left/input/thumbstick" : "/user/hand/right/input/thumbstick";
    *count = std::strlen(value) + 1;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(buffer, value, *count);
    return XR_SUCCESS;
}
XrResult next(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
    const std::pair<const char*, PFN_xrVoidFunction> commands[] = {
        {"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(toPath)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(createSession)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(destroySession)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(destroyInstance)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(createSet)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(createAction)},
        {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(waitFrame)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(sync)},
        {"xrGetActionStateVector2f", reinterpret_cast<PFN_xrVoidFunction>(vector)},
        {"xrEnumerateBoundSourcesForAction", reinterpret_cast<PFN_xrVoidFunction>(enumerate)},
        {"xrPathToString", reinterpret_cast<PFN_xrVoidFunction>(pathString)},
    };
    for (const auto& [command, address] : commands) {
        if (std::strcmp(command, name) == 0) { *fn = address; return XR_SUCCESS; }
    }
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

void run(const char* library) {
    auto* module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!module) throw std::runtime_error(dlerror());
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(dlsym(module, "xrNegotiateLoaderApiLayerInterface"));
    check(negotiate, "missing negotiation export");
    XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, 1, sizeof(loader), 1, 1,
                                 XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0)};
    XrNegotiateApiLayerRequest layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, 1, sizeof(layer)};
    check(negotiate(&loader, "XR_APILAYER_FRAMESTRIDE_locomotion", &layer) == XR_SUCCESS, "negotiation failed");
    XrApiLayerNextInfo info{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, 1, sizeof(info)};
    info.nextGetInstanceProcAddr = next;
    info.nextCreateApiLayerInstance = create;
    XrApiLayerCreateInfo createInfo{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, 1, sizeof(createInfo)};
    createInfo.nextInfo = &info;
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    XrInstance hInstance{};
    check(layer.createApiLayerInstance(&instanceInfo, &createInfo, &hInstance) == XR_SUCCESS && hInstance == instance, "instance failed");
    const auto get = layer.getInstanceProcAddr;
    XrSession hSession{};
    XrActionSet hSet{};
    XrAction hAction{};
    check(command<PFN_xrCreateSession>(get, "xrCreateSession")(instance, nullptr, &hSession) == XR_SUCCESS, "session failed");
    check(command<PFN_xrCreateActionSet>(get, "xrCreateActionSet")(instance, nullptr, &hSet) == XR_SUCCESS, "set failed");
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strcpy(actionInfo.actionName, "move");
    check(command<PFN_xrCreateAction>(get, "xrCreateAction")(set, &actionInfo, &hAction) == XR_SUCCESS, "action failed");
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    command<PFN_xrWaitFrame>(get, "xrWaitFrame")(session, nullptr, &frame);
    const auto doSync = command<PFN_xrSyncActions>(get, "xrSyncActions");
    const auto read = command<PFN_xrGetActionStateVector2f>(get, "xrGetActionStateVector2f");
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    XrActionStateGetInfo readInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, left};
    XrActionStateVector2f received{XR_TYPE_ACTION_STATE_VECTOR2F};
    Mailbox writer(Channel::Motion, true);
    auto publish = [&](float y, unsigned flags = Ready | Tracking | XrSelected | Override) {
        check(writer.write(y, .13f, flags, clockMilliseconds()), "mailbox publish failed");
    };
    auto query = [&](float expect, bool shouldSync = true) {
        if (shouldSync) doSync(session, &syncInfo);
        check(read(session, &readInfo, &received) == XR_SUCCESS, "game action read failed");
        check(received.currentState.x == .42f, "strafe axis was overwritten");
        check(std::abs(received.currentState.y - expect) < 0.002, "incorrect Y returned to game action");
    };
    publish(.68f);
    query(.68f);
    check(received.changedSinceLastSync && received.lastChangeTime == 4321, "synthetic change metadata was not supplied");
    query(.68f, false);
    check(received.changedSinceLastSync, "same-sync reads disagree about change");
    physicalY = -.40f;
    query(-.40f);
    physicalY = 0;
    focused = false;
    query(0);
    focused = true;
    mixed = true;
    readInfo.subactionPath = XR_NULL_PATH;
    query(0);
    readInfo.subactionPath = left;
    publish(.68f);
    query(.68f);
    publish(0, XrSelected | Tracking);
    query(0);
    publish(.68f, Ready | XrSelected);
    query(0);
    publish(.68f);
    query(.68f);
    std::this_thread::sleep_for(130ms);
    query(0);
    const auto destroyS = command<PFN_xrDestroySession>(get, "xrDestroySession");
    const auto destroyI = command<PFN_xrDestroyInstance>(get, "xrDestroyInstance");
    check(destroyS(session) == XR_SUCCESS && destroyI(instance) == XR_SUCCESS, "layer cleanup failed");
    dlclose(module);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto runtime = (std::filesystem::temp_directory_path() / "framestride-xr.XXXXXX").string();
    if (!mkdtemp(runtime.data())) return 2;
    setenv("FRAMESTRIDE_RUNTIME_DIR", runtime.c_str(), 1);
    setenv("FRAMESTRIDE_XR_ACTION", "move", 1);
    try { run(argv[1]); }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        std::filesystem::remove_all(runtime);
        return 1;
    }
    std::filesystem::remove_all(runtime);
    std::cout << "PASS: OpenXR loader negotiation, game action Y, physical override, X, focus, disable and watchdog\n";
}

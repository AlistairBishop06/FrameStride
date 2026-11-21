#include "core.hpp"
#include "ipc.hpp"

#define XR_NO_PROTOTYPES
#include <openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {
using namespace stride;
using namespace std::chrono_literals;
constexpr char layerName[] = "XR_APILAYER_FRAMESTRIDE_locomotion";

struct Instance {
    XrInstance handle{};
    PFN_xrGetInstanceProcAddr next{};
    XrPath left = XR_NULL_PATH;
    std::string actionName;
    std::unordered_map<XrAction, XrActionSet> actions;

    template<class F> F function(const char* name) const {
        PFN_xrVoidFunction address{};
        return XR_SUCCEEDED(next(handle, name, &address)) ? reinterpret_cast<F>(address) : nullptr;
    }
};

struct Result {
    Axes current{}, prior{};
    XrTime time = 0;
    std::uint64_t serial = 0;
    bool synthesised = false, priorSynthetic = false, known = false;
};

struct Session {
    explicit Session(std::shared_ptr<Instance> owner) : instance(std::move(owner)) {}
    std::shared_ptr<Instance> instance;
    std::unordered_set<XrActionSet> active;
    std::unordered_map<XrAction, std::array<Result, 2>> prior;
    XrTime frameTime = 0;
    std::uint64_t serial = 0, lastSync = 0, lastQuery = 0;
    float lastSynthetic = 0;
    bool live = false;
};

struct Registry {
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<XrInstance, std::shared_ptr<Instance>> instances;
    std::unordered_map<XrSession, std::shared_ptr<Session>> sessions;
    std::unordered_map<XrActionSet, std::shared_ptr<Instance>> sets;
    std::unordered_map<XrAction, std::shared_ptr<Instance>> actions;
    Mailbox motion{Channel::Motion, false};
    bool exiting = false;
    std::thread heartbeat;

    ~Registry() {
        { std::lock_guard lock(mutex); exiting = true; }
        changed.notify_one();
        if (heartbeat.joinable()) heartbeat.join();
    }

    void report() {
        if (heartbeat.joinable()) return;
        heartbeat = std::thread([this] {
            Mailbox status(Channel::OpenXR, true);
            std::unique_lock lock(mutex);
            while (!exiting) {
                const auto now = clockMilliseconds();
                bool registered = false;
                std::uint64_t newest = 0;
                float value = 0;
                for (const auto& [handle, session] : sessions) {
                    registered = registered || session->live;
                    if (session->live && session->lastQuery > newest && now - session->lastQuery < 300) {
                        newest = session->lastQuery;
                        value = session->lastSynthetic;
                    }
                }
                if (registered) {
                    status.write(value, 0, Ready | Submitted | (newest ? Physical : 0), now);
                    changed.wait_for(lock, 100ms);
                } else {
                    status.disconnect();
                    changed.wait(lock);
                }
            }
        });
    }
};

Registry& registry() { static Registry value; return value; }

template<class Map, class Key> auto lookup(Map Registry::*field, Key key) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto& map = r.*field;
    const auto found = map.find(key);
    return found == map.end() ? typename Map::mapped_type{} : found->second;
}

// The registered sources confirm the selected vector2 corresponds to the left thumbstick.
// An aggregate query is eligible only when it has no other bound sources.
bool leftStick(const Session& session, XrSession handle, XrAction action, bool aggregate) {
    const auto& instance = *session.instance;
    auto enumerate = instance.function<PFN_xrEnumerateBoundSourcesForAction>("xrEnumerateBoundSourcesForAction");
    auto pathString = instance.function<PFN_xrPathToString>("xrPathToString");
    if (!enumerate || !pathString) return false;
    XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, action};
    std::array<XrPath, 24> paths{};
    uint32_t size = 0;
    if (XR_FAILED(enumerate(handle, &info, paths.size(), &size, paths.data())) || size > paths.size()) return false;
    bool left = false;
    for (uint32_t i = 0; i < size; ++i) {
        char path[XR_MAX_PATH_LENGTH]{};
        uint32_t length = 0;
        if (XR_FAILED(pathString(instance.handle, paths[i], sizeof(path), &length, path))) return false;
        const std::string_view name(path);
        const bool matches = name == "/user/hand/left/input/thumbstick" || name == "/user/hand/left/input/joystick";
        if (aggregate && !matches) return false;
        left = left || matches;
    }
    return left;
}

XRAPI_ATTR XrResult XRAPI_CALL sessionCreate(XrInstance handle, const XrSessionCreateInfo* info, XrSession* session) {
    const auto instance = lookup(&Registry::instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrCreateSession>("xrCreateSession");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle, info, session);
    if (XR_SUCCEEDED(result) && session) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        r.sessions.emplace(*session, std::make_shared<Session>(instance));
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL sessionDestroy(XrSession handle) {
    const auto session = lookup(&Registry::sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto next = session->instance->function<PFN_xrDestroySession>("xrDestroySession");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle);
    if (XR_SUCCEEDED(result)) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        r.sessions.erase(handle);
        r.changed.notify_one();
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL instanceDestroy(XrInstance handle) {
    const auto instance = lookup(&Registry::instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrDestroyInstance>("xrDestroyInstance");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle);
    if (XR_SUCCEEDED(result)) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        std::erase_if(r.sessions, [&](const auto& pair) { return pair.second->instance == instance; });
        std::erase_if(r.sets, [&](const auto& pair) { return pair.second == instance; });
        std::erase_if(r.actions, [&](const auto& pair) { return pair.second == instance; });
        r.instances.erase(handle);
        r.changed.notify_one();
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL setCreate(XrInstance handle, const XrActionSetCreateInfo* info, XrActionSet* set) {
    const auto instance = lookup(&Registry::instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrCreateActionSet>("xrCreateActionSet");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle, info, set);
    if (XR_SUCCEEDED(result) && set) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        r.sets.emplace(*set, instance);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL setDestroy(XrActionSet handle) {
    const auto instance = lookup(&Registry::sets, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrDestroyActionSet>("xrDestroyActionSet");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle);
    if (XR_SUCCEEDED(result)) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        std::erase_if(instance->actions, [&](const auto& pair) {
            if (pair.second != handle) return false;
            for (auto& [sessionHandle, session] : r.sessions) session->prior.erase(pair.first);
            return true;
        });
        for (auto& [sessionHandle, session] : r.sessions) session->active.erase(handle);
        r.sets.erase(handle);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL actionCreate(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    const auto instance = lookup(&Registry::sets, set);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrCreateAction>("xrCreateAction");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(set, info, action);
    if (XR_SUCCEEDED(result) && action && info) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        if (info->actionType == XR_ACTION_TYPE_VECTOR2F_INPUT && instance->actionName == info->actionName) {
            instance->actions.emplace(*action, set);
        }
        r.actions.emplace(*action, instance);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL actionDestroy(XrAction action) {
    const auto instance = lookup(&Registry::actions, action);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->function<PFN_xrDestroyAction>("xrDestroyAction");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(action);
    if (XR_SUCCEEDED(result)) {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        r.actions.erase(action);
        instance->actions.erase(action);
        for (auto& [handle, session] : r.sessions) session->prior.erase(action);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL waitFrame(XrSession handle, const XrFrameWaitInfo* info, XrFrameState* state) {
    const auto session = lookup(&Registry::sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto next = session->instance->function<PFN_xrWaitFrame>("xrWaitFrame");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle, info, state);
    if (XR_SUCCEEDED(result) && state) {
        std::lock_guard lock(registry().mutex);
        session->frameTime = state->predictedDisplayTime;
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL sync(XrSession handle, const XrActionsSyncInfo* info) {
    const auto session = lookup(&Registry::sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto next = session->instance->function<PFN_xrSyncActions>("xrSyncActions");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle, info);
    std::lock_guard lock(registry().mutex);
    session->active.clear();
    ++session->serial;
    session->lastSync = 0;
    if (result == XR_SUCCESS && info) {
        session->lastSync = clockMilliseconds();
        for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
            const auto& active = info->activeActionSets[i];
            if (active.subactionPath == XR_NULL_PATH || active.subactionPath == session->instance->left)
                session->active.insert(active.actionSet);
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL vectorState(XrSession handle, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
    const auto session = lookup(&Registry::sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto next = session->instance->function<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(handle, info, state);
    if (result != XR_SUCCESS || !state || !info || !state->isActive || session->instance->left == XR_NULL_PATH ||
        (info->subactionPath != XR_NULL_PATH && info->subactionPath != session->instance->left)) return result;

    auto& r = registry();
    std::lock_guard lock(r.mutex);
    const auto action = session->instance->actions.find(info->action);
    if (action == session->instance->actions.end()) return result;
    const bool aggregate = info->subactionPath == XR_NULL_PATH;
    auto& history = session->prior[info->action][aggregate ? 0 : 1];
    const auto now = clockMilliseconds();
    const auto packet = r.motion.read(now);
    const Axes physical{state->currentState.x, state->currentState.y};
    auto output = physical;
    const bool active = session->active.contains(action->second) && session->lastSync && now - session->lastSync < watchdogMs;
    if (active && leftStick(*session, handle, info->action, aggregate)) {
        if (!session->live) r.changed.notify_one();
        session->live = true;
        session->lastQuery = now;
        if (packet.has(Ready) && packet.has(Tracking) && packet.has(XrSelected))
            output = merge(physical, packet.value, packet.has(Override), packet.threshold);
        session->lastSynthetic = output.y == physical.y ? 0 : output.y;
        try { r.report(); } catch (const std::exception&) { output = physical; session->lastSynthetic = 0; }
    }

    if (history.serial != session->serial) {
        history.prior = history.current;
        history.priorSynthetic = history.synthesised;
        history.serial = session->serial;
    }
    const bool synthetic = output.y != physical.y;
    if (synthetic || history.priorSynthetic || history.synthesised) {
        const bool changed = !history.known || output.x != history.prior.x || output.y != history.prior.y;
        if (!history.known || output.x != history.current.x || output.y != history.current.y)
            history.time = std::max(state->lastChangeTime, session->frameTime);
        state->changedSinceLastSync = changed ? XR_TRUE : XR_FALSE;
        state->lastChangeTime = history.time;
        state->currentState = {output.x, output.y};
    }
    history.current = output;
    history.synthesised = synthetic;
    history.known = true;
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL getProc(XrInstance handle, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(&getProc);
        return XR_SUCCESS;
    }
    const auto instance = lookup(&Registry::instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto result = instance->next(handle, name, function);
    if (XR_FAILED(result)) return result;
    const std::pair<std::string_view, PFN_xrVoidFunction> overrides[] = {
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(&instanceDestroy)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(&sessionCreate)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(&sessionDestroy)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(&setCreate)},
        {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(&setDestroy)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(&actionCreate)},
        {"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(&actionDestroy)},
        {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(&waitFrame)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&sync)},
        {"xrGetActionStateVector2f", reinterpret_cast<PFN_xrVoidFunction>(&vectorState)},
    };
    for (const auto& [command, entry] : overrides) {
        if (command == name) { *function = entry; break; }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL create(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer, XrInstance* handle) {
    if (!info || !layer || !handle || !layer->nextInfo || !layer->nextInfo->nextCreateApiLayerInstance ||
        !layer->nextInfo->nextGetInstanceProcAddr) return XR_ERROR_INITIALIZATION_FAILED;
    const auto& next = *layer->nextInfo;
    auto forwarded = *layer;
    forwarded.nextInfo = next.next;
    const auto result = next.nextCreateApiLayerInstance(info, &forwarded, handle);
    if (XR_FAILED(result)) return result;
    auto instance = std::make_shared<Instance>();
    instance->handle = *handle;
    instance->next = next.nextGetInstanceProcAddr;
    const char* action = std::getenv("FRAMESTRIDE_XR_ACTION");
    if (action && *action) {
        instance->actionName = action;
        if (auto toPath = instance->function<PFN_xrStringToPath>("xrStringToPath"))
            toPath(*handle, "/user/hand/left", &instance->left);
    }
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.instances.emplace(*handle, std::move(instance));
    return result;
}
}

extern "C" __attribute__((visibility("default"))) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* name, XrNegotiateApiLayerRequest* request) {
    if (!loader || !request || !name || std::strcmp(name, layerName) != 0 ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || loader->structSize < sizeof(*loader) ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST || request->structSize < sizeof(*request) ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loader->minApiVersion > XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min<XrVersion>(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = &getProc;
    request->createApiLayerInstance = &create;
    return XR_SUCCESS;
}

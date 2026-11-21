#include "legacy_apps.hpp"

#define vr vr_legacy
#include <openvr_legacy.h>
#undef vr

namespace stride::legacy {
namespace {
vr_legacy::IVRApplications& application(void* apps) { return *static_cast<vr_legacy::IVRApplications*>(apps); }
}
int manifest(void* apps, const char* path, bool remove) {
    return remove ? application(apps).RemoveApplicationManifest(path) : application(apps).AddApplicationManifest(path, true);
}
bool installed(void* apps, const char* key) { return application(apps).IsApplicationInstalled(key); }
int identify(void* apps, std::uint32_t pid, const char* key) { return application(apps).IdentifyApplication(pid, key); }
const char* errorName(void* apps, int error) {
    return application(apps).GetApplicationsErrorNameFromEnum(static_cast<vr_legacy::EVRApplicationError>(error));
}
std::uint32_t scene(void* apps) { return application(apps).GetCurrentSceneProcessId(); }
}

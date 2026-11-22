#include "driver_setup.hpp"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <utility>

extern char** environ;

namespace stride {
using Clock = std::chrono::steady_clock;

DriverSetup::DriverSetup(std::filesystem::path script) : script_(std::move(script)) {}

DriverSetup::~DriverSetup() {
    if (child_ < 0) return;
    kill(-child_, SIGKILL);
    while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
}

void DriverSetup::poll(DriverSetupState& state) {
    if (child_ >= 0) {
        int status = 0;
        const auto finished = waitpid(child_, &status, WNOHANG);
        if (finished < 0 && errno == EINTR) return;
        if (finished == 0) {
            if (Clock::now() >= deadline_) {
                kill(-child_, SIGKILL);
                state.error = "SteamVR registration tool timed out. Refresh to check its status.";
            }
            return;
        }
        child_ = -1;
        const auto action = std::exchange(state.inProgress, DriverAction::None);
        const int code = finished > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 0 || (action == DriverAction::Check && code == 3)) {
            state.registration = code == 3 || action == DriverAction::Disable ? DriverRegistration::Unregistered : DriverRegistration::Registered;
            if (action != DriverAction::Check) state.restartRequired = true;
        } else {
            state.registration = DriverRegistration::Unknown;
            if (state.error.empty()) state.error = "Couldn't check or change the driver. See the FrameStride service log.";
            std::cerr << "FrameStride treadmill helper exit status " << code << '\n';
        }
        return;
    }

    const auto action = std::exchange(state.request, DriverAction::None);
    if (action == DriverAction::None) return;
    state.error.clear();
    std::string verb = action == DriverAction::Enable ? "enable" : action == DriverAction::Disable ? "disable" : "status";
    std::string path = script_.string();
    char* argv[] = {path.data(), verb.data(), nullptr};
    posix_spawnattr_t attributes;
    int result = posix_spawnattr_init(&attributes);
    if (result == 0) {
        result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (result == 0) result = posix_spawnattr_setpgroup(&attributes, 0);
        pid_t pid = -1;
        if (result == 0) result = posix_spawn(&pid, path.c_str(), nullptr, &attributes, argv, environ);
        posix_spawnattr_destroy(&attributes);
        if (result == 0) {
            child_ = pid;
            state.inProgress = action;
            deadline_ = Clock::now() + std::chrono::seconds(12);
            return;
        }
    }
    state.error = "Couldn't launch the SteamVR driver setup tool.";
    std::cerr << "FrameStride couldn't start treadmill helper, error " << result << '\n';
}

}

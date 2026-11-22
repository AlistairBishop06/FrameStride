#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace stride {

enum class DriverAction { None, Check, Enable, Disable };
enum class DriverRegistration { Unknown, Unregistered, Registered };

struct DriverSetupState {
    DriverAction request = DriverAction::Check;
    DriverAction inProgress = DriverAction::None;
    DriverRegistration registration = DriverRegistration::Unknown;
    bool restartRequired = false;
    std::string error;

    bool busy() const { return inProgress != DriverAction::None || request != DriverAction::None; }
    bool availableToConnect() const {
        return registration == DriverRegistration::Registered && request != DriverAction::Disable &&
               inProgress != DriverAction::Disable;
    }
};

class DriverSetup {
public:
    explicit DriverSetup(std::filesystem::path script);
    ~DriverSetup();
    DriverSetup(const DriverSetup&) = delete;
    DriverSetup& operator=(const DriverSetup&) = delete;
    void poll(DriverSetupState& state);

private:
    std::filesystem::path script_;
    int child_ = -1;
    std::chrono::steady_clock::time_point deadline_{};
};

}

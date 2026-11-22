#include "dashboard.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace stride;
void require(bool success, const char* failure) { if (!success) throw std::runtime_error(failure); }

void test(Dashboard& dashboard, const char* snapshot) {
    PanelState state;
    auto click = [&](int x, int y) {
        dashboard.pointer(x, y, true, false, state);
        dashboard.pointer(x, y, false, true, state);
    };
    state.driver.request = DriverAction::None;
    state.driver.registration = DriverRegistration::Unregistered;
    click(670, 159);
    require(!state.view.armed, "dashboard enabled locomotion with no driver");
    click(75, 266);
    require(state.driver.request == DriverAction::Enable && !state.driver.availableToConnect(),
            "driver setup did not enqueue registration or allowed input early");
    state.driver.request = DriverAction::None;
    state.driver.registration = DriverRegistration::Registered;
    click(80, 98);
    state.view.backend.flags = Ready | Submitted;
    click(670, 159);
    require(state.view.armed, "dashboard could not enable a connected backend");
    click(720, 40);
    require(!state.view.armed && state.cancelStartup, "emergency stop did not disarm output");
    click(215, 100);
    dashboard.pointer(180, 167, true, false, state);
    dashboard.pointer(440, 167, false, false, state);
    dashboard.pointer(440, 167, false, true, state);
    require(state.settings.sensitivity == 3 && state.changed, "slider drag did not update and persist sensitivity");
    click(385, 100);
    click(55, 277);
    require(state.calibration.active(), "calibration did not start");
    click(55, 277);
    require(!state.calibration.active(), "calibration did not cancel");
    click(490, 100);
    click(750, 364);
    require(state.settings.openxr && !state.view.armed, "switching backends did not disarm output");
    click(90, 435);
    require(!state.settings.openxr && state.settings.sensitivity == 1, "reset did not restore defaults");
    click(83, 98);

    click(690, 165);
    click(748, 99);
    click(77, 266);
    require(state.driver.request == DriverAction::Disable && !state.view.armed && !state.driver.availableToConnect(),
            "driver removal did not disarm locomotion before unregistering");
    click(318, 266);
    require(state.driver.request == DriverAction::Disable, "refresh overwrote a pending removal");
    dashboard.draw(state);
    state.driver.request = DriverAction::None;
    click(318, 266);
    require(state.driver.request == DriverAction::Check, "driver registration status could not be refreshed");
    state.driver.request = DriverAction::None;
    click(83, 98);

    state.view.motion.tracking = true;
    state.view.allowed = true;
    click(690, 165);
    for (int i = 0; i < 160; ++i) {
        state.view.motion.filtered = .017 * std::sin(i * 0.81);
        state.view.motion.confidence = .86;
        state.view.motion.cadence = 1.85;
        state.view.motion.stick = .51f;
        state.view.motion.intensity = .52;
        state.view.sent = .51f;
        dashboard.sample(state.view.motion, i % 8 == 0);
    }
    dashboard.draw(state);
    auto* pixels = dashboard.pixels();
    require(pixels[3] == 255 && pixels[0] == 103, "dashboard frame did not rasterize");
    if (!snapshot) return;
    std::ofstream image(snapshot, std::ios::binary);
    image << "P6\n" << Dashboard::width << ' ' << Dashboard::height << "\n255\n";
    for (int i = 0; i < Dashboard::width * Dashboard::height; ++i)
        image.write(reinterpret_cast<const char*>(pixels + i * 4), 3);
    require(static_cast<bool>(image), "could not write dashboard preview");
}

void setupTest() {
    using namespace std::chrono_literals;
    auto temporary = (std::filesystem::temp_directory_path() / "framestride setup.XXXXXX").string();
    require(mkdtemp(temporary.data()) != nullptr, "could not create test directory");
    const std::filesystem::path root(temporary);
    try {
        const auto helper = root / "treadmill.sh";
        std::ofstream(helper) <<
            "#!/bin/sh\nmarker=\"${0%/*}/registered\"\n"
            "case \"$1\" in\n"
            "  status) [ -f \"$marker\" ] || exit 3;;\n"
            "  enable) touch \"$marker\";;\n"
            "  disable) rm -f \"$marker\";;\n"
            "  *) exit 1;;\n"
            "esac\n";
        std::filesystem::permissions(helper, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
        DriverSetup setup(helper);
        DriverSetupState state;
        auto finish = [&] {
            for (int i = 0; i < 300 && state.busy(); ++i) {
                setup.poll(state);
                if (state.busy()) std::this_thread::sleep_for(5ms);
            }
            require(!state.busy() && state.error.empty(), "driver setup helper failed");
        };
        finish();
        require(state.registration == DriverRegistration::Unregistered && !state.restartRequired,
                "checking the driver changed its registration");
        state.request = DriverAction::Enable;
        finish();
        require(state.registration == DriverRegistration::Registered && state.restartRequired, "enable did not report registration and restart");
        state.request = DriverAction::Disable;
        finish();
        require(state.registration == DriverRegistration::Unregistered && state.restartRequired, "disable did not remove registration");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    try {
        Dashboard dashboard(argv[1]);
        test(dashboard, argc == 3 ? argv[2] : nullptr);
        setupTest();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    std::cout << "PASS: dashboard controls, emergency stop, calibration and asynchronous driver setup\n";
}

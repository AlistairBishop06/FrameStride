#include "dashboard.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace stride;
void require(bool success, const char* failure) { if (!success) throw std::runtime_error(failure); }

void test(Dashboard& dashboard, const char* snapshot) {
    PanelState state;
    auto click = [&](int x, int y) {
        dashboard.pointer(x, y, true, false, state);
        dashboard.pointer(x, y, false, true, state);
    };
    click(670, 159);
    require(!state.view.armed, "dashboard enabled locomotion with no driver");
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
    click(545, 100);
    click(750, 364);
    require(state.settings.openxr && !state.view.armed, "switching backends did not disarm output");
    click(90, 435);
    require(!state.settings.openxr && state.settings.sensitivity == 1, "reset did not restore defaults");
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
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    try {
        Dashboard dashboard(argv[1]);
        test(dashboard, argc == 3 ? argv[2] : nullptr);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    std::cout << "PASS: raster display, controls, enable interlock, emergency stop and calibration controls\n";
}

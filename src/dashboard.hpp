#pragma once

#include "core.hpp"
#include "ipc.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace stride {

struct View {
    Motion motion;
    Packet backend;
    bool armed = false;
    bool allowed = false;
    bool physical = false;
    bool overrideActive = false;
    bool dashboard = false;
    bool scene = false;
    bool delivery = false;
    float sent = 0;
};

struct PanelState {
    Settings settings;
    View view;
    Calibration calibration;
    std::string error;
    bool changed = false;
    bool stop = false;
    bool cancelStartup = false;
};

class Dashboard {
public:
    static constexpr int width = 864;
    static constexpr int height = 544;
    explicit Dashboard(const std::filesystem::path& font);
    ~Dashboard();
    void sample(const Motion& motion, bool step);
    void draw(const PanelState& state);
    void pointer(float x, float y, bool down, bool up, PanelState& state);
    unsigned char* pixels();

private:
    struct Display;
    std::unique_ptr<Display> display_;
};

}

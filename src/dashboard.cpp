#include "dashboard.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace stride {
namespace {

struct Color { unsigned char r, g, b; };
constexpr Color background{12, 17, 23}, panel{23, 32, 41}, border{47, 63, 75};
constexpr Color ink{236, 243, 249}, muted{162, 181, 195}, teal{103, 226, 197};
constexpr Color blue{125, 169, 249}, amber{255, 197, 108}, red{255, 136, 139};

struct Box {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Slider { const char* label; double Settings::*member; double low, high; const char* unit; int precision; };
constexpr Slider movement[] = {
    {"Sensitivity", &Settings::sensitivity, 0.3, 3, "x", 2},
    {"Activation threshold", &Settings::activation, minimumMotionAmplitude, 0.025, " mm", 1},
    {"Walking speed", &Settings::walkSpeed, 0.1, 1, "%", 0},
    {"Running speed", &Settings::runSpeed, 0.1, 1, "%", 0},
    {"Maximum output", &Settings::maximum, 0.1, 1, "%", 0},
    {"Minimum output", &Settings::minimum, 0, 0.45, "%", 0},
    {"Acceleration", &Settings::accelerate, 0.025, 0.45, " ms", 0},
    {"Deceleration", &Settings::decelerate, 0.015, 0.12, " ms", 0},
    {"Stop limit", &Settings::stopLatency, 0.16, 0.28, " ms", 0},
    {"Running threshold", &Settings::runningThreshold, 0.45, 1.4, "x", 2},
    {"Response curve", &Settings::curve, 0.4, 2.8, "x", 2},
    {"Deadzone", &Settings::deadzone, 0, 0.2, "%", 0},
};
constexpr Slider tracking[] = {
    {"Detection confidence", &Settings::confidence, 0.45, 0.95, "%", 0},
    {"Physical Y override threshold", &Settings::overrideThreshold, 0.05, 0.5, "%", 0},
};
struct Toggle { const char* label; bool Settings::*member; };
constexpr Toggle general[] = {
    {"Enable locomotion when FrameStride opens", &Settings::startup}, {"Only enable output with a game running", &Settings::gameOnly},
    {"Pause when the SteamVR dashboard is open", &Settings::pauseDashboard},
    {"Physical stick Y takes priority", &Settings::physicalOverride},
    {"Emergency stop with left grip + stick click", &Settings::controllerStop},
    {"Use the opt-in OpenXR action layer", &Settings::openxr},
};

constexpr Box emergency{664, 25, 172, 48};
constexpr Box enable{623, 135, 211, 58};
constexpr Box calibration{31, 251, 253, 51};
constexpr Box resetCalibration{300, 251, 207, 51};
constexpr Box defaults{31, 412, 213, 46};
constexpr Box quit{648, 412, 186, 46};
constexpr Box driverChange{31, 244, 245, 51};
constexpr Box driverRefresh{294, 244, 185, 51};
constexpr const char* tabs[] = {"Overview", "Movement", "Tracking", "Settings", "Input Test", "Driver"};
constexpr int driverTab = 5;

Box tabBox(int tab) { return {30 + tab * 136, 81, 124, 38}; }

Box sliderBox(int tab, unsigned index) {
    return tab == 1 ? Box{32 + static_cast<int>(index % 2) * 414, 132 + static_cast<int>(index / 2) * 59, 388, 52}
                    : Box{32 + static_cast<int>(index) * 414, 171, 388, 52};
}

std::string number(double value, int precision = 0) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

bool canUseBackend(const PanelState& s) {
    return (s.settings.openxr || s.driver.availableToConnect()) &&
           s.view.backend.has(Ready) && s.view.backend.has(Submitted);
}

std::string status(const PanelState& s) {
    const auto& v = s.view;
    if (s.calibration.active()) return "Calibrating";
    if (!v.motion.tracking) return "Tracking unavailable";
    if (!s.settings.openxr && s.driver.registration == DriverRegistration::Unregistered) return "Treadmill driver disabled";
    if (!v.backend.has(Ready)) return s.settings.openxr ? "Waiting for a configured OpenXR game" : "SteamVR driver unavailable";
    if (!v.backend.has(Submitted)) return "Input updates unavailable";
    if (!v.armed) return "Locomotion disabled";
    if (v.dashboard && s.settings.pauseDashboard) return "Paused in dashboard";
    if (s.settings.gameOnly && !v.scene) return "Waiting for a game";
    if (!s.settings.openxr && s.settings.physicalOverride && !v.physical) return "Waiting for left stick input";
    if (v.overrideActive) return "Physical stick has priority";
    if (v.sent > 0) return "Walking";
    return "Ready to walk";
}

std::string driverStatus(const PanelState& state) {
    const auto& driver = state.driver;
    const auto action = driver.inProgress == DriverAction::None ? driver.request : driver.inProgress;
    if (action == DriverAction::Enable) return "Registering the treadmill driver...";
    if (action == DriverAction::Disable) return "Removing the treadmill driver...";
    if (action == DriverAction::Check) return "Checking SteamVR registration...";
    if (driver.registration == DriverRegistration::Unknown) return "Registration status unavailable";
    if (driver.registration == DriverRegistration::Unregistered) return "Driver disabled";
    if (!state.settings.openxr && canUseBackend(state)) return "Driver connected";
    return "Driver registered";
}
}

struct Dashboard::Display {
    struct Glyph { int x, y, width, height; float advance; std::vector<unsigned char> mask; };
    explicit Display(const std::filesystem::path& path) : bitmap(width * height * 4) {
        std::ifstream file(path, std::ios::binary);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
        stbtt_fontinfo font{};
        if (bytes.empty() || !stbtt_InitFont(&font, bytes.data(), 0)) throw std::runtime_error("Dashboard font is missing or unreadable");
        for (int size : {16, 18, 20, 24, 32, 37}) {
            auto& alphabet = fonts[size];
            const float scale = stbtt_ScaleForMappingEmToPixels(&font, static_cast<float>(size));
            for (int c = 32; c < 127; ++c) {
                Glyph glyph{};
                int advance = 0;
                stbtt_GetCodepointHMetrics(&font, c, &advance, nullptr);
                glyph.advance = advance * scale;
                auto* mask = stbtt_GetCodepointBitmap(&font, scale, scale, c, &glyph.width, &glyph.height, &glyph.x, &glyph.y);
                if (mask) {
                    glyph.mask.assign(mask, mask + glyph.width * glyph.height);
                    stbtt_FreeBitmap(mask, nullptr);
                }
                alphabet.push_back(std::move(glyph));
            }
        }
    }

    void pixel(int x, int y, Color color, unsigned char alpha = 255) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        auto* px = &bitmap[4 * (y * width + x)];
        px[0] = static_cast<unsigned char>((px[0] * (255 - alpha) + color.r * alpha) / 255);
        px[1] = static_cast<unsigned char>((px[1] * (255 - alpha) + color.g * alpha) / 255);
        px[2] = static_cast<unsigned char>((px[2] * (255 - alpha) + color.b * alpha) / 255);
        px[3] = 255;
    }

    void rect(Box box, Color color) {
        for (int y = box.y; y < box.y + box.h; ++y) {
            for (int x = box.x; x < box.x + box.w; ++x) pixel(x, y, color);
        }
    }

    void label(int x, int baseline, const std::string& text, int size = 18, Color color = ink, int right = width - 22) {
        float pen = static_cast<float>(x);
        for (unsigned char c : text) {
            const auto& glyph = fonts.at(size)[c >= 32 && c < 127 ? c - 32 : '?' - 32];
            if (pen + glyph.advance > right) break;
            for (int y = 0; y < glyph.height; ++y) {
                for (int col = 0; col < glyph.width; ++col) {
                    pixel(static_cast<int>(pen) + glyph.x + col, baseline + glyph.y + y, color,
                          glyph.mask[y * glyph.width + col]);
                }
            }
            pen += glyph.advance;
        }
    }

    void button(Box box, const std::string& title, Color color = border, Color text = ink) {
        rect(box, color);
        label(box.x + 13, box.y + box.h / 2 + 7, title, 18, text, box.x + box.w - 5);
    }

    void slider(const Slider& control, Box box, const Settings& settings) {
        const double value = settings.*control.member;
        const double multiplier = control.unit[0] == '%' ? 100 : (control.unit[0] == ' ' ? 1000 : 1);
        label(box.x, box.y + 17, control.label, 18);
        label(box.x + box.w - 76, box.y + 17, number(value * multiplier, control.precision) + control.unit, 16, teal);
        const int fill = static_cast<int>(std::clamp((value - control.low) / (control.high - control.low), 0.0, 1.0) * (box.w - 18));
        rect({box.x, box.y + 34, box.w - 10, 4}, border);
        rect({box.x, box.y + 34, fill, 4}, teal);
        rect({box.x + fill, box.y + 29, 8, 14}, teal);
    }

    void line(int x0, int y0, int x1, int y1, Color color) {
        const int length = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        if (!length) { pixel(x0, y0, color); return; }
        for (int i = 0; i <= length; ++i) {
            const int x = x0 + i * (x1 - x0) / length;
            const int y = y0 + i * (y1 - y0) / length;
            pixel(x, y, color);
            pixel(x, y + 1, color);
        }
    }

    void graph(const View& view, Box box) {
        rect(box, panel);
        label(box.x + 15, box.y + 25, "Displacement", 16, blue);
        label(box.x + 162, box.y + 25, "Steps", 16, amber);
        label(box.x + 242, box.y + 25, "Confidence", 16, muted);
        label(box.x + 373, box.y + 25, "Stick preview", 16, teal);
        const int left = box.x + 16, top = box.y + 41, plotW = box.w - 32, plotH = box.h - 57;
        for (int i = 0; i <= 4; ++i) rect({left, top + i * plotH / 4, plotW, 1}, border);
        if (!view.motion.tracking) {
            label(left + 13, top + plotH / 2, "Tracking unavailable", 24, amber);
            return;
        }
        const int first = static_cast<int>(history.size() - count);
        auto xy = [&](int i, double v) {
            return std::pair<int, int>{left + plotW * (first + i) / static_cast<int>(history.size()),
                top + static_cast<int>((1 - std::clamp(v, 0.0, 1.0)) * (plotH - 1))};
        };
        for (unsigned i = 1; i < count; ++i) {
            const auto& a = history[(cursor + history.size() - count + i - 1) % history.size()];
            const auto& b = history[(cursor + history.size() - count + i) % history.size()];
            if (!a.tracking || !b.tracking) continue;
            auto plot = [&](double va, double vb, Color color) {
                const auto [x0, y0] = xy(i - 1, va);
                const auto [x1, y1] = xy(i, vb);
                line(x0, y0, x1, y1, color);
            };
            if (b.step) {
                const auto [x, y] = xy(i, 1);
                rect({x, y, 2, 8}, amber);
            }
            plot(a.filtered * 12 + 0.5, b.filtered * 12 + 0.5, blue);
            plot(a.confidence, b.confidence, muted);
            plot(a.stick, b.stick, teal);
        }
    }

    std::vector<unsigned char> bitmap;
    std::map<int, std::vector<Glyph>> fonts;
    std::array<Motion, 160> history{};
    unsigned cursor = 0, count = 0;
    int tab = 0, activeSlider = -1;
};

Dashboard::Dashboard(const std::filesystem::path& path) : display_(std::make_unique<Display>(path)) {}
Dashboard::~Dashboard() = default;
unsigned char* Dashboard::pixels() { return display_->bitmap.data(); }

void Dashboard::sample(const Motion& motion, bool step) {
    auto& d = *display_;
    d.history[d.cursor] = motion;
    d.history[d.cursor].step = step;
    d.cursor = (d.cursor + 1) % d.history.size();
    d.count = std::min<unsigned>(d.count + 1, d.history.size());
}

void Dashboard::draw(const PanelState& state) {
    auto& d = *display_;
    const auto& motion = state.view.motion;
    d.rect({0, 0, width, height}, background);
    d.rect({0, 0, width, 4}, teal);
    d.label(30, 53, "FrameStride", 37);
    d.label(261, 52, FRAMESTRIDE_VERSION, 16, muted);
    d.button(emergency, "Emergency stop", {86, 37, 47}, red);
    for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
        const auto box = tabBox(i);
        d.button(box, tabs[i], d.tab == i ? panel : background, d.tab == i ? teal : muted);
        if (d.tab == i) d.rect({box.x, box.y + box.h - 2, box.w, 2}, teal);
    }
    d.rect({30, 497, 804, 1}, border);
    const auto backend = state.settings.openxr ? "OpenXR action layer" : "SteamVR treadmill";
    const auto footer = state.error.empty() ? std::string(backend) + "   /   " + status(state) : state.error;
    d.label(31, 526, footer, 16, state.error.empty() ? muted : amber);

    if (d.tab == 0) {
        d.label(31, 168, status(state), 32, state.view.armed ? teal : ink, 605);
        d.label(32, 193, state.settings.calibrated ? "Headset tracking calibrated" : "Calibrate in Tracking for a personal response", 16, muted);
        const bool connected = canUseBackend(state);
        const bool driverSetup = !connected && !state.settings.openxr;
        d.button(enable, state.view.armed ? "Disable locomotion" : connected ? "Enable locomotion" : driverSetup ? "Driver setup" : "Backend unavailable",
                 state.view.armed || driverSetup ? border : connected ? teal : panel, connected && !state.view.armed ? background : ink);
        const std::array<std::string, 4> values{number(motion.cadence * 60) + " spm", number(motion.confidence * 100) + "%",
            number(motion.intensity * 100) + "%", number(state.view.sent * 100) + "%"};
        const char* metrics[] = {"Cadence", "Confidence", "Intensity", "Output Y"};
        for (int i = 0; i < 4; ++i) {
            const int x = 31 + i * 205;
            d.rect({x, 211, 189, 75}, panel);
            d.label(x + 14, 237, metrics[i], 16, muted);
            d.label(x + 14, 272, values[i], 24, i == 3 ? teal : ink);
        }
        d.graph(state.view, {31, 301, 803, 183});
    } else if (d.tab == 1) {
        for (unsigned i = 0; i < std::size(movement); ++i) d.slider(movement[i], sliderBox(1, i), state.settings);
    } else if (d.tab == 2) {
        d.label(31, 155, "Headset-only motion tracking", 24);
        for (unsigned i = 0; i < std::size(tracking); ++i) d.slider(tracking[i], sliderBox(2, i), state.settings);
        d.button(calibration, state.calibration.active() ? "Cancel calibration" : "Start calibration", teal, background);
        d.button(resetCalibration, state.calibration.stage() == Calibration::Stage::Jog ? "Skip jogging" : "Reset calibration");
        const auto normal = state.settings.calibrated ? "Calibration is saved. Walk to check the preview." : "Stand still, then walk in place. Jogging is optional.";
        d.label(31, 339, state.calibration.message().empty() ? normal : state.calibration.message(), 20);
        if (state.calibration.active()) {
            std::string progress = number(std::ceil(state.calibration.remaining(clockSeconds()))) + " seconds remaining";
            progress += motion.tracking ? "   /   Tracking active" : "   /   Tracking unavailable";
            if (motion.tracking && motion.cadence > 0) progress += "   /   " + number(motion.cadence * 60) + " spm";
            d.label(31, 373, progress, 20, motion.tracking ? teal : amber);
        }
        d.label(31, 425, "Live stick preview   " + number(motion.stick * 100) + "%", 24, teal);
        d.rect({31, 445, 803, 8}, border);
        d.rect({31, 445, static_cast<int>(803 * motion.stick), 8}, teal);
    } else if (d.tab == 3) {
        for (unsigned i = 0; i < std::size(general); ++i) {
            const int y = 136 + static_cast<int>(i) * 43;
            const bool active = state.settings.*general[i].member;
            d.rect({31, y, 803, 37}, panel);
            d.label(44, y + 25, general[i].label, 20);
            d.button({737, y + 2, 86, 33}, active ? "On" : "Off", active ? teal : border, active ? background : ink);
        }
        d.button(defaults, "Reset to defaults");
        d.button(quit, "Stop application", {86, 37, 47}, red);
    } else if (d.tab == 4) {
        d.label(31, 152, std::string(backend) + (state.view.backend.has(Ready) ? " connected" : " disconnected"), 24);
        d.label(31, 183, "HMD Y  " + (motion.tracking ? number(motion.raw, 3) + " m" : "unavailable") + "     Filtered  " +
                         number(motion.filtered * 1000, 1) + " mm     Target Y  " + number(motion.stick * 100) + "%", 18);
        d.label(31, 212, "Submitted Y  " + number(state.view.backend.value * 100) + "%     Update  " +
                         (state.view.backend.timestamp ? number(clockMilliseconds() - state.view.backend.timestamp) + " ms ago" : "unavailable"), 18);
        d.label(31, 241, state.view.backend.has(Submitted) ? "Last update accepted by the selected backend" : "Waiting for a successful backend update", 18);
        d.label(31, 269, state.view.delivery ? "OpenXR game queried this action. Verify movement in the game." :
                   "Gameplay reception requires an in-game check.", 16, muted);
        d.graph(state.view, {31, 287, 803, 197});
    } else {
        const auto& driver = state.driver;
        const bool registered = driver.registration == DriverRegistration::Registered;
        const bool live = !state.settings.openxr && canUseBackend(state);
        d.label(31, 159, "SteamVR treadmill driver", 32);
        d.label(31, 196, driverStatus(state), 20, live ? teal : muted);
        if (driver.busy() || driver.registration != DriverRegistration::Unknown) {
            d.button(driverChange, driver.busy() ? "Working..." : registered ? "Disable driver" : "Enable driver",
                     driver.busy() ? panel : registered ? border : teal, driver.busy() || registered ? ink : background);
        }
        d.button(driverRefresh, "Refresh status", driver.busy() ? panel : border, driver.busy() ? muted : ink);
        if (!driver.error.empty()) {
            d.label(31, 338, driver.error, 18, amber);
        } else if (driver.restartRequired || (registered && !live && !state.settings.openxr)) {
            d.label(31, 338, "Restart SteamVR, then reopen FrameStride from the + menu.", 18, amber);
        }
        d.label(31, 385, "SteamVR loads a registered driver when it restarts.", 18, muted);
        d.label(31, 415, "FrameStride itself opens from the + menu.", 18, muted);
        d.label(31, 445, "Tracking calibration is available with the driver disabled.", 18, muted);
    }
}

void Dashboard::pointer(float x, float y, bool down, bool up, PanelState& state) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    auto& d = *display_;
    const auto ix = static_cast<int>(x), iy = static_cast<int>(y);
    auto setSlider = [&](int i) {
        const auto& control = d.tab == 1 ? movement[i] : tracking[i];
        const auto box = sliderBox(d.tab, i);
        state.settings.*control.member = control.low + (control.high - control.low) *
            std::clamp((x - box.x) / (box.w - 18), 0.0f, 1.0f);
        state.settings.minimum = std::min(state.settings.minimum, state.settings.maximum);
        state.settings.deadzone = std::min(state.settings.deadzone, state.settings.maximum * 0.5);
        state.changed = true;
    };
    if (d.activeSlider >= 0) {
        if (up) d.activeSlider = -1;
        else setSlider(d.activeSlider);
        return;
    }
    if (!down) return;
    if (emergency.contains(ix, iy)) {
        state.view.armed = false;
        state.calibration.cancel();
        state.cancelStartup = true;
        return;
    }
    for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
        if (tabBox(i).contains(ix, iy)) { d.tab = i; return; }
    }
    if (d.tab == 0 && enable.contains(ix, iy)) {
        if (state.view.armed || (!state.calibration.active() && canUseBackend(state))) {
            state.view.armed = !state.view.armed;
        } else if (!state.settings.openxr && !canUseBackend(state)) {
            d.tab = driverTab;
        }
    } else if (d.tab == 1 || d.tab == 2) {
        const unsigned count = d.tab == 1 ? std::size(movement) : std::size(tracking);
        for (unsigned i = 0; i < count; ++i) {
            if (!sliderBox(d.tab, i).contains(ix, iy)) continue;
            d.activeSlider = i;
            setSlider(i);
            return;
        }
        if (d.tab == 2 && calibration.contains(ix, iy)) {
            state.view.armed = false;
            if (state.calibration.active()) state.calibration.cancel();
            else state.calibration.start(clockSeconds());
        }
        if (d.tab == 2 && resetCalibration.contains(ix, iy)) {
            state.view.armed = false;
            if (state.calibration.stage() == Calibration::Stage::Jog) {
                state.calibration.finishWithoutJog(state.settings);
                state.changed = true;
                return;
            }
            state.calibration.cancel();
            const Settings standard;
            state.settings.calibrated = false;
            state.settings.activation = standard.activation;
            state.settings.noise = standard.noise;
            state.settings.walkAmplitude = standard.walkAmplitude;
            state.settings.walkCadence = standard.walkCadence;
            state.settings.runAmplitude = standard.runAmplitude;
            state.settings.runCadence = standard.runCadence;
            state.changed = true;
        }
    } else if (d.tab == 3) {
        for (unsigned i = 0; i < std::size(general); ++i) {
            if (!Box{31, 136 + static_cast<int>(i) * 43, 803, 37}.contains(ix, iy)) continue;
            auto& value = state.settings.*general[i].member;
            value = !value;
            if (general[i].member == &Settings::openxr) state.view.armed = false;
            state.changed = true;
        }
        if (defaults.contains(ix, iy)) {
            state.view.armed = false;
            state.calibration.cancel();
            state.settings = {};
            state.changed = true;
        }
        if (quit.contains(ix, iy)) state.stop = true;
    } else if (d.tab == driverTab && !state.driver.busy()) {
        if (driverRefresh.contains(ix, iy)) {
            state.driver.request = DriverAction::Check;
        } else if (driverChange.contains(ix, iy) && state.driver.registration != DriverRegistration::Unknown) {
            state.view.armed = false;
            state.cancelStartup = true;
            state.driver.request = state.driver.registration == DriverRegistration::Registered ? DriverAction::Disable : DriverAction::Enable;
        }
    }
}

}

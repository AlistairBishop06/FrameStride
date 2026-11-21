#include "core.hpp"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace stride {
namespace {
using Json = nlohmann::json;
struct Number { const char* key; double Settings::*field; double lo, hi; };
constexpr Number numbers[] = {
    {"sensitivity", &Settings::sensitivity, 0.3, 3},
    {"activation_m", &Settings::activation, 0.0015, 0.025},
    {"walk_speed", &Settings::walkSpeed, 0.1, 1}, {"run_speed", &Settings::runSpeed, 0.1, 1},
    {"minimum_output", &Settings::minimum, 0, 0.45}, {"maximum_output", &Settings::maximum, 0.1, 1},
    {"response_curve", &Settings::curve, 0.4, 2.8}, {"acceleration_s", &Settings::accelerate, 0.025, 0.45},
    {"deceleration_s", &Settings::decelerate, 0.015, 0.12}, {"stop_latency_s", &Settings::stopLatency, 0.16, 0.28},
    {"running_threshold", &Settings::runningThreshold, 0.45, 1.4}, {"deadzone", &Settings::deadzone, 0, 0.2},
    {"confidence", &Settings::confidence, 0.45, 0.95}, {"override_threshold", &Settings::overrideThreshold, 0.05, 0.5},
    {"noise_m", &Settings::noise, 0.00015, 0.003}, {"walk_amplitude_m", &Settings::walkAmplitude, 0.0015, 0.06},
    {"walk_cadence_hz", &Settings::walkCadence, 0.8, 3.2}, {"run_amplitude_m", &Settings::runAmplitude, 0.0015, 0.074},
    {"run_cadence_hz", &Settings::runCadence, 0.8, 3.2},
};
struct Toggle { const char* key; bool Settings::*field; };
constexpr Toggle toggles[] = {
    {"enable_on_startup", &Settings::startup}, {"game_only", &Settings::gameOnly},
    {"pause_dashboard", &Settings::pauseDashboard}, {"physical_override", &Settings::physicalOverride},
    {"controller_stop_shortcut", &Settings::controllerStop},
    {"openxr_backend", &Settings::openxr}, {"calibrated", &Settings::calibrated},
};
}

Settings loadSettings(const std::filesystem::path& path, std::string& error) {
    error.clear();
    Settings result;
    try {
        std::ifstream input(path);
        if (!input && !std::filesystem::exists(path)) return result;
        const auto data = Json::parse(input);
        if (!data.is_object() || data.value("schema", 0) != 1) throw std::runtime_error("unsupported schema");
        for (const auto& number : numbers) {
            const auto it = data.find(number.key);
            if (it == data.end() || !it->is_number()) continue;
            const double value = it->get<double>();
            if (std::isfinite(value)) result.*number.field = std::clamp(value, number.lo, number.hi);
        }
        for (const auto& toggle : toggles) {
            const auto it = data.find(toggle.key);
            if (it != data.end() && it->is_boolean()) result.*toggle.field = it->get<bool>();
        }
        result.minimum = std::min(result.minimum, result.maximum);
        result.deadzone = std::min(result.deadzone, result.maximum * 0.5);
    } catch (const std::exception& ex) {
        error = std::string("Settings could not be read. Using defaults. ") + ex.what();
        return {};
    }
    return result;
}

bool saveSettings(const std::filesystem::path& path, const Settings& settings, std::string& error) {
    error.clear();
    const std::filesystem::path temporary = path.string() + ".tmp";
    try {
        Json data = {{"schema", 1}};
        for (const auto& number : numbers) data[number.key] = settings.*number.field;
        for (const auto& toggle : toggles) data[toggle.key] = settings.*toggle.field;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(temporary, std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << data.dump(2) << '\n';
        output.close();
        std::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Settings could not be saved. ") + ex.what();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

}

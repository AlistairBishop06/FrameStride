#pragma once

#include <array>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace stride {

inline constexpr double minimumMotionAmplitude = 0.0012;

struct Settings {
    double sensitivity = 1.0;
    double activation = 0.0035;
    double walkSpeed = 0.50;
    double runSpeed = 1.0;
    double minimum = 0.20;
    double maximum = 1.0;
    double curve = 1.0;
    double accelerate = 0.13;
    double decelerate = 0.055;
    double stopLatency = 0.27;
    double runningThreshold = 0.90;
    double deadzone = 0.04;
    double confidence = 0.62;
    double overrideThreshold = 0.13;
    bool startup = false;
    bool gameOnly = true;
    bool pauseDashboard = true;
    bool physicalOverride = true;
    bool controllerStop = false;
    bool openxr = false;
    bool calibrated = false;
    double noise = 0.0007;
    double walkAmplitude = 0.012;
    double walkCadence = 1.8;
    double runAmplitude = 0.026;
    double runCadence = 2.8;
};

Settings loadSettings(const std::filesystem::path& path, std::string& error);
bool saveSettings(const std::filesystem::path& path, const Settings& settings, std::string& error);

struct Pose {
    double time = 0;
    double height = 0;
    double pitch = 0;
    bool valid = false;
};

struct Motion {
    double raw = 0;
    double filtered = 0;
    double cadence = 0;
    double amplitude = 0;
    double confidence = 0;
    double intensity = 0;
    float stick = 0;
    bool tracking = false;
    bool step = false;
};

class Detector {
public:
    Motion update(const Pose& pose, const Settings& settings);
    void reset();

private:
    struct Peak { double time, amplitude; };
    std::deque<Pose> recent_;
    std::deque<Peak> peaks_;
    double lastTime_ = 0;
    double lastHeight_ = 0;
    double slow_ = 0;
    double signal_ = 0;
    double lastRise_ = 0;
    double valley_ = 0;
    double holdUntil_ = 0;
    float stick_ = 0;
    bool primed_ = false;
    bool rising_ = false;
    bool walking_ = false;
};

class Calibration {
public:
    enum class Stage { Idle, Still, Walk, Jog, Complete, Failed };
    void start(double now);
    void cancel();
    void update(double now, const Motion& motion, Settings& settings);
    void finishWithoutJog(Settings& settings);
    Motion measure(Detector& detector, const Pose& pose, Settings settings) const;
    bool active() const;
    double remaining(double now) const;
    Stage stage() const { return stage_; }
    const std::string& message() const { return message_; }

private:
    void fail(std::string message);
    void beginStage(Stage stage, double now, std::string message);
    void save(Settings& settings, double runAmplitude, double runCadence, std::string message);
    Stage stage_ = Stage::Idle;
    double since_ = 0;
    double lastUpdate_ = 0;
    double lastSample_ = 0;
    double trackedSeconds_ = 0;
    double rhythmicSeconds_ = 0;
    double noise_ = 0;
    double walkAmplitude_ = 0;
    double walkCadence_ = 0;
    unsigned rhythmicSteps_ = 0;
    std::vector<double> amplitudes_, cadences_;
    std::string message_;
};

struct Axes { float x = 0; float y = 0; };
Axes merge(Axes physical, float forward, bool physicalOverride, float threshold);
double clockSeconds();

}

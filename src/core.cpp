#include "core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <utility>

namespace stride {
namespace {
constexpr double pi = 3.141592653589793;
constexpr double minimumPeriod = 1.0 / 3.2;
constexpr double maximumPeriod = 1.0 / 0.8;

double follow(double previous, double target, double timeConstant, double dt) {
    return previous + (target - previous) * (-std::expm1(-dt / timeConstant));
}

double median(std::vector<double> values) {
    auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

double effort(double amplitude, double cadence) {
    return std::pow(amplitude, 0.65) * std::pow(cadence, 1.35);
}

double duration(Calibration::Stage stage) {
    return stage == Calibration::Stage::Walk ? 10.0 : 5.0;
}
}

double clockSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Detector::reset() {
    recent_.clear();
    peaks_.clear();
    lastTime_ = lastHeight_ = slow_ = signal_ = lastRise_ = valley_ = holdUntil_ = 0;
    stick_ = 0;
    primed_ = rising_ = walking_ = false;
}

Motion Detector::update(const Pose& pose, const Settings& settings) {
    Motion motion;
    if (!pose.valid || !std::isfinite(pose.height) || !std::isfinite(pose.time) ||
        !std::isfinite(pose.pitch)) {
        reset();
        return motion;
    }
    motion.tracking = true;
    motion.raw = pose.height;
    const double dt = pose.time - lastTime_;
    const double heightJump = std::abs(pose.height - lastHeight_);
    if (!primed_ || dt <= 0 || dt > 0.09 || heightJump > 0.065 ||
        (heightJump > 0.02 && heightJump / dt > 1.65)) {
        const bool discontinuity = primed_;
        reset();
        primed_ = true;
        slow_ = lastHeight_ = pose.height;
        lastTime_ = pose.time;
        holdUntil_ = pose.time + (discontinuity ? 0.5 : 0.0);
        return motion;
    }
    lastTime_ = pose.time;
    lastHeight_ = pose.height;

    // High-pass at 0.55 Hz followed by a 4.6 Hz low-pass. Filter using the actual sample interval.
    slow_ = follow(slow_, pose.height, 1 / (2 * pi * 0.55), dt);
    const double previous = signal_;
    signal_ = follow(signal_, pose.height - slow_, 1 / (2 * pi * 4.6), dt);
    motion.filtered = signal_;

    if (std::abs(pose.pitch) > 0.56 || std::abs(signal_) > 0.13) {
        holdUntil_ = pose.time + 0.45;
    }
    if (pose.time < holdUntil_) {
        peaks_.clear();
        recent_.clear();
        walking_ = rising_ = false;
        valley_ = signal_;
        stick_ = 0;
        return motion;
    }

    recent_.push_back(pose);
    const double window = std::clamp(settings.stopLatency - 0.055, 0.105, 0.225);
    while (recent_.size() > 1 && pose.time - recent_[1].time >= window) recent_.pop_front();
    const auto [low, high] = std::minmax_element(recent_.begin(), recent_.end(),
        [](const Pose& a, const Pose& b) { return a.height < b.height; });
    const double range = high->height - low->height;

    if (signal_ > previous + 0.000005) {
        if (!rising_) lastRise_ = pose.time;
        rising_ = true;
    } else if (signal_ < previous - 0.000005) {
        const double peakTime = pose.time - dt;
        const double amplitude = (previous - valley_) / 2;
        const double gate = std::max(settings.activation, 3.0 * settings.noise);
        if (rising_ && peakTime - lastRise_ >= 0.074 && previous > gate * 0.55 &&
            amplitude >= gate && amplitude < 0.075) {
            const double period = peaks_.empty() ? 0 : peakTime - peaks_.back().time;
            if (period > maximumPeriod * 1.04) peaks_.clear();
            if (peaks_.empty() || period >= minimumPeriod * 0.96) {
                peaks_.push_back({peakTime, amplitude});
                if (peaks_.size() > 5) peaks_.pop_front();
                motion.step = true;
            }
        }
        if (rising_) valley_ = signal_;
        rising_ = false;
        valley_ = std::min(valley_, signal_);
    }

    if (!peaks_.empty() && pose.time - peaks_.back().time > maximumPeriod * 1.10) peaks_.clear();
    if (peaks_.size() >= 3) {
        std::array<double, 4> periods{};
        std::size_t n = 0;
        for (std::size_t i = 1; i < peaks_.size(); ++i) periods[n++] = peaks_[i].time - peaks_[i - 1].time;
        const double mean = std::accumulate(periods.begin(), periods.begin() + n, 0.0) / n;
        double deviation = 0;
        double amplitude = 0;
        double minAmplitude = 1;
        double maxAmplitude = 0;
        for (std::size_t i = 0; i < n; ++i) deviation += std::abs(periods[i] - mean) / n;
        for (const auto& peak : peaks_) {
            amplitude += peak.amplitude / peaks_.size();
            minAmplitude = std::min(minAmplitude, peak.amplitude);
            maxAmplitude = std::max(maxAmplitude, peak.amplitude);
        }
        motion.cadence = 1 / mean;
        motion.amplitude = amplitude;
        const double rhythm = std::clamp(1 - 2.6 * deviation / mean, 0.0, 1.0);
        const double consistency = std::sqrt(minAmplitude / maxAmplitude);
        const double age = pose.time - peaks_.back().time;
        const double recentStep = std::clamp(1 - (age / mean - 1.02) * 2.0, 0.0, 1.0);
        const double energy = std::clamp((amplitude / std::max(settings.activation, 3 * settings.noise) - 0.75) * 1.8,
                                         0.0, 1.0);
        motion.confidence = rhythm * recentStep * (0.54 + 0.24 * consistency + 0.22 * energy);
    }

    // At a step's apex the shortest recent height range is set by the cadence and
    // the window around that apex. Keep the stop gate below this expected turn.
    const double turnRange = motion.amplitude * (1 - std::cos(pi * motion.cadence * window));
    const double stillThreshold = std::max(0.00045, std::min(1.8 * settings.noise, 0.73 * turnRange));
    if (range < stillThreshold && recent_.size() > 1 && pose.time - recent_.front().time >= window) {
        motion.confidence = 0;
        if (walking_) peaks_.clear();
        walking_ = false;
        stick_ = 0;
        return motion;
    }

    walking_ = motion.confidence >= settings.confidence * (walking_ ? 0.76 : 1.0);
    double target = 0;
    if (walking_) {
        const double walk = effort(settings.walkAmplitude, settings.walkCadence);
        const double run = std::max(effort(settings.runAmplitude, settings.runCadence), walk * 1.22);
        const double current = effort(motion.amplitude, motion.cadence) * settings.sensitivity;
        const double fromWalk = std::pow(std::clamp(current / walk, 0.0, 1.0), settings.curve);
        const double toRun = std::pow(std::clamp((current - walk) / ((run - walk) * settings.runningThreshold),
                                                 0.0, 1.0), settings.curve);
        motion.intensity = std::clamp(current / run, 0.0, 1.0);
        const double walkSpeed = std::clamp(settings.walkSpeed, settings.minimum, settings.maximum);
        const double runSpeed = std::clamp(settings.runSpeed, walkSpeed, settings.maximum);
        target = settings.minimum + (walkSpeed - settings.minimum) * fromWalk + (runSpeed - walkSpeed) * toRun;
        target *= 0.78 + 0.22 * motion.confidence;
    }
    stick_ = static_cast<float>(follow(stick_, target, target > stick_ ? settings.accelerate : settings.decelerate, dt));
    if (target == 0 && stick_ < 0.012f) stick_ = 0;
    motion.stick = stick_ >= settings.deadzone ? stick_ : 0;
    return motion;
}

void Calibration::start(double now) {
    *this = {};
    since_ = now;
    stage_ = Stage::Still;
    message_ = "Stand still and face forward";
}

void Calibration::cancel() { *this = {}; }

bool Calibration::active() const {
    return stage_ == Stage::Still || stage_ == Stage::Walk || stage_ == Stage::Jog;
}

double Calibration::remaining(double now) const {
    return active() ? std::max(0.0, duration(stage_) - (now - since_)) : 0;
}

void Calibration::fail(std::string message) {
    stage_ = Stage::Failed;
    message_ = std::move(message);
}

void Calibration::update(double now, const Motion& motion, Settings& settings) {
    if (!active()) return;
    const double elapsed = now - since_;
    if (motion.tracking && elapsed > 0.6 && now - lastSample_ >= 0.024) {
        lastSample_ = now;
        ++samples_;
        if (stage_ == Stage::Still) {
            amplitudes_.push_back(std::abs(motion.filtered));
        } else if (elapsed > 2 && motion.confidence >= 0.54 && motion.amplitude > 0) {
            amplitudes_.push_back(motion.amplitude);
            cadences_.push_back(motion.cadence);
        }
    }
    if (elapsed < duration(stage_)) return;
    if (samples_ < static_cast<unsigned>(25 * (duration(stage_) - 0.6)) || amplitudes_.size() < 25) {
        fail("Too little steady tracking or rhythmic movement. Try again.");
        return;
    }
    const double amplitude = median(amplitudes_);
    if (stage_ == Stage::Still) {
        if (amplitude > 0.0025 || *std::max_element(amplitudes_.begin(), amplitudes_.end()) > 0.022) {
            fail("Too much movement while standing still. Try again.");
            return;
        }
        noise_ = std::clamp(amplitude * 1.25, 0.00015, 0.003);
        stage_ = Stage::Walk;
        message_ = "Walk normally in place";
    } else if (stage_ == Stage::Walk) {
        walkAmplitude_ = amplitude;
        walkCadence_ = median(cadences_);
        stage_ = Stage::Jog;
        message_ = "Jog in place";
    } else {
        const double cadence = median(cadences_);
        if (effort(amplitude, cadence) < effort(walkAmplitude_, walkCadence_) * 1.16) {
            fail("Jogging was too close to walking. Try again.");
            return;
        }
        settings.noise = noise_;
        settings.walkAmplitude = walkAmplitude_;
        settings.walkCadence = std::clamp(walkCadence_, 0.8, 3.2);
        settings.runAmplitude = amplitude;
        settings.runCadence = std::clamp(cadence, 0.8, 3.2);
        settings.activation = std::clamp(walkAmplitude_ * 0.38, 0.0015, 0.016);
        settings.calibrated = true;
        stage_ = Stage::Complete;
        message_ = "Calibration saved. Try the live stick preview.";
    }
    since_ = now;
    samples_ = 0;
    amplitudes_.clear();
    cadences_.clear();
}

Axes merge(Axes physical, float forward, bool physicalOverride, float threshold) {
    if (!std::isfinite(forward) || forward <= 0 || !std::isfinite(physical.x) || !std::isfinite(physical.y)) {
        return physical;
    }
    if (physicalOverride && std::abs(physical.y) >= threshold) return physical;
    physical.y = std::clamp(forward, 0.0f, 1.0f);
    return physical;
}

}

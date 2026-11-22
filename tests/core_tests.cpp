#include "core.hpp"
#include "ipc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace stride;
constexpr double pi = 3.141592653589793;

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

struct Trace {
    Detector detector;
    Settings settings;
    double time = 100;

    Motion sample(double y, double pitch = 0, bool valid = true) {
        time += 1.0 / 80.0;
        return detector.update({time, y, pitch, valid}, settings);
    }

    double steady(double amplitude, double frequency, double* jitter = nullptr) {
        double total = 0, prior = 0;
        for (int i = 0; i < 720; ++i) {
            const auto output = sample(1.65 + amplitude * std::sin(i * 2 * pi * frequency / 80)).stick;
            if (i >= 480) {
                total += output;
                if (jitter) *jitter = std::max(*jitter, std::abs(output - prior));
            }
            prior = output;
        }
        return total / 240;
    }
};

void motionTests() {
    require(Trace{}.steady(0, 0) == 0, "stationary output was nonzero");
    double jitter = 0;
    const auto slow = Trace{}.steady(0.008, 1.05, &jitter);
    const auto lowerEdge = Trace{}.steady(0.009, 0.82, &jitter);
    const auto normal = Trace{}.steady(0.015, 1.8, &jitter);
    const auto fast = Trace{}.steady(0.021, 2.35, &jitter);
    const auto jog = Trace{}.steady(0.033, 2.95, &jitter);
    const auto top = Trace{}.steady(0.036, 3.18, &jitter);
    std::cout << "Stationary/slow/walk/fast/jog/top outputs: 0/" << slow << '/' << normal << '/' << fast << '/' << jog << '/' << top << '\n';
    require(slow > 0.20 && slow < 0.40, "slow walking not in low output range");
    require(lowerEdge > 0.19 && lowerEdge < 0.36, "walking near the minimum cadence was not detected");
    require(normal >= 0.39 && normal <= 0.65, "normal walking not in moderate output range");
    require(slow < normal && normal < fast && fast < jog && top >= jog - 0.012, "output not monotonic with movement");
    require(jitter < 0.08, "severe per-frame output jitter");

    Trace noise, bob, crouch, look, drift, rebase;
    std::uint32_t rng = 13;
    for (int i = 0; i < 480; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const double seconds = i / 80.0;
        const double oneBob = i >= 100 && i < 145 ? .055 * std::sin((i - 100) * pi / 45) : 0;
        const double goDown = std::clamp((seconds - 1) * 0.38, 0.0, 0.43);
        const double comeUp = std::clamp((seconds - 3) * 0.38, 0.0, 0.43);
        require(noise.sample(1.7 + (static_cast<double>(rng) / UINT32_MAX - 0.5) * 0.003).stick == 0, "noise was accepted");
        require(bob.sample(1.7 + oneBob).stick == 0, "single bob was accepted");
        require(crouch.sample(1.7 - goDown + comeUp).stick == 0, "crouch was accepted");
        require(look.sample(1.7 + 0.002 * std::sin(seconds * 2 * pi * 1.8)).stick == 0, "looking around was accepted");
        require(drift.sample(1.7 + 0.045 * std::sin(seconds * 2 * pi * 0.2)).stick == 0, "slow drift was accepted");
        require(rebase.sample(i < 200 ? 1.7 : 1.45).stick == 0, "recentring was accepted");
    }

    Trace lost;
    require(lost.steady(.016, 1.8) > .4, "tracking loss test did not first activate");
    require(lost.sample(0, 0, false).stick == 0, "tracking loss did not immediately neutralize");
    require(lost.sample(1.7).stick == 0, "reacquisition reused a prior step history");

    double worstStop = 0;
    for (double frequency : {1.05, 1.8, 2.9}) {
        for (int phase = 0; phase < 24; ++phase) {
            Trace stop;
            double last = 0;
            Motion moving;
            const double amplitude = frequency < 1.1 ? 0.008 : frequency > 2.8 ? 0.030 : 0.016;
            for (int j = 0; j < 640 + phase; ++j) {
                last = 1.65 + amplitude * std::sin(j * 2 * pi * frequency / 80);
                moving = stop.sample(last);
            }
            require(moving.stick > 0.1, "stop test did not first activate");
            int n = 0;
            while (++n <= 40 && stop.sample(last).stick != 0) {}
            worstStop = std::max(worstStop, n / 80.0);
            require(n / 80.0 < 0.3, "stop latency exceeded 300 ms");
        }
    }
    std::cout << "Worst stop across 72 motion/phase combinations: " << worstStop * 1000 << " ms\n";
}

void calibrationTests(const std::filesystem::path& root) {
    using Stage = Calibration::Stage;
    auto calibrate = [](double hz, double walk, double jog, bool drops, bool skip, Settings& settings) {
        Calibration calibration;
        Detector detector;
        double time = 100, phaseStart = time;
        calibration.start(time);
        auto previous = Stage::Idle;
        for (int i = 0; calibration.active() && time < 130; ++i) {
            const auto stage = calibration.stage();
            if (stage != previous) {
                phaseStart = time;
                detector.reset();
                previous = stage;
            }
            if (stage == Stage::Jog && skip) {
                calibration.finishWithoutJog(settings);
                break;
            }
            const double seconds = time - phaseStart;
            const double amplitude = stage == Stage::Still ? 0.00025 : stage == Stage::Walk ? walk : jog;
            const double frequency = stage == Stage::Jog && jog > walk ? 2.85 : 1.83;
            const double y = 1.7 + amplitude * std::sin(2 * pi * seconds * frequency) + 0.00015 * std::sin(71 * seconds);
            const bool valid = !drops || (i % 39 != 1 && i % 39 != 2);
            const auto frame = calibration.measure(detector, {time, y, 0, valid}, settings);
            if (!valid) require(!frame.tracking && frame.stick == 0, "calibration treated an invalid pose as active");
            calibration.update(time, frame, settings);
            time += (1 + (drops ? 0.09 * std::sin(i) : 0)) / hz;
        }
        return calibration;
    };
    Settings settings;
    const auto measured = calibrate(80, 0.014, 0.031, false, false, settings);
    require(measured.stage() == Stage::Complete && settings.calibrated, "calibration failed on good data");
    require(settings.runCadence > settings.walkCadence && measured.message().find("estimated") == std::string::npos,
            "calibration did not measure a distinct jogging phase");

    Settings lightSteps;
    const auto partial = calibrate(48, 0.003, 0, true, false, lightSteps);
    require(partial.stage() == Stage::Complete && lightSteps.calibrated && lightSteps.walkAmplitude < 0.004,
            "shallow steps, jitter, or brief tracking drops prevented walking calibration");
    require(partial.message().find("estimated") != std::string::npos, "missing jogging should estimate a range");
    require(lightSteps.activation == minimumMotionAmplitude, "shallow calibration did not lower the movement threshold");

    Settings walkingOnly;
    const auto skipped = calibrate(49, .009, 0, false, true, walkingOnly);
    require(skipped.stage() == Stage::Complete && walkingOnly.calibrated, "skipping jogging discarded walking calibration");
    const auto unmoving = calibrate(49, 0, 0, false, false, walkingOnly);
    require(unmoving.stage() == Stage::Failed && unmoving.message().find("rhythm") != std::string::npos && walkingOnly.calibrated,
            "standing still was accepted as walking or obscured the reason for failure");
    const auto path = root / "settings.json";
    std::string error;
    require(saveSettings(path, lightSteps, error), "calibration could not be persisted");
    const auto restored = loadSettings(path, error);
    require(error.empty() && restored.calibrated && restored.walkCadence == lightSteps.walkCadence &&
            restored.activation == lightSteps.activation, "calibration did not round trip");
    std::ofstream(path) << "{\"schema\":1,\"sensitivity\":999,\"minimum_output\":0.4,\"maximum_output\":0.1,\"game_only\":0}";
    const auto clamped = loadSettings(path, error);
    require(clamped.sensitivity == 3 && clamped.minimum == clamped.maximum && clamped.gameOnly, "invalid field was not safely handled");
    std::ofstream(path) << "{unclosed";
    require(loadSettings(path, error).sensitivity == 1 && !error.empty(), "invalid JSON did not fall back safely");
    Calibration lost;
    lost.start(200);
    lost.update(205.1, {}, settings);
    require(lost.stage() == Stage::Failed && lost.message().find("tracking") != std::string::npos && settings.calibrated,
            "tracking failure was obscured or discarded prior settings");
    std::cout << "Calibration accepts light steps and brief pose drops, estimates jogging if needed, and identifies tracking loss.\n";
}

void safetyTests(const std::filesystem::path& root) {
    const auto walk = merge({0.4f, 0.02f}, 0.72f, true, 0.13f);
    require(walk.x == 0.4f && walk.y == 0.72f, "strafing did not survive merge");
    require(merge({0.5f, -0.3f}, 0.9f, true, 0.13f).y == -0.3f, "physical backwards input did not override");
    require(merge({0.5f, 0.2f}, 0.9f, true, 0.13f).y == 0.2f, "physical forward input did not override");
    require(merge({}, 0, false, 0.13f).y == 0, "disabled synthetic input did not release");
    const auto data = encode({0.6f, 0.13f, Ready | Tracking, 1000});
    require(current(data, 999, watchdogMs).value == 0, "future heartbeat was accepted");
    require(current(data, 1119, watchdogMs).has(Ready), "recent heartbeat was lost");
    require(current(data, 1120, watchdogMs).value == 0, "stale heartbeat did not neutralize");

#ifdef __linux__
    setenv("FRAMESTRIDE_RUNTIME_DIR", root.c_str(), 1);
    Mailbox reader(Channel::Motion, false);
    const auto now = 10000;
    for (unsigned i = 0; i != 5; ++i) {
        {
            Mailbox writer(Channel::Motion, true), duplicate(Channel::Motion, true);
            require(writer.write(.7f, .13f, Ready, now + i), "could not write motion packet");
            require(!duplicate.connect(now + i), "second source could write invalid competing input");
            require(reader.read(now + i).value > .69f, "IPC did not receive motion packet");
        }
        require(reader.read(now + i).value == 0, "destructor left output held");
    }
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        Mailbox writer(Channel::Motion, true);
        _exit(writer.write(1, .13f, Ready, now + 10) ? 0 : 1);
    }
    int childStatus = 0;
    require(waitpid(child, &childStatus, 0) == child && childStatus == 0, "crashed writer did not run");
    require(reader.read(now + 10).value == 1, "crash test did not receive final packet");
    require(reader.read(now + 10 + watchdogMs).value == 0, "crash left held motion past timeout");
#else
    (void)root;
#endif
}
}

int main() {
    const auto unique = stride::clockMilliseconds();
    const auto root = std::filesystem::temp_directory_path() / ("framestride-test-" + std::to_string(unique));
    std::filesystem::create_directory(root);
    try {
        motionTests();
        calibrationTests(root);
        safetyTests(root);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    std::cout << "PASS: motion, calibration, config, merge, watchdog and restart checks\n";
}

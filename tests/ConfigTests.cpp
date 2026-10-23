#include "Config.hpp"
#include "FixedStepClock.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#endif

namespace {

constexpr const char* kSetting = "POWDER_CONFIG_TEST_VALUE";
constexpr const char* kEmptySetting = "POWDER_CONFIG_TEST_EMPTY";

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void SetValue(const char* value, const char* name = kSetting) {
#ifdef _WIN32
    const int result = _putenv_s(name, value != nullptr ? value : "");
#else
    const int result = value != nullptr ? setenv(name, value, 1) : unsetenv(name);
#endif
    Require(result == 0, "could not set test environment variable");
}

template <typename T>
void ExpectInvalid(const char* name, bool (*parse)(const char*, T&)) {
    T value = static_cast<T>(7);
    bool threw = false;
    try {
        parse(name, value);
    } catch (const std::runtime_error& error) {
        threw = true;
        Require(std::string(error.what()).find(name) != std::string::npos,
                "invalid-value error must name its setting");
    }
    Require(threw, "present invalid value must throw std::runtime_error");
    Require(value == static_cast<T>(7), "invalid value changed the destination");
}

using powder_config::SimulationConfig;

enum class FloatDomain { Nonnegative, Positive, UnitInterval, TimeStep, Signed };

struct FloatSetting {
    const char* name;
    float SimulationConfig::* member;
    float defaultValue;
    FloatDomain domain;
};

constexpr FloatSetting kFloatSettings[] = {
    {"POWDER_FIXED_DT", &SimulationConfig::fixed_dt, 1.0F / 60.0F, FloatDomain::TimeStep},
    {"POWDER_WATER_CFL", &SimulationConfig::water_cfl_limit, 16.0F, FloatDomain::Positive},
    {"POWDER_WATER_GRAVITY", &SimulationConfig::water_gravity, -1962.0F, FloatDomain::Signed},
    {"POWDER_WATER_MAX_VELOCITY", &SimulationConfig::water_max_velocity, 2400.0F, FloatDomain::Positive},
    {"POWDER_WATER_DENSITY_RATE", &SimulationConfig::water_density_rate, 20.0F, FloatDomain::Nonnegative},
    {"POWDER_WATER_SURFACE_LEVEL", &SimulationConfig::water_surface_level, 0.25F, FloatDomain::UnitInterval},
    {"POWDER_WATER_GHOST_THETA_MIN", &SimulationConfig::water_ghost_theta_min, 0.25F, FloatDomain::UnitInterval},
    {"POWDER_WATER_FLIP_BLEND", &SimulationConfig::water_flip_blend, 0.97F, FloatDomain::UnitInterval},
    {"POWDER_WATER_PRESSURE_RESIDUAL", &SimulationConfig::water_pressure_residual_threshold, 0.12F, FloatDomain::Nonnegative},
    {"POWDER_GAS_CFL", &SimulationConfig::gas_cfl_limit, 8.0F, FloatDomain::Positive},
    {"POWDER_GAS_PRESSURE_RESIDUAL", &SimulationConfig::gas_pressure_residual_threshold, 0.25F, FloatDomain::Nonnegative},
    {"POWDER_GAS_BUOYANCY_TEMPERATURE", &SimulationConfig::gas_buoyancy_temperature, 1200.0F, FloatDomain::Signed},
    {"POWDER_GAS_BUOYANCY_SMOKE", &SimulationConfig::gas_buoyancy_smoke, 10.0F, FloatDomain::Signed},
    {"POWDER_GAS_COMBUSTION_RATE", &SimulationConfig::gas_combustion_rate, 10.0F, FloatDomain::Nonnegative},
    {"POWDER_GAS_WATER_COOLING", &SimulationConfig::gas_water_cooling, 2.4F, FloatDomain::Nonnegative},
    {"POWDER_GAS_TEMPERATURE_DIFFUSION", &SimulationConfig::gas_temperature_diffusion, 10.0F, FloatDomain::Nonnegative},
    {"POWDER_GAS_SMOKE_DISSIPATION", &SimulationConfig::gas_smoke_dissipation, 0.08F, FloatDomain::Nonnegative},
    {"POWDER_SAND_DRAG", &SimulationConfig::sand_drag, 60.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_MAX_VELOCITY", &SimulationConfig::sand_max_velocity, 2400.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_FRICTION", &SimulationConfig::sand_friction_coefficient, 0.62F, FloatDomain::Nonnegative},
    {"POWDER_SAND_REFERENCE_DENSITY", &SimulationConfig::sand_reference_density, 2.0F, FloatDomain::Positive},
    {"POWDER_SAND_SHEAR_MODULUS", &SimulationConfig::sand_shear_modulus, 784000.0F, FloatDomain::Positive},
    {"POWDER_SAND_LAME_LAMBDA", &SimulationConfig::sand_lame_lambda, 784000.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_FRICTION_ALPHA", &SimulationConfig::sand_friction_alpha, 0.5F, FloatDomain::Nonnegative},
    {"POWDER_SAND_FRICTION_RATE_GAIN", &SimulationConfig::sand_friction_rate_gain, 0.68F, FloatDomain::Nonnegative},
    {"POWDER_SAND_COHESION", &SimulationConfig::sand_cohesion, 0.15F, FloatDomain::Nonnegative},
    {"POWDER_SAND_COHESION_SOFTENING", &SimulationConfig::sand_cohesion_softening, 0.5F, FloatDomain::Positive},
    {"POWDER_SAND_MAX_DILATION", &SimulationConfig::sand_max_dilation, 0.14F, FloatDomain::Nonnegative},
    {"POWDER_SAND_CFL", &SimulationConfig::sand_cfl_limit, 0.4F, FloatDomain::Positive},
    {"POWDER_SAND_ADVECTION_CFL", &SimulationConfig::sand_advection_cfl, 1.0F, FloatDomain::Positive},
    {"POWDER_SAND_GRAVITY", &SimulationConfig::sand_gravity, 1962.0F, FloatDomain::Signed},
    {"POWDER_WATER_ADVECTION_CFL", &SimulationConfig::water_advection_cfl, 2.0F, FloatDomain::Positive},
    {"POWDER_SAND_VISCOSITY", &SimulationConfig::sand_viscosity, 200.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_CONTACT_DAMPING", &SimulationConfig::sand_contact_damping, 6.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_SLEEP_SPEED", &SimulationConfig::sand_sleep_speed, 4.0F, FloatDomain::Nonnegative},
    {"POWDER_SAND_WAKE_IMPACT_SPEED", &SimulationConfig::sand_wake_impact_speed, 60.0F, FloatDomain::Positive},
    {"POWDER_SAND_WAKE_PRESS_SPEED", &SimulationConfig::sand_wake_press_speed, 20.0F, FloatDomain::Positive},
    {"POWDER_SAND_WAKE_WATER_SPEED", &SimulationConfig::sand_wake_water_speed, 40.0F, FloatDomain::Positive},
    {"POWDER_WATER_SLEEP_SPEED", &SimulationConfig::water_sleep_speed, 2.0F, FloatDomain::Nonnegative},
    {"POWDER_WATER_WAKE_SPEED", &SimulationConfig::water_wake_speed, 5.0F, FloatDomain::Positive},
    {"POWDER_SAND_SPAWN_OCCUPANCY_LIMIT", &SimulationConfig::sand_spawn_occupancy_limit, 0.225F, FloatDomain::UnitInterval},
    {"POWDER_REPLAY_DETERMINISM_REL_TOL", &SimulationConfig::replay_determinism_relative_tolerance, 0.15F, FloatDomain::Nonnegative},
    {"POWDER_REPLAY_DETERMINISM_ABS_TOL", &SimulationConfig::replay_determinism_absolute_tolerance, 0.05F, FloatDomain::Nonnegative},
    {"POWDER_COMBINED_TARGET_FRAME_MS", &SimulationConfig::combined_target_frame_ms, 33.34F, FloatDomain::Positive},
    {"POWDER_COMBINED_WORST_FRAME_MS", &SimulationConfig::combined_worst_frame_ms, 45.0F, FloatDomain::Positive},
};

struct IntSetting {
    const char* name;
    int SimulationConfig::* member;
    int defaultValue;
};

constexpr IntSetting kIntSettings[] = {
    {"POWDER_WATER_MIN_SUBSTEPS", &SimulationConfig::water_min_substeps, 1},
    {"POWDER_WATER_MAX_SUBSTEPS", &SimulationConfig::water_max_substeps, 6},
    {"POWDER_WATER_PRESSURE_ITERATIONS", &SimulationConfig::water_pressure_iterations, 1024},
    {"POWDER_GAS_MIN_SUBSTEPS", &SimulationConfig::gas_min_substeps, 1},
    {"POWDER_GAS_MAX_SUBSTEPS", &SimulationConfig::gas_max_substeps, 2},
    {"POWDER_GAS_PRESSURE_ITERATIONS", &SimulationConfig::gas_pressure_iterations, 48},
    {"POWDER_SAND_MAX_SUBSTEPS", &SimulationConfig::sand_max_substeps, 64},
    {"POWDER_REPLAY_REPEAT", &SimulationConfig::replay_repeat_count, 2},
};

void ClearConfigEnvironment() {
    SetValue(nullptr, "POWDER_SAND_SLEEP_TICKS");
    SetValue(nullptr, "POWDER_WATER_SLEEP_TICKS");
    for (const auto& setting : kFloatSettings) {
        SetValue(nullptr, setting.name);
    }
    for (const auto& setting : kIntSettings) {
        SetValue(nullptr, setting.name);
    }
}

void ExpectInvalidConfig(const char* name, const char* value) {
    ClearConfigEnvironment();
    SetValue(value, name);
    bool threw = false;
    try {
        powder_config::LoadConfigFromEnvironment();
    } catch (const std::runtime_error& error) {
        threw = true;
        Require(std::string(error.what()).find(name) != std::string::npos,
                "configuration error must name its setting");
    }
    Require(threw, "invalid setting must throw instead of being accepted or clamped");
}

void ExpectFloatSetting(const FloatSetting& setting, float value) {
    ClearConfigEnvironment();
    std::ostringstream text;
    text.precision(std::numeric_limits<float>::max_digits10);
    text << value;
    SetValue(text.str().c_str(), setting.name);
    Require(powder_config::LoadConfigFromEnvironment().*(setting.member) == value,
            "valid setting was changed or ignored");
}

template <typename Run>
void CheckSimulationConfig(const Run& run) {
    run("fixed simulation clock is independent of render frequency", [] {
        const double dt = 1.0 / 60.0;
        for (int frames_per_second : {60, 144, 240}) {
            FixedStepClock clock;
            int steps = 0;
            for (int frame = 0; frame < 10 * frames_per_second; ++frame) {
                steps += clock.Advance(1.0 / frames_per_second, dt);
            }
            Require(steps >= 599 && steps <= 600, "ten elapsed seconds must produce 600 ticks within one tick");
        }
        // A slow display with cheap ticks catches up two ticks per frame and keeps real time.
        FixedStepClock slow_display;
        int slow_steps = 0;
        for (int frame = 0; frame < 300; ++frame) slow_steps += slow_display.Advance(1.0 / 30.0, dt);
        Require(slow_steps == 600, "a 30 Hz frame rate with cheap ticks runs two ticks per frame");
        FixedStepClock clock;
        Require(clock.Advance(dt * 0.5, dt) == 0 && clock.Advance(dt * 0.5, dt) == 1,
                "sub-frame elapsed time must carry into the next frame");
        Require(clock.Advance(dt * 0.9, dt) == 0 && clock.Advance(dt * 1.2, dt) == 2,
                "a frame that kept up may catch up two ticks");
        Require(clock.Advance(10.0, dt) == 2 && clock.Advance(0.0, dt) == 1 && clock.Advance(0.0, dt) == 0,
                "a stall runs at most two ticks and keeps at most one tick of backlog");
        Require(clock.Advance(-1.0, dt) == 0, "clock rollback must not advance or rewind the simulation");
        clock.MeasureTicks(0.010, 1);
        Require(clock.Advance(10.0, dt) == 1, "a measured tick too costly to double up runs once per frame");
    });
    run("fixed simulation clock cannot death-spiral when a tick costs more than its duration", [] {
        const double dt = 1.0 / 60.0;
        for (double tick_cost : {0.020, 0.050, 0.200}) {
            FixedStepClock clock;
            double frame = dt;
            for (int i = 0; i < 200; ++i) {
                const int steps = clock.Advance(frame, dt);
                Require(steps >= 1 || frame < dt, "a frame of at least one tick must advance the simulation");
                Require(i == 0 || steps == 1, "a measured tick longer than a frame must run once per frame");
                clock.MeasureTicks(steps * tick_cost, steps);
                frame = 0.002 + steps * tick_cost;  // the next frame waits for this one's ticks plus rendering
            }
        }
    });
    run("fixed simulation clock keeps the display rate when one tick fits a frame", [] {
        // A 60 Hz display that misses its interval by 2 ms every tenth frame. A 12 ms tick plus 3 ms of rendering fits a
        // refresh; two ticks do not. The old 1.5-tick rule doubled up whenever the drift added up to a tick, and those
        // 27 ms frames halved the frame rate. Ticks of 5 ms still catch up and keep real time.
        const double dt = 1.0 / 60.0;
        for (double tick_cost : {0.012, 0.005}) {
            FixedStepClock clock;
            double frame = dt, wall = 0.0;
            int ticks = 0, long_frames = 0;
            for (int i = 0; i < 600; ++i) {
                const int steps = clock.Advance(frame, dt);
                clock.MeasureTicks(steps * tick_cost, steps);
                ticks += steps;
                wall += frame;
                const double work = 0.003 + steps * tick_cost;
                if (i > 0 && work > dt) ++long_frames;
                frame = std::max(dt, work) + (i % 10 == 9 ? 0.002 : 0.0);
            }
            Require(long_frames == 0, "a tick that fits a frame must never be doubled into a long frame");
            const double sim_rate = ticks * dt / wall;
            Require(tick_cost > 0.01 ? sim_rate > 0.95 && sim_rate <= 1.0 : sim_rate > 0.99,
                    "the simulation must stay near real time while the frame rate holds");
        }
    });
    run("substep count respects finite inputs before narrowing", [] {
        using powder_config::SubstepCount;
        Require(SubstepCount(2400.0F, 1.0F / 60.0F, 16.0F, 1, 6) <= 6, "water default count at the speed cap fits the budget");
        Require(SubstepCount(40.0F, 1.0F / 60.0F, 0.9F, 1, 3) == 1, "slow water takes one substep");
        Require(SubstepCount(0.0F, 1.0F, 1.0F, 2, 8) == 2, "minimum count");
        Require(SubstepCount(3.25F, 1.0F, 1.0F, 1, 8) == 4, "fraction rounds upward");
        Require(SubstepCount(72.0F, 1.0F / 15.0F, 0.9F, 1, 6) == 6,
                "a sufficient budget must satisfy the slower tick's CFL limit");
    });
    run("substep arithmetic cannot overflow the integer conversion", [] {
        using powder_config::SubstepCount;
        const float largest = std::numeric_limits<float>::max();
        const int maximum = std::numeric_limits<int>::max();
        const auto reject = [&](float speed, float dt, float cfl, int budget) {
            bool rejected = false;
            try { SubstepCount(speed, dt, cfl, 1, budget); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("CFL") != std::string::npos;
            }
            Require(rejected, "an unmet CFL limit must be rejected before integer conversion");
        };
        reject(72.0F, 1.0F / 15.0F, 0.9F, 3);
        reject(1.0F, 1.0F, 1e-20F, 1000000);
        reject(largest, 0.5F, 0.001F, 3);
        reject(largest, largest, 0.001F, maximum);
    });
    run("gas CFL rejection identifies its configuration budget", [] {
        bool rejected = false;
        try {
            powder_config::SubstepCount(std::sqrt(2.0F) * 100.0F, 1.0F / 60.0F, 0.9F, 1, 2,
                                       "POWDER_GAS_MAX_SUBSTEPS");
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            rejected = message.find("POWDER_GAS_MAX_SUBSTEPS=2") != std::string::npos &&
                       message.find("reduce POWDER_FIXED_DT") != std::string::npos;
        }
        Require(rejected, "rejected timestep must name the gas budget and a corrective action");
    });
    run("absent simulation overrides retain defaults", [] {
        ClearConfigEnvironment();
        const auto config = powder_config::LoadConfigFromEnvironment();
        for (const auto& setting : kFloatSettings) {
            Require(config.*(setting.member) == setting.defaultValue, setting.name);
        }
        for (const auto& setting : kIntSettings) {
            Require(config.*(setting.member) == setting.defaultValue, setting.name);
        }
    });
    for (const auto& setting : kFloatSettings) {
        run(std::string("valid float domain ") + setting.name, [&] {
            if (setting.domain == FloatDomain::TimeStep) {
                ExpectFloatSetting(setting, 1.0F / 240.0F);
                ExpectFloatSetting(setting, 1.0F / 15.0F);
            } else {
                ExpectFloatSetting(setting, 0.5F);
                if (setting.domain != FloatDomain::Positive) {
                    ExpectFloatSetting(setting, 0.0F);
                }
                if (setting.domain == FloatDomain::Signed) {
                    ExpectFloatSetting(setting, -2.0F);
                } else if (setting.domain == FloatDomain::UnitInterval) {
                    ExpectFloatSetting(setting, 1.0F);
                } else {
                    ExpectFloatSetting(setting, 1000000.0F);
                }
            }
        });
        if (setting.domain != FloatDomain::Signed) {
            run(std::string("negative setting rejected ") + setting.name,
                [&] { ExpectInvalidConfig(setting.name, "-1"); });
        }
        if (setting.domain == FloatDomain::Positive) {
            run(std::string("zero setting rejected ") + setting.name,
                [&] { ExpectInvalidConfig(setting.name, "0"); });
        } else if (setting.domain == FloatDomain::UnitInterval) {
            run(std::string("unit interval upper bound ") + setting.name,
                [&] { ExpectInvalidConfig(setting.name, "1.01"); });
        }
    }
    for (const auto& setting : kIntSettings) {
        run(std::string("valid count ") + setting.name, [&] {
            ClearConfigEnvironment();
            SetValue("1", setting.name);
            Require(powder_config::LoadConfigFromEnvironment().*(setting.member) == 1,
                    "minimum count was changed or ignored");
        });
        for (const char* value : {"0", "-1"}) {
            run(std::string("invalid count ") + setting.name + "=" + value,
                [&] { ExpectInvalidConfig(setting.name, value); });
        }
    }
    for (const auto& [name, value] : {std::pair{"POWDER_FIXED_DT", "0.001"},
                                     {"POWDER_FIXED_DT", "0.1"},
                                     {"POWDER_REPLAY_REPEAT", "5"},
                                     {"POWDER_WATER_MIN_SUBSTEPS", "7"},
                                     {"POWDER_GAS_MIN_SUBSTEPS", "3"}}) {
        run(std::string("configuration upper or paired bound ") + name,
            [&] { ExpectInvalidConfig(name, value); });
    }
    run("replay upper endpoint accepted", [] {
        ClearConfigEnvironment();
        SetValue("4", "POWDER_REPLAY_REPEAT");
        const auto config = powder_config::LoadConfigFromEnvironment();
        Require(config.replay_repeat_count == 4, "upper endpoint changed");
    });
    run("equal substep bounds and uncapped iteration counts accepted", [] {
        ClearConfigEnvironment();
        const std::string maximum = std::to_string(std::numeric_limits<int>::max());
        for (const auto& setting : kIntSettings) {
            if (setting.member != &SimulationConfig::replay_repeat_count) {
                SetValue(maximum.c_str(), setting.name);
            }
        }
        const auto config = powder_config::LoadConfigFromEnvironment();
        for (const auto& setting : kIntSettings) {
            if (setting.member != &SimulationConfig::replay_repeat_count) {
                Require(config.*(setting.member) == std::numeric_limits<int>::max(), setting.name);
            }
        }
    });
    // Zero disables sleep-packing; the still-tick timer saturates at 255.
    run("sand sleep ticks domain", [] {
        ClearConfigEnvironment();
        Require(powder_config::LoadConfigFromEnvironment().sand_sleep_ticks == 60, "POWDER_SAND_SLEEP_TICKS default");
        for (const char* value : {"0", "255"}) {
            SetValue(value, "POWDER_SAND_SLEEP_TICKS");
            Require(powder_config::LoadConfigFromEnvironment().sand_sleep_ticks == std::atoi(value),
                    "sand sleep ticks endpoint");
        }
        ExpectInvalidConfig("POWDER_SAND_SLEEP_TICKS", "-1");
        ExpectInvalidConfig("POWDER_SAND_SLEEP_TICKS", "256");
    });
    run("water sleep ticks domain", [] {
        ClearConfigEnvironment();
        Require(powder_config::LoadConfigFromEnvironment().water_sleep_ticks == 30, "POWDER_WATER_SLEEP_TICKS default");
        for (const char* value : {"0", "255"}) {
            SetValue(value, "POWDER_WATER_SLEEP_TICKS");
            Require(powder_config::LoadConfigFromEnvironment().water_sleep_ticks == std::atoi(value),
                    "water sleep ticks endpoint");
        }
        ExpectInvalidConfig("POWDER_WATER_SLEEP_TICKS", "-1");
        ExpectInvalidConfig("POWDER_WATER_SLEEP_TICKS", "256");
    });
    run("loader preserves lexical error setting names", [] {
        ExpectInvalidConfig("POWDER_GAS_CFL", "1junk");
        ExpectInvalidConfig("POWDER_GAS_MAX_SUBSTEPS", "1junk");
    });
    ClearConfigEnvironment();
}

}  // namespace

int main(int argc, char** argv) {
    int failures = 0;
    int checks = 0;
    // Flush each result line so a crash still shows the last check that ran.
    std::cout << std::unitbuf;
    const auto run = [&](const std::string& name, const auto& check) {
        ++checks;
        try {
            check();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    if (argc == 2 && std::string(argv[1]) == "--empty") {
        // Windows _putenv_s(name, "") removes the variable; launch with NAME= instead.
        run("empty fixture is present", [] {
            const char* value = std::getenv(kEmptySetting);
            Require(value != nullptr && *value == '\0', "launch with POWDER_CONFIG_TEST_EMPTY= in the environment");
        });
        run("empty integer rejected", [] { ExpectInvalid(kEmptySetting, powder_config::EnvToInt); });
        run("empty float rejected", [] { ExpectInvalid(kEmptySetting, powder_config::EnvToFloat); });
    } else if (argc == 1) {
        run("absent values preserve defaults", [] {
            SetValue(nullptr);
            int integer = 7;
            float real = 2.5F;
            Require(!powder_config::EnvToInt(kSetting, integer) && integer == 7, "absent integer changed default");
            Require(!powder_config::EnvToFloat(kSetting, real) && real == 2.5F, "absent float changed default");
        });
        for (const auto& [text, expected] : {std::pair{"+17", 17}, {"-4", -4}, {"0", 0}}) {
            run(std::string("valid integer ") + text, [&] {
                SetValue(text);
                int value = 7;
                Require(powder_config::EnvToInt(kSetting, value) && value == expected, "valid integer was not assigned");
            });
        }
        for (int expected : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
            run("integer endpoint " + std::to_string(expected), [&] {
                SetValue(std::to_string(expected).c_str());
                int value = 7;
                Require(powder_config::EnvToInt(kSetting, value) && value == expected, "integer endpoint rejected");
            });
        }
        for (const auto& [text, expected] : {std::pair{"-1.25", -1.25F}, {"+2.5", 2.5F}, {"1.25e2", 125.0F}, {"0", 0.0F}}) {
            run(std::string("valid float ") + text, [&] {
                SetValue(text);
                float value = 7.0F;
                Require(powder_config::EnvToFloat(kSetting, value) && value == expected, "valid float was not assigned");
            });
        }
        for (const char* text : {"12junk", "1 2", " ", "\t", "nan", "inf", "+infinity", "++1", "1.5", "2147483648", "-2147483649"}) {
            run(std::string("invalid integer [") + text + "]", [&] {
                SetValue(text);
                ExpectInvalid(kSetting, powder_config::EnvToInt);
            });
        }
        for (const char* text : {"1.25junk", "1 2", " ", "\t", "nan", "inf", "+infinity", "++1", "1e9999", "1e-9999"}) {
            run(std::string("invalid float [") + text + "]", [&] {
                SetValue(text);
                ExpectInvalid(kSetting, powder_config::EnvToFloat);
            });
        }
        run("huge integer rejected", [] {
            SetValue(std::string(4096, '9').c_str());
            ExpectInvalid(kSetting, powder_config::EnvToInt);
        });
        run("huge float rejected", [] {
            SetValue(std::string(4096, '9').c_str());
            ExpectInvalid(kSetting, powder_config::EnvToFloat);
        });
        run("empty values rejected", [&] {
#ifdef _WIN32
            const char* arguments[] = {"powder_config_tests", "--empty", nullptr};
            const char* environment[] = {"POWDER_CONFIG_TEST_EMPTY=", nullptr};
            const auto result = _spawnve(_P_WAIT, argv[0], arguments, environment);
            Require(result == 0, "explicit-empty child failed with exit " + std::to_string(result));
#else
            Require(setenv(kEmptySetting, "", 1) == 0, "could not create empty environment fixture");
            ExpectInvalid(kEmptySetting, powder_config::EnvToInt);
            ExpectInvalid(kEmptySetting, powder_config::EnvToFloat);
#endif
        });
        CheckSimulationConfig(run);
    } else {
        std::cerr << "Usage: powder_config_tests [--empty]\n";
        return 2;
    }

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

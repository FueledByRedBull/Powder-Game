#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace powder_config {

inline int SubstepCount(float speed, float dt, float cfl, int minimum, int maximum,
                        const char* maximum_setting = "maximum substeps") {
    const double required = std::ceil(static_cast<double>(speed) * dt / cfl);
    if (!std::isfinite(required) || required > static_cast<double>(maximum)) {
        throw std::runtime_error(std::string("CFL limit exceeds ") + maximum_setting + "=" +
                                 std::to_string(maximum) + "; increase it or reduce POWDER_FIXED_DT");
    }
    return static_cast<int>(std::max(required, static_cast<double>(minimum)));
}

inline bool EnvToFloat(const char* name, float& value) {
    const char* text = std::getenv(name);
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(parsed)) {
        throw std::runtime_error(std::string("Invalid floating-point value for ") + name);
    }
    value = parsed;
    return true;
}

inline bool EnvToInt(const char* name, int& value) {
    const char* text = std::getenv(name);
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE ||
        parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Invalid integer value for ") + name);
    }
    value = static_cast<int>(parsed);
    return true;
}

struct SimulationConfig {
    float fixed_dt = 1.0F / 60.0F;
    float water_cfl_limit = 0.9F;
    int water_min_substeps = 1;
    int water_max_substeps = 3;
    float water_gravity = -140.0F;
    float water_velocity_damping = 0.992F;
    float water_max_velocity = 72.0F;
    float water_flip_blend = 0.95F;
    float water_particle_mass = 1.0F;
    float water_amount_per_particle = 0.04F;
    float water_liquid_phi_threshold = 0.42F;
    int water_pressure_iterations = 1024;
    float water_pressure_residual_threshold = 0.12F;
    float gas_cfl_limit = 1.1F;
    int gas_min_substeps = 1;
    int gas_max_substeps = 2;
    float gas_velocity_dissipation = 0.994F;
    float gas_max_velocity = 10.0F;
    float gas_density_dissipation = 0.996F;
    float gas_scalar_dissipation = 0.996F;
    float gas_buoyancy_temperature = 1.6F;
    float gas_buoyancy_smoke = 0.55F;
    float gas_temperature_diffusion = 0.045F;
    float gas_combustion_rate = 2.1F;
    float gas_oxidizer_consumption = 0.75F;
    float gas_reaction_decay = 0.18F;
    float gas_smoke_yield = 0.56F;
    float gas_heat_release = 1.05F;
    float gas_temperature_cooling = 0.12F;
    float gas_smoke_dissipation = 0.08F;
    float gas_water_cooling = 2.4F;
    int gas_pressure_iterations = 24;
    float gas_pressure_residual_threshold = 0.015F;
    float sand_gravity = 240.0F;
    float sand_damping = 0.985F;
    float sand_restitution = 0.04F;
    float sand_max_velocity = 280.0F;
    float sand_drag = 0.1F;
    float sand_spawn_occupancy_limit = 0.45F;
    float sand_friction_coefficient = 0.62F;
    float sand_reference_density = 2.0F;
    float sand_shear_modulus = 12000.0F;
    float sand_lame_lambda = 12000.0F;
    float sand_friction_alpha = 0.5F;
    float sand_cfl_limit = 0.4F;
    int sand_max_substeps = 64;
    int replay_repeat_count = 2;
    float replay_determinism_relative_tolerance = 0.15F;
    float replay_determinism_absolute_tolerance = 0.05F;
    float combined_target_frame_ms = 33.34F;
    float combined_worst_frame_ms = 45.0F;
};

inline SimulationConfig LoadConfigFromEnvironment() {
    SimulationConfig config;
    EnvToFloat("POWDER_FIXED_DT", config.fixed_dt);
    EnvToFloat("POWDER_WATER_CFL", config.water_cfl_limit);
    EnvToInt("POWDER_WATER_MIN_SUBSTEPS", config.water_min_substeps);
    EnvToInt("POWDER_WATER_MAX_SUBSTEPS", config.water_max_substeps);
    EnvToFloat("POWDER_WATER_GRAVITY", config.water_gravity);
    EnvToFloat("POWDER_WATER_FLIP_BLEND", config.water_flip_blend);
    EnvToFloat("POWDER_WATER_PRESSURE_RESIDUAL", config.water_pressure_residual_threshold);
    EnvToInt("POWDER_WATER_PRESSURE_ITERATIONS", config.water_pressure_iterations);
    EnvToFloat("POWDER_GAS_CFL", config.gas_cfl_limit);
    EnvToInt("POWDER_GAS_MIN_SUBSTEPS", config.gas_min_substeps);
    EnvToInt("POWDER_GAS_MAX_SUBSTEPS", config.gas_max_substeps);
    EnvToFloat("POWDER_GAS_PRESSURE_RESIDUAL", config.gas_pressure_residual_threshold);
    EnvToInt("POWDER_GAS_PRESSURE_ITERATIONS", config.gas_pressure_iterations);
    EnvToFloat("POWDER_GAS_BUOYANCY_TEMPERATURE", config.gas_buoyancy_temperature);
    EnvToFloat("POWDER_GAS_BUOYANCY_SMOKE", config.gas_buoyancy_smoke);
    EnvToFloat("POWDER_GAS_COMBUSTION_RATE", config.gas_combustion_rate);
    EnvToFloat("POWDER_GAS_WATER_COOLING", config.gas_water_cooling);
    EnvToFloat("POWDER_GAS_TEMPERATURE_DIFFUSION", config.gas_temperature_diffusion);
    EnvToFloat("POWDER_GAS_SMOKE_DISSIPATION", config.gas_smoke_dissipation);
    EnvToFloat("POWDER_SAND_DRAG", config.sand_drag);
    EnvToFloat("POWDER_SAND_MAX_VELOCITY", config.sand_max_velocity);
    EnvToFloat("POWDER_SAND_FRICTION", config.sand_friction_coefficient);
    EnvToFloat("POWDER_SAND_REFERENCE_DENSITY", config.sand_reference_density);
    EnvToFloat("POWDER_SAND_SHEAR_MODULUS", config.sand_shear_modulus);
    EnvToFloat("POWDER_SAND_LAME_LAMBDA", config.sand_lame_lambda);
    EnvToFloat("POWDER_SAND_FRICTION_ALPHA", config.sand_friction_alpha);
    EnvToFloat("POWDER_SAND_CFL", config.sand_cfl_limit);
    EnvToInt("POWDER_SAND_MAX_SUBSTEPS", config.sand_max_substeps);
    EnvToFloat("POWDER_SAND_SPAWN_OCCUPANCY_LIMIT", config.sand_spawn_occupancy_limit);
    EnvToInt("POWDER_REPLAY_REPEAT", config.replay_repeat_count);
    EnvToFloat("POWDER_REPLAY_DETERMINISM_REL_TOL", config.replay_determinism_relative_tolerance);
    EnvToFloat("POWDER_REPLAY_DETERMINISM_ABS_TOL", config.replay_determinism_absolute_tolerance);
    EnvToFloat("POWDER_COMBINED_TARGET_FRAME_MS", config.combined_target_frame_ms);
    EnvToFloat("POWDER_COMBINED_WORST_FRAME_MS", config.combined_worst_frame_ms);

    const auto require = [](bool valid, const char* setting) {
        if (!valid) {
            throw std::runtime_error(std::string("Out-of-range value for ") + setting);
        }
    };
    require(config.fixed_dt >= 1.0F / 240.0F && config.fixed_dt <= 1.0F / 15.0F, "POWDER_FIXED_DT");
    require(config.water_cfl_limit > 0.0F, "POWDER_WATER_CFL");
    require(config.water_min_substeps >= 1, "POWDER_WATER_MIN_SUBSTEPS");
    require(config.water_max_substeps >= 1, "POWDER_WATER_MAX_SUBSTEPS");
    require(config.water_min_substeps <= config.water_max_substeps,
            "POWDER_WATER_MIN_SUBSTEPS / POWDER_WATER_MAX_SUBSTEPS");
    require(config.water_flip_blend >= 0.0F && config.water_flip_blend <= 1.0F, "POWDER_WATER_FLIP_BLEND");
    require(config.water_pressure_residual_threshold >= 0.0F, "POWDER_WATER_PRESSURE_RESIDUAL");
    require(config.water_pressure_iterations >= 1, "POWDER_WATER_PRESSURE_ITERATIONS");
    require(config.gas_cfl_limit > 0.0F, "POWDER_GAS_CFL");
    require(config.gas_min_substeps >= 1, "POWDER_GAS_MIN_SUBSTEPS");
    require(config.gas_max_substeps >= 1, "POWDER_GAS_MAX_SUBSTEPS");
    require(config.gas_min_substeps <= config.gas_max_substeps,
            "POWDER_GAS_MIN_SUBSTEPS / POWDER_GAS_MAX_SUBSTEPS");
    require(config.gas_pressure_residual_threshold >= 0.0F, "POWDER_GAS_PRESSURE_RESIDUAL");
    require(config.gas_pressure_iterations >= 1, "POWDER_GAS_PRESSURE_ITERATIONS");
    require(config.gas_combustion_rate >= 0.0F, "POWDER_GAS_COMBUSTION_RATE");
    require(config.gas_water_cooling >= 0.0F, "POWDER_GAS_WATER_COOLING");
    require(config.gas_temperature_diffusion >= 0.0F, "POWDER_GAS_TEMPERATURE_DIFFUSION");
    require(config.gas_smoke_dissipation >= 0.0F, "POWDER_GAS_SMOKE_DISSIPATION");
    require(config.sand_drag >= 0.0F, "POWDER_SAND_DRAG");
    require(config.sand_max_velocity >= 0.0F, "POWDER_SAND_MAX_VELOCITY");
    require(config.sand_friction_coefficient >= 0.0F, "POWDER_SAND_FRICTION");
    require(config.sand_reference_density > 0.0F, "POWDER_SAND_REFERENCE_DENSITY");
    require(config.sand_shear_modulus > 0.0F, "POWDER_SAND_SHEAR_MODULUS");
    require(config.sand_lame_lambda >= 0.0F, "POWDER_SAND_LAME_LAMBDA");
    require(config.sand_friction_alpha >= 0.0F, "POWDER_SAND_FRICTION_ALPHA");
    require(config.sand_cfl_limit > 0.0F, "POWDER_SAND_CFL");
    require(config.sand_max_substeps >= 1, "POWDER_SAND_MAX_SUBSTEPS");
    require(config.sand_spawn_occupancy_limit >= 0.0F && config.sand_spawn_occupancy_limit <= 1.0F,
            "POWDER_SAND_SPAWN_OCCUPANCY_LIMIT");
    require(config.replay_repeat_count >= 1 && config.replay_repeat_count <= 4, "POWDER_REPLAY_REPEAT");
    require(config.replay_determinism_relative_tolerance >= 0.0F, "POWDER_REPLAY_DETERMINISM_REL_TOL");
    require(config.replay_determinism_absolute_tolerance >= 0.0F, "POWDER_REPLAY_DETERMINISM_ABS_TOL");
    require(config.combined_target_frame_ms > 0.0F, "POWDER_COMBINED_TARGET_FRAME_MS");
    require(config.combined_worst_frame_ms > 0.0F, "POWDER_COMBINED_WORST_FRAME_MS");
    return config;
}

}  // namespace powder_config

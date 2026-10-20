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
    // Physical scale: one cell is 5 mm, so the 1000-cell domain is 5 m and g = 9.81 m/s^2 is 1962 cells/s^2 (PLAN, Scale).
    // Cells the fastest particle may cross per pressure substep: each substep is one pressure solve (~2 ms), so this is
    // the budget limit; particles still move at most water_advection_cfl cells per swept advection sub-move.
    float water_cfl_limit = 16.0F;
    float water_advection_cfl = 2.0F;
    int water_min_substeps = 1;
    // Substeps follow the fastest water (see RunWaterPass); this ceiling must fit water_max_velocity at the CFL limit.
    int water_max_substeps = 6;
    float water_gravity = -1962.0F;
    // Retained per 1/120 s. Water at this scale has no measurable bulk damping (viscous decay of a 5 cm wave takes
    // minutes), so none is applied; the grid transfer's own dissipation remains.
    float water_velocity_damping = 1.0F;
    // Rate (1/s) of turbulent and boundary-layer losses in pressure-supported water (water_particle_step.comp); free
    // fall is exempt. Real viscosity is negligible here, but a poured pool's turbulence dies within seconds, which an
    // inviscid 5 mm grid does not resolve: without it a poured half-screen pool sloshed at p50 92 cells/s after 40 s.
    float water_bulk_damping = 0.4F;
    // 12 m/s, above the 9.9 m/s of a free fall across the whole 5 m domain (1981 cells/s), so falling water is never
    // cut: five to six substeps at the CFL limit. Faster splash particles are clamped and counted (water_velocity_clamped).
    float water_max_velocity = 2400.0F;
    float water_flip_blend = 0.97F;
    float water_particle_mass = 1.0F;
    float water_amount_per_particle = 0.04F;
    // Particle mass per cell at rest: the four particles of mass one the water brush emits.
    float water_rest_density = 4.0F;
    // Rate (1/s) at which a cell's density error is relaxed; higher rates also slow thin flows.
    float water_density_rate = 20.0F;
    float water_liquid_phi_threshold = 0.42F;
    int water_pressure_iterations = 1024;
    float water_pressure_residual_threshold = 0.12F;
    // Gas rates are per second and gas cells (1 cm, two simulation cells); see PLAN "Scale and constants".
    // Semi-Lagrangian traces stop at walls, so a trace may cross several cells: one substep per tick at the
    // speed cap (about 7 cells).
    float gas_cfl_limit = 8.0F;
    int gas_min_substeps = 1;
    int gas_max_substeps = 2;
    float gas_velocity_dissipation = 0.994F;
    // 3 m/s: above buoyant flame and plume speeds (about 2 m/s, McCaffrey 1979).
    float gas_max_velocity = 300.0F;
    float gas_density_dissipation = 0.996F;
    float gas_scalar_dissipation = 0.996F;
    // Buoyant acceleration per unit temperature (gas cells/s^2); T = 1 is a flame about 900 K above ambient.
    float gas_buoyancy_temperature = 1200.0F;
    float gas_buoyancy_smoke = 10.0F;
    // Vorticity confinement strength (1/s, Fedkiw et al. 2001).
    float gas_vorticity = 40.0F;
    // Volume released per unit of fuel burned (Nguyen et al. 2002).
    float gas_expansion = 2.0F;
    // Rate (1/s) at which oxygen is replenished toward fresh air.
    float gas_oxygen_rate = 3.0F;
    // Effective diffusivity is 0.3 of this (gas cells^2/s) through the normalised nine-point stencil.
    float gas_temperature_diffusion = 10.0F;
    float gas_combustion_rate = 10.0F;
    float gas_oxidizer_consumption = 1.5F;
    float gas_reaction_decay = 8.0F;
    float gas_smoke_yield = 0.35F;
    float gas_heat_release = 1.6F;
    float gas_temperature_cooling = 2.5F;
    float gas_smoke_dissipation = 0.08F;
    float gas_water_cooling = 2.4F;
    // V-cycle cap. Most solves stop after four; dense sand walls with fire took up to 48 in the combined replay.
    int gas_pressure_iterations = 48;
    // Zero pressure above the top edge: hot gas and smoke leave through the cave roof.
    bool gas_open_top = true;
    // About one half-float step of a face at the speed cap: R16F faces cannot hold a finer divergence.
    float gas_pressure_residual_threshold = 0.25F;
    float sand_gravity = 1962.0F;
    // Retained per 1/60 s: none; dissipation comes from plasticity, wall friction and the grid transfer.
    float sand_damping = 1.0F;
    float sand_restitution = 0.04F;
    float sand_max_velocity = 2400.0F;
    // Rate (1/s) at which fully submerged sand relaxes toward the water velocity; water feels the equal drag. Settling
    // speed is g (1 - 1/2.65) / drag: 20 cells/s = 10 cm/s, a dense cloud of 0.3-0.5 mm quartz grains.
    float sand_drag = 60.0F;
    // Quartz grain density over water: submerged grains lose 1/2.65 of their weight (buoyancy).
    float sand_grain_density = 2.65F;
    // Occupancy is mass / reference density (a full cell is 1): 0.225 is the 0.45 mass the gate used before.
    float sand_spawn_occupancy_limit = 0.225F;
    float sand_friction_coefficient = 0.62F;
    float sand_reference_density = 2.0F;
    // Scaled with gravity from 24000 at 240 cells/s^2 (x 1962/240), so a 100-cell column still compresses to J ~0.68.
    float sand_shear_modulus = 196000.0F;
    float sand_lame_lambda = 196000.0F;
    // Kelvin-Voigt viscosity (mass / (cell s)): grain contacts are inelastic, so elastic vibration of a resting bed dies
    // out (a deep fill otherwise rings for seconds above the sleep speed). Explicit: it adds diffusion substeps.
    float sand_viscosity = 200.0F;
    // Contact damping rate (1/s) of confined sand, ramped in from J 0.95 to full at J 0.85: a deep bed under its own
    // weight keeps its grains in enduring contact, which dissipates the bed's bulk elastic vibration (period ~2 s at
    // these moduli, beyond reach of the explicit viscosity). Free, loose and surface sand (J ~1) is untouched.
    float sand_contact_damping = 6.0F;
    // sin(phi) = sqrt(2) alpha in 2D: 0.5 is 45 degrees nominal, but poured piles measure ~30 (0.385 gives ~24).
    float sand_friction_alpha = 0.6F;
    // Per-grain friction spread (+-), as natural sand mixes grain shapes: each pile comes out irregular.
    float sand_friction_variation = 0.2F;
    // mu(I) rate-dependent friction (Jop et al. 2006): alpha scales by 1 + gain I / (I0 + I), I the inertial number of a
    // grain of diameter sand_grain_diameter (cells; 0.4 mm). Gain mu_2 / mu_s - 1 = tan 32.76 / tan 20.9 - 1, I0 0.279.
    float sand_friction_rate_gain = 0.68F;
    float sand_inertial_i0 = 0.279F;
    float sand_grain_diameter = 0.08F;
    // Wet-sand cohesion: the compression (log volume strain) the yield cone widens by at full capillary strength
    // (sand_particle_step.comp's capillary). At these soft moduli the shear strength c (lambda + mu) needs large elastic
    // strains: 0.3 keeps a damp 80-cell column 64 cells tall where dry sand slumps to 33 (sand-wet); 0.06-0.15 measured
    // no steeper than dry. Moisture rises toward the surrounding water fill at sand_wetting_rate and dries at
    // sand_drying_rate (1/s).
    float sand_cohesion = 0.3F;
    // Volume correction (Klar et al. 2016) remembers at most this dilation (log volume): dense over loose random packing,
    // ln(0.64 / 0.555).
    float sand_max_dilation = 0.14F;
    float sand_wetting_rate = 4.0F;
    float sand_drying_rate = 0.02F;
    // Material-wave CFL (stress waves per substep) and advective CFL (cells the fastest grain moves per substep): the
    // substep count is the larger of the two, not their sum.
    float sand_cfl_limit = 0.4F;
    float sand_advection_cfl = 1.0F;
    int sand_max_substeps = 64;
    // Sleep-packing: a grain slower than sand_sleep_speed (cells/s) for sand_sleep_ticks ticks is ready; a supported cell
    // whose grains are all ready becomes an integer count in the packed texture (0 ticks disables packing). A packed cell
    // wakes when it loses support, a side opens steeper than 45 degrees, sand nearby moves faster than
    // sand_wake_impact_speed, or full water nearby flows faster than sand_wake_water_speed (cells/s).
    float sand_sleep_speed = 4.0F;
    int sand_sleep_ticks = 12;
    float sand_wake_impact_speed = 60.0F;
    float sand_wake_water_speed = 40.0F;
    // Calm-water packing: a particle slower than water_sleep_speed (cells/s) for water_sleep_ticks ticks is ready; a cell
    // holding exactly the rest count of ready particles deep inside liquid becomes an integer count (0 ticks disables
    // packing). A packed cell wakes when a face of it moves faster than water_wake_speed (cells/s) after projection or
    // the surface, a wall or sand comes within two cells.
    float water_sleep_speed = 2.0F;
    int water_sleep_ticks = 30;
    float water_wake_speed = 5.0F;
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
    EnvToFloat("POWDER_WATER_ADVECTION_CFL", config.water_advection_cfl);
    EnvToFloat("POWDER_WATER_DAMPING", config.water_velocity_damping);
    EnvToFloat("POWDER_WATER_BULK_DAMPING", config.water_bulk_damping);
    EnvToFloat("POWDER_SAND_GRAVITY", config.sand_gravity);
    EnvToFloat("POWDER_SAND_ADVECTION_CFL", config.sand_advection_cfl);
    EnvToInt("POWDER_WATER_MIN_SUBSTEPS", config.water_min_substeps);
    EnvToInt("POWDER_WATER_MAX_SUBSTEPS", config.water_max_substeps);
    EnvToFloat("POWDER_WATER_GRAVITY", config.water_gravity);
    EnvToFloat("POWDER_WATER_MAX_VELOCITY", config.water_max_velocity);
    EnvToFloat("POWDER_WATER_DENSITY_RATE", config.water_density_rate);
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
    EnvToFloat("POWDER_SAND_VISCOSITY", config.sand_viscosity);
    EnvToFloat("POWDER_SAND_CONTACT_DAMPING", config.sand_contact_damping);
    EnvToFloat("POWDER_SAND_FRICTION_ALPHA", config.sand_friction_alpha);
    EnvToFloat("POWDER_SAND_FRICTION_RATE_GAIN", config.sand_friction_rate_gain);
    EnvToFloat("POWDER_SAND_COHESION", config.sand_cohesion);
    EnvToFloat("POWDER_SAND_MAX_DILATION", config.sand_max_dilation);
    EnvToFloat("POWDER_SAND_CFL", config.sand_cfl_limit);
    EnvToInt("POWDER_SAND_MAX_SUBSTEPS", config.sand_max_substeps);
    EnvToFloat("POWDER_SAND_SLEEP_SPEED", config.sand_sleep_speed);
    EnvToInt("POWDER_SAND_SLEEP_TICKS", config.sand_sleep_ticks);
    EnvToFloat("POWDER_SAND_WAKE_IMPACT_SPEED", config.sand_wake_impact_speed);
    EnvToFloat("POWDER_SAND_WAKE_WATER_SPEED", config.sand_wake_water_speed);
    EnvToFloat("POWDER_WATER_SLEEP_SPEED", config.water_sleep_speed);
    EnvToInt("POWDER_WATER_SLEEP_TICKS", config.water_sleep_ticks);
    EnvToFloat("POWDER_WATER_WAKE_SPEED", config.water_wake_speed);
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
    require(config.water_advection_cfl > 0.0F, "POWDER_WATER_ADVECTION_CFL");
    require(config.water_velocity_damping > 0.0F && config.water_velocity_damping <= 1.0F, "POWDER_WATER_DAMPING");
    require(config.water_bulk_damping >= 0.0F && config.water_bulk_damping <= 10.0F, "POWDER_WATER_BULK_DAMPING");
    require(config.sand_advection_cfl > 0.0F, "POWDER_SAND_ADVECTION_CFL");
    require(config.water_min_substeps >= 1, "POWDER_WATER_MIN_SUBSTEPS");
    require(config.water_max_substeps >= 1, "POWDER_WATER_MAX_SUBSTEPS");
    require(config.water_min_substeps <= config.water_max_substeps,
            "POWDER_WATER_MIN_SUBSTEPS / POWDER_WATER_MAX_SUBSTEPS");
    require(config.water_flip_blend >= 0.0F && config.water_flip_blend <= 1.0F, "POWDER_WATER_FLIP_BLEND");
    require(config.water_max_velocity > 0.0F, "POWDER_WATER_MAX_VELOCITY");
    require(config.water_density_rate >= 0.0F, "POWDER_WATER_DENSITY_RATE");
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
    require(config.sand_viscosity >= 0.0F, "POWDER_SAND_VISCOSITY");
    require(config.sand_contact_damping >= 0.0F, "POWDER_SAND_CONTACT_DAMPING");
    require(config.sand_friction_alpha >= 0.0F, "POWDER_SAND_FRICTION_ALPHA");
    require(config.sand_friction_rate_gain >= 0.0F, "POWDER_SAND_FRICTION_RATE_GAIN");
    require(config.sand_cohesion >= 0.0F, "POWDER_SAND_COHESION");
    require(config.sand_max_dilation >= 0.0F, "POWDER_SAND_MAX_DILATION");
    require(config.sand_cfl_limit > 0.0F, "POWDER_SAND_CFL");
    require(config.sand_max_substeps >= 1, "POWDER_SAND_MAX_SUBSTEPS");
    require(config.sand_sleep_speed >= 0.0F, "POWDER_SAND_SLEEP_SPEED");
    // The grain's still-tick timer saturates at 255 (sand_sleep.comp).
    require(config.sand_sleep_ticks >= 0 && config.sand_sleep_ticks <= 255, "POWDER_SAND_SLEEP_TICKS");
    require(config.sand_wake_impact_speed > 0.0F, "POWDER_SAND_WAKE_IMPACT_SPEED");
    require(config.sand_wake_water_speed > 0.0F, "POWDER_SAND_WAKE_WATER_SPEED");
    require(config.water_sleep_speed >= 0.0F, "POWDER_WATER_SLEEP_SPEED");
    // The particle still-tick timer saturates at 255 (water_sleep.comp).
    require(config.water_sleep_ticks >= 0 && config.water_sleep_ticks <= 255, "POWDER_WATER_SLEEP_TICKS");
    require(config.water_wake_speed > 0.0F, "POWDER_WATER_WAKE_SPEED");
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

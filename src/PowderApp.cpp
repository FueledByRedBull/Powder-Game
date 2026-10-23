#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "PowderApp.hpp"

#include "GLUtil.hpp"
#include "FixedStepClock.hpp"
#include "HudFont.hpp"

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr GLuint kEmptyMaterial = 0U;
constexpr GLuint kSolidMaterial = 2U;
// Intact wood; burning and charred wood use 256 and up (shaders/wood_burn.comp).
constexpr GLuint kWoodMaterial = 3U;
// Oil (crude or vegetable oil, 0.85-0.92 g/cm^3) is a liquid record tagged in state2.z. Bodies of it rise through water
// under the variable-density correction (1 / kOilDensity - 1) times the pressure acceleration on the local oil share,
// which vanishes in open air, and within a cell the liquids separate by a drift (water_particle_step.comp): 20 cells/s
// (10 cm/s) is the terminal rise of a 5 mm oil droplet in water (Schiller-Naumann drag: 3 cm/s at 1 mm, about
// 10 cm/s at 5 mm).
constexpr float kOilDensity = 0.87F;
constexpr float kOilDrift = 20.0F;
// Surface oil under flame burns at 2/s per record, so a 5-cell slick (20 records per column) is gone in about 3 s;
// surface water (and the edges of a stream) under full flame boils off at 2/s per record, a thin film each second: the
// dousing still quenches, with a visible plume.
constexpr float kOilBurnRate = 2.0F;
constexpr float kOilIgnitionTemperature = 0.3F;
constexpr float kWaterEvaporateRate = 2.0F;
// Water that meets burning oil flashes within a couple of ticks (mean life 1/30 s): a lit slick's surface is far above
// the boiling point, so water does not survive sinking into it (phase_particles.comp).
constexpr float kWaterFlashRate = 30.0F;
// Steam rises at 1 m/s (100 gas cells/s) through the gas and condenses within about 1.5 s.
constexpr float kSteamRise = 100.0F;
constexpr float kSteamCondense = 0.8F;
constexpr float kSteamPerParticle = 0.3F;
// Sand occupancy (2x2 mean) that smothers a gas cell's fire completely.
constexpr float kSandSmother = 0.35F;

constexpr std::array<const char*, 5> kDebugCounterNames{
    "nan_inf_detected",
    "particle_overflow_blocked",
    "pressure_nonconverged",
    "inactive_tile_misses",
    "water_velocity_clamped",
};
constexpr std::size_t kDebugFailureCounters = 4;  // the counters before water_velocity_clamped

// The last description GLFW reported, so a failed startup says why and not only that it failed.
std::string g_last_glfw_error;

void GlfwErrorCallback(int, const char* description) {
    g_last_glfw_error = description != nullptr ? description : "";
}

std::string WithGlfwError(std::string message) {
    if (!g_last_glfw_error.empty()) message += " GLFW: " + g_last_glfw_error;
    return message;
}

}  // namespace

int PowderApp::Run() {
    try {
        Initialize();

        if (replay_mode_) {
            const int replay_result = RunReplay();
            Shutdown();
            return replay_result;
        }

        // Adaptive vsync where available: a late frame tears instead of waiting a whole extra refresh.
        const bool tear_control = glfwExtensionSupported("WGL_EXT_swap_control_tear") == GLFW_TRUE ||
                                  glfwExtensionSupported("GLX_EXT_swap_control_tear") == GLFW_TRUE;
        glfwSwapInterval(tear_control ? -1 : 1);
        help_hide_time_ = glfwGetTime() + hud::kHelpSeconds;  // the panel greets a new session, then gets out of the way
        FixedStepClock clock;
        double previous_time = glfwGetTime();
        double title_time = previous_time;
        int title_frames = 0;

        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            UpdateInput();
            const double current_time = glfwGetTime();
            RunInteractiveFrame(clock, current_time - previous_time);
            previous_time = current_time;

            ++title_frames;
            if (current_time - title_time >= 0.5) {
                std::string material(hud::kBrushNames[static_cast<std::size_t>(brush_mode_)]);
                std::transform(material.begin() + 1, material.end(), material.begin() + 1,
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::ostringstream title;
                title << "Powder Game - " << material << ", brush " << brush_radius_ << " - "
                      << std::lround(static_cast<double>(title_frames) / (current_time - title_time)) << " fps";
                glfwSetWindowTitle(window_, title.str().c_str());
                title_time = current_time;
                title_frames = 0;
            }
        }

        Shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        Shutdown();
        return 1;
    }
}

// The body of the interactive loop after input, shared with the interactive profiling fixture.
int PowderApp::RunInteractiveFrame(FixedStepClock& clock, double elapsed) {
    const int steps = clock.Advance(elapsed, config_.fixed_dt);
    // Ticks end in blocking readbacks, so their wall time tracks their GPU cost.
    const double tick_start = glfwGetTime();
    for (int step = 0; step < steps; ++step) RunFrame(config_.fixed_dt, nullptr, step == 0);
    clock.MeasureTicks(glfwGetTime() - tick_start, steps);
    Render(true);
    DrawHud();
    glfwSwapBuffers(window_);
    return steps;
}

void PowderApp::Initialize() {
    config_ = powder_config::LoadConfigFromEnvironment();

    glfwSetErrorCallback(GlfwErrorCallback);
    if (glfwInit() == GLFW_FALSE) {
        throw std::runtime_error(WithGlfwError("Failed to initialize GLFW."));
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    replay_mode_ = std::getenv("POWDER_REPLAY") != nullptr;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // Replays keep the reference 1280x900. Interactive windows take 85% of the work area's height and just enough
    // width for the HUD to sit in the letterbox margin at its UI scale (1280x900 on a 1080p screen), centred.
    int width = 1280;
    int height = 900;
    int work_x = 0, work_y = 0, work_width = 0, work_height = 0;
    GLFWmonitor* monitor = replay_mode_ ? nullptr : glfwGetPrimaryMonitor();
    if (monitor != nullptr) glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_width, &work_height);
    if (work_width > 0 && work_height > 0) {
        height = std::max(450, static_cast<int>(0.85 * work_height));
        const int ui_scale = std::max(1, (height + 225) / 450);
        width = std::min(height + 2 * (2 * hud::kEdge + hud::kPanelWidth) * ui_scale, static_cast<int>(0.95 * work_width));
    }
    window_ = glfwCreateWindow(width, height, "Powder Game", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error(WithGlfwError("Failed to create GLFW window."));
    }
    if (work_width > 0 && work_height > 0) {
        glfwSetWindowPos(window_, work_x + (work_width - width) / 2, work_y + (work_height - height) / 2);
    }
    if (!replay_mode_) glfwShowWindow(window_);
    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, KeyCallback);
    glfwSetCursorPosCallback(window_, CursorPosCallback);
    glfwSetMouseButtonCallback(window_, MouseButtonCallback);
    glfwSetInputMode(window_, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);

    glfwMakeContextCurrent(window_);

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        throw std::runtime_error("Failed to load OpenGL functions through GLAD.");
    }

    if (GLAD_GL_VERSION_4_3 == 0) {
        throw std::runtime_error(WithGlfwError("OpenGL 4.3 is required."));
    }

    gl_ready_ = true;

    struct GpuLimit { GLenum parameter; GLint minimum; const char* name; };
    const std::array<GpuLimit, 4> limits{{
        {GL_MAX_IMAGE_UNITS, 18, "image units"},
        {GL_MAX_COMPUTE_IMAGE_UNIFORMS, 18, "compute image uniforms"},
        {GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES, 19, "combined shader output resources"},
        {GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, 8, "shader storage buffer bindings"}}};
    for (const auto& limit : limits) {
        GLint available = 0;
        glGetIntegerv(limit.parameter, &available);
        if (available < limit.minimum) {
            throw std::runtime_error(std::string("GPU supports ") + std::to_string(available) + " " + limit.name +
                                     "; Powder Game requires " + std::to_string(limit.minimum));
        }
    }

    glDisable(GL_DEPTH_TEST);

    glGenVertexArrays(1, &fullscreen_vao_);

    while (glGetError() != GL_NO_ERROR) {
    }  // drop stale errors so only the allocations below can raise one
    CreateTextures();
    CreateBuffers();
    bool out_of_memory = false;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        out_of_memory = out_of_memory || error == GL_OUT_OF_MEMORY;
    }
    if (out_of_memory) {
        constexpr long long kMiB = 1 << 20;
        throw std::runtime_error("The GPU ran out of memory allocating textures and buffers (water pool " +
                                 std::to_string(kMaxWaterParticles) + " particles, " +
                                 std::to_string(kMaxWaterParticles * 4LL * kWaterParticleComponents / kMiB) +
                                 " MiB; sand pool " + std::to_string(kMaxSandParticles) + " grains, " +
                                 std::to_string(kMaxSandParticles * 4LL * kSandParticleComponents / kMiB) + " MiB).");
    }
    CreatePrograms();
    InitializeState();
}

void PowderApp::Shutdown() {
    if (gl_ready_) {
        DeleteTexture(hud_font_texture_);
        DeleteProgram(hud_program_);
        DeleteProgram(spark_draw_program_);
        DeleteProgram(spark_step_program_);
        DeleteProgram(spark_pick_program_);
        DeleteProgram(light_build_program_);
        DeleteProgram(render_extract_program_);
        DeleteProgram(render_program_);
        DeleteProgram(coupling_heat_program_);
        DeleteProgram(coupling_fire_program_);
        DeleteProgram(fire_rd_program_);
        DeleteProgram(pressure_residual_program_);
        DeleteProgram(smoke_project_program_);
        DeleteProgram(gas_pressure_relax_program_);
        DeleteProgram(gas_pressure_restrict_program_);
        DeleteProgram(gas_pressure_kind_program_);
        DeleteProgram(gas_cg_program_);
        DeleteProgram(wood_burn_program_);
        DeleteProgram(smoke_divergence_program_);
        DeleteProgram(gas_face_to_center_program_);
        DeleteProgram(gas_velocity_correct_program_);
        DeleteProgram(gas_velocity_advect_program_);
        DeleteProgram(smoke_correct_program_);
        DeleteProgram(smoke_advect_program_);
        DeleteProgram(water_particle_p2g_program_);
        DeleteProgram(water_particle_finalize_program_);
        DeleteProgram(water_particle_step_program_);
        DeleteProgram(water_particle_clear_program_);
        DeleteProgram(water_particle_compact_program_);
        DeleteProgram(water_particle_spawn_program_);
        DeleteProgram(water_project_program_);
        DeleteProgram(pressure_cg_reduce_program_);
        DeleteProgram(pressure_cg_program_);
        DeleteProgram(pressure_pockets_program_);
        DeleteProgram(pressure_mg_program_);
        DeleteProgram(water_divergence_program_);
        DeleteProgram(water_mac_finalize_program_);
        DeleteProgram(water_mac_build_program_);
        DeleteProgram(boundary_downsample_program_);
        DeleteProgram(boundary_distance_program_);
        DeleteProgram(boundary_jumpflood_program_);
        DeleteProgram(boundary_seed_program_);
        DeleteProgram(boundary_build_program_);
        DeleteProgram(sand_rasterize_program_);
        DeleteProgram(sand_pack_program_);
        DeleteProgram(sand_sleep_program_);
        DeleteProgram(water_pack_program_);
        DeleteProgram(water_sleep_program_);
        DeleteProgram(sand_particle_step_program_);
        DeleteProgram(sand_grid_update_program_);
        DeleteProgram(sand_particle_p2g_program_);
        DeleteProgram(sand_particle_clear_program_);
        DeleteProgram(sand_particle_compact_program_);
        DeleteProgram(sand_particle_spawn_program_);
        DeleteProgram(spawn_gas_program_);
        DeleteProgram(spawn_full_program_);
        DeleteProgram(init_state_program_);
        DeleteBuffer(gas_state_.pressure_check);
        DeleteBuffer(debug_state_.counters);
        DeleteBuffer(phase_state_.cell_counts);
        DeleteBuffer(phase_state_.gas_sources);
        DeleteBuffer(phase_state_.sinks);
        DeleteBuffer(spark_state_.buckets);
        DeleteBuffer(spark_state_.particles);
        if (spark_state_.vao != 0U) {
            glDeleteVertexArrays(1, &spark_state_.vao);
            spark_state_.vao = 0;
        }
        DeleteTexture(phase_state_.oil_field);
        DeleteTexture(phase_state_.steam_a);
        DeleteTexture(phase_state_.steam_b);
        DeleteProgram(phase_particles_program_);
        DeleteProgram(oil_field_program_);
        DeleteProgram(steam_update_program_);
        DeleteBuffer(sand_state_.grid_moisture);
        DeleteBuffer(sand_state_.tile_flags);
        DeleteBuffer(sand_state_.grid_momentum_y);
        DeleteBuffer(sand_state_.grid_momentum_x);
        DeleteBuffer(sand_state_.grid_mass);
        DeleteBuffer(sand_state_.cell_fsum);
        DeleteBuffer(sand_state_.cell_tally);
        DeleteBuffer(water_state_.cell_tally);
        DeleteBuffer(sand_state_.spawn_cursor);
        DeleteBuffer(sand_state_.scratch_particles);
        DeleteBuffer(sand_state_.particles);
        DeleteBuffer(water_state_.v_face_velocity_sums);
        DeleteBuffer(water_state_.pressure_sums);
        DeleteBuffer(water_state_.pressure_levels);
        DeleteBuffer(water_state_.pressure_state);
        DeleteBuffer(water_state_.pockets);
        DeleteBuffer(water_state_.pressure_tiles);
        DeleteBuffer(water_state_.pressure_scalars);
        DeleteBuffer(water_state_.pressure_partials);
        DeleteBuffer(water_state_.pressure_vectors);
        DeleteBuffer(water_state_.u_face_velocity_sums);
        DeleteBuffer(water_state_.v_face_weights);
        DeleteBuffer(water_state_.u_face_weights);
        DeleteBuffer(water_state_.cell_counts);
        DeleteBuffer(water_state_.spawn_cursor);
        DeleteBuffer(water_state_.scratch_particles);
        DeleteBuffer(water_state_.bins);
        DeleteBuffer(water_state_.particles);

        DeleteTexture(gas_state_.reaction_b);
        DeleteTexture(gas_state_.reaction_a);
        DeleteTexture(gas_state_.reaction_tmp);
        DeleteTexture(gas_state_.oxidizer_b);
        DeleteTexture(gas_state_.oxidizer_a);
        DeleteTexture(gas_state_.oxidizer_tmp);
        DeleteTexture(gas_state_.temperature_b);
        DeleteTexture(gas_state_.temperature_a);
        DeleteTexture(gas_state_.temperature_tmp);
        DeleteTexture(gas_state_.smoke_b);
        DeleteTexture(gas_state_.smoke_a);
        DeleteTexture(gas_state_.smoke_tmp);
        DeleteTexture(gas_state_.fuel_b);
        DeleteTexture(gas_state_.fuel_a);
        DeleteTexture(gas_state_.fuel_tmp);
        for (GLuint& texture : gas_state_.level_kind) DeleteTexture(texture);
        for (GLuint& texture : gas_state_.level_pressure) DeleteTexture(texture);
        for (GLuint& texture : gas_state_.level_rhs) DeleteTexture(texture);
        for (GLuint& texture : gas_state_.level_residual) DeleteTexture(texture);
        for (GLuint& texture : gas_state_.level_smoothed) DeleteTexture(texture);
        for (GLuint* texture : {&gas_state_.cg_s, &gas_state_.cg_z, &gas_state_.cg_d, &gas_state_.cg_q}) DeleteTexture(*texture);
        DeleteBuffer(gas_state_.cg_partials);
        DeleteBuffer(gas_state_.cg_scalars);
        DeleteTexture(gas_state_.divergence);
        DeleteTexture(gas_state_.pressure_a);
        DeleteTexture(gas_state_.mask_solve);
        DeleteTexture(gas_state_.velocity_center_tmp);
        DeleteTexture(gas_state_.velocity_center_b);
        DeleteTexture(gas_state_.velocity_center_a);
        DeleteTexture(gas_state_.mac_v_tmp);
        DeleteTexture(gas_state_.mac_v_b);
        DeleteTexture(gas_state_.mac_v_a);
        DeleteTexture(gas_state_.mac_u_tmp);
        DeleteTexture(gas_state_.mac_u_b);
        DeleteTexture(gas_state_.mac_u_a);

        DeleteTexture(water_state_.pressure_a);
        DeleteTexture(water_state_.divergence);
        DeleteTexture(water_state_.liquid_phi);
        DeleteTexture(water_state_.liquid_volume);
        DeleteTexture(water_state_.mac_v_prev);
        DeleteTexture(water_state_.mac_u_prev);
        DeleteTexture(water_state_.mac_v);
        DeleteTexture(water_state_.mac_u);
        DeleteTexture(water_state_.render_velocity);
        DeleteTexture(water_state_.amount);
        DeleteTexture(light_texture_);
        DeleteTexture(render_gas_velocity_);
        DeleteTexture(render_temperature_);
        DeleteTexture(render_fire_emissive_);
        DeleteTexture(render_smoke_density_);

        DeleteTexture(material_a_);
        DeleteTexture(sand_state_.grid_velocity);
        DeleteTexture(sand_state_.occupancy);
        DeleteTexture(sand_state_.moisture);
        DeleteTexture(sand_state_.packed_f);
        DeleteTexture(sand_state_.packed);
        DeleteTexture(water_state_.wake_velocity);
        DeleteTexture(water_state_.packed);
        DeleteTexture(boundary_state_.mask_gas);
        DeleteTexture(boundary_state_.mask_static);
        DeleteTexture(boundary_state_.mask_full);
        DeleteTexture(boundary_state_.seed_full_b);
        DeleteTexture(boundary_state_.seed_full_a);
        DeleteTexture(boundary_state_.sdf_static);

        if (fullscreen_vao_ != 0U) {
            glDeleteVertexArrays(1, &fullscreen_vao_);
            fullscreen_vao_ = 0;
        }

        if (timing_queries_[0] != 0U) {
            glDeleteQueries(static_cast<GLsizei>(timing_queries_.size()), timing_queries_.data());
            timing_queries_.fill(0U);
        }
        if (replay_frame_queries_[0] != 0U) {
            glDeleteQueries(static_cast<GLsizei>(replay_frame_queries_.size()), replay_frame_queries_.data());
            replay_frame_queries_.fill(0U);
        }

        gl_ready_ = false;
    }

    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    glfwTerminate();
}

void PowderApp::CreateTextures() {
    material_a_ = CreateTexture(kGridWidth, kGridHeight, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST);

    water_state_.liquid_volume = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    water_state_.liquid_phi = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    water_state_.amount = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    water_state_.render_velocity = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    water_state_.mac_u = CreateTexture(kGridWidth + 1, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    water_state_.mac_v = CreateTexture(kGridWidth, kGridHeight + 1, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    water_state_.mac_u_prev = CreateTexture(kGridWidth + 1, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    water_state_.mac_v_prev = CreateTexture(kGridWidth, kGridHeight + 1, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    water_state_.pressure_a = CreateTexture(kGridWidth, kGridHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    water_state_.divergence = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);

    gas_state_.mac_u_a = CreateTexture(kGasWidth + 1, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mac_u_b = CreateTexture(kGasWidth + 1, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mac_u_tmp = CreateTexture(kGasWidth + 1, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mac_v_a = CreateTexture(kGasWidth, kGasHeight + 1, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mac_v_b = CreateTexture(kGasWidth, kGasHeight + 1, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mac_v_tmp = CreateTexture(kGasWidth, kGasHeight + 1, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.velocity_center_a = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.velocity_center_b = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.velocity_center_tmp = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);

    gas_state_.pressure_a = CreateTexture(kGasWidth, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.mask_solve = CreateTexture(kGasWidth, kGasHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    gas_state_.divergence = CreateTexture(kGasWidth, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    for (int level = 0, size = kGasWidth; level < kGasLevels; ++level, size = (size + 1) / 2) {
        gas_state_.level_kind[level] = CreateTexture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
        if (level + 1 < kGasLevels) {
            gas_state_.level_residual[level] = CreateTexture(size, size, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
            gas_state_.level_smoothed[level] = CreateTexture(size, size, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
        }
        if (level == 0) continue;
        gas_state_.level_pressure[level - 1] = CreateTexture(size, size, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
        gas_state_.level_rhs[level - 1] = CreateTexture(size, size, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    }
    for (GLuint* texture : {&gas_state_.cg_s, &gas_state_.cg_z, &gas_state_.cg_d, &gas_state_.cg_q})
        *texture = CreateTexture(kGasWidth, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);

    gas_state_.smoke_a = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.smoke_b = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.smoke_tmp = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.temperature_a = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.temperature_b = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.temperature_tmp = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.fuel_a = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.fuel_b = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.fuel_tmp = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.oxidizer_a = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.oxidizer_b = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.oxidizer_tmp = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.reaction_a = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.reaction_b = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.reaction_tmp = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    render_smoke_density_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    render_fire_emissive_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    render_temperature_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    render_gas_velocity_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    phase_state_.oil_field = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    phase_state_.steam_a = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    phase_state_.steam_b = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    // Emitted light with a full mip chain; composite.frag reads explicit levels, so each is filtered bilinearly.
    glGenTextures(1, &light_texture_);
    glBindTexture(GL_TEXTURE_2D, light_texture_);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(std::bit_width(static_cast<unsigned>(std::max(kLightWidth, kLightHeight)))),
                   GL_RGBA16F, kLightWidth, kLightHeight);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    sand_state_.occupancy = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    sand_state_.moisture = CreateTexture(kGridWidth, kGridHeight, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR);
    sand_state_.packed = CreateTexture(kGridWidth, kGridHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    sand_state_.packed_f = CreateTexture(kGridWidth, kGridHeight, GL_RGBA32F, GL_RGBA, GL_FLOAT, GL_NEAREST);
    water_state_.packed = CreateTexture(kGridWidth, kGridHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    water_state_.wake_velocity = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);
    sand_state_.grid_velocity =
        CreateTexture(kGridWidth + 2, kGridHeight + 2, GL_RG32F, GL_RG, GL_FLOAT, GL_NEAREST);

    boundary_state_.sdf_static =
        CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    boundary_state_.mask_full = CreateTexture(kGridWidth, kGridHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    boundary_state_.mask_static =
        CreateTexture(kGridWidth, kGridHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    boundary_state_.mask_gas = CreateTexture(kGasWidth, kGasHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    boundary_state_.seed_full_a =
        CreateTexture(kGridWidth, kGridHeight, GL_RG32I, GL_RG_INTEGER, GL_INT, GL_NEAREST);
    boundary_state_.seed_full_b =
        CreateTexture(kGridWidth, kGridHeight, GL_RG32I, GL_RG_INTEGER, GL_INT, GL_NEAREST);
}

void PowderApp::CreateBuffers() {
    const GLsizeiptr count_bytes = static_cast<GLsizeiptr>(sizeof(GLuint));
    const GLsizeiptr water_particles_bytes =
        static_cast<GLsizeiptr>(kMaxWaterParticles) * static_cast<GLsizeiptr>(sizeof(float)) * kWaterParticleComponents;
    const GLsizeiptr sand_particles_bytes =
        static_cast<GLsizeiptr>(kMaxSandParticles) * static_cast<GLsizeiptr>(sizeof(float)) * kSandParticleComponents;
    const GLsizeiptr water_count_bytes = static_cast<GLsizeiptr>(CellCount() * static_cast<int>(sizeof(GLuint)));
    const GLsizeiptr sand_grid_uint_bytes =
        static_cast<GLsizeiptr>((kGridWidth + 2) * (kGridHeight + 2) * static_cast<int>(sizeof(GLuint)));
    const GLsizeiptr sand_grid_int_bytes =
        static_cast<GLsizeiptr>((kGridWidth + 2) * (kGridHeight + 2) * static_cast<int>(sizeof(GLint) * 2));
    const GLsizeiptr water_u_face_weight_bytes =
        static_cast<GLsizeiptr>((kGridWidth + 1) * kGridHeight * static_cast<int>(sizeof(GLuint)));
    const GLsizeiptr water_v_face_weight_bytes =
        static_cast<GLsizeiptr>(kGridWidth * (kGridHeight + 1) * static_cast<int>(sizeof(GLuint)));
    const GLsizeiptr water_u_face_velocity_bytes =
        static_cast<GLsizeiptr>((kGridWidth + 1) * kGridHeight * static_cast<int>(sizeof(GLint) * 2));
    const GLsizeiptr water_v_face_velocity_bytes =
        static_cast<GLsizeiptr>(kGridWidth * (kGridHeight + 1) * static_cast<int>(sizeof(GLint) * 2));
    const GLsizeiptr debug_counter_bytes = static_cast<GLsizeiptr>(sizeof(GLuint) * 5);
    // GL 4.3 only promises 16 MB per storage block; the particle pools are one block each.
    GLint64 max_block_bytes = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_block_bytes);
    if (max_block_bytes < std::max<GLint64>(water_particles_bytes, sand_particles_bytes)) {
        throw std::runtime_error("GL_MAX_SHADER_STORAGE_BLOCK_SIZE is " + std::to_string(max_block_bytes) +
                                 " bytes; the particle pools need " +
                                 std::to_string(std::max<GLint64>(water_particles_bytes, sand_particles_bytes)) +
                                 " (lower kMaxWaterParticles / kMaxSandParticles for this GPU)");
    }

    glGenBuffers(1, &gas_state_.pressure_check);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gas_state_.pressure_check);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(((kGasWidth + 15) / 16) * ((kGasHeight + 15) / 16) * sizeof(float) * 4),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &gas_state_.cg_partials);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gas_state_.cg_partials);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(((kGasWidth + 15) / 16) * ((kGasHeight + 15) / 16) * sizeof(float) * 4),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &gas_state_.cg_scalars);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gas_state_.cg_scalars);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * 4, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.pressure_vectors);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_vectors);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(CellCount() * sizeof(float) * 4), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_partials);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_partials);
    const int pressure_partial_count = std::max((CellCount() + 255) / 256,
                                               ((kGridWidth + 15) / 16) * ((kGridHeight + 15) / 16));
    // Also the water CG's per-cell (direction, A*direction) scratch between its phases 1 and 2 (pressure_cg.comp).
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(std::max(pressure_partial_count * 8, CellCount() * 2) * sizeof(float)),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_scalars);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_scalars);
    // The solver's scalars, then the restart's arming flag and indirect arguments (pressure_cg.comp).
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(kWaterRestartDispatchOffset + sizeof(GLuint) * 4),
                 nullptr, GL_DYNAMIC_DRAW);
    const int tile_slots = ((kGridWidth + 15) / 16) * ((kGridHeight + 15) / 16);
    glGenBuffers(1, &water_state_.pressure_tiles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_tiles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(GLuint) * (8 + tile_slots)), nullptr,
                 GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_sums);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_sums);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 12 * tile_slots), nullptr,
                 GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_state);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_state);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(GLuint) * 4), nullptr, GL_STREAM_READ);
    glGenBuffers(1, &water_state_.pockets);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pockets);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(CellCount()) * 16, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_levels);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_levels);
    // Eight indirect argument and eight box slots, then a vec4 per coarse cell (pressure_mg.comp).
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(GLuint) * 64 + kWaterMultigridCells * 16),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.scratch_particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.scratch_particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.bins);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.bins);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(GLuint) * kWaterBinCount), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.spawn_cursor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    // The live count, then the fastest particle speed's float bits (see water_particle_compact.comp).
    glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * count_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.cell_counts);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.cell_counts);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_count_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.u_face_weights);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_weights);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_u_face_weight_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.v_face_weights);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_weights);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_v_face_weight_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.u_face_velocity_sums);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_velocity_sums);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_u_face_velocity_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.v_face_velocity_sums);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_velocity_sums);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_v_face_velocity_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.scratch_particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.scratch_particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.spawn_cursor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    // The live count, the fastest grain speed's float bits and the live cell box (see sand_particle_compact.comp).
    glBufferData(GL_SHADER_STORAGE_BUFFER, 6 * count_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.cell_tally);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.cell_tally);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_count_bytes, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.cell_tally);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.cell_tally);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_count_bytes, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &sand_state_.cell_fsum);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.cell_fsum);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4 * water_count_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_mass);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_uint_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_moisture);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_moisture);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_uint_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.tile_flags);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.tile_flags);
    const int sand_tile_rows = (kGridHeight + 2 + kWorkgroupSize - 1) / kWorkgroupSize;
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(kSandTileStride * sand_tile_rows * sizeof(GLuint)),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_momentum_x);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_x);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_int_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_momentum_y);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_y);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_int_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &debug_state_.counters);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
    glBufferData(GL_SHADER_STORAGE_BUFFER, debug_counter_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &phase_state_.cell_counts);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, phase_state_.cell_counts);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * water_count_bytes, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &phase_state_.gas_sources);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, phase_state_.gas_sources);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(2 * kGasWidth * kGasHeight * sizeof(GLuint)), nullptr,
                 GL_DYNAMIC_DRAW);
    glGenBuffers(1, &phase_state_.sinks);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, phase_state_.sinks);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * count_bytes, nullptr, GL_DYNAMIC_DRAW);

    // Sparks start dead (life 0): zeroed here, and again by InitializeState.
    const std::vector<float> no_sparks(static_cast<std::size_t>(kSparkCapacity) * 8U, 0.0F);
    const std::vector<GLuint> no_buckets(static_cast<std::size_t>(kSparkSpawnsPerTick), 0U);
    glGenBuffers(1, &spark_state_.particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, spark_state_.particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(no_sparks.size() * sizeof(float)), no_sparks.data(),
                 GL_DYNAMIC_DRAW);
    glGenBuffers(1, &spark_state_.buckets);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, spark_state_.buckets);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(no_buckets.size() * sizeof(GLuint)), no_buckets.data(),
                 GL_DYNAMIC_DRAW);
    // The same buffer, one record per instance: motion at offset 0, life at 16.
    glGenVertexArrays(1, &spark_state_.vao);
    glBindVertexArray(spark_state_.vao);
    glBindBuffer(GL_ARRAY_BUFFER, spark_state_.particles);
    for (GLuint attribute = 0; attribute < 2U; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(8 * sizeof(float)),
                              reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute * 4U * sizeof(float))));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenQueries(static_cast<GLsizei>(timing_queries_.size()), timing_queries_.data());
    glGenQueries(static_cast<GLsizei>(replay_frame_queries_.size()), replay_frame_queries_.data());
}

void PowderApp::CreatePrograms() {
    init_state_program_ = glutil::CreateComputeProgram("init_state.comp");
    spawn_full_program_ = glutil::CreateComputeProgram("spawn_full.comp");
    spawn_gas_program_ = glutil::CreateComputeProgram("spawn_smoke.comp");
    sand_particle_spawn_program_ =
        glutil::CreateComputeProgram("sand_particle_spawn.comp");
    sand_particle_compact_program_ =
        glutil::CreateComputeProgram("sand_particle_compact.comp");
    sand_particle_clear_program_ =
        glutil::CreateComputeProgram("sand_particle_clear.comp");
    sand_particle_p2g_program_ = glutil::CreateComputeProgram("sand_particle_p2g.comp");
    sand_grid_update_program_ = glutil::CreateComputeProgram("sand_grid_update.comp");
    sand_particle_step_program_ =
        glutil::CreateComputeProgram("sand_particle_step.comp");
    sand_rasterize_program_ = glutil::CreateComputeProgram("sand_rasterize.comp");
    sand_sleep_program_ = glutil::CreateComputeProgram("sand_sleep.comp");
    sand_pack_program_ = glutil::CreateComputeProgram("sand_pack.comp");
    water_sleep_program_ = glutil::CreateComputeProgram("water_sleep.comp");
    water_pack_program_ = glutil::CreateComputeProgram("water_pack.comp");
    boundary_build_program_ = glutil::CreateComputeProgram("boundary_build.comp");
    boundary_downsample_program_ =
        glutil::CreateComputeProgram("boundary_downsample.comp");
    boundary_seed_program_ = glutil::CreateComputeProgram("boundary_seed.comp");
    boundary_jumpflood_program_ =
        glutil::CreateComputeProgram("boundary_jumpflood.comp");
    boundary_distance_program_ =
        glutil::CreateComputeProgram("boundary_distance.comp");
    water_particle_p2g_program_ =
        glutil::CreateComputeProgram("water_particle_p2g.comp");
    water_particle_spawn_program_ =
        glutil::CreateComputeProgram("water_particle_spawn.comp");
    water_particle_compact_program_ =
        glutil::CreateComputeProgram("water_particle_compact.comp");
    water_particle_clear_program_ =
        glutil::CreateComputeProgram("water_particle_clear.comp");
    water_particle_step_program_ =
        glutil::CreateComputeProgram("water_particle_step.comp");
    water_particle_finalize_program_ =
        glutil::CreateComputeProgram("water_particle_finalize.comp");
    water_mac_build_program_ = glutil::CreateComputeProgram("water_mac_build.comp");
    water_mac_finalize_program_ =
        glutil::CreateComputeProgram("water_mac_finalize.comp");
    water_divergence_program_ =
        glutil::CreateComputeProgram("water_divergence.comp");
    pressure_cg_program_ = glutil::CreateComputeProgram("pressure_cg.comp");
    pressure_pockets_program_ = glutil::CreateComputeProgram("pressure_pockets.comp");
    pressure_cg_reduce_program_ = glutil::CreateComputeProgram("pressure_cg_reduce.comp");
    pressure_mg_program_ = glutil::CreateComputeProgram("pressure_mg.comp");
    water_project_program_ = glutil::CreateComputeProgram("water_project.comp");

    smoke_advect_program_ = glutil::CreateComputeProgram("smoke_advect.comp");
    smoke_correct_program_ = glutil::CreateComputeProgram("smoke_correct.comp");
    gas_velocity_advect_program_ =
        glutil::CreateComputeProgram("gas_velocity_advect.comp");
    gas_velocity_correct_program_ =
        glutil::CreateComputeProgram("gas_velocity_correct.comp");
    gas_face_to_center_program_ =
        glutil::CreateComputeProgram("gas_face_to_center.comp");
    smoke_divergence_program_ = glutil::CreateComputeProgram("smoke_divergence.comp");
    gas_pressure_kind_program_ = glutil::CreateComputeProgram("gas_pressure_kind.comp");
    gas_cg_program_ = glutil::CreateComputeProgram("gas_cg.comp");
    wood_burn_program_ = glutil::CreateComputeProgram("wood_burn.comp");
    phase_particles_program_ = glutil::CreateComputeProgram("phase_particles.comp");
    oil_field_program_ = glutil::CreateComputeProgram("oil_field.comp");
    steam_update_program_ = glutil::CreateComputeProgram("steam_update.comp");
    gas_pressure_restrict_program_ =
        glutil::CreateComputeProgram("gas_pressure_restrict.comp");
    gas_pressure_relax_program_ =
        glutil::CreateComputeProgram("gas_pressure_relax.comp");
    smoke_project_program_ = glutil::CreateComputeProgram("smoke_project.comp");
    pressure_residual_program_ = glutil::CreateComputeProgram("pressure_residual.comp");

    fire_rd_program_ = glutil::CreateComputeProgram("fire_rd.comp");

    coupling_fire_program_ = glutil::CreateComputeProgram("coupling_fire.comp");
    coupling_heat_program_ = glutil::CreateComputeProgram("coupling_heat.comp");
    render_extract_program_ = glutil::CreateComputeProgram("render_extract.comp");
    light_build_program_ = glutil::CreateComputeProgram("light_build.comp");
    spark_pick_program_ = glutil::CreateComputeProgram("spark_pick.comp");
    spark_step_program_ = glutil::CreateComputeProgram("spark_step.comp");
    spark_draw_program_ = glutil::CreateProgram("spark.vert", "spark.frag");

    render_program_ = glutil::CreateProgram("fullscreen.vert", "composite.frag");

    hud_program_ = glutil::CreateProgram("fullscreen.vert", "hud.frag");
    hud_font_texture_ =
        CreateTexture(hud::kAtlasWidth, hud::kAtlasHeight, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST);
    const std::vector<std::uint8_t> atlas = hud::BakeAtlas();
    GLint previous_unpack_alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, hud_font_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud::kAtlasWidth, hud::kAtlasHeight, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
}

void PowderApp::InitializeState() {
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    gas_mac_ping_ = smoke_ping_ = true;
    frame_index_ = 0U;
    boundary_state_.static_dirty = true;
    wood_present_ = false;
    oil_present_ = heat_present_ = false;
    phase_state_.steam_ping = true;
    // The gas solve's check schedule follows the last solve, so a stale count makes repeated replays diverge.
    gas_state_.pressure_iterations = water_state_.pressure_iterations = 0;
    gas_state_.solve_pending = gas_state_.solve_running = gas_state_.solve_started = false;

    glUseProgram(init_state_program_);
    glUniform2i(CachedUniform(init_state_program_, "gridSize"), kGridWidth, kGridHeight);

    glBindImageTexture(0, material_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16UI);
    glBindImageTexture(3, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(4, water_state_.render_velocity, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    const GLuint zero_uint = 0U;
    const GLint zero_int = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.scratch_particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.cell_counts);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_weights);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_weights);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_velocity_sums);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_velocity_sums);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.scratch_particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.cell_fsum);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.tile_flags);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_x);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_y);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.cell_tally);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    sand_state_.live_count = sand_state_.record_bound = 0U;
    water_state_.live_count = water_state_.record_bound = 0U;
    sand_state_.node_box = {0, 0, kGridWidth + 2, kGridHeight + 2};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    for (GLuint buffer : {phase_state_.cell_counts, phase_state_.gas_sources, phase_state_.sinks, spark_state_.particles,
                          spark_state_.buckets}) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    const std::vector<std::uint16_t> zero_r16(static_cast<std::size_t>((kGridWidth + 1) * (kGridHeight + 1)), 0U);
    const std::vector<std::uint16_t> zero_rg16(static_cast<std::size_t>((kGridWidth + 2) * (kGridHeight + 2) * 2), 0U);
    const std::vector<float> zero_rg32(static_cast<std::size_t>((kGridWidth + 2) * (kGridHeight + 2) * 2), 0.0F);
    const std::vector<float> air_phi(static_cast<std::size_t>(kGridWidth * kGridHeight), 1.0F);
    const std::vector<std::uint16_t> zero_r16_gas(static_cast<std::size_t>(kGasWidth * kGasHeight), 0U);
    const std::vector<std::uint16_t> zero_rg16_gas(static_cast<std::size_t>(kGasWidth * kGasHeight * 2), 0U);
    const std::vector<std::uint16_t> zero_r16_gas_u(static_cast<std::size_t>((kGasWidth + 1) * kGasHeight), 0U);
    const std::vector<std::uint16_t> zero_r16_gas_v(static_cast<std::size_t>(kGasWidth * (kGasHeight + 1)), 0U);

    GLint previous_unpack_alignment = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, sand_state_.occupancy);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, sand_state_.grid_velocity);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth + 2, kGridHeight + 2, GL_RG, GL_FLOAT, zero_rg32.data());
    {
        const std::vector<float> zero_rgba32(static_cast<std::size_t>(kGridWidth * kGridHeight * 4), 0.0F);
        glBindTexture(GL_TEXTURE_2D, sand_state_.packed_f);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RGBA, GL_FLOAT, zero_rgba32.data());
        const std::vector<std::uint8_t> zero_r8(static_cast<std::size_t>(kGridWidth * kGridHeight), 0U);
        glBindTexture(GL_TEXTURE_2D, sand_state_.packed);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED_INTEGER, GL_UNSIGNED_BYTE, zero_r8.data());
        glBindTexture(GL_TEXTURE_2D, water_state_.packed);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED_INTEGER, GL_UNSIGNED_BYTE, zero_r8.data());
        glBindTexture(GL_TEXTURE_2D, sand_state_.moisture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_UNSIGNED_BYTE, zero_r8.data());
    }
    glBindTexture(GL_TEXTURE_2D, water_state_.amount);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, water_state_.liquid_phi);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_FLOAT, air_phi.data());
    for (GLuint texture : {water_state_.pressure_a, water_state_.divergence}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    }
    for (GLuint texture : {water_state_.mac_u, water_state_.mac_u_prev}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth + 1, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    }
    for (GLuint texture : {water_state_.mac_v, water_state_.mac_v_prev}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight + 1, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    }
    glBindTexture(GL_TEXTURE_2D, phase_state_.oil_field);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RG, GL_HALF_FLOAT, zero_rg16.data());
    for (GLuint texture : {phase_state_.steam_a, phase_state_.steam_b}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    }
    glBindTexture(GL_TEXTURE_2D, gas_state_.fuel_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.fuel_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.fuel_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.velocity_center_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.velocity_center_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.velocity_center_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_u_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth + 1, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas_u.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_u_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth + 1, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas_u.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_u_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth + 1, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas_u.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_v_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight + 1, GL_RED, GL_HALF_FLOAT, zero_r16_gas_v.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_v_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight + 1, GL_RED, GL_HALF_FLOAT, zero_r16_gas_v.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.mac_v_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight + 1, GL_RED, GL_HALF_FLOAT, zero_r16_gas_v.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.smoke_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.smoke_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.smoke_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.temperature_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.temperature_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.temperature_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    // Oxidizer is the oxygen fraction relative to fresh air: the cave starts full of air.
    const std::vector<std::uint16_t> air_r16_gas(static_cast<std::size_t>(kGasWidth * kGasHeight), 0x3C00U);
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, air_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, air_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, air_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    // Coarse levels: zero is a valid correction and the kinds are rebuilt by every solve.
    for (int level = 1, size = (kGasWidth + 1) / 2; level < kGasLevels; ++level, size = (size + 1) / 2) {
        for (GLuint texture : {gas_state_.level_pressure[level - 1], gas_state_.level_rhs[level - 1]}) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
        }
    }
    glBindTexture(GL_TEXTURE_2D, render_smoke_density_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, render_temperature_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, render_fire_emissive_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, render_gas_velocity_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RG, GL_HALF_FLOAT, zero_rg16.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    RunBoundaryPass();
}

void PowderApp::KeyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    auto* app = static_cast<PowderApp*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || app->replay_mode_) return;
    if (key == GLFW_KEY_LEFT_BRACKET) app->brush_radius_ = std::max(1, app->brush_radius_ - 1);
    if (key == GLFW_KEY_RIGHT_BRACKET) app->brush_radius_ = std::min(96, app->brush_radius_ + 1);
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (key == GLFW_KEY_H) {
        app->help_visible_ = !app->help_visible_;
        app->help_hide_time_ = app->help_visible_ ? glfwGetTime() + hud::kHelpSeconds : -1.0;
    }
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_8)
        app->brush_mode_ = static_cast<BrushMode>(key - GLFW_KEY_1);
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F10)
        app->render_mode_ = static_cast<RenderMode>(key - GLFW_KEY_F1);
}

// Cursor events arrive at event rate, between rendered frames and fixed ticks. Only cells in the simulation join
// the trail; an event outside it ends the stroke, so leaving and re-entering between ticks does not connect.
// Windows coalesces the mouse moves between two events into the last one; the moves it coalesced come back from its
// mouse-move history, so a fast stroke keeps its curve even at a low frame rate. Elsewhere the events alone count.
void PowderApp::CursorPosCallback(GLFWwindow* window, double x, double y) {
    auto* app = static_cast<PowderApp*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || app->replay_mode_) return;
    int window_width = 1;
    int window_height = 1;
    glfwGetWindowSize(window, &window_width, &window_height);
    int framebuffer_width = 1;
    int framebuffer_height = 1;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    auto& trail = app->cursor_trail_;
    const auto add = [&](double cursor_x, double cursor_y) {
        std::array<int, 2> cell{};
        if (!MapCursorToGrid(cursor_x, cursor_y, window_width, window_height, framebuffer_width, framebuffer_height, cell[0], cell[1])) {
            app->stroke_.active = false;
            trail.clear();
            return;
        }
        if (!trail.empty() && trail.back() == cell) return;
        // shortcut: past kMaxCursorTrail unstamped events the newest overwrites the last slot, cutting the path straight; keep a ring only if stalls that long matter.
        if (trail.size() == kMaxCursorTrail) trail.back() = cell;
        else trail.push_back(cell);
    };
#ifdef _WIN32
    if (HWND hwnd = glfwGetWin32Window(window); hwnd != nullptr) {
        POINT at{static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y))};
        ClientToScreen(hwnd, &at);
        MOUSEMOVEPOINT query{};
        query.x = at.x & 0xFFFF;
        query.y = at.y & 0xFFFF;
        query.time = static_cast<DWORD>(GetMessageTime());
        std::array<MOUSEMOVEPOINT, 64> raw{};
        const int count = GetMouseMovePointsEx(sizeof(MOUSEMOVEPOINT), &query, raw.data(), static_cast<int>(raw.size()),
                                               GMMP_USE_DISPLAY_POINTS);
        std::vector<MovePoint> history;
        for (int i = 0; i < count; ++i) {
            // Display points are 16-bit; monitors left of or above the primary one have negative coordinates.
            history.push_back({raw[i].x > 32767 ? raw[i].x - 65536 : raw[i].x, raw[i].y > 32767 ? raw[i].y - 65536 : raw[i].y,
                               static_cast<std::uint32_t>(raw[i].time)});
        }
        if (!history.empty()) {
            if (app->last_move_valid_) {
                for (const MovePoint& move : MovesSince(history, app->last_move_)) {
                    POINT client{move.x, move.y};
                    ScreenToClient(hwnd, &client);
                    add(client.x + 0.5, client.y + 0.5);
                }
            }
            app->last_move_ = history.front();
            app->last_move_valid_ = true;
        }
    }
#endif
    add(x, y);
}

// Of a mouse-move history listed newest first, starting with the current event, the moves after `last` and before the
// current event, oldest first; none when `last` is not in the history (then the gap is bridged by a straight line).
std::vector<PowderApp::MovePoint> PowderApp::MovesSince(const std::vector<MovePoint>& newest_first, const MovePoint& last) {
    for (std::size_t i = 1; i < newest_first.size(); ++i) {
        if (newest_first[i] == last) return {newest_first.rend() - static_cast<std::ptrdiff_t>(i), newest_first.rend() - 1};
    }
    return {};
}

// A press starts the stroke at the cursor, so motion before the first tick that sees the button is not lost.
void PowderApp::MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    auto* app = static_cast<PowderApp*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || app->replay_mode_ || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    app->stroke_.active = false;
    app->cursor_trail_.clear();
    app->last_move_valid_ = false;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    CursorPosCallback(window, x, y);
}

void PowderApp::UpdateInput() {
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window_, &cursor_x, &cursor_y);

    int window_width = 1;
    int window_height = 1;
    glfwGetWindowSize(window_, &window_width, &window_height);
    int framebuffer_width = 1;
    int framebuffer_height = 1;
    glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);

    // GLFW keeps reporting the last in-window position after the cursor leaves, so also require hover.
    cursor_in_sim_ = MapCursorToGrid(cursor_x, cursor_y, window_width, window_height, framebuffer_width,
                                     framebuffer_height, brush_x_, brush_y_) &&
                     glfwGetWindowAttrib(window_, GLFW_HOVERED) == GLFW_TRUE;
    painting_ = cursor_in_sim_ && glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
}

// Largest grid-aspect rectangle centred in the framebuffer; the letterboxed margins stay unpainted.
PowderApp::SimRect PowderApp::FitSimRect(int framebuffer_width, int framebuffer_height) {
    SimRect rect;
    rect.width = std::min(framebuffer_width, framebuffer_height * kGridWidth / kGridHeight);
    rect.height = std::min(framebuffer_height, framebuffer_width * kGridHeight / kGridWidth);
    rect.x = (framebuffer_width - rect.width) / 2;
    rect.y = (framebuffer_height - rect.height) / 2;
    return rect;
}

// Cursor coordinates are window units from the top-left; the viewport is framebuffer pixels, so scale
// by the DPI ratio first. The grid cell is always clamped to the grid; the result says whether the
// cursor is actually over the simulation viewport.
bool PowderApp::MapCursorToGrid(double cursor_x,
                                double cursor_y,
                                int window_width,
                                int window_height,
                                int framebuffer_width,
                                int framebuffer_height,
                                int& grid_x,
                                int& grid_y) {
    const SimRect rect = FitSimRect(framebuffer_width, framebuffer_height);
    if (window_width <= 0 || window_height <= 0 || rect.width <= 0 || rect.height <= 0) return false;

    const double u = (cursor_x * framebuffer_width / window_width - rect.x) / rect.width;
    const double top_down_v =
        (cursor_y * framebuffer_height / window_height - (framebuffer_height - rect.y - rect.height)) / rect.height;
    grid_x = static_cast<int>(std::clamp(std::floor(u * kGridWidth), 0.0, kGridWidth - 1.0));
    grid_y = static_cast<int>(std::clamp(kGridHeight - 1.0 - std::floor(top_down_v * kGridHeight), 0.0, kGridHeight - 1.0));
    return u >= 0.0 && u < 1.0 && top_down_v >= 0.0 && top_down_v < 1.0;
}

void PowderApp::RunFrame(float dt, const BrushConfig* scripted_brush, bool reset_counters) {
    ++frame_index_;
    if (reset_counters) {
        const GLuint zero_uint = 0U;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    BeginTimedPass(TimedPass::Spawn);
    RunSpawnPass(scripted_brush);
    EndTimedPass(TimedPass::Spawn);
    BeginTimedPass(TimedPass::Boundary);
    RunBoundaryPass();
    EndTimedPass(TimedPass::Boundary);
    BeginTimedPass(TimedPass::Water);
    RunWaterPass(dt);
    EndTimedPass(TimedPass::Water);
    BeginTimedPass(TimedPass::Gas);
    RunGasTick(dt);
    EndTimedPass(TimedPass::Gas);
    BeginTimedPass(TimedPass::Sand);
    RunSandPass(dt);
    EndTimedPass(TimedPass::Sand);
    BeginTimedPass(TimedPass::Coupling);
    RunCouplingPass(dt);
    EndTimedPass(TimedPass::Coupling);
    BeginTimedPass(TimedPass::Extract);
    RunRenderFieldExtractionPass();
    RunSparkPass(dt);
    EndTimedPass(TimedPass::Extract);
}

void PowderApp::RunSpawnPass(const BrushConfig* override_config) {
    if (override_config != nullptr) {
        StampBrush(*override_config);
        return;
    }
    // painting_ is this frame's button state from UpdateInput; querying again would miss a sticky click it consumed.
    if (replay_mode_ || !painting_) {
        stroke_.active = false;
        cursor_trail_.clear();
        return;
    }
    StampStroke();
}

// Bridges the previous interactive stamp to the cursor along every cursor event since, so a fast or curved
// mouse stroke leaves no gaps. Catch-up ticks in the same frame start at the cursor, so they stamp it once
// and keep the per-tick emission rate. Stamps overlap, so each particle stamp skips the previous stamp's
// disc: a swept cell receives one tick of particles, as under a held brush, and the finite pools last.
void PowderApp::StampStroke() {
    {
        // The polyline since the last stamp covers one tick of cursor motion; a new stroke starts at rest.
        const bool moving = stroke_.active && stroke_.mode == brush_mode_;
        const float blend = 1.0F - std::exp(-config_.fixed_dt / kBrushVelocityTau);
        for (int axis = 0; axis < 2; ++axis) {
            const int now = axis == 0 ? brush_x_ : brush_y_;
            const int before = axis == 0 ? stroke_.x : stroke_.y;
            const float sample = moving ? static_cast<float>(now - before) / config_.fixed_dt : 0.0F;
            brush_velocity_[axis] = moving ? brush_velocity_[axis] + (sample - brush_velocity_[axis]) * blend : 0.0F;
        }
        const float speed = std::hypot(brush_velocity_[0], brush_velocity_[1]);
        if (speed > kBrushVelocityMax) {
            brush_velocity_[0] *= kBrushVelocityMax / speed;
            brush_velocity_[1] *= kBrushVelocityMax / speed;
        }
    }
    BrushConfig config = BuildBrushConfig();
    config.brush_velocity = brush_velocity_;
    for (int axis = 0; axis < 2; ++axis) {
        config.water_velocity[axis] += brush_velocity_[axis];
        config.sand_velocity[axis] += brush_velocity_[axis];
        // Gas velocities are in gas cells (two simulation cells) per second.
        config.smoke_velocity[axis] += 0.5F * brush_velocity_[axis];
    }
    config.water_density_limit = static_cast<float>(config.water_particles) * config_.water_amount_per_particle;
    const int end_x = brush_x_;
    const int end_y = brush_y_;
    const bool continues = stroke_.active && stroke_.mode == brush_mode_;
    std::vector<std::array<int, 2>> path;
    if (continues) path.push_back({stroke_.x, stroke_.y});
    path.insert(path.end(), cursor_trail_.begin(), cursor_trail_.end());
    if (path.empty() || path.back() != std::array<int, 2>{end_x, end_y}) path.push_back({end_x, end_y});
    cursor_trail_.clear();
    auto points = StrokePoints(path, brush_radius_);
    if (!continues && points.front() != path.front()) points.insert(points.begin(), path.front());
    for (const auto& point : points) {
        brush_x_ = point[0];
        brush_y_ = point[1];
        StampBrush(config);
        config.skip_previous_stamp = true;
        config.previous_stamp = point;
    }
    brush_x_ = end_x;
    brush_y_ = end_y;
    stroke_ = {true, brush_mode_, end_x, end_y};
}

// Centres after path.front() up to and including path.back(), evenly spaced along the path and at most
// max(1, radius / 2) apart. With three or more points the path is a centripetal Catmull-Rom spline through them
// (Yuksel et al. 2011: no cusps or overshoot loops), so a quick circle stays round instead of a polygon of cursor
// samples. More than kMaxStrokeStamps would be needed only for cursor jumps; those spread evenly and gap. A single
// point yields itself.
std::vector<std::array<int, 2>> PowderApp::StrokePoints(const std::vector<std::array<int, 2>>& path, int radius) {
    if (path.size() < 2) return path;
    using Point = std::array<double, 2>;
    const auto at_index = [&](std::ptrdiff_t i) {
        const auto& p = path[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, std::ssize(path) - 1))];
        return Point{static_cast<double>(p[0]), static_cast<double>(p[1])};
    };
    std::vector<Point> dense{at_index(0)};
    for (std::ptrdiff_t i = 0; i + 1 < std::ssize(path); ++i) {
        const Point p1 = at_index(i), p2 = at_index(i + 1);
        const double chord = std::hypot(p2[0] - p1[0], p2[1] - p1[1]);
        if (path.size() < 3 || chord == 0.0) {
            dense.push_back(p2);
            continue;
        }
        // End segments reflect their neighbour through the end point for the missing outer control point.
        const auto outer = [&](std::ptrdiff_t j, const Point& end, const Point& inner) {
            return j >= 0 && j < std::ssize(path) ? at_index(j) : Point{2.0 * end[0] - inner[0], 2.0 * end[1] - inner[1]};
        };
        const Point p0 = outer(i - 1, p1, p2), p3 = outer(i + 2, p2, p1);
        // A deliberate corner (a turn sharper than 60 degrees at either end) stays a corner: a spline would bow
        // off both legs by up to 7% of their length.
        const auto cosine = [](const Point& a, const Point& b, const Point& c) {
            const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
            return (ux * vx + uy * vy) / std::max(std::hypot(ux, uy) * std::hypot(vx, vy), 1e-9);
        };
        if (cosine(p0, p1, p2) < 0.5 || cosine(p1, p2, p3) < 0.5) {
            dense.push_back(p2);
            continue;
        }
        const auto knot = [](const Point& a, const Point& b) { return std::max(std::sqrt(std::hypot(b[0] - a[0], b[1] - a[1])), 1e-6); };
        const double t1 = knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        const int steps = std::clamp(static_cast<int>(std::ceil(chord / 2.0)), 1, 32);
        for (int s = 1; s <= steps; ++s) {
            const double t = t1 + (t2 - t1) * s / steps;
            Point q{};
            for (int axis = 0; axis < 2; ++axis) {
                // Barry-Goldman pyramid with knots 0, t1, t2, t3.
                const double a1 = ((t1 - t) * p0[axis] + t * p1[axis]) / t1;
                const double a2 = ((t2 - t) * p1[axis] + (t - t1) * p2[axis]) / (t2 - t1);
                const double a3 = ((t3 - t) * p2[axis] + (t - t2) * p3[axis]) / (t3 - t2);
                const double b1 = ((t2 - t) * a1 + t * a2) / t2;
                const double b2 = ((t3 - t) * a2 + (t - t1) * a3) / (t3 - t1);
                q[axis] = ((t2 - t) * b1 + (t - t1) * b2) / (t2 - t1);
            }
            dense.push_back(s == steps ? p2 : q);
        }
    }
    std::vector<double> arc(dense.size(), 0.0);
    for (std::size_t i = 1; i < dense.size(); ++i)
        arc[i] = arc[i - 1] + std::hypot(dense[i][0] - dense[i - 1][0], dense[i][1] - dense[i - 1][1]);
    const double length = arc.back();
    const int count = std::clamp(static_cast<int>(std::ceil(length / std::max(1, radius / 2))), 1, kMaxStrokeStamps);
    std::vector<std::array<int, 2>> points;
    points.reserve(static_cast<std::size_t>(count));
    std::size_t segment = 1;
    for (int i = 1; i <= count; ++i) {
        const double at = length * static_cast<double>(i) / count;
        while (segment + 1 < dense.size() && arc[segment] < at) ++segment;
        const double span = arc[segment] - arc[segment - 1];
        const double t = span > 0.0 ? std::min(1.0, (at - arc[segment - 1]) / span) : 1.0;
        const auto& from = dense[segment - 1];
        const auto& to = dense[segment];
        points.push_back({static_cast<int>(std::lround(from[0] + (to[0] - from[0]) * t)),
                          static_cast<int>(std::lround(from[1] + (to[1] - from[1]) * t))});
    }
    return points;
}

void PowderApp::StampBrush(const BrushConfig& config) {
    const DispatchRegion full_region = ComputeDispatchRegion(1, kGridWidth, kGridHeight);
    if (full_region.Empty()) {
        return;
    }

    if (brush_mode_ == BrushMode::Erase) {
        RunClearFullRes(full_region);
        RunClearSandParticles(full_region);
        RunClearWaterParticles(full_region);
        RunClearGas(ComputeDispatchRegion(2, kGasWidth, kGasHeight));
        return;
    }

    if (config.replace_material) {
        RunSpawnFullRes(config, full_region);
    }

    // Particles frozen inside a new wall would still draw, count toward caps and feed P2G.
    const bool fills = config.replace_material && config.material_value != kEmptyMaterial;
    if (fills) {
        RunClearWaterParticles(full_region);
    }
    if (fills) {
        RunClearSandParticles(full_region);
    }

    if (config.spawn_sand) {
        RunSpawnSandParticles(config, full_region);
    }

    if (config.spawn_water) {
        RunSpawnWaterParticles(config, full_region);
    }

    const DispatchRegion gas_region = ComputeDispatchRegion(2, kGasWidth, kGasHeight);
    if (config.spawn_gas) {
        RunSpawnGas(config, gas_region);
    }
}

PowderApp::BrushConfig PowderApp::BuildBrushConfig() const {
    BrushConfig config{};
    config.blocked_by_solid = true;

    switch (brush_mode_) {
        case BrushMode::Sand:
            config.spawn_sand = true;
            config.sand_particles = 2;
            config.velocity_spread = 10.0F;
            break;
        case BrushMode::Water:
            config.spawn_water = true;
            config.water_particles = 4;
            // 0.5 m/s out of the spout, plus a 5 cm/s random spread so the stream wobbles and breaks up.
            config.water_velocity = {0.0F, -100.0F};
            config.velocity_spread = 10.0F;
            break;
        case BrushMode::Oil:
            // Poured like water, from the same spout; the records are tagged as oil.
            config.spawn_water = true;
            config.oil = true;
            config.water_particles = 4;
            config.water_velocity = {0.0F, -100.0F};
            config.velocity_spread = 10.0F;
            break;
        case BrushMode::Solid:
            config.replace_material = true;
            config.material_value = kSolidMaterial;
            break;
        case BrushMode::Smoke:
            // Warm smoke (about 180 K above ambient) rising at 0.2 m/s from the brush, +-0.15 m/s spread.
            config.spawn_gas = true;
            config.smoke_density = 0.5F;
            config.smoke_velocity = {0.0F, 20.0F};
            config.temperature_add = 0.2F;
            config.velocity_spread = 15.0F;
            break;
        case BrushMode::Fire:
            // Hot fuel vapour that burns in the surrounding air; the brush adds no oxygen of its own.
            config.blocked_by_water = true;
            config.spawn_gas = true;
            config.smoke_velocity = {0.0F, 20.0F};
            config.temperature_add = 0.6F;
            config.fuel_add = 0.6F;
            config.reaction_add = 0.4F;
            config.velocity_spread = 15.0F;
            break;
        case BrushMode::Wood:
            config.replace_material = true;
            config.material_value = kWoodMaterial;
            break;
        case BrushMode::Erase:
            break;
    }

    return config;
}

PowderApp::DispatchRegion PowderApp::ComputeDispatchRegion(int scale, int domain_width, int domain_height) const {
    DispatchRegion region{};

    const int min_x = std::clamp(brush_x_ - brush_radius_, 0, kGridWidth - 1);
    const int max_x = std::clamp(brush_x_ + brush_radius_, 0, kGridWidth - 1);
    const int min_y = std::clamp(brush_y_ - brush_radius_, 0, kGridHeight - 1);
    const int max_y = std::clamp(brush_y_ + brush_radius_, 0, kGridHeight - 1);

    region.origin_x = min_x / scale;
    region.origin_y = min_y / scale;

    const int max_domain_x = std::min(domain_width - 1, max_x / scale);
    const int max_domain_y = std::min(domain_height - 1, max_y / scale);

    region.width = max_domain_x - region.origin_x + 1;
    region.height = max_domain_y - region.origin_y + 1;
    if (region.width < 0 || region.height < 0) {
        region.width = 0;
        region.height = 0;
    }

    return region;
}

void PowderApp::RunSpawnFullRes(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    if (config.replace_material) boundary_state_.static_dirty = true;
    if (config.replace_material && config.material_value == kWoodMaterial) wood_present_ = heat_present_ = true;

    glUseProgram(spawn_full_program_);
    glUniform2i(CachedUniform(spawn_full_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spawn_full_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(spawn_full_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(spawn_full_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(spawn_full_program_, "replaceMaterial"), config.replace_material ? 1 : 0);
    glUniform1ui(CachedUniform(spawn_full_program_, "materialValue"), config.material_value);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedBySolid"), config.blocked_by_solid ? 1 : 0);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedByWater"), config.blocked_by_water ? 1 : 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearWater"), 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearMode"), 0);

    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(3, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(7, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(1, water_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);

    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSpawnGas(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    if (config.fuel_add > 0.0F) heat_present_ = true;

    glUseProgram(spawn_gas_program_);
    glUniform2i(CachedUniform(spawn_gas_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform2i(CachedUniform(spawn_gas_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spawn_gas_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(spawn_gas_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(spawn_gas_program_, "brushRadius"), brush_radius_);
    glUniform1f(CachedUniform(spawn_gas_program_, "smokeDensity"), config.smoke_density);
    glUniform2f(CachedUniform(spawn_gas_program_, "gasVelocity"),
                config.smoke_velocity[0],
                config.smoke_velocity[1]);
    glUniform1f(CachedUniform(spawn_gas_program_, "temperatureAdd"), config.temperature_add);
    glUniform1f(CachedUniform(spawn_gas_program_, "fuelAdd"), config.fuel_add);
    glUniform1f(CachedUniform(spawn_gas_program_, "oxidizerAdd"), config.oxidizer_add);
    glUniform1f(CachedUniform(spawn_gas_program_, "reactionAdd"), config.reaction_add);
    glUniform1i(CachedUniform(spawn_gas_program_, "blockedBySolid"), config.blocked_by_solid ? 1 : 0);
    glUniform1i(CachedUniform(spawn_gas_program_, "blockedByWater"), config.blocked_by_water ? 1 : 0);
    glUniform1i(CachedUniform(spawn_gas_program_, "clearMode"), 0);
    glUniform1f(CachedUniform(spawn_gas_program_, "maxVelocity"), config_.gas_max_velocity);
    glUniform1f(CachedUniform(spawn_gas_program_, "velocitySpread"), config.velocity_spread);
    glUniform1ui(CachedUniform(spawn_gas_program_, "spawnSeed"), frame_index_ * 977U + static_cast<GLuint>(brush_x_ * 131 + brush_y_));

    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(3, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(5, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(7, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(9, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(10, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);

    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSpawnSandParticles(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    // A stamp that might not fit is skipped whole, keeping kSandWakeReserve slots free for packed sand that wakes (as
    // RunSpawnWaterParticles): the brush stops at a full pool the same way every run, where the GPU's per-grain
    // reservation at the cap dropped grains in atomic order and counted each as overflow.
    const std::uint64_t spawned = static_cast<std::uint64_t>(region.width) * region.height * std::max(1, config.sand_particles);
    if (sand_state_.record_bound + spawned > static_cast<std::uint64_t>(kMaxSandParticles - kSandWakeReserve)) {
        return;
    }

    glUseProgram(sand_particle_spawn_program_);
    glUniform2i(CachedUniform(sand_particle_spawn_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(sand_particle_spawn_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(sand_particle_spawn_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "brushRadius"), brush_radius_);
    glUniform2f(CachedUniform(sand_particle_spawn_program_, "spawnVelocity"),
                config.sand_velocity[0],
                config.sand_velocity[1]);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "spawnCountPerCell"), config.sand_particles);
    glUniform1f(CachedUniform(sand_particle_spawn_program_, "particleMass"), 1.0F);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "maxParticles"), kMaxSandParticles);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "blockedBySolid"), config.blocked_by_solid ? 1 : 0);
    glUniform1f(CachedUniform(sand_particle_spawn_program_, "occupancyLimit"), config_.sand_spawn_occupancy_limit);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "skipActive"), config.skip_previous_stamp ? 1 : 0);
    glUniform2i(CachedUniform(sand_particle_spawn_program_, "skipCenter"), config.previous_stamp[0], config.previous_stamp[1]);
    glUniform1ui(CachedUniform(sand_particle_spawn_program_, "spawnSeed"), frame_index_ * 977U + static_cast<GLuint>(brush_x_ * 131 + brush_y_));
    glUniform1f(CachedUniform(sand_particle_spawn_program_, "velocitySpread"), config.velocity_spread);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, sand_state_.occupancy, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, debug_state_.counters);
    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    sand_state_.record_bound = static_cast<GLuint>(
        std::min<std::uint64_t>(sand_state_.record_bound + spawned, static_cast<std::uint64_t>(kMaxSandParticles)));
}

void PowderApp::RunSpawnWaterParticles(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    // A stamp that might not fit is skipped whole, keeping kWaterWakeReserve slots for waking packed water: the brush
    // stops at a full pool. record_bound is the exact live count read back after compaction plus every earlier stamp's
    // upper bound, so the decision is the same every run, where the GPU's per-particle reservation at the cap dropped
    // particles in atomic order and counted each as overflow.
    const std::uint64_t spawned = static_cast<std::uint64_t>(region.width) * region.height * std::max(1, config.water_particles);
    if (water_state_.record_bound + spawned > static_cast<std::uint64_t>(kMaxWaterParticles - kWaterWakeReserve)) {
        return;
    }

    glUseProgram(water_particle_spawn_program_);
    glUniform2i(CachedUniform(water_particle_spawn_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(water_particle_spawn_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(water_particle_spawn_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(water_particle_spawn_program_, "brushRadius"), brush_radius_);
    glUniform2f(CachedUniform(water_particle_spawn_program_, "spawnVelocity"),
                config.water_velocity[0],
                config.water_velocity[1]);
    glUniform1i(CachedUniform(water_particle_spawn_program_, "spawnCountPerCell"), std::max(1, config.water_particles));
    glUniform1f(CachedUniform(water_particle_spawn_program_, "particleMass"), config_.water_particle_mass);
    glUniform1i(CachedUniform(water_particle_spawn_program_, "maxParticles"), kMaxWaterParticles);
    glUniform1f(CachedUniform(water_particle_spawn_program_, "densityLimit"), config.water_density_limit);
    glUniform1i(CachedUniform(water_particle_spawn_program_, "skipActive"), config.skip_previous_stamp ? 1 : 0);
    glUniform2i(CachedUniform(water_particle_spawn_program_, "skipCenter"), config.previous_stamp[0], config.previous_stamp[1]);
    glUniform1ui(CachedUniform(water_particle_spawn_program_, "spawnSeed"), frame_index_ * 977U + static_cast<GLuint>(brush_x_ * 131 + brush_y_));
    glUniform1f(CachedUniform(water_particle_spawn_program_, "velocitySpread"), config.velocity_spread);
    glUniform1f(CachedUniform(water_particle_spawn_program_, "oilFlag"), config.oil ? 1.0F : 0.0F);
    glUniform1f(CachedUniform(water_particle_spawn_program_, "tickDt"), config_.fixed_dt);
    glUniform2f(CachedUniform(water_particle_spawn_program_, "gravity"), 0.0F, config_.water_gravity);
    if (config.oil) oil_present_ = true;
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, debug_state_.counters);
    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    water_state_.record_bound = static_cast<GLuint>(
        std::min<std::uint64_t>(water_state_.record_bound + spawned, static_cast<std::uint64_t>(kMaxWaterParticles)));
}

void PowderApp::RunClearSandParticles(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

    glUseProgram(sand_particle_clear_program_);
    glUniform2i(CachedUniform(sand_particle_clear_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(sand_particle_clear_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(sand_particle_clear_program_, "maxParticles"), static_cast<GLint>(sand_state_.record_bound));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    if (sand_state_.record_bound > 0U) glDispatchCompute((sand_state_.record_bound + 255U) / 256U, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void PowderApp::RunClearWaterParticles(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

    glUseProgram(water_particle_clear_program_);
    glUniform2i(CachedUniform(water_particle_clear_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(water_particle_clear_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(water_particle_clear_program_, "maxParticles"), static_cast<GLint>(water_state_.record_bound));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    if (water_state_.record_bound > 0U) glDispatchCompute((water_state_.record_bound + 255U) / 256U, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void PowderApp::RunClearFullRes(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    boundary_state_.static_dirty = true;

    glUseProgram(spawn_full_program_);
    glUniform2i(CachedUniform(spawn_full_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spawn_full_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(spawn_full_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(spawn_full_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(spawn_full_program_, "replaceMaterial"), 0);
    glUniform1ui(CachedUniform(spawn_full_program_, "materialValue"), kEmptyMaterial);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedBySolid"), 0);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedByWater"), 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearWater"), 1);
    glUniform1i(CachedUniform(spawn_full_program_, "clearMode"), 1);

    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(3, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(7, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(1, water_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);

    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunClearGas(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

    glUseProgram(spawn_gas_program_);
    glUniform2i(CachedUniform(spawn_gas_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform2i(CachedUniform(spawn_gas_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spawn_gas_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(spawn_gas_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(spawn_gas_program_, "brushRadius"), brush_radius_);
    glUniform1f(CachedUniform(spawn_gas_program_, "smokeDensity"), 0.0F);
    glUniform2f(CachedUniform(spawn_gas_program_, "gasVelocity"), 0.0F, 0.0F);
    glUniform1f(CachedUniform(spawn_gas_program_, "temperatureAdd"), 0.0F);
    glUniform1f(CachedUniform(spawn_gas_program_, "fuelAdd"), 0.0F);
    glUniform1f(CachedUniform(spawn_gas_program_, "oxidizerAdd"), 0.0F);
    glUniform1f(CachedUniform(spawn_gas_program_, "reactionAdd"), 0.0F);
    glUniform1i(CachedUniform(spawn_gas_program_, "blockedBySolid"), 0);
    glUniform1i(CachedUniform(spawn_gas_program_, "blockedByWater"), 0);
    glUniform1i(CachedUniform(spawn_gas_program_, "clearMode"), 1);

    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(3, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(5, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(7, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);

    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunBoundaryPass() {
    glUseProgram(boundary_build_program_);
    glUniform2i(CachedUniform(boundary_build_program_, "gridSize"), kGridWidth, kGridHeight);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, sand_state_.occupancy, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(3, boundary_state_.mask_static, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // Sand contact needs static distances; other consumers use the authoritative masks.
    if (boundary_state_.static_dirty) {
        glUseProgram(boundary_seed_program_);
        glUniform2i(CachedUniform(boundary_seed_program_, "gridSize"), kGridWidth, kGridHeight);
        glBindImageTexture(0, boundary_state_.mask_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(1, boundary_state_.seed_full_a, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32I);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        int step = 1;
        while (step < std::max(kGridWidth, kGridHeight)) step <<= 1;
        step >>= 1;
        GLuint current_seed = boundary_state_.seed_full_a;
        GLuint next_seed = boundary_state_.seed_full_b;
        while (step >= 1) {
            glUseProgram(boundary_jumpflood_program_);
            glUniform2i(CachedUniform(boundary_jumpflood_program_, "gridSize"), kGridWidth, kGridHeight);
            glUniform1i(CachedUniform(boundary_jumpflood_program_, "stepSize"), step);
            glBindImageTexture(0, current_seed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32I);
            glBindImageTexture(1, next_seed, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32I);
            DispatchGrid(kGridWidth, kGridHeight);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            std::swap(current_seed, next_seed);
            step >>= 1;
        }

        glUseProgram(boundary_distance_program_);
        glUniform2i(CachedUniform(boundary_distance_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(boundary_distance_program_, "maxDistance"),
                    std::sqrt(static_cast<float>(kGridWidth * kGridWidth + kGridHeight * kGridHeight)));
        glBindImageTexture(0, boundary_state_.mask_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(1, current_seed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32I);
        glBindImageTexture(2, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        boundary_state_.static_dirty = false;
    }

    glUseProgram(boundary_downsample_program_);
    glUniform2i(CachedUniform(boundary_downsample_program_, "srcSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(boundary_downsample_program_, "dstSize"), kGasWidth, kGasHeight);
    glUniform1i(CachedUniform(boundary_downsample_program_, "scale"), 2);
    // Half the rest volume: a settled body, not spray or a thin film. The liquid volume is the previous tick's.
    glUniform1f(CachedUniform(boundary_downsample_program_, "waterBlockVolume"),
                0.5F * config_.water_rest_density * config_.water_amount_per_particle);
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(2, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, phase_state_.oil_field, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSandPass(float dt) {
    const double wave_speed = std::sqrt((static_cast<double>(config_.sand_lame_lambda) +
                                        2.0 * config_.sand_shear_modulus) / config_.sand_reference_density);
    // Stress waves and grain advection limit the substep separately; the count is the larger of the two. The budget must
    // hold the speed cap, checked before any dispatch; the tick then runs what its fastest grain needs.
    // Explicit viscosity is diffusion at nu = eta / rho: stable for nu sub_dt <= 1/4 per cell in 2D, kept at half that.
    const double wave_substeps = std::max(std::ceil(static_cast<double>(dt) * wave_speed / config_.sand_cfl_limit),
                                          std::ceil(static_cast<double>(dt) * 8.0 * config_.sand_viscosity / config_.sand_reference_density));
    const auto advective_substeps = [&](double speed) {
        return std::ceil(static_cast<double>(dt) * speed / config_.sand_advection_cfl);
    };
    const double required = std::max(wave_substeps, advective_substeps(config_.sand_max_velocity));
    if (!std::isfinite(required) || dt < 0.0F || required > config_.sand_max_substeps) {
        throw std::runtime_error("Sand timestep exceeds the configured wave-speed substep budget");
    }
    // The shader evaluates these intermediates in float even when the double wave bound is finite.
    const float twice_shear = 2.0F * config_.sand_shear_modulus;
    const float lame_sum = config_.sand_lame_lambda + config_.sand_shear_modulus;
    const float yield_ratio = lame_sum / config_.sand_shear_modulus;
    const float yield_slope = yield_ratio * config_.sand_friction_alpha;
    if (!std::isfinite(twice_shear) || !std::isfinite(lame_sum) ||
        !std::isfinite(yield_ratio) || !std::isfinite(yield_slope)) {
        throw std::runtime_error("Sand material coefficients exceed GPU float range: check "
                                 "POWDER_SAND_SHEAR_MODULUS, POWDER_SAND_LAME_LAMBDA and POWDER_SAND_FRICTION_ALPHA");
    }
    const double gravity_delta = static_cast<double>(config_.sand_gravity) * dt / std::max(1.0, wave_substeps);
    if (!std::isfinite(gravity_delta) || std::abs(gravity_delta) > 65504.0) {
        throw std::runtime_error("SimulationConfig::sand_gravity * substep dt exceeds the 65504 cells/s sand grid velocity bound; "
                                 "reduce sand_gravity, POWDER_FIXED_DT, or POWDER_SAND_CFL "
                                 "(increase POWDER_SAND_MAX_SUBSTEPS if needed)");
    }
    const GLuint zero_uint = 0U;
    const GLint zero_int = 0;

    // Grid passes cover node rows/columns [x0, x1) x [y0, y1); rows are contiguous in the grid buffers.
    std::array<int, 4> grid_box = sand_state_.node_box;
    // Cleared once per tick: within it the grid update zeroes the momentum it reads and the particle step the mass (the
    // occupancy transfer's momentum is left for the next tick's clear).
    const auto clear_sand_grid = [this, zero_uint, zero_int, &grid_box]() {
        const int y0 = grid_box[1];
        const int y1 = grid_box[3];
        if (y1 <= y0) return;
        const GLintptr row_words = kGridWidth + 2;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, y0 * row_words * 4, (y1 - y0) * row_words * 4,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        for (GLuint buffer : {sand_state_.grid_momentum_x, sand_state_.grid_momentum_y}) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32I, y0 * row_words * 8, (y1 - y0) * row_words * 8,
                                 GL_RED_INTEGER, GL_INT, &zero_int);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    };
    const auto particle_groups = [this]() { return static_cast<GLuint>((sand_state_.live_count + 255U) / 256U); };

    const auto particle_to_grid = [this, &particle_groups](float stress_dt) {
        glUseProgram(sand_particle_p2g_program_);
        glUniform2i(CachedUniform(sand_particle_p2g_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(sand_particle_p2g_program_, "maxParticles"), static_cast<GLint>(sand_state_.live_count));
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "massScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "velocityScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "dt"), stress_dt);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "shearModulus"), config_.sand_shear_modulus);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "lameLambda"), config_.sand_lame_lambda);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "viscosity"), config_.sand_viscosity);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "referenceDensity"), config_.sand_reference_density);
        // The stress-free occupancy transfer also gathers moisture for sand_rasterize.comp.
        glUniform1i(CachedUniform(sand_particle_p2g_program_, "depositMoisture"), stress_dt == 0.0F ? 1 : 0);
        glUniform1i(CachedUniform(sand_particle_p2g_program_, "tileStride"), kSandTileStride);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.grid_momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sand_state_.grid_momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sand_state_.grid_moisture);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sand_state_.tile_flags);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        if (particle_groups() > 0U) glDispatchCompute(particle_groups(), 1, 1);  // zero groups is an error on some drivers
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    };

    // Sleep-packing: tally still grains per cell and decide pack/wake per cell before compaction, which drops exactly
    // the grains of the cells that pack; apply after it, so woken grains are appended at the compacted cursor (and join
    // its live box and fastest speed). The apply phase zeroes the tally it consumed, so neither it nor the full pools are
    // cleared per tick.
    const bool sleeping = config_.sand_sleep_ticks > 0;
    const auto pack_phase = [this, dt](int phase) {
        glUseProgram(sand_pack_program_);
        glUniform2i(CachedUniform(sand_pack_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(sand_pack_program_, "phase"), phase);
        glUniform1i(CachedUniform(sand_pack_program_, "maxPacked"), kMaxPackedGrains);
        glUniform1f(CachedUniform(sand_pack_program_, "impactSpeed"), config_.sand_wake_impact_speed);
        glUniform1f(CachedUniform(sand_pack_program_, "sleepSpeed"), config_.sand_sleep_speed);
        glUniform1f(CachedUniform(sand_pack_program_, "pressSpeed"), config_.sand_wake_press_speed);
        glUniform1f(CachedUniform(sand_pack_program_, "waterSpeed"), config_.sand_wake_water_speed);
        glUniform1f(CachedUniform(sand_pack_program_, "waterFullVolume"), 4.0F * config_.water_amount_per_particle);
        glUniform1f(CachedUniform(sand_pack_program_, "wettingRate"), config_.sand_wetting_rate);
        glUniform1f(CachedUniform(sand_pack_program_, "dryingRate"), config_.sand_drying_rate);
        glUniform1f(CachedUniform(sand_pack_program_, "dt"), dt);
        glUniform1i(CachedUniform(sand_pack_program_, "maxParticles"), kMaxSandParticles);
        glUniform1ui(CachedUniform(sand_pack_program_, "seed"), frame_index_ * 2654435761U + 12345U);
        glBindImageTexture(0, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
        glBindImageTexture(1, sand_state_.packed_f, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        glBindImageTexture(2, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(3, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(4, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glBindImageTexture(5, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.cell_tally);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.cell_fsum);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sand_state_.spawn_cursor);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    };
    if (sleeping) {
        glUseProgram(sand_sleep_program_);
        glUniform2i(CachedUniform(sand_sleep_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(sand_sleep_program_, "maxParticles"), static_cast<GLint>(sand_state_.record_bound));
        glUniform1f(CachedUniform(sand_sleep_program_, "sleepSpeed"), config_.sand_sleep_speed);
        glUniform1i(CachedUniform(sand_sleep_program_, "sleepTicks"), config_.sand_sleep_ticks);
        glUniform1f(CachedUniform(sand_sleep_program_, "cohesionSoftening"), config_.sand_cohesion_softening);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.cell_tally);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.cell_fsum);
        if (sand_state_.record_bound > 0U) glDispatchCompute((sand_state_.record_bound + 255U) / 256U, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        pack_phase(0);
    }

    // No scratch clear: compaction deactivates every record it reads, so the source it leaves is all inactive.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(sand_particle_compact_program_);
    glUniform1i(CachedUniform(sand_particle_compact_program_, "maxParticles"), static_cast<GLint>(sand_state_.record_bound));
    glUniform2i(CachedUniform(sand_particle_compact_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(sand_particle_compact_program_, "dropPacked"), sleeping ? 1 : 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.scratch_particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sand_state_.cell_tally);
    if (sand_state_.record_bound > 0U) glDispatchCompute((sand_state_.record_bound + 255U) / 256U, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    std::swap(sand_state_.particles, sand_state_.scratch_particles);
    if (sleeping) pack_phase(1);

    // Live count, fastest grain's float bits, live cell box (see sand_particle_compact.comp). This read is the tick's
    // one sync: the particle passes dispatch the live count and the grid passes the box instead of the whole pool.
    std::array<GLuint, 6> cursor{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), cursor.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    sand_state_.live_count = sand_state_.record_bound = std::min<GLuint>(cursor[0], static_cast<GLuint>(kMaxSandParticles));
    const float fastest = std::bit_cast<float>(cursor[1]);
    const double speed_bound =
        std::isfinite(fastest) ? std::min<double>(config_.sand_max_velocity, fastest) : config_.sand_max_velocity;
    // Without grains there is no stress wave to resolve; one substep still clears the grid velocity water reads.
    sand_state_.substeps = cursor[0] == 0U ? 1 :
        std::max(1, static_cast<int>(std::max(wave_substeps, advective_substeps(speed_bound))));
    const float sub_dt = dt / static_cast<float>(sand_state_.substeps);

    // Node box of this tick's grains: their cells, the one-cell quadratic stencil, and the distance the fastest can
    // travel this tick. The passes cover it joined with last tick's box, which zeroes the nodes the sand left.
    std::array<int, 4> live_box{0, 0, 0, 0};
    if (cursor[4] != 0U) {
        const int margin = static_cast<int>(std::ceil(speed_bound * dt)) + 3;
        live_box = {std::max(0, static_cast<int>(0xffffffffU - cursor[2]) + 1 - margin),
                    std::max(0, static_cast<int>(0xffffffffU - cursor[3]) + 1 - margin),
                    std::min(kGridWidth + 2, static_cast<int>(cursor[4]) + 1 + margin),
                    std::min(kGridHeight + 2, static_cast<int>(cursor[5]) + 1 + margin)};
        if (grid_box[2] <= grid_box[0] || grid_box[3] <= grid_box[1]) {
            grid_box = live_box;
        } else {
            grid_box = {std::min(grid_box[0], live_box[0]), std::min(grid_box[1], live_box[1]),
                        std::max(grid_box[2], live_box[2]), std::max(grid_box[3], live_box[3])};
        }
    }
    sand_state_.node_box = live_box;

    for (int substep = 0; substep < sand_state_.substeps; ++substep) {
        if (substep == 0) clear_sand_grid();
        particle_to_grid(sub_dt);

        glUseProgram(sand_grid_update_program_);
        // Tile-aligned origin: each workgroup is one deposit tile (sand_particle_p2g.comp).
        const int tile_x0 = grid_box[0] / kWorkgroupSize * kWorkgroupSize;
        const int tile_y0 = grid_box[1] / kWorkgroupSize * kWorkgroupSize;
        glUniform2i(CachedUniform(sand_grid_update_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform2i(CachedUniform(sand_grid_update_program_, "nodeOrigin"), tile_x0, tile_y0);
        glUniform1i(CachedUniform(sand_grid_update_program_, "tileStride"), kSandTileStride);
        glUniform1f(CachedUniform(sand_grid_update_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(sand_grid_update_program_, "gravity"), config_.sand_gravity);
        glUniform1f(CachedUniform(sand_grid_update_program_, "damping"), config_.sand_damping);
        glUniform1f(CachedUniform(sand_grid_update_program_, "drag"), config_.sand_drag);
        glUniform1f(CachedUniform(sand_grid_update_program_, "buoyancy"), 1.0F / config_.sand_grain_density);
        // Liquid volume of the four particles per cell the water brush emits: a cell that counts as full.
        glUniform1f(CachedUniform(sand_grid_update_program_, "waterFullVolume"), 4.0F * config_.water_amount_per_particle);
        glUniform1f(CachedUniform(sand_grid_update_program_, "wallFriction"), config_.sand_friction_coefficient);
        glUniform1f(CachedUniform(sand_grid_update_program_, "massScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_grid_update_program_, "velocityScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_grid_update_program_, "shearModulus"), config_.sand_shear_modulus);
        glUniform1f(CachedUniform(sand_grid_update_program_, "lameLambda"), config_.sand_lame_lambda);
        glUniform1f(CachedUniform(sand_grid_update_program_, "referenceDensity"), config_.sand_reference_density);
        glUniform1f(CachedUniform(sand_grid_update_program_, "frictionAlpha"), config_.sand_friction_alpha);
        glUniform1f(CachedUniform(sand_grid_update_program_, "cohesion"), config_.sand_cohesion);
        glUniform1i(CachedUniform(sand_grid_update_program_, "skipEmptyTiles"), substep + 1 < sand_state_.substeps ? 1 : 0);
        glBindImageTexture(0, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glBindImageTexture(3, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
        glBindImageTexture(4, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(5, sand_state_.packed_f, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.grid_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.grid_momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sand_state_.tile_flags);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        if (grid_box[2] > grid_box[0] && grid_box[3] > grid_box[1]) {
            DispatchGrid(grid_box[2] - tile_x0, grid_box[3] - tile_y0);
        }
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

        glUseProgram(sand_particle_step_program_);
        glUniform2i(CachedUniform(sand_particle_step_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(sand_particle_step_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(sand_particle_step_program_, "restitution"), config_.sand_restitution);
        glUniform1f(CachedUniform(sand_particle_step_program_, "maxVelocity"), config_.sand_max_velocity);
        glUniform1i(CachedUniform(sand_particle_step_program_, "maxParticles"), static_cast<GLint>(sand_state_.live_count));
        glUniform1f(CachedUniform(sand_particle_step_program_, "shearModulus"), config_.sand_shear_modulus);
        glUniform1f(CachedUniform(sand_particle_step_program_, "lameLambda"), config_.sand_lame_lambda);
        glUniform1f(CachedUniform(sand_particle_step_program_, "frictionAlpha"), config_.sand_friction_alpha);
        glUniform1f(CachedUniform(sand_particle_step_program_, "frictionVariation"), config_.sand_friction_variation);
        glUniform1f(CachedUniform(sand_particle_step_program_, "contactDamping"), config_.sand_contact_damping);
        glUniform1f(CachedUniform(sand_particle_step_program_, "frictionRateGain"), config_.sand_friction_rate_gain);
        glUniform1f(CachedUniform(sand_particle_step_program_, "inertialI0"), config_.sand_inertial_i0);
        glUniform1f(CachedUniform(sand_particle_step_program_, "grainDiameter"), config_.sand_grain_diameter);
        glUniform1f(CachedUniform(sand_particle_step_program_, "referenceDensity"), config_.sand_reference_density);
        glUniform1f(CachedUniform(sand_particle_step_program_, "cohesion"), config_.sand_cohesion);
        glUniform1f(CachedUniform(sand_particle_step_program_, "cohesionSoftening"), config_.sand_cohesion_softening);
        glUniform1f(CachedUniform(sand_particle_step_program_, "maxDilation"), config_.sand_max_dilation);
        glUniform1f(CachedUniform(sand_particle_step_program_, "wettingRate"), config_.sand_wetting_rate);
        glUniform1f(CachedUniform(sand_particle_step_program_, "dryingRate"), config_.sand_drying_rate);
        glUniform1f(CachedUniform(sand_particle_step_program_, "moistureDt"), substep == 0 ? dt : 0.0F);
        glUniform1f(CachedUniform(sand_particle_step_program_, "waterFullVolume"), 4.0F * config_.water_amount_per_particle);
        glUniform1ui(CachedUniform(sand_particle_step_program_, "roundSeed"),
                     frame_index_ * 2654435761U + static_cast<GLuint>(substep));
        glBindImageTexture(0, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
        glBindImageTexture(2, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(3, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        if (particle_groups() > 0U) glDispatchCompute(particle_groups(), 1, 1);  // zero groups is an error on some drivers
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    // Moisture is gathered only here, so like the mass it is zero outside the rows this clear covers.
    if (grid_box[3] > grid_box[1]) {
        const GLintptr row_bytes = (kGridWidth + 2) * 4;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_moisture);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, grid_box[1] * row_bytes, (grid_box[3] - grid_box[1]) * row_bytes,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    particle_to_grid(0.0F);

    glUseProgram(sand_rasterize_program_);
    glUniform2i(CachedUniform(sand_rasterize_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(sand_rasterize_program_, "massScale"), 4096.0F);
    glUniform1f(CachedUniform(sand_rasterize_program_, "referenceDensity"), config_.sand_reference_density);
    glBindImageTexture(0, sand_state_.occupancy, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(1, sand_state_.packed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(2, sand_state_.moisture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindImageTexture(3, sand_state_.packed_f, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.grid_mass);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_moisture);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunWaterPass(float dt) {
    // The substep count follows the fastest water (measured below), but the speed cap must fit the CFL budget, and one
    // substep of the fewest allowed must store the gravity impulse: both are rejected before any dispatch.
    powder_config::SubstepCount(config_.water_max_velocity, dt, config_.water_cfl_limit, config_.water_min_substeps,
                                config_.water_max_substeps, "POWDER_WATER_MAX_SUBSTEPS");
    const double largest_gravity_delta = static_cast<double>(config_.water_gravity) * dt / config_.water_min_substeps;
    if (!std::isfinite(largest_gravity_delta) || std::abs(largest_gravity_delta) > 65504.0) {
        throw std::runtime_error("POWDER_WATER_GRAVITY * substep dt exceeds the finite R16F range; "
                                 "reduce POWDER_FIXED_DT or increase POWDER_WATER_MIN_SUBSTEPS and "
                                 "POWDER_WATER_MAX_SUBSTEPS");
    }
    const GLuint zero_uint = 0U;
    const GLint zero_int = 0;

    const auto build_particle_fields = [this, zero_uint, zero_int]() {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.cell_counts);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_weights);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_weights);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.u_face_velocity_sums);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.v_face_velocity_sums);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        glUseProgram(water_particle_p2g_program_);
        glUniform2i(CachedUniform(water_particle_p2g_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(water_particle_p2g_program_, "maxParticles"), static_cast<GLint>(water_state_.live_count));
        glUniform1f(CachedUniform(water_particle_p2g_program_, "velocityScale"), 4096.0F);
        glUniform1f(CachedUniform(water_particle_p2g_program_, "weightScale"), 4096.0F);
        glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.u_face_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.v_face_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, water_state_.u_face_velocity_sums);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, water_state_.v_face_velocity_sums);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, water_state_.cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        if (water_state_.live_count > 0U) glDispatchCompute((water_state_.live_count + 255U) / 256U, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    };

    const auto build_mac_faces = [this](bool previous_grid) {
        glUseProgram(water_mac_build_program_);
        glUniform2i(CachedUniform(water_mac_build_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_mac_build_program_, "velocityScale"), 4096.0F);
        glUniform1f(CachedUniform(water_mac_build_program_, "weightScale"), 4096.0F);
        glBindImageTexture(0, previous_grid ? water_state_.mac_u_prev : water_state_.mac_u,
                           0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(1, previous_grid ? water_state_.mac_v_prev : water_state_.mac_v,
                           0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(3, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
        glUniform1f(CachedUniform(water_mac_build_program_, "restMass"), config_.water_rest_density);
        glBindImageTexture(4, water_state_.packed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, water_state_.cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.u_face_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.v_face_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.u_face_velocity_sums);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, water_state_.v_face_velocity_sums);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth + 1, kGridHeight + 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    };

    const auto finalize_particle_fields = [this]() {
        constexpr float kWeightScale = 4096.0F;
        glUseProgram(water_particle_finalize_program_);
        glUniform2i(CachedUniform(water_particle_finalize_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_particle_finalize_program_, "weightScale"), kWeightScale);
        glUniform1f(CachedUniform(water_particle_finalize_program_, "amountPerParticle"), config_.water_amount_per_particle);
        glUniform1f(CachedUniform(water_particle_finalize_program_, "surfaceLevel"), config_.water_surface_level);
        glUniform1f(CachedUniform(water_particle_finalize_program_, "restMass"), config_.water_rest_density);
        glUniform1f(CachedUniform(water_particle_finalize_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
        glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(2, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.cell_counts);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    };

    const auto finalize_mac_faces = [this]() {
        glUseProgram(water_mac_finalize_program_);
        glUniform2i(CachedUniform(water_mac_finalize_program_, "gridSize"), kGridWidth, kGridHeight);
        glBindImageTexture(0, water_state_.mac_u, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.mac_v, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(3, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    };

    // Calm-water packing (water_pack.comp). Phase 0 flags cells to pack from this tally, compaction drops exactly their
    // particles, phase 1 stores the counts and turns cells flagged to wake (by phase 2 after the last tick's final
    // projection) back into particles at the cursor, so the cursor read below counts them.
    const bool packing = config_.water_sleep_ticks > 0;
    const auto pack_phase = [this](int phase) {
        glUseProgram(water_pack_program_);
        glUniform2i(CachedUniform(water_pack_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(water_pack_program_, "phase"), phase);
        glUniform1i(CachedUniform(water_pack_program_, "restCount"),
                    static_cast<GLint>(std::lround(config_.water_rest_density / config_.water_particle_mass)));
        // A cell at least half full is liquid (counts carry weightScale 4096).
        glUniform1f(CachedUniform(water_pack_program_, "liquidCount"), 0.5F * config_.water_rest_density * 4096.0F);
        glUniform1f(CachedUniform(water_pack_program_, "wakeSpeed"), config_.water_wake_speed);
        glUniform1f(CachedUniform(water_pack_program_, "particleMass"), config_.water_particle_mass);
        glUniform1i(CachedUniform(water_pack_program_, "maxParticles"), kMaxWaterParticles);
        glUniform1ui(CachedUniform(water_pack_program_, "seed"), frame_index_ * 2654435761U + 777U);
        glBindImageTexture(0, water_state_.packed, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
        glBindImageTexture(1, water_state_.wake_velocity, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
        glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(3, water_state_.mac_u, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(4, water_state_.mac_v, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.cell_tally);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, water_state_.spawn_cursor);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    };
    if (packing) {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.cell_tally);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glUseProgram(water_sleep_program_);
        glUniform2i(CachedUniform(water_sleep_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(water_sleep_program_, "maxParticles"), static_cast<GLint>(water_state_.record_bound));
        glUniform1f(CachedUniform(water_sleep_program_, "sleepSpeed"), config_.water_sleep_speed);
        glUniform1i(CachedUniform(water_sleep_program_, "sleepTicks"), config_.water_sleep_ticks);
        glUniform1f(CachedUniform(water_sleep_program_, "particleMass"), config_.water_particle_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.cell_tally);
        if (water_state_.record_bound > 0U) glDispatchCompute((water_state_.record_bound + 255U) / 256U, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        pack_phase(0);
    }

    // No scratch clear: compaction deactivates every record it copies, so the source it leaves is all inactive.
    // Compaction covers the slots that may hold records; the particle passes below dispatch the exact live count.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.bins);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Compaction sorts the records by bin (count, scan, scatter), so the particle passes touch nearby cells together.
    glUseProgram(water_particle_compact_program_);
    glUniform1i(CachedUniform(water_particle_compact_program_, "maxParticles"), static_cast<GLint>(water_state_.record_bound));
    glUniform2i(CachedUniform(water_particle_compact_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(water_particle_compact_program_, "dropPacked"), packing ? 1 : 0);
    glUniform1i(CachedUniform(water_particle_compact_program_, "binSize"), kWaterBinSize);
    glUniform1i(CachedUniform(water_particle_compact_program_, "binCount"), kWaterBinCount);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.scratch_particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, water_state_.cell_tally);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, water_state_.bins);
    for (int phase = 0; phase < 3; ++phase) {
        glUniform1i(CachedUniform(water_particle_compact_program_, "phase"), phase);
        if (phase == 1) glDispatchCompute(1, 1, 1);
        else if (water_state_.record_bound > 0U) glDispatchCompute((water_state_.record_bound + 255U) / 256U, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | (phase == 2 ? GL_BUFFER_UPDATE_BARRIER_BIT : 0U));
    }
    std::swap(water_state_.particles, water_state_.scratch_particles);
    if (packing) pack_phase(1);

    // CFL from the measured speed: the fastest particle at the start of the tick, never above the cap (the particle step
    // counts every particle the cap cuts); the CFL limit's own slack covers a tick of gravity. Still water takes one substep.
    // The same read gives the live count.
    std::array<GLuint, 2> cursor{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), cursor.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    water_state_.live_count = water_state_.record_bound = std::min<GLuint>(cursor[0], static_cast<GLuint>(kMaxWaterParticles));
    const float fastest = std::bit_cast<float>(cursor[1]);
    const float speed_bound = std::isfinite(fastest) ? std::min(config_.water_max_velocity, fastest) : config_.water_max_velocity;
    const int substeps = powder_config::SubstepCount(speed_bound, dt, config_.water_cfl_limit, config_.water_min_substeps,
                                                    config_.water_max_substeps, "POWDER_WATER_MAX_SUBSTEPS");
    water_state_.substeps = substeps;
    const float sub_dt = dt / static_cast<float>(substeps);

    for (int step = 0; step < substeps; ++step) {
        build_particle_fields();
        build_mac_faces(true);
        finalize_particle_fields();

        glUseProgram(water_divergence_program_);
        glUniform2i(CachedUniform(water_divergence_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_divergence_program_, "gravityDelta"), config_.water_gravity * sub_dt);
        glUniform1f(CachedUniform(water_divergence_program_, "liquidPhiThreshold"),
                    config_.water_liquid_phi_threshold);
        glUniform1f(CachedUniform(water_divergence_program_, "weightScale"), 4096.0F);
        glUniform1f(CachedUniform(water_divergence_program_, "restDensity"), config_.water_rest_density);
        glUniform1f(CachedUniform(water_divergence_program_, "densityRate"), config_.water_density_rate);
        glBindImageTexture(0, water_state_.mac_u_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.mac_v_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, CurrentWaterDivergence(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(4, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(5, water_state_.packed, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.cell_counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        RemovePocketMeans(false);

        RunWaterProjection();

        glUseProgram(water_project_program_);
        glUniform2i(CachedUniform(water_project_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_project_program_, "gravityDelta"), config_.water_gravity * sub_dt);
        glUniform1f(CachedUniform(water_project_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
        glUniform1f(CachedUniform(water_project_program_, "sandLeak"), kSandLeak);
        glUniform1f(CachedUniform(water_project_program_, "thetaMin"), config_.water_ghost_theta_min);
        glBindImageTexture(0, water_state_.mac_u_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.mac_v_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, CurrentWaterPressure(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(3, water_state_.mac_u, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(4, water_state_.mac_v, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(5, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(6, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth + 1, kGridHeight + 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        // Wake decisions read the tick's final projected faces; the next tick's phase 1 applies them.
        if (packing && step == substeps - 1) pack_phase(2);

        finalize_mac_faces();

        glUseProgram(water_particle_step_program_);
        glUniform2i(CachedUniform(water_particle_step_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_particle_step_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(water_particle_step_program_, "velocityDamping"), config_.water_velocity_damping);
        glUniform1f(CachedUniform(water_particle_step_program_, "bulkDamping"), config_.water_bulk_damping);
        glUniform2f(CachedUniform(water_particle_step_program_, "gravity"), 0.0F, config_.water_gravity);
        glUniform1f(CachedUniform(water_particle_step_program_, "maxVelocity"), config_.water_max_velocity);
        glUniform1f(CachedUniform(water_particle_step_program_, "flipBlend"), config_.water_flip_blend);
        glUniform1i(CachedUniform(water_particle_step_program_, "maxParticles"), static_cast<GLint>(water_state_.live_count));
        glUniform1f(CachedUniform(water_particle_step_program_, "sandDrag"),
                    config_.sand_drag * config_.sand_reference_density);
        glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(1, water_state_.mac_u_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, water_state_.mac_v_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(3, water_state_.mac_u, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(4, water_state_.mac_v, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(5, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
        glBindImageTexture(6, sand_state_.occupancy, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(7, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(8, phase_state_.oil_field, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glUniform1f(CachedUniform(water_particle_step_program_, "oilDrift"), oil_present_ ? kOilDrift : 0.0F);
        glUniform1f(CachedUniform(water_particle_step_program_, "restVolume"),
                    config_.water_rest_density * config_.water_amount_per_particle);
        glUniform1f(CachedUniform(water_particle_step_program_, "oilBuoyancy"),
                    oil_present_ ? 1.0F / kOilDensity - 1.0F : 0.0F);
        glUniform1f(CachedUniform(water_particle_step_program_, "advectionCfl"), config_.water_advection_cfl);
        glUniform1f(CachedUniform(water_particle_step_program_, "clumpVolume"),
                    3.0F * config_.water_rest_density * config_.water_amount_per_particle);
        glUniform1ui(CachedUniform(water_particle_step_program_, "jitterSeed"), frame_index_ * 8U + static_cast<GLuint>(step));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, debug_state_.counters);
        if (water_state_.live_count > 0U) glDispatchCompute((water_state_.live_count + 255U) / 256U, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    build_particle_fields();
    build_mac_faces(false);
    finalize_mac_faces();
    finalize_particle_fields();
}

// Preconditioned CG over the 16x16 tiles that hold the solve (phases 8 and 9 list them), three indirect dispatches
// per iteration around a multigrid V-cycle preconditioner (pressure_mg.comp): every tile workgroup forms the global
// sums it needs from the per-tile sums itself, and a stopped solve empties every dispatch. The host never waits before
// the solve: it dispatches the previous solve's iteration count blind, then confirms the GPU's running flag at fences
// while the GPU works on further iterations, and only a solve still running continues up to the configured cap.
void PowderApp::BindWaterSolver() {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.pressure_vectors);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.pressure_partials);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.pressure_scalars);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, debug_state_.counters);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, water_state_.pressure_tiles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, water_state_.pressure_sums);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, water_state_.pressure_levels);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, water_state_.pressure_tiles);
    glBindImageTexture(0, CurrentWaterDivergence(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentWaterPressure(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(3, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glUseProgram(pressure_mg_program_);
    glUniform2i(CachedUniform(pressure_mg_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(pressure_mg_program_, "levels"), kWaterMultigridLevels);
    glUniform1f(CachedUniform(pressure_mg_program_, "sandLeak"), kSandLeak);
}

// Dispatch from the multigrid's own indirect arguments (slot 0 the coarsest solve, l the level-l box) or, for -1,
// from the CG tile list, which stays bound afterwards.
static void DispatchWaterLevel(GLuint levels, GLuint tiles, int slot) {
    if (slot >= 0) glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, levels);
    glDispatchComputeIndirect(slot >= 0 ? static_cast<GLintptr>(slot) * 16 : 0);
    if (slot >= 0) glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, tiles);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

// The hierarchy of this solve's liquid mask: boxes and arguments from the liquid bounds, then every level's cell types
// and diagonals. Expects BindWaterSolver and the CG's phases 0 to 9.
void PowderApp::SetupWaterMultigrid() {
    const GLint phase = CachedUniform(pressure_mg_program_, "phase");
    const GLint level = CachedUniform(pressure_mg_program_, "level");
    glUseProgram(pressure_mg_program_);
    glUniform1i(phase, 0);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    glUniform1i(phase, 1);
    for (int l = 1; l <= kWaterMultigridLevels; ++l) {
        glUniform1i(level, l);
        DispatchWaterLevel(water_state_.pressure_levels, water_state_.pressure_tiles, l);
    }
    glUniform1i(phase, 5);
    DispatchWaterLevel(water_state_.pressure_levels, water_state_.pressure_tiles, 0);
}

// z = M^-1 r for the current CG residual, with its sums in tile sum `slot`; `initial` also sets d = z.
void PowderApp::ApplyWaterMultigrid(int slot, bool initial) {
    const GLint phase = CachedUniform(pressure_mg_program_, "phase");
    const GLint level = CachedUniform(pressure_mg_program_, "level");
    glUseProgram(pressure_mg_program_);
    glUniform1i(CachedUniform(pressure_mg_program_, "slot"), slot);
    glUniform1i(CachedUniform(pressure_mg_program_, "initial"), initial ? 1 : 0);
    glUniform1i(phase, 2);
    for (int l = 0; l < kWaterMultigridLevels; ++l) {
        glUniform1i(level, l);
        DispatchWaterLevel(water_state_.pressure_levels, water_state_.pressure_tiles, l == 0 ? -1 : l + 1);
    }
    glUniform1i(phase, 3);
    DispatchWaterLevel(water_state_.pressure_levels, water_state_.pressure_tiles, 0);
    glUniform1i(phase, 4);
    for (int l = kWaterMultigridLevels - 1; l >= 0; --l) {
        glUniform1i(level, l);
        DispatchWaterLevel(water_state_.pressure_levels, water_state_.pressure_tiles, l == 0 ? -1 : l + 1);
    }
}

// Removes the right-hand side's mean over every sealed pocket of a pressure system, liquid or gas walled in by Neumann
// faces alone with no Dirichlet or porous neighbour, so each such singular system is compatible (pressure_pockets.comp).
void PowderApp::RemovePocketMeans(bool gas) {
    const int width = gas ? kGasWidth : kGridWidth, height = gas ? kGasHeight : kGridHeight;
    const GLuint rhs = gas ? gas_state_.divergence : CurrentWaterDivergence();
    const GLenum rhs_format = gas ? GL_R32F : GL_R16F;
    const bool wide = rhs_format == GL_R32F;
    glUseProgram(pressure_pockets_program_);
    glUniform2i(CachedUniform(pressure_pockets_program_, "gridSize"), width, height);
    glUniform1i(CachedUniform(pressure_pockets_program_, "gas"), gas ? 1 : 0);
    glUniform1i(CachedUniform(pressure_pockets_program_, "openTop"), config_.gas_open_top ? 1 : 0);
    glUniform1i(CachedUniform(pressure_pockets_program_, "wide"), wide ? 1 : 0);
    glUniform1f(CachedUniform(pressure_pockets_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
    glBindImageTexture(wide ? 1 : 0, rhs, 0, GL_FALSE, 0, GL_READ_WRITE, rhs_format);
    glBindImageTexture(wide ? 0 : 1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, wide ? GL_R16F : GL_R32F);
    glBindImageTexture(2, gas ? gas_state_.level_kind[0] : boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(3, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.pockets);
    const GLint phase = CachedUniform(pressure_pockets_program_, "phase");
    for (int p = 0; p < 5; ++p) {
        glUniform1i(phase, p);
        DispatchGrid(width, height);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
                        (p == 4 ? GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT : 0U));
    }
}

void PowderApp::RunWaterProjection() {
    const GLuint cell_groups = static_cast<GLuint>((CellCount() + 255) / 256);
    const GLuint tile_slots = static_cast<GLuint>(((kGridWidth + 15) / 16) * ((kGridHeight + 15) / 16));
    BindWaterSolver();
    glUseProgram(pressure_cg_program_);
    glUniform2i(CachedUniform(pressure_cg_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(pressure_cg_program_, "solveOrigin"), 0, 0);
    glUniform2i(CachedUniform(pressure_cg_program_, "solveSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(pressure_cg_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
    glUniform1f(CachedUniform(pressure_cg_program_, "sandLeak"), kSandLeak);
    glUniform1f(CachedUniform(pressure_cg_program_, "thetaMin"), config_.water_ghost_theta_min);
    glUniform1f(CachedUniform(pressure_cg_program_, "residualThreshold"), config_.water_pressure_residual_threshold);
    const GLint cg_phase = CachedUniform(pressure_cg_program_, "phase");
    const GLint cg_parity = CachedUniform(pressure_cg_program_, "parity");
    const GLint cg_first = CachedUniform(pressure_cg_program_, "firstIteration");
    const GLint cg_gated = CachedUniform(pressure_cg_program_, "gated");
    const GLint mg_gated = CachedUniform(pressure_mg_program_, "gated");
    glUseProgram(pressure_cg_reduce_program_);
    glUniform2i(CachedUniform(pressure_cg_reduce_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(pressure_cg_reduce_program_, "solveOrigin"), 0, 0);
    glUniform2i(CachedUniform(pressure_cg_reduce_program_, "solveSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(pressure_cg_reduce_program_, "cellCount"), CellCount());
    glUniform1f(CachedUniform(pressure_cg_reduce_program_, "residualThreshold"), config_.water_pressure_residual_threshold);
    glUniform1i(CachedUniform(pressure_cg_reduce_program_, "partialCount"), static_cast<GLint>(cell_groups));
    const GLint reduce_phase = CachedUniform(pressure_cg_reduce_program_, "phase");
    const auto step = [&](GLuint program, GLint location, int phase, GLuint groups) {
        glUseProgram(program);
        glUniform1i(location, phase);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    };
    const auto tile_step = [&](int phase) {
        glUniform1i(cg_phase, phase);
        glDispatchComputeIndirect(0);
        // A stopped solve zeroes the group counts; the command barrier lets the next dispatches launch no workgroups.
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |
                        (phase == 2 ? GL_SHADER_IMAGE_ACCESS_BARRIER_BIT : 0U));
    };
    int dispatched = 0;
    // Each iteration ends with phase 1, which tests it and already forms the next direction and A*direction. The sum
    // slots alternate with the global iteration number, so a restart the GPU decides on needs no host knowledge.
    const auto iterate = [&](int count) {
        for (int end = dispatched + count; dispatched < end; ++dispatched) {
            const int parity = dispatched & 1;
            glUseProgram(pressure_cg_program_);
            glUniform1i(cg_parity, parity);
            tile_step(2);
            ApplyWaterMultigrid(1 + parity, false);
            glUseProgram(pressure_cg_program_);
            glUniform1i(cg_parity, parity);
            tile_step(1);
        }
    };
    const auto step_indirect = [&](GLuint program, GLint location, int phase, GLintptr offset) {
        glUseProgram(program);
        glUniform1i(location, phase);
        glDispatchComputeIndirect(offset);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    };
    // Forms b - Ap from the stored pressure and starts CG on it. A restart keeps the tile list and iteration count and
    // is queued blind: phase 10 arms it only for a solve that has stopped after iterating, and every pass of it
    // launches nothing (or returns) otherwise, so the host never waits to learn whether it is due.
    const auto start = [&](bool restart) {
        const int parity = dispatched & 1;  // of the iteration that follows, whose previous sum slot z fills
        glUseProgram(pressure_cg_reduce_program_);
        glUniform1i(CachedUniform(pressure_cg_reduce_program_, "restart"), restart ? 1 : 0);
        if (restart) {
            step(pressure_cg_program_, cg_phase, 10, 1);
            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, water_state_.pressure_scalars);
            step_indirect(pressure_cg_program_, cg_phase, 0, kWaterRestartDispatchOffset);
            step_indirect(pressure_cg_reduce_program_, reduce_phase, 0, kWaterRestartDispatchOffset);
            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, water_state_.pressure_tiles);
        } else {
            step(pressure_cg_program_, cg_phase, 0, cell_groups);
            step(pressure_cg_reduce_program_, reduce_phase, 0, cell_groups);
        }
        step(pressure_cg_reduce_program_, reduce_phase, 1, 1);
        if (!restart) {
            step(pressure_cg_program_, cg_phase, 8, tile_slots);
            step(pressure_cg_program_, cg_phase, 9, 1);
        }
        glUseProgram(pressure_mg_program_);
        glUniform1i(mg_gated, restart ? 1 : 0);
        SetupWaterMultigrid();
        ApplyWaterMultigrid(2 - parity, true);
        glUseProgram(pressure_mg_program_);
        glUniform1i(mg_gated, 0);
        glUseProgram(pressure_cg_program_);
        glUniform1i(cg_parity, parity);
        glUniform1i(cg_first, 1);
        glUniform1i(cg_gated, restart ? 1 : 0);
        tile_step(1);
        glUniform1i(cg_first, 0);
        glUniform1i(cg_gated, 0);
    };
    start(false);

    const int cap = config_.water_pressure_iterations;
    iterate(std::min(cap, water_state_.pressure_iterations));
    std::array<GLuint, 3> state{};  // running, invalidInput, iterations
    const auto read_state = [&]() {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_scalars);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(float), sizeof(state), state.data());
        return state[0] != 0;
    };
    // CG stops on its recursive residual, which drifts from the true b - Ap as float rounds a large pressure (deep packed
    // pools), so a stopped solve restarts from the true residual until that passes the test itself, fails or reaches the
    // cap. Each pass queues that restart (a no-op unless the solve has just stopped), copies the state behind a fence and,
    // once a pass has found the solve running, keeps the GPU busy with 1, then 2, then up to 4 more iterations while the
    // CPU waits for the fence (they launch nothing once the solve has stopped).
    for (int ahead = 0; dispatched < cap;) {
        if (dispatched > 0) start(true);  // a solve that never iterated has formed its true residual already
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_COPY_READ_BUFFER, water_state_.pressure_scalars);
        glBindBuffer(GL_COPY_WRITE_BUFFER, water_state_.pressure_state);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 4 * sizeof(float), 0, sizeof(state));
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        iterate(std::min(ahead, cap - dispatched));
        const GLenum waited = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
        if (waited == GL_WAIT_FAILED) throw std::runtime_error("Waiting for the water pressure solve failed.");
        // The copy finished before the fence, so mapping it does not wait for the speculative iterations.
        const void* copied = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, sizeof(state), GL_MAP_READ_BIT);
        if (copied == nullptr) throw std::runtime_error("Mapping the water pressure state failed.");
        std::memcpy(state.data(), copied, sizeof(state));
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        if (state[0] == 0 || dispatched >= cap) break;
        if (ahead == 0) iterate(std::min(1, cap - dispatched));  // else the next fence would sit where this one did
        ahead = std::min(4, std::max(1, ahead * 2));
    }
    if (dispatched == cap) read_state();
    water_state_.pressure_iterations = static_cast<int>(state[2]);
    water_state_.pressure_dispatched = dispatched;
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);

    // Always check b-Lp from the stored pressure, including after early convergence or breakdown.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(pressure_residual_program_);
    glUniform2i(CachedUniform(pressure_residual_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(pressure_residual_program_, "boundaryScale"), 1);
    glUniform1f(CachedUniform(pressure_residual_program_, "residualThreshold"), config_.water_pressure_residual_threshold);
    glUniform1i(CachedUniform(pressure_residual_program_, "collectStats"), 0);
    glUniform1i(CachedUniform(pressure_residual_program_, "useLiquidMask"), 1);
    glUniform1f(CachedUniform(pressure_residual_program_, "thetaMin"), config_.water_ghost_theta_min);
    glUniform1f(CachedUniform(pressure_residual_program_, "sandLeak"), kSandLeak);
    glUniform1f(CachedUniform(pressure_residual_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
    glUniform1i(CachedUniform(pressure_residual_program_, "pressureTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CurrentWaterPressure());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

// Every tick advances the gas by its own dt (transport, forces, combustion, heat); the pressure solve spans a pair of
// ticks. The first tick of a pair (the first one after InitializeState) builds the solve from the velocity it just
// advected and runs about half of the previous solve's CG iterations; the second finishes it, projects that velocity,
// then advects the projected field. Brush stamps, coupling and the water and sand passes act between the halves, so
// the correction lands on a field that has been stamped once since its divergence was measured (a stamped velocity is
// corrected a pair later), while the solve keeps the gas mask it was built on and stays one consistent operator.
void PowderApp::RunGasTick(float dt) {
    const bool begins = (frame_index_ - 1U) % kGasTickStride == 0U;
    if (!begins) RunGasProjectionStage(false, true, -1);
    RunGasPass(dt, false);
    // Half of the last solve's iterations, less one pair: the first half also builds the right-hand side and the
    // preconditioner's first V-cycle, the second ends with the final check and the projection (measured: 2.94 and 2.98 ms).
    if (begins) RunGasProjectionStage(true, false, std::max(0, (gas_state_.pressure_iterations - 2) / 2));
}

// project: every substep ends with a complete projection (fixtures); otherwise the caller owns the pressure solve.
void PowderApp::RunGasPass(float dt, bool project) {
    const float gas_speed_bound = std::sqrt(2.0F) * config_.gas_max_velocity;
    const int transport_substeps = powder_config::SubstepCount(gas_speed_bound, dt, config_.gas_cfl_limit,
                                                    config_.gas_min_substeps, config_.gas_max_substeps,
                                                    "POWDER_GAS_MAX_SUBSTEPS");
    // The nine-point weights sum to one; keep its central weight nonnegative after cooling.
    const int diffusion_substeps = powder_config::SubstepCount(
        config_.gas_temperature_diffusion + config_.gas_temperature_cooling, dt, 1.0F,
        config_.gas_min_substeps, config_.gas_max_substeps,
        "POWDER_GAS_TEMPERATURE_DIFFUSION / POWDER_GAS_MAX_SUBSTEPS");
    const int substeps = std::max(transport_substeps, diffusion_substeps);
    gas_state_.substeps = substeps;
    const float sub_dt = dt / static_cast<float>(substeps);
    for (int step = 0; step < substeps; ++step) {
        RunSmokePass(sub_dt, project);
        RunFireHeatPass(sub_dt);
    }
}

void PowderApp::RunSmokePass(float dt, bool project) {
    glUseProgram(gas_velocity_advect_program_);
    glUniform2i(CachedUniform(gas_velocity_advect_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(gas_velocity_advect_program_, "dt"), dt);
    glUniform1f(CachedUniform(gas_velocity_advect_program_, "maxVelocity"), config_.gas_max_velocity);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(4, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(5, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, gas_state_.velocity_center_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(smoke_advect_program_);
    glUniform2i(CachedUniform(smoke_advect_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(smoke_advect_program_, "dt"), dt);
    glUniform1f(CachedUniform(smoke_advect_program_, "maxVelocity"), config_.gas_max_velocity);
    glBindImageTexture(0, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(4, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(6, gas_state_.smoke_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(7, gas_state_.temperature_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(8, gas_state_.fuel_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(9, gas_state_.oxidizer_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(10, gas_state_.reaction_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(11, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(smoke_correct_program_);
    glUniform2i(CachedUniform(smoke_correct_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(smoke_correct_program_, "dt"), dt);
    glUniform1f(CachedUniform(smoke_correct_program_, "maxVelocity"), config_.gas_max_velocity);
    glUniform1f(CachedUniform(smoke_correct_program_, "densityDissipation"),
                std::pow(config_.gas_density_dissipation, dt * 60.0F));
    glUniform1f(CachedUniform(smoke_correct_program_, "scalarDissipation"),
                std::pow(config_.gas_scalar_dissipation, dt * 60.0F));
    glBindImageTexture(0, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, gas_state_.velocity_center_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(4, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(6, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(7, gas_state_.smoke_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(8, gas_state_.temperature_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(9, gas_state_.fuel_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(10, gas_state_.oxidizer_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(11, gas_state_.reaction_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(12, NextSmokeDen(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(13, NextTemperature(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(14, NextFuel(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(15, NextOxidizer(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(16, NextReactionRate(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(17, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_velocity_correct_program_);
    glUniform2i(CachedUniform(gas_velocity_correct_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "dt"), dt);
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "velocityDissipation"),
                std::pow(config_.gas_velocity_dissipation, dt * 60.0F));
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "buoyancyTemperature"), config_.gas_buoyancy_temperature);
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "buoyancySmoke"), config_.gas_buoyancy_smoke);
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "maxVelocity"), config_.gas_max_velocity);
    glUniform1f(CachedUniform(gas_velocity_correct_program_, "vorticity"), config_.gas_vorticity);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(4, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(5, gas_state_.velocity_center_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(6, NextSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(7, NextTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(8, NextGasU(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(9, NextGasV(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(10, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapSmokePing();
    SwapGasMacPing();

    if (project) RunGasProjection();
}

void PowderApp::RunGasProjection() { RunGasProjectionStage(true, true, -1); }

void PowderApp::RunGasProjectionStage(bool begin, bool finish, int iteration_budget) {
    if (!begin && !gas_state_.solve_pending) return;  // the solve was reset between its halves
    if (begin) {
        // The operator, residual checks and projection all use this copy: water, sand and rock keep moving the live mask
        // between the halves of a tick pair.
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glCopyImageSubData(boundary_state_.mask_gas, GL_TEXTURE_2D, 0, 0, 0, 0, gas_state_.mask_solve, GL_TEXTURE_2D, 0, 0,
                           0, 0, kGasWidth, kGasHeight, 1);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
    const GLuint solve_mask = gas_state_.mask_solve;
    if (begin) {
        glUseProgram(smoke_divergence_program_);
        glUniform2i(CachedUniform(smoke_divergence_program_, "gasSize"), kGasWidth, kGasHeight);
        glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(2, gas_state_.divergence, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glBindImageTexture(4, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glUniform1f(CachedUniform(smoke_divergence_program_, "expansion"), config_.gas_expansion);
        glBindImageTexture(3, solve_mask, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kGasWidth, kGasHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    // Level sizes and V-cycle textures; on level 0 the V-cycle maps the CG residual s to z.
    std::array<int, kGasLevels> sizes{};
    for (int level = 0, size = kGasWidth; level < kGasLevels; ++level, size = (size + 1) / 2) sizes[level] = size;
    const auto level_pressure = [this](int level) {
        return level == 0 ? gas_state_.cg_z : gas_state_.level_pressure[level - 1];
    };
    const auto level_rhs = [this](int level) { return level == 0 ? gas_state_.cg_s : gas_state_.level_rhs[level - 1]; };
    const int open_top = config_.gas_open_top ? 1 : 0;

    if (begin) {
        // Footprint kinds per level, so no pass scans a coarse cell's whole fine footprint.
        glUseProgram(gas_pressure_kind_program_);
        for (int level = 0; level < kGasLevels; ++level) {
            glUniform2i(CachedUniform(gas_pressure_kind_program_, "fineSize"), sizes[std::max(level - 1, 0)],
                        sizes[std::max(level - 1, 0)]);
            glUniform2i(CachedUniform(gas_pressure_kind_program_, "coarseSize"), sizes[level], sizes[level]);
            glUniform1i(CachedUniform(gas_pressure_kind_program_, "fromMask"), level == 0 ? 1 : 0);
            glBindImageTexture(0, level == 0 ? solve_mask : gas_state_.level_kind[level - 1], 0, GL_FALSE, 0,
                               GL_READ_ONLY, GL_R8UI);
            glBindImageTexture(1, gas_state_.level_kind[level], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
            DispatchGrid(sizes[level], sizes[level]);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
        // Only burning (fire, wood, lit oil) adds a source; wall faces carry no flux, so without one a sealed pocket's
        // divergence already sums to zero.
        if (oil_present_ || heat_present_) RemovePocketMeans(true);
    }

    const auto check_residual = [this, open_top, solve_mask](bool collect_stats) {
        glUseProgram(pressure_residual_program_);
        glUniform2i(CachedUniform(pressure_residual_program_, "gridSize"), kGasWidth, kGasHeight);
        glUniform1i(CachedUniform(pressure_residual_program_, "boundaryScale"), 1);
        glUniform1f(CachedUniform(pressure_residual_program_, "residualThreshold"), config_.gas_pressure_residual_threshold);
        glUniform1i(CachedUniform(pressure_residual_program_, "collectStats"), collect_stats ? 1 : 0);
        glUniform1i(CachedUniform(pressure_residual_program_, "useLiquidMask"), 0);
        glUniform1i(CachedUniform(pressure_residual_program_, "openTop"), open_top);
        glUniform1i(CachedUniform(pressure_residual_program_, "porousSand"), 1);
        glUniform1f(CachedUniform(pressure_residual_program_, "sandLeak"), kSandLeak);
        glUniform1f(CachedUniform(pressure_residual_program_, "liquidPhiThreshold"), 0.0F);
        glUniform1i(CachedUniform(pressure_residual_program_, "wideRhs"), 1);
        glBindImageTexture(4, gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glUniform1i(CachedUniform(pressure_residual_program_, "pressureTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_a);
        glBindImageTexture(2, solve_mask, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gas_state_.pressure_check);
        DispatchGrid(kGasWidth, kGasHeight);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        // The water solve shares this program and has a closed top, its own sand rule and a half-float right-hand side.
        glUniform1i(CachedUniform(pressure_residual_program_, "openTop"), 0);
        glUniform1i(CachedUniform(pressure_residual_program_, "porousSand"), 0);
        glUniform1i(CachedUniform(pressure_residual_program_, "wideRhs"), 0);
    };
    const auto needs_iteration = [&]() {
        check_residual(true);
        std::array<std::array<float, 4>, ((kGasWidth + 15) / 16) * ((kGasHeight + 15) / 16)> partials{};
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gas_state_.pressure_check);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(partials), partials.data());
        double square_sum = 0.0, count = 0.0;
        float maximum = 0.0F;
        for (const auto& partial : partials) {
            if (partial[3] != 0.0F || !std::isfinite(partial[0]) || !std::isfinite(partial[1])) return false;
            square_sum += partial[0];
            maximum = std::max(maximum, partial[1]);
            count += partial[2];
        }
        // Invalid inputs still reach the independent final diagnostic, without spreading through the solve.
        // Stop margins below the final gate: two thirds of it for the maximum, a fifteenth for the RMS.
        const double rms_limit = config_.gas_pressure_residual_threshold / 15.0;
        return count > 0.0 && (maximum > config_.gas_pressure_residual_threshold * (2.0F / 3.0F) ||
                               square_sum > count * rms_limit * rms_limit);
    };

    // Red-black Gauss-Seidel sweeps: each sweep is a red and a black step, all of a level's steps in one fused dispatch
    // from level_pressure into level_smoothed (pre-smoothing, which also forms the residual) or back (post-smoothing,
    // which first adds the coarse correction). Post-smoothing runs the colours in reverse, so the V-cycle is close to
    // symmetric for the conjugate gradient around it. The bottom level runs all its sweeps in place in one workgroup.
    const auto relax_level = [&](int level, int sweeps, bool reverse) {
        glUseProgram(gas_pressure_relax_program_);
        glUniform2i(CachedUniform(gas_pressure_relax_program_, "gridSize"), sizes[level], sizes[level]);
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "boundaryScale"), 1 << level);
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "openTop"), open_top);
        glUniform1f(CachedUniform(gas_pressure_relax_program_, "sandLeak"), kSandLeak);
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "fineHeight"), kGasHeight);
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "bottomSweeps"), level == kGasLevels - 1 ? sweeps : 0);
        glBindImageTexture(0, level_rhs(level), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(3, gas_state_.level_kind[level], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        const bool bottom = level == kGasLevels - 1;
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "smoothSteps"), bottom ? 0 : 2 * sweeps);
        if (bottom) {
            glBindImageTexture(1, level_pressure(level), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
            DispatchGrid(sizes[level], sizes[level]);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            return;
        }
        const GLuint from = reverse ? gas_state_.level_smoothed[level] : level_pressure(level);
        const GLuint to = reverse ? level_pressure(level) : gas_state_.level_smoothed[level];
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "parity"), reverse ? 1 : 0);
        glUniform1i(CachedUniform(gas_pressure_relax_program_, "prolongate"), reverse ? 1 : 0);
        glUniform2i(CachedUniform(gas_pressure_relax_program_, "coarseSize"), sizes[level + 1], sizes[level + 1]);
        glBindImageTexture(1, from, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(2, to, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glBindImageTexture(4, gas_state_.level_residual[level], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glBindImageTexture(5, level_pressure(level + 1), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(6, gas_state_.level_kind[level + 1], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(sizes[level], sizes[level]);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    };
    // The preconditioner: one V-cycle from zero for z ~ L^-1 s.
    const auto v_cycle = [&]() {
        for (int level = 0; level + 1 < kGasLevels; ++level) {
            relax_level(level, kGasSmoothSweeps, false);
            glUseProgram(gas_pressure_restrict_program_);
            glUniform2i(CachedUniform(gas_pressure_restrict_program_, "fineSize"), sizes[level], sizes[level]);
            glUniform2i(CachedUniform(gas_pressure_restrict_program_, "coarseSize"), sizes[level + 1], sizes[level + 1]);
            glUniform1i(CachedUniform(gas_pressure_restrict_program_, "boundaryScale"), 1 << level);
            glUniform1i(CachedUniform(gas_pressure_restrict_program_, "openTop"), open_top);
            glUniform1f(CachedUniform(gas_pressure_restrict_program_, "sandLeak"), kSandLeak);
            glUniform1i(CachedUniform(gas_pressure_restrict_program_, "fineHeight"), kGasHeight);
            glBindImageTexture(0, level_rhs(level), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            glBindImageTexture(1, level_pressure(level), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            glBindImageTexture(2, level_rhs(level + 1), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            glBindImageTexture(3, gas_state_.level_kind[level], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
            glBindImageTexture(4, level_pressure(level + 1), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            glBindImageTexture(5, gas_state_.level_kind[level + 1], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
            glBindImageTexture(6, gas_state_.level_residual[level], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
            // The residual (gatherPass 0) came with the fused pre-smoothing.
            glUniform1i(CachedUniform(gas_pressure_restrict_program_, "gatherPass"), 1);
            DispatchGrid(sizes[level + 1], sizes[level + 1]);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
        relax_level(kGasLevels - 1, kGasCoarsestSweeps, false);
        // Prolongation is fused into each level's post-smoothing.
        for (int level = kGasLevels - 2; level >= 0; --level) relax_level(level, kGasSmoothSweeps, true);
    };
    constexpr int partial_count = ((kGasWidth + 15) / 16) * ((kGasHeight + 15) / 16);
    const auto cg_phase = [&](int phase, bool first = false) {
        glUseProgram(gas_cg_program_);
        glUniform2i(CachedUniform(gas_cg_program_, "gridSize"), kGasWidth, kGasHeight);
        glUniform1i(CachedUniform(gas_cg_program_, "phase"), phase);
        glUniform1i(CachedUniform(gas_cg_program_, "openTop"), open_top);
        glUniform1f(CachedUniform(gas_cg_program_, "sandLeak"), kSandLeak);
        glUniform1i(CachedUniform(gas_cg_program_, "partialCount"), partial_count);
        glUniform1i(CachedUniform(gas_cg_program_, "first"), first ? 1 : 0);
        glBindImageTexture(0, gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, gas_state_.pressure_a, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(2, gas_state_.cg_s, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(3, gas_state_.cg_z, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(4, gas_state_.cg_d, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(5, gas_state_.cg_q, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(6, gas_state_.level_kind[0], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gas_state_.cg_partials);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gas_state_.cg_scalars);
        if (phase == 2 || phase == 5) {
            glDispatchCompute(1, 1, 1);
        } else {
            DispatchGrid(kGasWidth, kGasHeight);
        }
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    };
    // z = M^-1 s, then rz and the new direction.
    const auto precondition = [&](bool first) {
        v_cycle();
        cg_phase(4);
        cg_phase(5, first);
        cg_phase(6);
    };

    if (begin) {
        // Flexible preconditioned CG (the V-cycle varies slightly from a fixed linear map); the stored-operator
        // residual check decides convergence, independent of the CG recurrences. Each check stalls the CPU on the
        // GPU, so the first comes just before the previous solve stopped, then every two iterations. A stopped or
        // capped solve skips the next V-cycle: n iterations run n V-cycles.
        gas_state_.solve_pending = true;
        gas_state_.solve_running = needs_iteration();
        gas_state_.solve_started = gas_state_.solve_running;
        gas_state_.solve_iterations = 0;
        gas_state_.solve_next_check = std::max(3, gas_state_.pressure_iterations - 1);
        if (gas_state_.solve_running) {
            cg_phase(0);
            precondition(true);
        }
    }

    for (int ran = 0; gas_state_.solve_running && gas_state_.solve_iterations < config_.gas_pressure_iterations &&
                      (iteration_budget < 0 || ran < iteration_budget);
         ++ran) {
        cg_phase(1);
        cg_phase(2);
        cg_phase(3);
        const int iterations = ++gas_state_.solve_iterations;
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        if (iterations == gas_state_.solve_next_check && iterations < config_.gas_pressure_iterations) {
            gas_state_.solve_running = needs_iteration();
            gas_state_.solve_next_check += 2;
        }
        if (gas_state_.solve_running && iterations < config_.gas_pressure_iterations) precondition(false);
    }
    if (!finish) return;
    gas_state_.solve_pending = false;
    if (gas_state_.solve_started) gas_state_.pressure_iterations = gas_state_.solve_iterations;

    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    check_residual(false);

    glUseProgram(smoke_project_program_);
    glUniform2i(CachedUniform(smoke_project_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1i(CachedUniform(smoke_project_program_, "openTop"), open_top);
    glUniform1f(CachedUniform(smoke_project_program_, "sandLeak"), kSandLeak);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, gas_state_.pressure_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(3, NextGasU(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(4, NextGasV(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindImageTexture(5, solve_mask, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, NextGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, NextGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(2, NextGasVelocityCenter(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapGasMacPing();
}

void PowderApp::RunFireHeatPass(float dt) {
    glUseProgram(fire_rd_program_);
    glUniform2i(CachedUniform(fire_rd_program_, "gridSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(fire_rd_program_, "dt"), dt);
    glUniform1f(CachedUniform(fire_rd_program_, "combustionRate"), config_.gas_combustion_rate);
    glUniform1f(CachedUniform(fire_rd_program_, "oxidizerConsumption"), config_.gas_oxidizer_consumption);
    glUniform1f(CachedUniform(fire_rd_program_, "reactionDecay"), config_.gas_reaction_decay);
    glUniform1f(CachedUniform(fire_rd_program_, "temperatureDiffusion"), config_.gas_temperature_diffusion);
    glUniform1f(CachedUniform(fire_rd_program_, "temperatureCooling"), config_.gas_temperature_cooling);
    glUniform1f(CachedUniform(fire_rd_program_, "heatRelease"), config_.gas_heat_release);
    glUniform1f(CachedUniform(fire_rd_program_, "smokeYield"), config_.gas_smoke_yield);
    glUniform1f(CachedUniform(fire_rd_program_, "smokeDissipation"), config_.gas_smoke_dissipation);
    glUniform1f(CachedUniform(fire_rd_program_, "oxygenRate"), config_.gas_oxygen_rate);
    glBindImageTexture(0, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(4, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(5, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(6, NextFuel(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(7, NextOxidizer(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(8, NextTemperature(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(9, NextReactionRate(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(10, NextSmokeDen(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapSmokePing();
}

void PowderApp::RunCouplingPass(float dt) {
    // Coupling contract: liquid_volume suppresses reaction/fuel/oxidizer/temperature here.
    // Fire-to-smoke/temperature is owned by RunFireHeatPass; water-to-sand drag is owned by RunSandPass.
    // Render-extracted textures are outputs only and are never bound as physics inputs.
    glUseProgram(coupling_fire_program_);
    glUniform2i(CachedUniform(coupling_fire_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(coupling_fire_program_, "heatSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(coupling_fire_program_, "dt"), dt);
    glUniform1f(CachedUniform(coupling_fire_program_, "waterCooling"), config_.gas_water_cooling);
    glBindImageTexture(0, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(4, CurrentOxidizer(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(5, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, phase_state_.oil_field, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(7, sand_state_.occupancy, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glUniform1f(CachedUniform(coupling_fire_program_, "sandSmother"), kSandSmother);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(coupling_heat_program_);
    glUniform2i(CachedUniform(coupling_heat_program_, "heatSize"), kGasWidth, kGasHeight);
    glUniform2i(CachedUniform(coupling_heat_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(coupling_heat_program_, "dt"), dt);
    glUniform1f(CachedUniform(coupling_heat_program_, "waterCooling"), config_.gas_water_cooling * 0.7F);
    glBindImageTexture(0, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, phase_state_.oil_field, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (wood_present_) RunWoodPass();
    if (oil_present_ || heat_present_) RunPhasePass(dt);
}

// Surface water under flame evaporates into steam and surface oil under flame burns; both leave the particle pool as
// counted sinks (phase_state_.sinks). Then the oil field is rebuilt, burning slicks feed fuel vapour to the gas and the
// steam is carried and fed.
void PowderApp::RunPhasePass(float dt) {
    glUseProgram(phase_particles_program_);
    glUniform2i(CachedUniform(phase_particles_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(phase_particles_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1i(CachedUniform(phase_particles_program_, "maxParticles"), static_cast<GLint>(water_state_.record_bound));
    glUniform1f(CachedUniform(phase_particles_program_, "dt"), dt);
    glUniform1ui(CachedUniform(phase_particles_program_, "seed"), frame_index_ * 2246822519U + 7U);
    glUniform1f(CachedUniform(phase_particles_program_, "evaporateRate"), kWaterEvaporateRate);
    glUniform1f(CachedUniform(phase_particles_program_, "burnRate"), kOilBurnRate);
    glUniform1f(CachedUniform(phase_particles_program_, "ignitionTemperature"), kOilIgnitionTemperature);
    glUniform1f(CachedUniform(phase_particles_program_, "restVolume"),
                config_.water_rest_density * config_.water_amount_per_particle);
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(4, phase_state_.oil_field, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glUniform1f(CachedUniform(phase_particles_program_, "flashRate"), oil_present_ ? kWaterFlashRate : 0.0F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, phase_state_.cell_counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, phase_state_.gas_sources);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, phase_state_.sinks);
    if (water_state_.record_bound > 0U) glDispatchCompute((water_state_.record_bound + 255U) / 256U, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(oil_field_program_);
    glUniform2i(CachedUniform(oil_field_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(oil_field_program_, "restDensity"), config_.water_rest_density);
    glBindImageTexture(0, phase_state_.oil_field, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, phase_state_.cell_counts);
    DispatchGrid(kGridWidth, kGridHeight);

    glUseProgram(steam_update_program_);
    glUniform2i(CachedUniform(steam_update_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(steam_update_program_, "dt"), dt);
    glUniform1f(CachedUniform(steam_update_program_, "steamRise"), kSteamRise);
    glUniform1f(CachedUniform(steam_update_program_, "steamCondense"), kSteamCondense);
    glUniform1f(CachedUniform(steam_update_program_, "steamPerParticle"), kSteamPerParticle);
    glBindImageTexture(0, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, phase_state_.steam_ping ? phase_state_.steam_b : phase_state_.steam_a, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(2, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(3, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(5, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(6, CurrentOxidizer(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CurrentSteam());
    glUniform1i(CachedUniform(steam_update_program_, "steamSrc"), 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, phase_state_.gas_sources);
    DispatchGrid(kGasWidth, kGasHeight);
    glBindTexture(GL_TEXTURE_2D, 0);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    phase_state_.steam_ping = !phase_state_.steam_ping;
}

GLuint PowderApp::CurrentSteam() const {
    return phase_state_.steam_ping ? phase_state_.steam_a : phase_state_.steam_b;
}

std::array<GLuint, 2> PowderApp::ReadPhaseSinks() const {
    std::array<GLuint, 2> sinks{};
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, phase_state_.sinks);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(GLuint) * sinks.size()), sinks.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return sinks;
}

// Wood catches from hot gas, burns down feeding the gas fire, and goes out in water (shaders/wood_burn.comp).
void PowderApp::RunWoodPass() {
    glUseProgram(wood_burn_program_);
    glUniform2i(CachedUniform(wood_burn_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1ui(CachedUniform(wood_burn_program_, "seed"), frame_index_ * 2654435761U);
    glUniform1f(CachedUniform(wood_burn_program_, "ignitionTemperature"), 0.35F);
    // About 0.8 s to catch while hot, so fire creeps along a beam instead of jumping its length in one tick.
    glUniform1f(CachedUniform(wood_burn_program_, "igniteChance"), 0.02F);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(1, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(3, CurrentFuel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    for (int mode = 0; mode < 2; ++mode) {
        glUniform1i(CachedUniform(wood_burn_program_, "mode"), mode);
        DispatchGrid(kGasWidth, kGasHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    // shortcut: burning wood changes the static geometry with no readback, so the distance field is rebuilt
    // every quarter second while wood exists; track burnt-out cells if this shows in profiles.
    if (frame_index_ % 15U == 0U) boundary_state_.static_dirty = true;
}

void PowderApp::RunRenderFieldExtractionPass() {
    glUseProgram(render_extract_program_);
    glUniform2i(CachedUniform(render_extract_program_, "fullSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(render_extract_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(4, render_gas_velocity_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(5, render_smoke_density_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(6, render_temperature_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(7, render_fire_emissive_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

// One texel per 4x4 cells of the fire's emitted light, then the mip chain the composite reads for its wide glow.
void PowderApp::RunLightBuildPass() {
    glUseProgram(light_build_program_);
    glUniform2i(CachedUniform(light_build_program_, "lightSize"), kLightWidth, kLightHeight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, render_temperature_);
    glUniform1i(CachedUniform(light_build_program_, "temperatureTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, render_fire_emissive_);
    glUniform1i(CachedUniform(light_build_program_, "fireEmissiveTex"), 1);
    glBindImageTexture(0, light_texture_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    DispatchGrid(kLightWidth, kLightHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, light_texture_);
    glGenerateMipmap(GL_TEXTURE_2D);
}

// Sparks and embers are simulation state: one tick of the ring (spark_pick.comp bids, spark_step.comp spawns and
// advances). Nothing burns, and none are alive, until something was painted that can burn.
void PowderApp::RunSparkPass(float dt) {
    if (!heat_present_) return;
    glUseProgram(spark_pick_program_);
    glUniform2i(CachedUniform(spark_pick_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1ui(CachedUniform(spark_pick_program_, "tick"), frame_index_);
    glUniform1ui(CachedUniform(spark_pick_program_, "bucketCount"), static_cast<GLuint>(kSparkSpawnsPerTick));
    glUniform1i(CachedUniform(spark_pick_program_, "woodPresent"), wood_present_ ? 1 : 0);
    glBindImageTexture(0, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(3, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, spark_state_.buckets);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(spark_step_program_);
    glUniform2i(CachedUniform(spark_step_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spark_step_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1ui(CachedUniform(spark_step_program_, "tick"), frame_index_);
    glUniform1f(CachedUniform(spark_step_program_, "dt"), dt);
    glUniform1ui(CachedUniform(spark_step_program_, "capacity"), static_cast<GLuint>(kSparkCapacity));
    glUniform1ui(CachedUniform(spark_step_program_, "perTick"), static_cast<GLuint>(kSparkSpawnsPerTick));
    glUniform1f(CachedUniform(spark_step_program_, "gravity"), std::abs(config_.water_gravity));
    glBindImageTexture(0, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(2, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, spark_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, spark_state_.buckets);
    glDispatchCompute((kSparkCapacity + 63U) / 64U, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
}

// Lit frames only, over the composite: a wide soft quad that brightens the scene around each spark, then its streak.
void PowderApp::DrawSparks(const SimRect& sim, int fb_width, int fb_height) {
    glUseProgram(spark_draw_program_);
    glUniform2i(CachedUniform(spark_draw_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform4i(CachedUniform(spark_draw_program_, "simRect"), sim.x, sim.y, sim.width, sim.height);
    glUniform2i(CachedUniform(spark_draw_program_, "fbSize"), fb_width, fb_height);
    glBindVertexArray(spark_state_.vao);
    glEnable(GL_BLEND);
    // Pass 1 multiplies: dst * (1 + src). Pass 0 screens: dst + (1 - dst) src, the fire's own soft saturation.
    const std::array<GLenum, 2> source_factor{GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR};
    for (int pass = 1; pass >= 0; --pass) {
        glBlendFunc(source_factor[static_cast<std::size_t>(pass)], GL_ONE);
        glUniform1i(CachedUniform(spark_draw_program_, "pass"), pass);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kSparkCapacity);
    }
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// Only the interactive loop letterboxes; tests and replays keep the simulation filling the framebuffer.
void PowderApp::Render(bool letterbox) {
    // Lighting only dresses the composite view; the debug views read the fields directly.
    const bool lit = lighting_enabled_ && render_mode_ == RenderMode::Composite;
    if (lit) RunLightBuildPass();
    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);

    const SimRect sim = letterbox ? FitSimRect(fb_width, fb_height) : SimRect{0, 0, fb_width, fb_height};
    // The shader draws the whole framebuffer: the simulation inside `sim`, the dark cave wall in the letterbox
    // margin; the clear only shows if a draw fails.
    glViewport(0, 0, fb_width, fb_height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(render_program_);
    glUniform2i(CachedUniform(render_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(render_program_, "fbSize"), fb_width, fb_height);
    glUniform4i(CachedUniform(render_program_, "simRect"), sim.x, sim.y, sim.width, sim.height);
    glUniform2i(CachedUniform(render_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform1i(CachedUniform(render_program_, "renderMode"), static_cast<int>(render_mode_));
    glUniform1i(CachedUniform(render_program_, "lighting"), lit ? 1 : 0);
    // The cursor lantern lights the world around the brush; letterboxed (interactive) frames only, so replays and
    // pixel tests never see it. It swells a little while painting.
    const float lantern_radius = lit && letterbox && cursor_in_sim_
                                     ? (kLanternRadius + kLanternPerBrushCell * static_cast<float>(brush_radius_)) *
                                           (1.0F + 0.12F * hud_motion_.painting)
                                     : 0.0F;
    glUniform3f(CachedUniform(render_program_, "lantern"), static_cast<float>(brush_x_) + 0.5F,
                static_cast<float>(brush_y_) + 0.5F, lantern_radius);
    // Flame tongues, sparks and heat shimmer move with simulation time, so a frame stays a function of the state.
    glUniform1f(CachedUniform(render_program_, "fxTime"),
                static_cast<float>(std::fmod(static_cast<double>(frame_index_) * config_.fixed_dt, 3600.0)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CurrentMaterial());
    glUniform1i(CachedUniform(render_program_, "materialTex"), 0);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, CurrentWaterAmount());
    glUniform1i(CachedUniform(render_program_, "waterAmountTex"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, render_smoke_density_);
    glUniform1i(CachedUniform(render_program_, "smokeDensityTex"), 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, render_fire_emissive_);
    glUniform1i(CachedUniform(render_program_, "fireEmissiveTex"), 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, render_temperature_);
    glUniform1i(CachedUniform(render_program_, "temperatureTex"), 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, CurrentWaterVelocity());
    glUniform1i(CachedUniform(render_program_, "waterVelocityTex"), 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, render_gas_velocity_);
    glUniform1i(CachedUniform(render_program_, "gasVelocityTex"), 7);

    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, CurrentGasPressure());
    glUniform1i(CachedUniform(render_program_, "gasPressureTex"), 8);

    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, CurrentReactionRate());
    glUniform1i(CachedUniform(render_program_, "combustionTex"), 9);

    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, boundary_state_.mask_full);
    glUniform1i(CachedUniform(render_program_, "boundaryTex"), 10);

    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D, CurrentFuel());
    glUniform1i(CachedUniform(render_program_, "fuelTex"), 11);

    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, sand_state_.occupancy);
    glUniform1i(CachedUniform(render_program_, "sandOccupancyTex"), 12);

    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, light_texture_);
    glUniform1i(CachedUniform(render_program_, "lightTex"), 13);

    // Unit 1 is free in the composite: 16 samplers on units 0..15 fit the GL 4.3 minimum of 16.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sand_state_.moisture);
    glUniform1i(CachedUniform(render_program_, "sandMoistureTex"), 1);

    glActiveTexture(GL_TEXTURE14);
    glBindTexture(GL_TEXTURE_2D, phase_state_.oil_field);
    glUniform1i(CachedUniform(render_program_, "oilTex"), 14);

    glActiveTexture(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_2D, CurrentSteam());
    glUniform1i(CachedUniform(render_program_, "steamTex"), 15);

    glUniform1i(CachedUniform(render_program_, "debugReplayOk"), replay_failed_ ? 0 : 1);
    glUniform1i(CachedUniform(render_program_, "debugPassCount"), static_cast<int>(pass_timings_ms_.size()));
    glUniform1fv(CachedUniform(render_program_, "debugPassTimings[0]"),
                 static_cast<GLsizei>(pass_timings_ms_.size()),
                 pass_timings_ms_.data());

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
    glBindVertexArray(fullscreen_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (lit && heat_present_) DrawSparks(sim, fb_width, fb_height);
}

// Interactive-only overlay; Render() stays HUD-free so replays and pixel tests are unaffected.
void PowderApp::DrawHud(double now) {
    if (now < 0.0) now = glfwGetTime();
    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);
    const SimRect sim = FitSimRect(fb_width, fb_height);

    // An armed help timer hides the panel once; tests that set help_visible_ directly never arm it.
    if (help_visible_ && help_hide_time_ >= 0.0 && now >= help_hide_time_) {
        help_visible_ = false;
        help_hide_time_ = -1.0;
    }

    // Ease the HUD toward the current state. Each value closes 1 - exp(-dt / tau) of its remaining distance, so a
    // change settles in about 150 ms, starts fast, and reversing mid-way retargets without a jump. The first call
    // (and any stall) snaps.
    HudMotion& motion = hud_motion_;
    const double dt = motion.last_time < 0.0 ? 1.0 : std::clamp(now - motion.last_time, 0.0, 1.0);
    motion.last_time = now;
    const auto ease = [dt](float& value, float target, double tau) {
        value += (target - value) * static_cast<float>(1.0 - std::exp(-dt / tau));
        if (std::abs(target - value) < 0.002F) value = target;
    };
    ease(motion.help, help_visible_ ? 1.0F : 0.0F, 0.06);
    ease(motion.painting, painting_ ? 1.0F : 0.0F, painting_ ? 0.05 : 0.09);
    for (std::size_t i = 0; i < motion.slot_pop.size(); ++i) {
        ease(motion.slot_pop[i], static_cast<std::size_t>(brush_mode_) == i ? 1.0F : 0.0F, 0.05);
    }

    glViewport(0, 0, fb_width, fb_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // hud.frag writes premultiplied alpha

    glUseProgram(hud_program_);
    glUniform2i(CachedUniform(hud_program_, "fbSize"), fb_width, fb_height);
    glUniform4i(CachedUniform(hud_program_, "simRect"), sim.x, sim.y, sim.width, sim.height);
    glUniform2i(CachedUniform(hud_program_, "gridSize"), kGridWidth, kGridHeight);
    // Whole-number scale keeps glyph texels crisp: one step per ~450 framebuffer rows.
    const int ui_scale = std::max(1, (fb_height + 225) / 450);
    glUniform1i(CachedUniform(hud_program_, "uiScale"), ui_scale);
    glUniform1i(CachedUniform(hud_program_, "brushMode"), static_cast<int>(brush_mode_));
    glUniform1i(CachedUniform(hud_program_, "brushRadius"), brush_radius_);
    glUniform2i(CachedUniform(hud_program_, "brushCell"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(hud_program_, "cursorValid"), cursor_in_sim_ ? 1 : 0);
    glUniform1f(CachedUniform(hud_program_, "painting"), motion.painting);
    glUniform1f(CachedUniform(hud_program_, "helpMix"), motion.help);
    glUniform1fv(CachedUniform(hud_program_, "slotPop[0]"), static_cast<GLsizei>(motion.slot_pop.size()),
                 motion.slot_pop.data());
    glUniform1i(CachedUniform(hud_program_, "overlay"), hud::FitsInMargin(sim.x, ui_scale) ? 0 : 1);
    glUniform1i(CachedUniform(hud_program_, "edge"), hud::kEdge);
    glUniform1i(CachedUniform(hud_program_, "pad"), hud::kPlatePad);
    glUniform4i(CachedUniform(hud_program_, "inventory"), hud::kPanelWidth, hud::kInventoryHeight, hud::kSlotSize,
                hud::kSlotGap);
    glUniform4i(CachedUniform(hud_program_, "spacing"), hud::kDigitGap, hud::kInfoGap, hud::kPanelGap, hud::kIconSize);
    glUniform4i(CachedUniform(hud_program_, "helpLayout"), hud::kHelpPanelHeight, hud::kHelpRowPitch,
                hud::kHelpColumnGap, hud::kKeyColumn);
    const auto set_rect = [&](const char* name, const hud::Rect& rect) {
        glUniform4i(CachedUniform(hud_program_, name), rect.x, rect.y, rect.w, rect.h);
    };
    set_rect("hintKeyRect", hud::kHintKeyRect);
    set_rect("hintTextRect", hud::kHintTextRect);
    set_rect("digitRect", hud::kDigitRect);
    const auto set_rects = [&](const char* name, std::size_t count, const auto& rect_at) {
        std::vector<GLint> values(4 * count);
        for (std::size_t i = 0; i < count; ++i) {
            const hud::Rect rect = rect_at(i);
            values[4 * i] = rect.x;
            values[4 * i + 1] = rect.y;
            values[4 * i + 2] = rect.w;
            values[4 * i + 3] = rect.h;
        }
        glUniform4iv(CachedUniform(hud_program_, name), static_cast<GLsizei>(count), values.data());
    };
    set_rects("keyRects[0]", hud::kHelpKeys.size(), hud::KeyRect);
    set_rects("actionRects[0]", hud::kHelpActions.size(), hud::ActionRect);
    set_rects("labelRects[0]", hud::kBrushNames.size(), hud::LabelRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hud_font_texture_);
    glUniform1i(CachedUniform(hud_program_, "fontTex"), 0);

    glBindVertexArray(fullscreen_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void PowderApp::SwapGasMacPing() {
    gas_mac_ping_ = !gas_mac_ping_;
}

void PowderApp::SwapSmokePing() {
    smoke_ping_ = !smoke_ping_;
}

GLuint PowderApp::CurrentMaterial() const {
    return material_a_;
}

GLuint PowderApp::CurrentWaterAmount() const {
    return water_state_.amount;
}

GLuint PowderApp::CurrentWaterVelocity() const {
    return water_state_.render_velocity;
}

GLuint PowderApp::CurrentWaterDivergence() const {
    return water_state_.divergence;
}

GLuint PowderApp::CurrentWaterPressure() const {
    return water_state_.pressure_a;
}

GLuint PowderApp::CurrentGasVelocityCenter() const {
    return gas_mac_ping_ ? gas_state_.velocity_center_a : gas_state_.velocity_center_b;
}

GLuint PowderApp::CurrentSmokeDen() const {
    return smoke_ping_ ? gas_state_.smoke_a : gas_state_.smoke_b;
}

GLuint PowderApp::NextGasVelocityCenter() const {
    return gas_mac_ping_ ? gas_state_.velocity_center_b : gas_state_.velocity_center_a;
}

GLuint PowderApp::NextSmokeDen() const {
    return smoke_ping_ ? gas_state_.smoke_b : gas_state_.smoke_a;
}

GLuint PowderApp::CurrentGasU() const {
    return gas_mac_ping_ ? gas_state_.mac_u_a : gas_state_.mac_u_b;
}

GLuint PowderApp::NextGasU() const {
    return gas_mac_ping_ ? gas_state_.mac_u_b : gas_state_.mac_u_a;
}

GLuint PowderApp::CurrentGasV() const {
    return gas_mac_ping_ ? gas_state_.mac_v_a : gas_state_.mac_v_b;
}

GLuint PowderApp::NextGasV() const {
    return gas_mac_ping_ ? gas_state_.mac_v_b : gas_state_.mac_v_a;
}

GLuint PowderApp::CurrentReactionRate() const {
    return smoke_ping_ ? gas_state_.reaction_a : gas_state_.reaction_b;
}

GLuint PowderApp::NextReactionRate() const {
    return smoke_ping_ ? gas_state_.reaction_b : gas_state_.reaction_a;
}

GLuint PowderApp::CurrentFuel() const {
    return smoke_ping_ ? gas_state_.fuel_a : gas_state_.fuel_b;
}

GLuint PowderApp::NextFuel() const {
    return smoke_ping_ ? gas_state_.fuel_b : gas_state_.fuel_a;
}

GLuint PowderApp::CurrentOxidizer() const {
    return smoke_ping_ ? gas_state_.oxidizer_a : gas_state_.oxidizer_b;
}

GLuint PowderApp::NextOxidizer() const {
    return smoke_ping_ ? gas_state_.oxidizer_b : gas_state_.oxidizer_a;
}

GLuint PowderApp::CurrentTemperature() const {
    return smoke_ping_ ? gas_state_.temperature_a : gas_state_.temperature_b;
}

GLuint PowderApp::NextTemperature() const {
    return smoke_ping_ ? gas_state_.temperature_b : gas_state_.temperature_a;
}

GLuint PowderApp::CurrentGasPressure() const {
    return gas_state_.pressure_a;
}

void PowderApp::DispatchGrid(int width, int height) {
    const GLuint groups_x = static_cast<GLuint>((width + kWorkgroupSize - 1) / kWorkgroupSize);
    const GLuint groups_y = static_cast<GLuint>((height + kWorkgroupSize - 1) / kWorkgroupSize);
    glDispatchCompute(groups_x, groups_y, 1);
}

void PowderApp::DispatchRegionGrid(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

    DispatchGrid(region.width, region.height);
}

int PowderApp::CellCount() {
    return kGridWidth * kGridHeight;
}

double PowderApp::ReadScalarTextureSum(GLuint texture, int width, int height) const {
    std::vector<float> values(static_cast<std::size_t>(width * height), 0.0F);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    double sum = 0.0;
    for (float value : values) {
        if (!std::isfinite(value)) {
            throw std::runtime_error("Non-finite value in scalar texture sum");
        }
        sum += std::max(0.0F, value);
    }
    return sum;
}

double PowderApp::ReadReactionTextureSum(GLuint texture, int width, int height) const {
    std::vector<float> values(static_cast<std::size_t>(width * height * 2), 0.0F);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, values.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    double sum = 0.0;
    for (std::size_t i = 0; i < values.size(); i += 2) {
        if (!std::isfinite(values[i]) || !std::isfinite(values[i + 1])) {
            throw std::runtime_error("Non-finite value in reaction texture sum");
        }
        sum += std::max(0.0F, values[i + 1]);
    }
    return sum;
}

GLuint PowderApp::CreateTexture(int width,
                                int height,
                                GLenum internal_format,
                                GLenum format,
                                GLenum type,
                                GLint filter) const {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal_format, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint previous_unpack_alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int texel_count = width * height;
    if (internal_format == GL_R16UI) {
        const std::vector<unsigned short> zeros(static_cast<std::size_t>(texel_count), 0U);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else if (internal_format == GL_R8UI) {
        const std::vector<unsigned char> zeros(static_cast<std::size_t>(texel_count), 0U);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else if (type == GL_UNSIGNED_BYTE) {
        const int channels = (format == GL_RG) ? 2 : (format == GL_RGBA ? 4 : 1);
        const std::vector<unsigned char> zeros(static_cast<std::size_t>(texel_count * channels), 0U);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else if (type == GL_INT) {
        const int channels = (format == GL_RG_INTEGER) ? 2 : (format == GL_RGBA_INTEGER ? 4 : 1);
        const std::vector<int> zeros(static_cast<std::size_t>(texel_count * channels), 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else if (type == GL_HALF_FLOAT) {
        const int channels = (format == GL_RG) ? 2 : (format == GL_RGBA ? 4 : 1);
        const std::vector<std::uint16_t> zeros(static_cast<std::size_t>(texel_count * channels), 0U);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else {
        const int channels = (format == GL_RG) ? 2 : (format == GL_RGBA ? 4 : 1);
        const std::vector<float> zeros(static_cast<std::size_t>(texel_count * channels), 0.0F);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_FLOAT, zeros.data());
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

GLint PowderApp::CachedUniform(GLuint program, const char* name) {
    auto& entry = uniform_locations_[program];
    const auto it = entry.find(std::string_view(name));
    if (it != entry.end()) return it->second;
    const GLint location = glGetUniformLocation(program, name);
    entry.emplace(name, location);
    return location;
}

void PowderApp::DeleteProgram(GLuint& program) {
    if (program != 0U) {
        uniform_locations_.erase(program);
        glDeleteProgram(program);
        program = 0;
    }
}

void PowderApp::DeleteTexture(GLuint& texture) {
    if (texture != 0U) {
        glDeleteTextures(1, &texture);
        texture = 0;
    }
}

void PowderApp::DeleteBuffer(GLuint& buffer) {
    if (buffer != 0U) {
        glDeleteBuffers(1, &buffer);
        buffer = 0;
    }
}

int PowderApp::RunReplay() {
    const char* replay_scene_env = std::getenv("POWDER_REPLAY");
    const std::string requested_scene = replay_scene_env != nullptr ? std::string(replay_scene_env) : std::string("all");
    const std::vector<ReplayScene> scenes = BuildReplayScenes();
    bool ran_scene = false;

    for (const ReplayScene& scene : scenes) {
        if (requested_scene != "all" && requested_scene != scene.name) {
            continue;
        }
        ran_scene = true;

        replay_has_reference_signature_ = false;
        replay_determinism_ok_ = true;
        ReplaySignature reference_signature{};
        const int repetitions = replay_mode_ ? config_.replay_repeat_count : 1;
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            replay_scene_name_ = scene.name;
            replay_frame_index_ = 0;
            replay_failed_ = false;
            ResetReplayMetrics();
            InitializeState();

            for (replay_frame_index_ = 0; replay_frame_index_ < scene.total_frames; ++replay_frame_index_) {
                glfwPollEvents();
                glQueryCounter(replay_frame_queries_[0], GL_TIMESTAMP);
                ++frame_index_;
                const GLuint zero_uint = 0U;
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
                glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                BeginTimedPass(TimedPass::Spawn);
                for (const ReplayBrushEvent& event : scene.events) {
                    if (event.frame > replay_frame_index_ || replay_frame_index_ >= event.frame + event.hold_frames) {
                        continue;
                    }
                    ApplyReplayBrush(event);
                    const BrushConfig config = BuildBrushConfig();
                    RunSpawnPass(&config);
                }
                EndTimedPass(TimedPass::Spawn);
                BeginTimedPass(TimedPass::Boundary);
                RunBoundaryPass();
                EndTimedPass(TimedPass::Boundary);
                BeginTimedPass(TimedPass::Water);
                RunWaterPass(config_.fixed_dt);
                EndTimedPass(TimedPass::Water);
                BeginTimedPass(TimedPass::Gas);
                RunGasTick(config_.fixed_dt);
                EndTimedPass(TimedPass::Gas);
                BeginTimedPass(TimedPass::Sand);
                RunSandPass(config_.fixed_dt);
                EndTimedPass(TimedPass::Sand);
                BeginTimedPass(TimedPass::Coupling);
                RunCouplingPass(config_.fixed_dt);
                EndTimedPass(TimedPass::Coupling);
                BeginTimedPass(TimedPass::Extract);
                RunRenderFieldExtractionPass();
                EndTimedPass(TimedPass::Extract);
                Render();
                glQueryCounter(replay_frame_queries_[1], GL_TIMESTAMP);
                std::array<GLuint64, 2> frame_timestamps{};
                for (std::size_t i = 0; i < frame_timestamps.size(); ++i) {
                    glGetQueryObjectui64v(replay_frame_queries_[i], GL_QUERY_RESULT, &frame_timestamps[i]);
                }
                const float frame_ms = static_cast<float>(frame_timestamps[1] - frame_timestamps[0]) / 1000000.0F;
                if (replay_frame_timing_.samples == 0) replay_frame_timing_.min_ms = frame_ms;
                replay_frame_timing_.min_ms = std::min(replay_frame_timing_.min_ms, frame_ms);
                replay_frame_timing_.max_ms = std::max(replay_frame_timing_.max_ms, frame_ms);
                replay_frame_timing_.sum_ms += frame_ms;
                ++replay_frame_timing_.samples;
                ReadBackTimingQueries(true);
                UpdateReplayMetrics(scene, replay_frame_index_);
                AccumulateReplayDebugCounters();
            }

            replay_current_signature_ = CaptureReplaySignature();
            if (repetition == 0) {
                reference_signature = replay_current_signature_;
                replay_reference_signature_ = reference_signature;
                replay_has_reference_signature_ = true;
            } else if (!ReplaySignaturesClose(reference_signature, replay_current_signature_)) {
                replay_determinism_ok_ = false;
            }

            if (!ReplayMetricsOk()) {
                return 1;
            }
            std::cerr << "Replay scene passed: " << replay_scene_name_ << " frame avg/max ms: "
                      << replay_frame_timing_.sum_ms / replay_frame_timing_.samples << '/'
                      << replay_frame_timing_.max_ms << " pass avg/max ms:";
            for (const ReplayTimingAggregate& value : replay_timing_aggregates_) {
                const double avg = value.samples > 0 ? value.sum_ms / static_cast<double>(value.samples) : -1.0;
                std::cerr << ' ' << std::fixed << std::setprecision(2) << avg << '/' << value.max_ms;
            }
            std::cerr << " signature water/liquid/smoke/temp/reaction/sand="
                      << replay_current_signature_.water_amount_sum << '/'
                      << replay_current_signature_.liquid_volume_sum << '/'
                      << replay_current_signature_.smoke_sum << '/'
                      << replay_current_signature_.temperature_sum << '/'
                      << replay_current_signature_.reaction_sum << '/'
                      << replay_current_signature_.sand_occupancy_sum << " water_velocity_clamped="
                      << replay_validation_.accumulated_debug_counters[kDebugFailureCounters] << '\n';
        }
    }

    if (!ran_scene) {
        std::cerr << "Unknown POWDER_REPLAY scene: " << requested_scene << " (valid: all";
        for (const ReplayScene& scene : scenes) std::cerr << ", " << scene.name;
        std::cerr << ")\n";
        return 1;
    }
    return 0;
}

void PowderApp::ApplyReplayBrush(const ReplayBrushEvent& event) {
    brush_mode_ = event.mode;
    brush_x_ = std::clamp(event.x, 0, kGridWidth - 1);
    brush_y_ = std::clamp(event.y, 0, kGridHeight - 1);
    brush_radius_ = std::clamp(event.radius, 1, 96);
}

std::vector<PowderApp::ReplayScene> PowderApp::BuildReplayScenes() const {
    std::vector<ReplayScene> scenes{
        {"water", 240, {{0, BrushMode::Water, 500, 920, 12, 90}}},
        {"gas", 240, {{0, BrushMode::Smoke, 500, 96, 18, 140},
                      {24, BrushMode::Fire, 500, 128, 12, 120},
                      {120, BrushMode::Water, 500, 164, 24, 33}}},
        {"sand", 220, {{0, BrushMode::Sand, 500, 900, 18, 140}, {110, BrushMode::Water, 500, 760, 12, 50}}},
        {"combined", 300, {{0, BrushMode::Sand, 300, 900, 18, 140},
                           {0, BrushMode::Water, 700, 920, 10, 70},
                           {24, BrushMode::Smoke, 500, 120, 18, 180},
                           {56, BrushMode::Fire, 500, 180, 12, 150}}}
    };
    for (const int radius : {24, 3}) {
        ReplayScene scene{radius == 24 ? "boundary" : "leakage", 220, {}};
        // Overlapping disks seal the entire domain, including both edges.
        const int spacing = radius == 24 ? 32 : 4;
        for (int x = 0; x <= kGridWidth; x += spacing) {
            scene.events.push_back({0, BrushMode::Solid, x, 500, radius, 1});
        }
        scene.events.push_back({4, BrushMode::Water, 500, 530 + radius, 10, 70});
        scenes.push_back(std::move(scene));
    }
    return scenes;
}

void PowderApp::ResetReplayMetrics() {
    pass_timings_ms_.fill(-1.0F);
    replay_timing_aggregates_.fill(ReplayTimingAggregate{});
    replay_frame_timing_ = ReplayTimingAggregate{};
    replay_failed_ = false;
    replay_validation_ = ReplayValidationState{};
    replay_current_signature_ = ReplaySignature{};
}

void PowderApp::UpdateReplayMetrics(const ReplayScene& scene, int frame_index) {
    if (scene.name == "water") {
        const double water_sum = ReadScalarTextureSum(CurrentWaterAmount(), kGridWidth, kGridHeight);
        const ParticleMetrics water = ReadWaterMetrics();
        replay_validation_.water_live_particles = water.count;
        if (frame_index <= 89 && water.mass + 1e-3 < replay_validation_.last_water_mass) {
            replay_validation_.water_monotonic = false;
        }
        if (frame_index == 89) replay_validation_.water_reference_mass = water.mass;
        if (frame_index > 89 && std::abs(water.mass - replay_validation_.water_reference_mass) >
                                    replay_validation_.water_reference_mass * 0.005) {
            replay_validation_.water_mass_conserved = false;
        }
        replay_validation_.last_water_mass = water.mass;
        replay_validation_.last_water_sum = water_sum;
        return;
    }

    if (scene.name == "gas") {
        replay_validation_.gas_reaction_sum = ReadReactionTextureSum(CurrentReactionRate(), kGasWidth, kGasHeight);
        if (frame_index < 120 && replay_validation_.gas_reaction_sum >= 2.5) {
            replay_validation_.gas_ignited = true;
        }
        if (frame_index >= 180) {
            if (replay_validation_.gas_reaction_sum < 2.5) {
                replay_validation_.gas_extinguished = true;
            }
        }
        return;
    }

    if (scene.name == "sand") {
        // Moving grains plus the sleeping ones, each of mass one.
        const double sand_mass = ReadParticleMetrics(sand_state_.particles, kMaxSandParticles, kSandParticleComponents).mass +
                                 static_cast<double>(ReadSandPackedSum());
        if (frame_index == 139) {
            replay_validation_.sand_reference_mass = sand_mass;
        }
        if (frame_index == scene.total_frames - 1) {
            replay_validation_.sand_final_mass = sand_mass;
        }
        if (frame_index > 139 && std::abs(sand_mass - replay_validation_.sand_reference_mass) >
                                     replay_validation_.sand_reference_mass * 0.005) {
            replay_validation_.sand_mass_conserved = false;
        }
        return;
    }

    if (scene.name == "combined") {
        const double reaction = ReadReactionTextureSum(CurrentReactionRate(), kGasWidth, kGasHeight);
        replay_validation_.gas_ignited = replay_validation_.gas_ignited || reaction >= 2.5;
    }

    if (scene.name == "boundary" || scene.name == "leakage") {
        const ParticleMetrics water = ReadWaterMetrics();
        int last_source_frame = -1;
        for (const ReplayBrushEvent& event : scene.events) {
            if (event.mode == BrushMode::Water) last_source_frame = std::max(last_source_frame, event.frame + event.hold_frames - 1);
        }
        if (frame_index == last_source_frame) replay_validation_.water_reference_mass = water.mass;
        replay_validation_.last_water_mass = water.mass;
        if (frame_index > last_source_frame && replay_validation_.water_reference_mass > 0.0 &&
            std::abs(water.mass - replay_validation_.water_reference_mass) > replay_validation_.water_reference_mass * 0.005) {
            replay_validation_.water_mass_conserved = false;
        }
    }

    if ((scene.name == "boundary" || scene.name == "leakage") && frame_index == scene.total_frames - 1) {
        std::vector<float> water(static_cast<std::size_t>(kGridWidth * kGridHeight), 0.0F);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, CurrentWaterAmount());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, water.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        for (int y = 0; y < kGridHeight; ++y) {
            for (int x = 0; x < kGridWidth; ++x) {
                const float raw_value = water[static_cast<std::size_t>(y * kGridWidth + x)];
                if (!std::isfinite(raw_value)) {
                    throw std::runtime_error("Non-finite value in leakage water sum");
                }
                const float value = std::max(0.0F, raw_value);
                replay_validation_.leakage_total_water += value;
                if (y < 480) {
                    replay_validation_.leakage_below_barrier += value;
                }
            }
        }
        std::vector<std::array<float, kWaterParticleComponents>> particles(water_state_.record_bound);
        std::vector<GLuint> solid(kGridWidth * kGridHeight);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                          static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, boundary_state_.mask_static);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, solid.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        for (const auto& particle : particles) {
            if (particle[4] < 0.5F) continue;
            if (particle[0] < 0.0F || particle[0] >= kGridWidth || particle[1] < 0.0F || particle[1] >= kGridHeight) {
                replay_validation_.boundary_crossed_mass += particle[5];
                continue;
            }
            const int x = static_cast<int>(particle[0]);
            const int y = static_cast<int>(particle[1]);
            if (y < 500 || solid[y * kGridWidth + x] != 0U) {
                replay_validation_.boundary_crossed_mass += particle[5];
            } else if (y >= 2 && (solid[(y - 1) * kGridWidth + x] != 0U || solid[(y - 2) * kGridWidth + x] != 0U)) {
                replay_validation_.boundary_contact_mass += particle[5];
            }
        }
    }
}

void PowderApp::AccumulateReplayDebugCounters() {
    const std::array<GLuint, 5> counters = ReadDebugCounters();
    for (std::size_t i = 0; i < counters.size(); ++i) {
        replay_validation_.accumulated_debug_counters[i] += counters[i];
    }
}

void PowderApp::BeginTimedPass(TimedPass pass) {
    const GLuint query = timing_queries_[static_cast<std::size_t>(pass)];
    if (query == 0U) {
        return;
    }
    glBeginQuery(GL_TIME_ELAPSED, query);
}

void PowderApp::EndTimedPass(TimedPass pass) {
    const GLuint query = timing_queries_[static_cast<std::size_t>(pass)];
    if (query == 0U) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
}

void PowderApp::ReadBackTimingQueries() {
    ReadBackTimingQueries(false);
}

void PowderApp::ReadBackTimingQueries(bool accumulate) {
    if (!replay_mode_) {
        return;
    }

    bool timings_valid = true;
    for (std::size_t i = 0; i < timing_queries_.size(); ++i) {
        if (timing_queries_[i] == 0U) {
            pass_timings_ms_[i] = -1.0F;
            timings_valid = false;
            continue;
        }
        GLuint64 elapsed_ns = 0;
        glGetQueryObjectui64v(timing_queries_[i], GL_QUERY_RESULT, &elapsed_ns);
        pass_timings_ms_[i] = static_cast<float>(elapsed_ns) / 1000000.0F;
        if (!std::isfinite(pass_timings_ms_[i]) || pass_timings_ms_[i] < 0.0F) {
            timings_valid = false;
            continue;
        }
        if (accumulate) {
            ReplayTimingAggregate& aggregate = replay_timing_aggregates_[i];
            if (aggregate.samples == 0) {
                aggregate.min_ms = pass_timings_ms_[i];
                aggregate.max_ms = pass_timings_ms_[i];
            } else {
                aggregate.min_ms = std::min(aggregate.min_ms, pass_timings_ms_[i]);
                aggregate.max_ms = std::max(aggregate.max_ms, pass_timings_ms_[i]);
            }
            aggregate.sum_ms += pass_timings_ms_[i];
            ++aggregate.samples;
        }
    }

    replay_failed_ = replay_failed_ || !timings_valid;
    if (window_ == nullptr) {
        return;
    }

    std::ostringstream title;
    title << "Powder Game";
    if (!replay_scene_name_.empty()) {
        title << " [" << replay_scene_name_ << "]";
    }
    title << ' ' << std::fixed << std::setprecision(2);
    for (std::size_t i = 0; i < pass_timings_ms_.size(); ++i) {
        title << pass_timings_ms_[i];
        if (i + 1U != pass_timings_ms_.size()) {
            title << '/';
        }
    }
    glfwSetWindowTitle(window_, title.str().c_str());
}

std::array<GLuint, 5> PowderApp::ReadDebugCounters() const {
    std::array<GLuint, 5> counters{};
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                       0,
                       static_cast<GLsizeiptr>(sizeof(GLuint) * counters.size()),
                       counters.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return counters;
}

PowderApp::ParticleMetrics PowderApp::ReadParticleMetrics(GLuint buffer, int capacity, int components) const {
    // Every live record of a pool sits below its spawn cursor (compaction count plus the appends since), so only
    // that prefix is read.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    GLuint cursor = static_cast<GLuint>(capacity);
    if (buffer == water_state_.particles || buffer == sand_state_.particles) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,
                     buffer == water_state_.particles ? water_state_.spawn_cursor : sand_state_.spawn_cursor);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
    }
    std::vector<float> particles(static_cast<std::size_t>(std::min<GLuint>(cursor, static_cast<GLuint>(capacity))) * components);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    ParticleMetrics result;
    for (std::size_t offset = 0; offset < particles.size(); offset += components) {
        const float* particle = particles.data() + offset;
        if (!std::isfinite(particle[4])) throw std::runtime_error("Non-finite particle active flag");
        if (particle[4] < 0.5F) continue;
        if (!std::all_of(particle, particle + components, [](float value) { return std::isfinite(value); }) ||
            particle[5] <= 0.0F) {
            throw std::runtime_error("Invalid live particle state or mass");
        }
        ++result.count;
        result.mass += particle[5];
    }
    return result;
}

GLuint PowderApp::ReadSandLiveParticles() const {
    return ReadParticleMetrics(sand_state_.particles, kMaxSandParticles, kSandParticleComponents).count;
}

std::uint64_t PowderApp::ReadSandPackedSum() const {
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(CellCount()));
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, sand_state_.packed);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, packed.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    return std::accumulate(packed.begin(), packed.end(), std::uint64_t{0});
}

std::uint64_t PowderApp::ReadWaterPackedSum() const {
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(CellCount()));
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, water_state_.packed);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, packed.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    // Bit 7 flags a pending wake (water_pack.comp).
    return std::accumulate(packed.begin(), packed.end(), std::uint64_t{0},
                           [](std::uint64_t sum, std::uint8_t value) { return sum + (value & 127U); });
}

PowderApp::ParticleMetrics PowderApp::ReadWaterMetrics() const {
    ParticleMetrics metrics = ReadParticleMetrics(water_state_.particles, kMaxWaterParticles, kWaterParticleComponents);
    const std::uint64_t packed = ReadWaterPackedSum();
    metrics.count += static_cast<GLuint>(packed);
    metrics.mass += static_cast<double>(packed) * config_.water_particle_mass;
    return metrics;
}

// Moving plus packed particles.
GLuint PowderApp::ReadWaterLiveParticles() const {
    return ReadWaterMetrics().count;
}

PowderApp::ReplaySignature PowderApp::CaptureReplaySignature() const {
    ReplaySignature signature{};
    signature.water_amount_sum = ReadScalarTextureSum(CurrentWaterAmount(), kGridWidth, kGridHeight);
    signature.liquid_volume_sum = ReadScalarTextureSum(water_state_.liquid_volume, kGridWidth, kGridHeight);
    signature.smoke_sum = ReadScalarTextureSum(CurrentSmokeDen(), kGasWidth, kGasHeight);
    signature.temperature_sum = ReadScalarTextureSum(CurrentTemperature(), kGasWidth, kGasHeight);
    signature.reaction_sum = ReadReactionTextureSum(CurrentReactionRate(), kGasWidth, kGasHeight);
    signature.sand_occupancy_sum = ReadScalarTextureSum(sand_state_.occupancy, kGridWidth, kGridHeight);
    const ParticleMetrics water = ReadWaterMetrics();
    const ParticleMetrics sand = ReadParticleMetrics(sand_state_.particles, kMaxSandParticles, kSandParticleComponents);
    signature.water_live_particles = water.count;
    const std::uint64_t packed = ReadSandPackedSum();
    // Grains moving or sleeping: the split may differ between runs, the total may not.
    signature.sand_live_particles = sand.count + static_cast<GLuint>(packed);
    signature.water_mass = water.mass;
    signature.sand_mass = sand.mass + static_cast<double>(packed);
    return signature;
}

bool PowderApp::ReplaySignaturesClose(const ReplaySignature& a, const ReplaySignature& b) const {
    const auto close_scalar = [this](double lhs, double rhs) {
        if (!std::isfinite(lhs) || !std::isfinite(rhs)) return false;
        const double diff = std::abs(lhs - rhs);
        const double scale = std::max({1.0, std::abs(lhs), std::abs(rhs)});
        return diff <= static_cast<double>(config_.replay_determinism_absolute_tolerance) ||
               diff <= scale * static_cast<double>(config_.replay_determinism_relative_tolerance);
    };

    // Particle masses are immutable; tolerate only double-precision summation roundoff.
    const auto close_mass = [](double lhs, double rhs) {
        return std::isfinite(lhs) && std::isfinite(rhs) &&
               std::abs(lhs - rhs) <= 1e-9 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
    };
    return a.water_live_particles == b.water_live_particles && a.sand_live_particles == b.sand_live_particles &&
           close_mass(a.water_mass, b.water_mass) && close_mass(a.sand_mass, b.sand_mass) &&
           close_scalar(a.water_amount_sum, b.water_amount_sum) &&
           close_scalar(a.liquid_volume_sum, b.liquid_volume_sum) && close_scalar(a.smoke_sum, b.smoke_sum) &&
           close_scalar(a.temperature_sum, b.temperature_sum) && close_scalar(a.reaction_sum, b.reaction_sum) &&
           close_scalar(a.sand_occupancy_sum, b.sand_occupancy_sum);
}

bool PowderApp::ReplayMetricsOk() const {
    const auto valid_timing = [](const ReplayTimingAggregate& value) {
        return value.samples > 0 && std::isfinite(value.min_ms) && std::isfinite(value.max_ms) &&
               std::isfinite(value.sum_ms) && value.min_ms >= 0.0F && value.max_ms >= 0.0F;
    };
    const bool timings_ok = valid_timing(replay_frame_timing_) &&
        std::all_of(replay_timing_aggregates_.begin(), replay_timing_aggregates_.end(), valid_timing);
    // The first four counters are failures. Water velocity clamps are a diagnostic: a splash past the speed cap is
    // reported in every replay line and in the failure listing, but is not an invalid state.
    const bool counters_ok =
        std::all_of(replay_validation_.accumulated_debug_counters.begin(),
                    replay_validation_.accumulated_debug_counters.begin() + kDebugFailureCounters,
                    [](GLuint value) { return value == 0U; });

    bool scene_ok = true;
    std::string scene_failure;
    if (replay_scene_name_ == "water") {
        scene_ok = replay_validation_.water_monotonic && replay_validation_.water_mass_conserved &&
                   replay_validation_.water_reference_mass > 0.0 && replay_validation_.last_water_sum > 10.0 &&
                   replay_validation_.water_live_particles > 0U &&
                   replay_validation_.water_live_particles <= static_cast<GLuint>(kMaxWaterParticles);
        if (!scene_ok) {
            scene_failure = "water source or mass conservation metric failed";
        }
    } else if (replay_scene_name_ == "gas") {
        scene_ok = replay_validation_.gas_ignited && replay_validation_.gas_extinguished &&
                   replay_validation_.gas_reaction_sum < 2.5;
        if (!scene_ok) {
            scene_failure = "gas ignition/suppression metric failed";
        }
    } else if (replay_scene_name_ == "sand") {
        scene_ok = replay_validation_.sand_mass_conserved && replay_validation_.sand_reference_mass > 0.0 &&
                   std::abs(replay_validation_.sand_final_mass - replay_validation_.sand_reference_mass) <=
                       replay_validation_.sand_reference_mass * 0.005;
        if (!scene_ok) {
            scene_failure = "sand mass conservation metric failed";
        }
    } else if (replay_scene_name_ == "boundary" || replay_scene_name_ == "leakage") {
        scene_ok = replay_validation_.water_reference_mass > 0.0 && replay_validation_.water_mass_conserved &&
                   replay_validation_.last_water_mass > 0.0 && replay_validation_.boundary_contact_mass > 0.0 &&
                   replay_validation_.boundary_crossed_mass == 0.0;
        if (!scene_ok) {
            scene_failure = "boundary leakage metric failed";
        }
    } else if (replay_scene_name_ == "combined") {
        const double average_total_ms = replay_frame_timing_.samples > 0 ?
            replay_frame_timing_.sum_ms / replay_frame_timing_.samples : std::numeric_limits<double>::infinity();
        scene_ok = replay_current_signature_.water_live_particles > 0U && replay_current_signature_.water_mass > 0.0 &&
                   replay_current_signature_.sand_live_particles > 0U && replay_current_signature_.sand_mass > 0.0 &&
                   replay_current_signature_.smoke_sum > 0.0 && replay_validation_.gas_ignited &&
                   average_total_ms <= static_cast<double>(config_.combined_target_frame_ms) &&
                   replay_frame_timing_.max_ms <= config_.combined_worst_frame_ms;
        if (!scene_ok) {
            scene_failure = "combined content or performance metric failed";
        }
    }

    const bool ok = timings_ok && counters_ok && scene_ok && replay_determinism_ok_ && !replay_failed_;
    if (!ok) {
        std::cerr << "Replay scene failed: " << replay_scene_name_ << '\n';
        if (!timings_ok) {
            std::cerr << "  timing query results were invalid\n";
        } else {
            std::cerr << "  frame avg/max ms: " << replay_frame_timing_.sum_ms / replay_frame_timing_.samples
                      << '/' << replay_frame_timing_.max_ms << '\n';
            std::cerr << "  timing aggregates avg/max ms:";
            for (const ReplayTimingAggregate& value : replay_timing_aggregates_) {
                const double avg = value.samples > 0 ? value.sum_ms / static_cast<double>(value.samples) : -1.0;
                std::cerr << ' ' << std::fixed << std::setprecision(2) << avg << '/' << value.max_ms;
            }
            std::cerr << '\n';
        }
        if (!counters_ok) {
            std::cerr << "  accumulated debug counters:\n";
            for (std::size_t i = 0; i < replay_validation_.accumulated_debug_counters.size(); ++i) {
                const GLuint value = replay_validation_.accumulated_debug_counters[i];
                if (value != 0U) {
                    std::cerr << "    " << kDebugCounterNames[i] << ": " << value << '\n';
                }
            }
        }
        if (!scene_ok) {
            std::cerr << "  " << scene_failure << '\n';
            std::cerr << "  water_sum=" << replay_validation_.last_water_sum
                      << " water_reference_mass=" << replay_validation_.water_reference_mass
                      << " water_final_mass=" << replay_validation_.last_water_mass
                      << " water_live_particles=" << replay_validation_.water_live_particles
                      << " gas_ignited=" << replay_validation_.gas_ignited
                      << " gas_reaction_sum=" << replay_validation_.gas_reaction_sum
                      << " sand_reference_mass=" << replay_validation_.sand_reference_mass
                      << " sand_final_mass=" << replay_validation_.sand_final_mass
                      << " leakage_total_water=" << replay_validation_.leakage_total_water
                      << " leakage_below_barrier=" << replay_validation_.leakage_below_barrier
                      << " boundary_contact_mass=" << replay_validation_.boundary_contact_mass
                      << " boundary_crossed_mass=" << replay_validation_.boundary_crossed_mass << '\n';
        }
        if (replay_failed_) {
            std::cerr << "  replay internal failure flag was set\n";
        }
        if (!replay_determinism_ok_) {
            std::cerr << "  deterministic replay signature drifted beyond tolerance\n";
            std::cerr << "  reference water/liquid/smoke/temp/reaction/sand="
                      << replay_reference_signature_.water_amount_sum << '/'
                      << replay_reference_signature_.liquid_volume_sum << '/'
                      << replay_reference_signature_.smoke_sum << '/'
                      << replay_reference_signature_.temperature_sum << '/'
                      << replay_reference_signature_.reaction_sum << '/'
                      << replay_reference_signature_.sand_occupancy_sum << '\n';
            std::cerr << "  current water/liquid/smoke/temp/reaction/sand="
                      << replay_current_signature_.water_amount_sum << '/'
                      << replay_current_signature_.liquid_volume_sum << '/'
                      << replay_current_signature_.smoke_sum << '/'
                      << replay_current_signature_.temperature_sum << '/'
                      << replay_current_signature_.reaction_sum << '/'
                      << replay_current_signature_.sand_occupancy_sum << '\n';
        }
    }

    return ok;
}

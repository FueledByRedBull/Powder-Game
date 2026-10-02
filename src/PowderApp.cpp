#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "PowderApp.hpp"

#include "GLUtil.hpp"
#include "FixedStepClock.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

constexpr GLuint kEmptyMaterial = 0U;
constexpr GLuint kSolidMaterial = 2U;

constexpr std::array<const char*, 4> kDebugCounterNames{
    "nan_inf_detected",
    "particle_overflow_blocked",
    "pressure_nonconverged",
    "inactive_tile_misses",
};

std::filesystem::path GetExecutableDirectory() {
#ifdef _WIN32
    std::array<wchar_t, 32768> path_buffer{};
    const DWORD written = GetModuleFileNameW(nullptr, path_buffer.data(), static_cast<DWORD>(path_buffer.size()));
    if (written > 0 && written < path_buffer.size()) {
        return std::filesystem::path(std::wstring(path_buffer.data(), written)).parent_path();
    }
#else
    std::array<char, 4096> path_buffer{};
    const ssize_t written = readlink("/proc/self/exe", path_buffer.data(), path_buffer.size() - 1);
    if (written > 0 && static_cast<std::size_t>(written) < path_buffer.size() - 1) {
        path_buffer[static_cast<std::size_t>(written)] = '\0';
        return std::filesystem::path(path_buffer.data()).parent_path();
    }
#endif
    throw std::runtime_error("Could not determine the executable directory.");
}

}  // namespace

int PowderApp::Run() {
    try {
        if (!Initialize()) {
            return 1;
        }

        if (replay_mode_) {
            const int replay_result = RunReplay();
            Shutdown();
            return replay_result;
        }

        glfwSwapInterval(1);
        FixedStepClock clock;
        double previous_time = glfwGetTime();

        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            UpdateInput();
            const double current_time = glfwGetTime();
            const int steps = clock.Advance(current_time - previous_time, config_.fixed_dt);
            previous_time = current_time;
            for (int step = 0; step < steps; ++step) RunFrame(config_.fixed_dt, nullptr, step == 0);
            Render();
            glfwSwapBuffers(window_);
        }

        Shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        Shutdown();
        return 1;
    }
}

bool PowderApp::Initialize() {
    config_ = powder_config::LoadConfigFromEnvironment();

    if (glfwInit() == GLFW_FALSE) {
        throw std::runtime_error("Failed to initialize GLFW.");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    replay_mode_ = std::getenv("POWDER_REPLAY") != nullptr;
    glfwWindowHint(GLFW_VISIBLE, replay_mode_ ? GLFW_FALSE : GLFW_TRUE);

    window_ = glfwCreateWindow(1280, 900, "Powder Game", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window.");
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, KeyCallback);
    glfwSetInputMode(window_, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);

    glfwMakeContextCurrent(window_);

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        throw std::runtime_error("Failed to load OpenGL functions through GLAD.");
    }

    if (GLAD_GL_VERSION_4_3 == 0) {
        throw std::runtime_error("OpenGL 4.3 is required.");
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

    CreateTextures();
    CreateBuffers();
    CreatePrograms();
    InitializeState();

    return true;
}

void PowderApp::Shutdown() {
    if (gl_ready_) {
        DeleteProgram(render_extract_program_);
        DeleteProgram(render_program_);
        DeleteProgram(coupling_heat_program_);
        DeleteProgram(coupling_fire_program_);
        DeleteProgram(fire_rd_program_);
        DeleteProgram(pressure_residual_program_);
        DeleteProgram(smoke_project_program_);
        DeleteProgram(gas_pressure_prolongate_program_);
        DeleteProgram(gas_pressure_relax_program_);
        DeleteProgram(gas_pressure_restrict_program_);
        DeleteProgram(smoke_pressure_clear_program_);
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
        DeleteProgram(water_divergence_program_);
        DeleteProgram(water_mac_finalize_program_);
        DeleteProgram(water_mac_build_program_);
        DeleteProgram(boundary_downsample_program_);
        DeleteProgram(boundary_distance_program_);
        DeleteProgram(boundary_jumpflood_program_);
        DeleteProgram(boundary_seed_program_);
        DeleteProgram(boundary_build_program_);
        DeleteProgram(sand_rasterize_program_);
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
        DeleteBuffer(sand_state_.grid_momentum_y);
        DeleteBuffer(sand_state_.grid_momentum_x);
        DeleteBuffer(sand_state_.grid_mass);
        DeleteBuffer(sand_state_.spawn_cursor);
        DeleteBuffer(sand_state_.scratch_particles);
        DeleteBuffer(sand_state_.particles);
        DeleteBuffer(water_state_.v_face_velocity_sums);
        DeleteBuffer(water_state_.pressure_scalars);
        DeleteBuffer(water_state_.pressure_partials);
        DeleteBuffer(water_state_.pressure_vectors);
        DeleteBuffer(water_state_.u_face_velocity_sums);
        DeleteBuffer(water_state_.v_face_weights);
        DeleteBuffer(water_state_.u_face_weights);
        DeleteBuffer(water_state_.cell_counts);
        DeleteBuffer(water_state_.spawn_cursor);
        DeleteBuffer(water_state_.scratch_particles);
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
        DeleteTexture(gas_state_.rhs_coarse);
        DeleteTexture(gas_state_.rhs_mid);
        DeleteTexture(gas_state_.divergence);
        DeleteTexture(gas_state_.pressure_coarse_b);
        DeleteTexture(gas_state_.pressure_coarse_a);
        DeleteTexture(gas_state_.pressure_mid_b);
        DeleteTexture(gas_state_.pressure_mid_a);
        DeleteTexture(gas_state_.pressure_b);
        DeleteTexture(gas_state_.pressure_a);
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
        DeleteTexture(render_gas_velocity_);
        DeleteTexture(render_temperature_);
        DeleteTexture(render_fire_emissive_);
        DeleteTexture(render_smoke_density_);

        DeleteTexture(material_a_);
        DeleteTexture(sand_state_.grid_velocity);
        DeleteTexture(sand_state_.occupancy);
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

    gas_state_.mac_u_a = CreateTexture(kGasWidth + 1, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.mac_u_b = CreateTexture(kGasWidth + 1, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.mac_u_tmp = CreateTexture(kGasWidth + 1, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.mac_v_a = CreateTexture(kGasWidth, kGasHeight + 1, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.mac_v_b = CreateTexture(kGasWidth, kGasHeight + 1, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.mac_v_tmp = CreateTexture(kGasWidth, kGasHeight + 1, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.velocity_center_a = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.velocity_center_b = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    gas_state_.velocity_center_tmp = CreateTexture(kGasWidth, kGasHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);

    gas_state_.pressure_a = CreateTexture(kGasWidth, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.pressure_b = CreateTexture(kGasWidth, kGasHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.pressure_mid_a = CreateTexture(kGasMidWidth, kGasMidHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.pressure_mid_b = CreateTexture(kGasMidWidth, kGasMidHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.pressure_coarse_a =
        CreateTexture(kGasCoarseWidth, kGasCoarseHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.pressure_coarse_b =
        CreateTexture(kGasCoarseWidth, kGasCoarseHeight, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
    gas_state_.divergence = CreateTexture(kGasWidth, kGasHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.rhs_mid = CreateTexture(kGasMidWidth, kGasMidHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    gas_state_.rhs_coarse =
        CreateTexture(kGasCoarseWidth, kGasCoarseHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);

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

    sand_state_.occupancy = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    sand_state_.grid_velocity =
        CreateTexture(kGridWidth + 2, kGridHeight + 2, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);

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
        static_cast<GLsizeiptr>(kMaxWaterParticles * static_cast<int>(sizeof(float) * 12));
    const GLsizeiptr sand_particles_bytes =
        static_cast<GLsizeiptr>(kMaxSandParticles * static_cast<int>(sizeof(float) * kSandParticleComponents));
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
    const GLsizeiptr debug_counter_bytes = static_cast<GLsizeiptr>(sizeof(GLuint) * 4);

    glGenBuffers(1, &gas_state_.pressure_check);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gas_state_.pressure_check);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(((kGasWidth + 15) / 16) * ((kGasHeight + 15) / 16) * sizeof(float) * 4),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.pressure_vectors);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_vectors);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(CellCount() * sizeof(float) * 4), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_partials);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_partials);
    const int pressure_partial_count = std::max((CellCount() + 255) / 256,
                                               ((kGridWidth + 15) / 16) * ((kGridHeight + 15) / 16));
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(pressure_partial_count * sizeof(float) * 8),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &water_state_.pressure_scalars);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_scalars);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 4 + sizeof(GLuint) * 8),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.scratch_particles);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.scratch_particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, water_particles_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &water_state_.spawn_cursor);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    glBufferData(GL_SHADER_STORAGE_BUFFER, count_bytes, nullptr, GL_DYNAMIC_DRAW);

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
    glBufferData(GL_SHADER_STORAGE_BUFFER, count_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_mass);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_uint_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_momentum_x);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_x);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_int_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sand_state_.grid_momentum_y);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_y);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sand_grid_int_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &debug_state_.counters);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
    glBufferData(GL_SHADER_STORAGE_BUFFER, debug_counter_bytes, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenQueries(static_cast<GLsizei>(timing_queries_.size()), timing_queries_.data());
    glGenQueries(static_cast<GLsizei>(replay_frame_queries_.size()), replay_frame_queries_.data());
}

void PowderApp::CreatePrograms() {
    const std::filesystem::path shader_root = GetExecutableDirectory() / "shaders";

    init_state_program_ = glutil::CreateComputeProgramFromFile(shader_root / "init_state.comp");
    spawn_full_program_ = glutil::CreateComputeProgramFromFile(shader_root / "spawn_full.comp");
    spawn_gas_program_ = glutil::CreateComputeProgramFromFile(shader_root / "spawn_smoke.comp");
    sand_particle_spawn_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "sand_particle_spawn.comp");
    sand_particle_compact_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "sand_particle_compact.comp");
    sand_particle_clear_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "sand_particle_clear.comp");
    sand_particle_p2g_program_ = glutil::CreateComputeProgramFromFile(shader_root / "sand_particle_p2g.comp");
    sand_grid_update_program_ = glutil::CreateComputeProgramFromFile(shader_root / "sand_grid_update.comp");
    sand_particle_step_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "sand_particle_step.comp");
    sand_rasterize_program_ = glutil::CreateComputeProgramFromFile(shader_root / "sand_rasterize.comp");
    boundary_build_program_ = glutil::CreateComputeProgramFromFile(shader_root / "boundary_build.comp");
    boundary_downsample_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "boundary_downsample.comp");
    boundary_seed_program_ = glutil::CreateComputeProgramFromFile(shader_root / "boundary_seed.comp");
    boundary_jumpflood_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "boundary_jumpflood.comp");
    boundary_distance_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "boundary_distance.comp");
    water_particle_p2g_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_p2g.comp");
    water_particle_spawn_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_spawn.comp");
    water_particle_compact_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_compact.comp");
    water_particle_clear_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_clear.comp");
    water_particle_step_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_step.comp");
    water_particle_finalize_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_particle_finalize.comp");
    water_mac_build_program_ = glutil::CreateComputeProgramFromFile(shader_root / "water_mac_build.comp");
    water_mac_finalize_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_mac_finalize.comp");
    water_divergence_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "water_divergence.comp");
    pressure_cg_program_ = glutil::CreateComputeProgramFromFile(shader_root / "pressure_cg.comp");
    pressure_cg_reduce_program_ = glutil::CreateComputeProgramFromFile(shader_root / "pressure_cg_reduce.comp");
    water_project_program_ = glutil::CreateComputeProgramFromFile(shader_root / "water_project.comp");

    smoke_advect_program_ = glutil::CreateComputeProgramFromFile(shader_root / "smoke_advect.comp");
    smoke_correct_program_ = glutil::CreateComputeProgramFromFile(shader_root / "smoke_correct.comp");
    gas_velocity_advect_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_velocity_advect.comp");
    gas_velocity_correct_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_velocity_correct.comp");
    gas_face_to_center_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_face_to_center.comp");
    smoke_divergence_program_ = glutil::CreateComputeProgramFromFile(shader_root / "smoke_divergence.comp");
    smoke_pressure_clear_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "smoke_pressure_clear.comp");
    gas_pressure_restrict_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_pressure_restrict.comp");
    gas_pressure_relax_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_pressure_relax.comp");
    gas_pressure_prolongate_program_ =
        glutil::CreateComputeProgramFromFile(shader_root / "gas_pressure_prolongate.comp");
    smoke_project_program_ = glutil::CreateComputeProgramFromFile(shader_root / "smoke_project.comp");
    pressure_residual_program_ = glutil::CreateComputeProgramFromFile(shader_root / "pressure_residual.comp");

    fire_rd_program_ = glutil::CreateComputeProgramFromFile(shader_root / "fire_rd.comp");

    coupling_fire_program_ = glutil::CreateComputeProgramFromFile(shader_root / "coupling_fire.comp");
    coupling_heat_program_ = glutil::CreateComputeProgramFromFile(shader_root / "coupling_heat.comp");
    render_extract_program_ = glutil::CreateComputeProgramFromFile(shader_root / "render_extract.comp");

    render_program_ = glutil::CreateProgramFromFiles(shader_root / "fullscreen.vert",
                                                     shader_root / "composite.frag");
}

void PowderApp::InitializeState() {
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    gas_mac_ping_ = smoke_ping_ = true;
    frame_index_ = 0U;
    boundary_state_.static_dirty = true;

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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_x);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_y);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug_state_.counters);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    const std::vector<std::uint16_t> zero_r16(static_cast<std::size_t>((kGridWidth + 1) * (kGridHeight + 1)), 0U);
    const std::vector<std::uint16_t> zero_rg16(static_cast<std::size_t>((kGridWidth + 2) * (kGridHeight + 2) * 2), 0U);
    const std::vector<float> air_phi(static_cast<std::size_t>(kGridWidth * kGridHeight), 1.0F);
    const std::vector<std::uint16_t> zero_r16_gas(static_cast<std::size_t>(kGasWidth * kGasHeight), 0U);
    const std::vector<std::uint16_t> zero_rg16_gas(static_cast<std::size_t>(kGasWidth * kGasHeight * 2), 0U);
    const std::vector<std::uint16_t> zero_r16_gas_u(static_cast<std::size_t>((kGasWidth + 1) * kGasHeight), 0U);
    const std::vector<std::uint16_t> zero_r16_gas_v(static_cast<std::size_t>(kGasWidth * (kGasHeight + 1)), 0U);
    const std::vector<std::uint16_t> zero_r16_mid(static_cast<std::size_t>(kGasMidWidth * kGasMidHeight), 0U);
    const std::vector<std::uint16_t> zero_r16_coarse(static_cast<std::size_t>(kGasCoarseWidth * kGasCoarseHeight), 0U);

    GLint previous_unpack_alignment = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, sand_state_.occupancy);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth, kGridHeight, GL_RED, GL_HALF_FLOAT, zero_r16.data());
    glBindTexture(GL_TEXTURE_2D, sand_state_.grid_velocity);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGridWidth + 2, kGridHeight + 2, GL_RG, GL_HALF_FLOAT, zero_rg16.data());
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
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.oxidizer_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.reaction_tmp);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RG, GL_HALF_FLOAT, zero_rg16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_mid_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasMidWidth, kGasMidHeight, GL_RED, GL_HALF_FLOAT, zero_r16_mid.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_mid_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasMidWidth, kGasMidHeight, GL_RED, GL_HALF_FLOAT, zero_r16_mid.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_coarse_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasCoarseWidth, kGasCoarseHeight, GL_RED, GL_HALF_FLOAT, zero_r16_coarse.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_coarse_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasCoarseWidth, kGasCoarseHeight, GL_RED, GL_HALF_FLOAT, zero_r16_coarse.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasWidth, kGasHeight, GL_RED, GL_HALF_FLOAT, zero_r16_gas.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.rhs_mid);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasMidWidth, kGasMidHeight, GL_RED, GL_HALF_FLOAT, zero_r16_mid.data());
    glBindTexture(GL_TEXTURE_2D, gas_state_.rhs_coarse);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGasCoarseWidth, kGasCoarseHeight, GL_RED, GL_HALF_FLOAT, zero_r16_coarse.data());
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
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_6)
        app->brush_mode_ = static_cast<BrushMode>(key - GLFW_KEY_1);
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F10)
        app->render_mode_ = static_cast<RenderMode>(key - GLFW_KEY_F1);
}

void PowderApp::UpdateInput() {
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window_, &cursor_x, &cursor_y);

    int window_width = 1;
    int window_height = 1;
    glfwGetWindowSize(window_, &window_width, &window_height);

    const double u = std::clamp(cursor_x / std::max(1, window_width), 0.0, 1.0);
    const double v = std::clamp(1.0 - cursor_y / std::max(1, window_height), 0.0, 1.0);

    brush_x_ = std::min(static_cast<int>(u * kGridWidth), kGridWidth - 1);
    brush_y_ = std::min(static_cast<int>(v * kGridHeight), kGridHeight - 1);
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
    RunGasPass(dt);
    EndTimedPass(TimedPass::Gas);
    BeginTimedPass(TimedPass::Sand);
    RunSandPass(dt);
    EndTimedPass(TimedPass::Sand);
    BeginTimedPass(TimedPass::Coupling);
    RunCouplingPass(dt);
    EndTimedPass(TimedPass::Coupling);
    BeginTimedPass(TimedPass::Extract);
    RunRenderFieldExtractionPass();
    EndTimedPass(TimedPass::Extract);
}

void PowderApp::RunSpawnPass(const BrushConfig* override_config) {
    if (override_config == nullptr &&
        (replay_mode_ || glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS)) {
        return;
    }

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

    const BrushConfig config = override_config != nullptr ? *override_config : BuildBrushConfig();
    if (config.replace_material || config.clear_water || config.clear_gas) {
        RunSpawnFullRes(config, full_region);
    }

    if (config.clear_water) {
        RunClearWaterParticles(full_region);
    }

    if (config.spawn_sand) {
        RunSpawnSandParticles(config, full_region);
    }

    if (config.spawn_water) {
        RunSpawnWaterParticles(config, full_region);
    }

    const DispatchRegion gas_region = ComputeDispatchRegion(2, kGasWidth, kGasHeight);
    if (config.clear_gas) {
        RunClearGas(gas_region);
    }
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
            break;
        case BrushMode::Water:
            config.spawn_water = true;
            config.water_particles = 4;
            config.water_velocity = {0.0F, -18.0F};
            break;
        case BrushMode::Solid:
            config.replace_material = true;
            config.material_value = kSolidMaterial;
            break;
        case BrushMode::Smoke:
            config.spawn_gas = true;
            config.smoke_density = 0.24F;
            config.smoke_velocity = {0.0F, 0.9F};
            config.temperature_add = 0.08F;
            break;
        case BrushMode::Fire:
            config.blocked_by_water = true;
            config.spawn_gas = true;
            config.smoke_density = 0.08F;
            config.smoke_velocity = {0.0F, 1.1F};
            config.temperature_add = 0.42F;
            config.fuel_add = 0.42F;
            config.oxidizer_add = 0.16F;
            config.reaction_add = 0.34F;
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

    glUseProgram(spawn_full_program_);
    glUniform2i(CachedUniform(spawn_full_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(spawn_full_program_, "dispatchOrigin"), region.origin_x, region.origin_y);
    glUniform2i(CachedUniform(spawn_full_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(spawn_full_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(spawn_full_program_, "replaceMaterial"), config.replace_material ? 1 : 0);
    glUniform1ui(CachedUniform(spawn_full_program_, "materialValue"), config.material_value);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedBySolid"), config.blocked_by_solid ? 1 : 0);
    glUniform1i(CachedUniform(spawn_full_program_, "blockedByWater"), config.blocked_by_water ? 1 : 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearWater"), config.clear_water ? 1 : 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearGas"), 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearMode"), 0);

    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(3, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);

    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSpawnGas(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

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

    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
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
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSpawnSandParticles(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
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
    glUniform1f(CachedUniform(sand_particle_spawn_program_, "referenceDensity"), config_.sand_reference_density);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "maxParticles"), kMaxSandParticles);
    glUniform1i(CachedUniform(sand_particle_spawn_program_, "blockedBySolid"), config.blocked_by_solid ? 1 : 0);
    glUniform1f(CachedUniform(sand_particle_spawn_program_, "occupancyLimit"), config_.sand_spawn_occupancy_limit);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, sand_state_.occupancy, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, debug_state_.counters);
    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSpawnWaterParticles(const BrushConfig& config, const DispatchRegion& region) {
    if (region.Empty()) {
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
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.spawn_cursor);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, debug_state_.counters);
    DispatchRegionGrid(region);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void PowderApp::RunClearSandParticles(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }

    glUseProgram(sand_particle_clear_program_);
    glUniform2i(CachedUniform(sand_particle_clear_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(sand_particle_clear_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(sand_particle_clear_program_, "maxParticles"), kMaxSandParticles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    glDispatchCompute(static_cast<GLuint>((kMaxSandParticles + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void PowderApp::RunClearWaterParticles(const DispatchRegion& region) {
    if (region.Empty()) {
        return;
    }
    (void)region;

    glUseProgram(water_particle_clear_program_);
    glUniform2i(CachedUniform(water_particle_clear_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(water_particle_clear_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(CachedUniform(water_particle_clear_program_, "brushRadius"), brush_radius_);
    glUniform1i(CachedUniform(water_particle_clear_program_, "maxParticles"), kMaxWaterParticles);
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glDispatchCompute(static_cast<GLuint>((kMaxWaterParticles + 255) / 256), 1, 1);
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
    glUniform1i(CachedUniform(spawn_full_program_, "clearGas"), 0);
    glUniform1i(CachedUniform(spawn_full_program_, "clearMode"), 1);

    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(3, CurrentWaterAmount(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, CurrentReactionRate(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);

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

    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
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
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
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
    glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSandPass(float dt) {
    const double wave_speed = std::sqrt((static_cast<double>(config_.sand_lame_lambda) +
                                        2.0 * config_.sand_shear_modulus) / config_.sand_reference_density);
    const double required = std::ceil(static_cast<double>(dt) * (config_.sand_max_velocity + wave_speed) /
                                     config_.sand_cfl_limit);
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
    sand_state_.substeps = std::max(1, static_cast<int>(required));
    const float sub_dt = dt / static_cast<float>(sand_state_.substeps);
    const double gravity_delta = static_cast<double>(config_.sand_gravity) * sub_dt;
    if (!std::isfinite(gravity_delta) || std::abs(gravity_delta) > 65504.0) {
        throw std::runtime_error("SimulationConfig::sand_gravity * substep dt exceeds the finite R16F range; "
                                 "reduce sand_gravity, POWDER_FIXED_DT, or POWDER_SAND_CFL "
                                 "(increase POWDER_SAND_MAX_SUBSTEPS if needed)");
    }
    const GLuint zero_uint = 0U;
    const GLint zero_int = 0;

    const auto clear_sand_grid = [this, zero_uint, zero_int]() {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_mass);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_x);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.grid_momentum_y);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32I, GL_RED_INTEGER, GL_INT, &zero_int);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    };

    const auto particle_to_grid = [this](float stress_dt) {
        glUseProgram(sand_particle_p2g_program_);
        glUniform2i(CachedUniform(sand_particle_p2g_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1i(CachedUniform(sand_particle_p2g_program_, "maxParticles"), kMaxSandParticles);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "massScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "velocityScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "dt"), stress_dt);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "shearModulus"), config_.sand_shear_modulus);
        glUniform1f(CachedUniform(sand_particle_p2g_program_, "lameLambda"), config_.sand_lame_lambda);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.grid_momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sand_state_.grid_momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        glDispatchCompute(static_cast<GLuint>((kMaxSandParticles + 255) / 256), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    };

    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.scratch_particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sand_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(sand_particle_compact_program_);
    glUniform1i(CachedUniform(sand_particle_compact_program_, "maxParticles"), kMaxSandParticles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.scratch_particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.spawn_cursor);
    glDispatchCompute(static_cast<GLuint>((kMaxSandParticles + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    std::swap(sand_state_.particles, sand_state_.scratch_particles);

    for (int substep = 0; substep < sand_state_.substeps; ++substep) {
        clear_sand_grid();
        particle_to_grid(sub_dt);

        glUseProgram(sand_grid_update_program_);
        glUniform2i(CachedUniform(sand_grid_update_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(sand_grid_update_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(sand_grid_update_program_, "gravity"), config_.sand_gravity);
        glUniform1f(CachedUniform(sand_grid_update_program_, "damping"), config_.sand_damping);
        glUniform1f(CachedUniform(sand_grid_update_program_, "drag"), config_.sand_drag);
        glUniform1f(CachedUniform(sand_grid_update_program_, "wallFriction"), config_.sand_friction_coefficient);
        glUniform1f(CachedUniform(sand_grid_update_program_, "massScale"), 4096.0F);
        glUniform1f(CachedUniform(sand_grid_update_program_, "velocityScale"), 4096.0F);
        glBindImageTexture(0, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, CurrentWaterVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glBindImageTexture(3, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.grid_mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sand_state_.grid_momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sand_state_.grid_momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth + 2, kGridHeight + 2);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

        glUseProgram(sand_particle_step_program_);
        glUniform2i(CachedUniform(sand_particle_step_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(sand_particle_step_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(sand_particle_step_program_, "restitution"), config_.sand_restitution);
        glUniform1f(CachedUniform(sand_particle_step_program_, "maxVelocity"), config_.sand_max_velocity);
        glUniform1i(CachedUniform(sand_particle_step_program_, "maxParticles"), kMaxSandParticles);
        glUniform1f(CachedUniform(sand_particle_step_program_, "shearModulus"), config_.sand_shear_modulus);
        glUniform1f(CachedUniform(sand_particle_step_program_, "lameLambda"), config_.sand_lame_lambda);
        glUniform1f(CachedUniform(sand_particle_step_program_, "frictionAlpha"), config_.sand_friction_alpha);
        glBindImageTexture(0, boundary_state_.sdf_static, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, sand_state_.grid_velocity, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        glDispatchCompute(static_cast<GLuint>((kMaxSandParticles + 255) / 256), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    clear_sand_grid();
    particle_to_grid(0.0F);

    glUseProgram(sand_rasterize_program_);
    glUniform2i(CachedUniform(sand_rasterize_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(sand_rasterize_program_, "massScale"), 4096.0F);
    glBindImageTexture(0, sand_state_.occupancy, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sand_state_.grid_mass);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunWaterPass(float dt) {
    const int substeps = powder_config::SubstepCount(config_.water_max_velocity, dt, config_.water_cfl_limit,
                                                    config_.water_min_substeps, config_.water_max_substeps,
                                                    "POWDER_WATER_MAX_SUBSTEPS");
    water_state_.substeps = substeps;
    const float sub_dt = dt / static_cast<float>(substeps);
    const double gravity_delta = static_cast<double>(config_.water_gravity) * sub_dt;
    if (!std::isfinite(gravity_delta) || std::abs(gravity_delta) > 65504.0) {
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
        glUniform1i(CachedUniform(water_particle_p2g_program_, "maxParticles"), kMaxWaterParticles);
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
        glDispatchCompute(static_cast<GLuint>((kMaxWaterParticles + 255) / 256), 1, 1);
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

    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.scratch_particles);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.spawn_cursor);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero_uint);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(water_particle_compact_program_);
    glUniform1i(CachedUniform(water_particle_compact_program_, "maxParticles"), kMaxWaterParticles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.scratch_particles);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.spawn_cursor);
    glDispatchCompute(static_cast<GLuint>((kMaxWaterParticles + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    std::swap(water_state_.particles, water_state_.scratch_particles);

    for (int step = 0; step < substeps; ++step) {
        build_particle_fields();
        build_mac_faces(true);
        finalize_particle_fields();

        glUseProgram(water_divergence_program_);
        glUniform2i(CachedUniform(water_divergence_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_divergence_program_, "gravityDelta"), config_.water_gravity * sub_dt);
        glUniform1f(CachedUniform(water_divergence_program_, "liquidPhiThreshold"),
                    config_.water_liquid_phi_threshold);
        glBindImageTexture(0, water_state_.mac_u_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, water_state_.mac_v_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, CurrentWaterDivergence(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(4, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
        DispatchGrid(kGridWidth, kGridHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

        RunWaterProjection();

        glUseProgram(water_project_program_);
        glUniform2i(CachedUniform(water_project_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_project_program_, "gravityDelta"), config_.water_gravity * sub_dt);
        glUniform1f(CachedUniform(water_project_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
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

        finalize_mac_faces();

        glUseProgram(water_particle_step_program_);
        glUniform2i(CachedUniform(water_particle_step_program_, "gridSize"), kGridWidth, kGridHeight);
        glUniform1f(CachedUniform(water_particle_step_program_, "dt"), sub_dt);
        glUniform1f(CachedUniform(water_particle_step_program_, "velocityDamping"), config_.water_velocity_damping);
        glUniform1f(CachedUniform(water_particle_step_program_, "maxVelocity"), config_.water_max_velocity);
        glUniform1f(CachedUniform(water_particle_step_program_, "flipBlend"), config_.water_flip_blend);
        glUniform1i(CachedUniform(water_particle_step_program_, "maxParticles"), kMaxWaterParticles);
        glBindImageTexture(0, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(1, water_state_.mac_u_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, water_state_.mac_v_prev, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(3, water_state_.mac_u, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(4, water_state_.mac_v, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, debug_state_.counters);
        glDispatchCompute(static_cast<GLuint>((kMaxWaterParticles + 255) / 256), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    build_particle_fields();
    build_mac_faces(false);
    finalize_mac_faces();
    finalize_particle_fields();
}

void PowderApp::RunWaterProjection() {
    GLuint groups = static_cast<GLuint>((CellCount() + 255) / 256);
    GLuint precondition_groups = 0;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, water_state_.pressure_vectors);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, water_state_.pressure_partials);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, water_state_.pressure_scalars);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, debug_state_.counters);
    glBindImageTexture(0, CurrentWaterDivergence(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentWaterPressure(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(2, boundary_state_.mask_full, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(3, water_state_.liquid_phi, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glUseProgram(pressure_cg_program_);
    glUniform2i(CachedUniform(pressure_cg_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(pressure_cg_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
    glUseProgram(pressure_cg_reduce_program_);
    glUniform2i(CachedUniform(pressure_cg_reduce_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(pressure_cg_reduce_program_, "residualThreshold"), config_.water_pressure_residual_threshold);
    const auto solve_region = [&](GLint x, GLint y, GLint width, GLint height) {
        groups = static_cast<GLuint>((width * height + 255) / 256);
        precondition_groups = static_cast<GLuint>(((width + 15) / 16) * ((height + 15) / 16));
        for (GLuint program : {pressure_cg_program_, pressure_cg_reduce_program_}) {
            glUseProgram(program);
            glUniform2i(CachedUniform(program, "solveOrigin"), x, y);
            glUniform2i(CachedUniform(program, "solveSize"), width, height);
        }
        glUniform1i(CachedUniform(pressure_cg_reduce_program_, "cellCount"), width * height);
    };
    solve_region(0, 0, kGridWidth, kGridHeight);
    const auto vector_step = [&](int phase) {
        glUseProgram(pressure_cg_program_);
        glUniform1i(CachedUniform(pressure_cg_program_, "phase"), phase);
        glDispatchCompute(phase >= 4 ? precondition_groups : groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    };
    const auto final_reduce = [&](int phase, GLuint partial_count) {
        glUseProgram(pressure_cg_reduce_program_);
        glUniform1i(CachedUniform(pressure_cg_reduce_program_, "phase"), phase);
        glUniform1i(CachedUniform(pressure_cg_reduce_program_, "partialCount"), static_cast<GLint>(partial_count));
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    };
    vector_step(0);
    glUseProgram(pressure_cg_reduce_program_);
    glUniform1i(CachedUniform(pressure_cg_reduce_program_, "phase"), 0);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    final_reduce(1, groups);
    const auto still_running = [&]() {
        GLuint running = 0;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_scalars);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(float), sizeof(running), &running);
        return running != 0;
    };
    std::array<GLuint, 12> initial_state{};
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, water_state_.pressure_scalars);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(initial_state), initial_state.data());
    bool running = initial_state[4] != 0;
    if (running) {
        solve_region(static_cast<GLint>(initial_state[8]), static_cast<GLint>(initial_state[9]),
                     static_cast<GLint>(initial_state[10] - initial_state[8] + 1),
                     static_cast<GLint>(initial_state[11] - initial_state[9] + 1));
        vector_step(5);
        final_reduce(7, precondition_groups);
    }
    for (int iteration = 0; iteration < config_.water_pressure_iterations && running; ++iteration) {
        vector_step(1);
        final_reduce(3, groups);
        vector_step(2);
        vector_step(4);
        final_reduce(5, precondition_groups);
        vector_step(3);
        // Amortize the readback while avoiding full-cap dispatches after GPU convergence.
        if ((iteration + 1) % 8 == 0) running = still_running();
    }

    // Always check b-Lp from the stored pressure, including after early convergence or breakdown.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(pressure_residual_program_);
    glUniform2i(CachedUniform(pressure_residual_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1i(CachedUniform(pressure_residual_program_, "boundaryScale"), 1);
    glUniform1f(CachedUniform(pressure_residual_program_, "residualThreshold"), config_.water_pressure_residual_threshold);
    glUniform1i(CachedUniform(pressure_residual_program_, "collectStats"), 0);
    glUniform1i(CachedUniform(pressure_residual_program_, "useLiquidMask"), 1);
    glUniform1f(CachedUniform(pressure_residual_program_, "liquidPhiThreshold"), config_.water_liquid_phi_threshold);
    glUniform1i(CachedUniform(pressure_residual_program_, "pressureTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CurrentWaterPressure());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void PowderApp::RunGasPass(float dt) {
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
        RunSmokePass(sub_dt);
        RunFireHeatPass(sub_dt);
    }
}

void PowderApp::RunSmokePass(float dt) {
    glUseProgram(gas_velocity_advect_program_);
    glUniform2i(CachedUniform(gas_velocity_advect_program_, "gasSize"), kGasWidth, kGasHeight);
    glUniform1f(CachedUniform(gas_velocity_advect_program_, "dt"), dt);
    glUniform1f(CachedUniform(gas_velocity_advect_program_, "maxVelocity"), config_.gas_max_velocity);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(4, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(5, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
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
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentGasVelocityCenter(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, gas_state_.mac_u_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(4, gas_state_.mac_v_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, gas_state_.velocity_center_tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(6, NextSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(7, NextTemperature(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(8, NextGasU(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(9, NextGasV(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(10, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapSmokePing();
    SwapGasMacPing();

    RunGasProjection();
}

void PowderApp::RunGasProjection() {
    glUseProgram(smoke_divergence_program_);
    glUniform2i(CachedUniform(smoke_divergence_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, gas_state_.divergence, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    const auto clear_pressure = [this](GLuint program, GLuint texture, int width, int height) {
        glUseProgram(program);
        glUniform2i(CachedUniform(program, "gridSize"), width, height);
        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        DispatchGrid(width, height);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    };

    clear_pressure(smoke_pressure_clear_program_, gas_state_.pressure_a, kGasWidth, kGasHeight);

    const auto check_residual = [this](bool collect_stats) {
        glUseProgram(pressure_residual_program_);
        glUniform2i(CachedUniform(pressure_residual_program_, "gridSize"), kGasWidth, kGasHeight);
        glUniform1i(CachedUniform(pressure_residual_program_, "boundaryScale"), 1);
        glUniform1f(CachedUniform(pressure_residual_program_, "residualThreshold"), config_.gas_pressure_residual_threshold);
        glUniform1i(CachedUniform(pressure_residual_program_, "collectStats"), collect_stats ? 1 : 0);
        glUniform1i(CachedUniform(pressure_residual_program_, "useLiquidMask"), 0);
        glUniform1f(CachedUniform(pressure_residual_program_, "liquidPhiThreshold"), 0.0F);
        glBindImageTexture(0, gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glUniform1i(CachedUniform(pressure_residual_program_, "pressureTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gas_state_.pressure_a);
        glBindImageTexture(2, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        glBindImageTexture(3, gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gas_state_.pressure_check);
        DispatchGrid(kGasWidth, kGasHeight);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
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
        return count > 0.0 && (maximum > std::min(config_.gas_pressure_residual_threshold, 0.01F) ||
                               square_sum > count * 0.001 * 0.001);
    };

    const auto relax_level = [this](GLuint rhs,
                                    GLuint pressure_a,
                                    GLuint pressure_b,
                                    int width,
                                    int height,
                                    int boundary_scale,
                                    int iterations) {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            glUseProgram(gas_pressure_relax_program_);
            glUniform2i(CachedUniform(gas_pressure_relax_program_, "gridSize"), width, height);
            glUniform1i(CachedUniform(gas_pressure_relax_program_, "boundaryScale"), boundary_scale);
            glBindImageTexture(0, rhs, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
            glBindImageTexture(1, pressure_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            glBindImageTexture(2, pressure_b, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
            DispatchGrid(width, height);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            std::swap(pressure_a, pressure_b);
        }
    };

    bool running = needs_iteration();
    for (int cycle = 0; cycle < config_.gas_pressure_iterations && running; ++cycle) {
        relax_level(gas_state_.divergence, gas_state_.pressure_a, gas_state_.pressure_b, kGasWidth, kGasHeight, 1, 2);

        glUseProgram(gas_pressure_restrict_program_);
        glUniform2i(CachedUniform(gas_pressure_restrict_program_, "fineSize"), kGasWidth, kGasHeight);
        glUniform2i(CachedUniform(gas_pressure_restrict_program_, "coarseSize"), kGasMidWidth, kGasMidHeight);
        glUniform1i(CachedUniform(gas_pressure_restrict_program_, "boundaryScale"), 1);
        glBindImageTexture(0, gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, gas_state_.pressure_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(2, gas_state_.rhs_mid, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kGasMidWidth, kGasMidHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        // Each V-cycle solves a new residual correction, starting from zero.
        clear_pressure(smoke_pressure_clear_program_, gas_state_.pressure_mid_a, kGasMidWidth, kGasMidHeight);
        relax_level(gas_state_.rhs_mid,
                    gas_state_.pressure_mid_a,
                    gas_state_.pressure_mid_b,
                    kGasMidWidth,
                    kGasMidHeight,
                    2,
                    2);

        glUseProgram(gas_pressure_restrict_program_);
        glUniform2i(CachedUniform(gas_pressure_restrict_program_, "fineSize"), kGasMidWidth, kGasMidHeight);
        glUniform2i(CachedUniform(gas_pressure_restrict_program_, "coarseSize"), kGasCoarseWidth, kGasCoarseHeight);
        glUniform1i(CachedUniform(gas_pressure_restrict_program_, "boundaryScale"), 2);
        glBindImageTexture(0, gas_state_.rhs_mid, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, gas_state_.pressure_mid_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(2, gas_state_.rhs_coarse, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kGasCoarseWidth, kGasCoarseHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        clear_pressure(smoke_pressure_clear_program_, gas_state_.pressure_coarse_a, kGasCoarseWidth, kGasCoarseHeight);
        relax_level(gas_state_.rhs_coarse,
                    gas_state_.pressure_coarse_a,
                    gas_state_.pressure_coarse_b,
                    kGasCoarseWidth,
                    kGasCoarseHeight,
                    4,
                    8);

        glUseProgram(gas_pressure_prolongate_program_);
        glUniform2i(CachedUniform(gas_pressure_prolongate_program_, "fineSize"), kGasMidWidth, kGasMidHeight);
        glUniform2i(CachedUniform(gas_pressure_prolongate_program_, "coarseSize"), kGasCoarseWidth, kGasCoarseHeight);
        glUniform1i(CachedUniform(gas_pressure_prolongate_program_, "boundaryScale"), 2);
        glBindImageTexture(0, gas_state_.pressure_coarse_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, gas_state_.pressure_mid_a, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(2, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kGasMidWidth, kGasMidHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        relax_level(gas_state_.rhs_mid,
                    gas_state_.pressure_mid_a,
                    gas_state_.pressure_mid_b,
                    kGasMidWidth,
                    kGasMidHeight,
                    2,
                    2);

        glUseProgram(gas_pressure_prolongate_program_);
        glUniform2i(CachedUniform(gas_pressure_prolongate_program_, "fineSize"), kGasWidth, kGasHeight);
        glUniform2i(CachedUniform(gas_pressure_prolongate_program_, "coarseSize"), kGasMidWidth, kGasMidHeight);
        glUniform1i(CachedUniform(gas_pressure_prolongate_program_, "boundaryScale"), 1);
        glBindImageTexture(0, gas_state_.pressure_mid_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, gas_state_.pressure_a, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
        glBindImageTexture(2, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kGasWidth, kGasHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        relax_level(gas_state_.divergence, gas_state_.pressure_a, gas_state_.pressure_b, kGasWidth, kGasHeight, 1, 2);
        if ((cycle + 1) % 4 == 0 && cycle + 1 < config_.gas_pressure_iterations) running = needs_iteration();
    }

    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    check_residual(false);

    glUseProgram(smoke_project_program_);
    glUniform2i(CachedUniform(smoke_project_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, CurrentGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, CurrentGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, gas_state_.pressure_a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(3, NextGasU(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(4, NextGasV(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(5, boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug_state_.counters);
    DispatchGrid(kGasWidth + 1, kGasHeight + 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(gas_face_to_center_program_);
    glUniform2i(CachedUniform(gas_face_to_center_program_, "gasSize"), kGasWidth, kGasHeight);
    glBindImageTexture(0, NextGasU(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, NextGasV(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
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
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(coupling_heat_program_);
    glUniform2i(CachedUniform(coupling_heat_program_, "heatSize"), kGasWidth, kGasHeight);
    glUniform2i(CachedUniform(coupling_heat_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform1f(CachedUniform(coupling_heat_program_, "dt"), dt);
    glUniform1f(CachedUniform(coupling_heat_program_, "waterCooling"), config_.gas_water_cooling * 0.7F);
    glBindImageTexture(0, CurrentTemperature(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(1, water_state_.liquid_volume, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    DispatchGrid(kGasWidth, kGasHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

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

void PowderApp::Render() {
    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);

    glViewport(0, 0, fb_width, fb_height);
    glClearColor(0.09F, 0.11F, 0.14F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(render_program_);
    glUniform2i(CachedUniform(render_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(CachedUniform(render_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(CachedUniform(render_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform1i(CachedUniform(render_program_, "renderMode"), static_cast<int>(render_mode_));

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

    glUniform1i(CachedUniform(render_program_, "debugReplayOk"), replay_failed_ ? 0 : 1);
    glUniform1i(CachedUniform(render_program_, "debugPassCount"), static_cast<int>(pass_timings_ms_.size()));
    glUniform1fv(CachedUniform(render_program_, "debugPassTimings[0]"),
                 static_cast<GLsizei>(pass_timings_ms_.size()),
                 pass_timings_ms_.data());

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, debug_state_.counters);
    glBindVertexArray(fullscreen_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
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
    const auto it = entry.find(name);
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
                RunGasPass(config_.fixed_dt);
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
                      << replay_current_signature_.sand_occupancy_sum << '\n';
        }
    }

    return ran_scene ? 0 : 1;
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
        const ParticleMetrics water = ReadParticleMetrics(water_state_.particles, kMaxWaterParticles, kWaterParticleComponents);
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
        const double sand_mass = ReadParticleMetrics(sand_state_.particles, kMaxSandParticles, kSandParticleComponents).mass;
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
        const ParticleMetrics water = ReadParticleMetrics(water_state_.particles, kMaxWaterParticles, kWaterParticleComponents);
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
        std::vector<std::array<float, kWaterParticleComponents>> particles(kMaxWaterParticles);
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
    const std::array<GLuint, 4> counters = ReadDebugCounters();
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

std::array<GLuint, 4> PowderApp::ReadDebugCounters() const {
    std::array<GLuint, 4> counters{};
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
    std::vector<float> particles(static_cast<std::size_t>(capacity) * components);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
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

GLuint PowderApp::ReadWaterLiveParticles() const {
    return ReadParticleMetrics(water_state_.particles, kMaxWaterParticles, kWaterParticleComponents).count;
}

PowderApp::ReplaySignature PowderApp::CaptureReplaySignature() const {
    ReplaySignature signature{};
    signature.water_amount_sum = ReadScalarTextureSum(CurrentWaterAmount(), kGridWidth, kGridHeight);
    signature.liquid_volume_sum = ReadScalarTextureSum(water_state_.liquid_volume, kGridWidth, kGridHeight);
    signature.smoke_sum = ReadScalarTextureSum(CurrentSmokeDen(), kGasWidth, kGasHeight);
    signature.temperature_sum = ReadScalarTextureSum(CurrentTemperature(), kGasWidth, kGasHeight);
    signature.reaction_sum = ReadReactionTextureSum(CurrentReactionRate(), kGasWidth, kGasHeight);
    signature.sand_occupancy_sum = ReadScalarTextureSum(sand_state_.occupancy, kGridWidth, kGridHeight);
    const ParticleMetrics water = ReadParticleMetrics(water_state_.particles, kMaxWaterParticles, kWaterParticleComponents);
    const ParticleMetrics sand = ReadParticleMetrics(sand_state_.particles, kMaxSandParticles, kSandParticleComponents);
    signature.water_live_particles = water.count;
    signature.sand_live_particles = sand.count;
    signature.water_mass = water.mass;
    signature.sand_mass = sand.mass;
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
    const bool counters_ok =
        std::all_of(replay_validation_.accumulated_debug_counters.begin(),
                    replay_validation_.accumulated_debug_counters.end(),
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

#include "PowderApp.hpp"

#include "GLUtil.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr GLuint kEmptyMaterial = 0U;
constexpr GLuint kSandMaterial = 1U;
constexpr GLuint kSolidMaterial = 2U;

constexpr GLint kSpawnMaterialNone = -1;

std::filesystem::path GetExecutableDirectory() {
#ifdef _WIN32
    std::array<char, 4096> path_buffer{};
    const DWORD written = GetModuleFileNameA(nullptr, path_buffer.data(), static_cast<DWORD>(path_buffer.size()));
    if (written > 0 && written < path_buffer.size()) {
        return std::filesystem::path(std::string(path_buffer.data(), written)).parent_path();
    }
#else
    std::array<char, 4096> path_buffer{};
    const ssize_t written = readlink("/proc/self/exe", path_buffer.data(), path_buffer.size() - 1);
    if (written > 0) {
        path_buffer[static_cast<std::size_t>(written)] = '\0';
        return std::filesystem::path(path_buffer.data()).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path ResolveShaderRoot() {
    const std::filesystem::path exe_dir = GetExecutableDirectory();

    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::path(POWDER_SHADER_DIR));
    candidates.emplace_back(exe_dir / "shaders");
    candidates.emplace_back(exe_dir.parent_path() / "shaders");
    candidates.emplace_back(std::filesystem::current_path() / "shaders");

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "fullscreen.vert", ec)) {
            return candidate;
        }
    }

    std::string message = "Could not locate shader directory. Checked:";
    for (const auto& candidate : candidates) {
        message += "\n - " + candidate.string();
    }
    throw std::runtime_error(message);
}

}  // namespace

int PowderApp::Run() {
    try {
        if (!Initialize()) {
            return 1;
        }

        glfwSwapInterval(1);

        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            UpdateInput();
            RunFrame(1.0F / 60.0F);
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
    if (glfwInit() == GLFW_FALSE) {
        throw std::runtime_error("Failed to initialize GLFW.");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = glfwCreateWindow(1280, 900, "Powder Game", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("Failed to create GLFW window.");
    }

    glfwMakeContextCurrent(window_);

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        throw std::runtime_error("Failed to load OpenGL functions through GLAD.");
    }

    if (GLAD_GL_VERSION_4_3 == 0) {
        throw std::runtime_error("OpenGL 4.3 is required.");
    }

    gl_ready_ = true;

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
        DeleteProgram(render_program_);
        DeleteProgram(coupling_sand_drag_program_);
        DeleteProgram(coupling_smoke_program_);
        DeleteProgram(coupling_heat_program_);
        DeleteProgram(coupling_fire_program_);
        DeleteProgram(heat_diffuse_program_);
        DeleteProgram(fire_rd_program_);
        DeleteProgram(smoke_project_program_);
        DeleteProgram(smoke_pressure_jacobi_program_);
        DeleteProgram(smoke_pressure_clear_program_);
        DeleteProgram(smoke_divergence_program_);
        DeleteProgram(smoke_vorticity_program_);
        DeleteProgram(smoke_advect_program_);
        DeleteProgram(water_pressure_program_);
        DeleteProgram(water_lbm_program_);
        DeleteProgram(sand_commit_program_);
        DeleteProgram(sand_resolve_program_);
        DeleteProgram(sand_desired_program_);
        DeleteProgram(sand_prepare_program_);
        DeleteProgram(spawn_program_);
        DeleteProgram(init_state_program_);
        DeleteProgram(tile_dilate_program_);
        DeleteProgram(tile_build_program_);

        DeleteBuffer(winner_buffer_);
        DeleteBuffer(proposed_velocity_buffer_);
        DeleteBuffer(desired_buffer_);

        DeleteTexture(heat_b_);
        DeleteTexture(heat_a_);
        DeleteTexture(fire_b_);
        DeleteTexture(fire_a_);
        DeleteTexture(smoke_divergence_);
        DeleteTexture(smoke_pressure_b_);
        DeleteTexture(smoke_pressure_a_);
        DeleteTexture(smoke_den_b_);
        DeleteTexture(smoke_den_a_);
        DeleteTexture(smoke_vel_b_);
        DeleteTexture(smoke_vel_a_);

        DeleteTexture(water_pressure_);
        DeleteTexture(lbm_b2_);
        DeleteTexture(lbm_b1_);
        DeleteTexture(lbm_b0_);
        DeleteTexture(lbm_a2_);
        DeleteTexture(lbm_a1_);
        DeleteTexture(lbm_a0_);

        DeleteTexture(stress_b_);
        DeleteTexture(stress_a_);
        DeleteTexture(velocity_b_);
        DeleteTexture(velocity_a_);
        DeleteTexture(material_b_);
        DeleteTexture(material_a_);
        DeleteTexture(tile_active_b_);
        DeleteTexture(tile_active_a_);

        if (fullscreen_vao_ != 0U) {
            glDeleteVertexArrays(1, &fullscreen_vao_);
            fullscreen_vao_ = 0;
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
    material_b_ = CreateTexture(kGridWidth, kGridHeight, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST);

    velocity_a_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);
    velocity_b_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);

    stress_a_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    stress_b_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);

    lbm_a0_ = CreateTexture(kGridWidth, kGridHeight, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
    lbm_a1_ = CreateTexture(kGridWidth, kGridHeight, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
    lbm_a2_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);

    lbm_b0_ = CreateTexture(kGridWidth, kGridHeight, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
    lbm_b1_ = CreateTexture(kGridWidth, kGridHeight, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
    lbm_b2_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);

    water_pressure_ = CreateTexture(kGridWidth, kGridHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);

    smoke_vel_a_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    smoke_vel_b_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    smoke_den_a_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR);
    smoke_den_b_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR);
    smoke_pressure_a_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    smoke_pressure_b_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);
    smoke_divergence_ = CreateTexture(kSmokeWidth, kSmokeHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST);

    fire_a_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    fire_b_ = CreateTexture(kGridWidth, kGridHeight, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);

    heat_a_ = CreateTexture(kHeatWidth, kHeatHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
    heat_b_ = CreateTexture(kHeatWidth, kHeatHeight, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);

    tile_active_a_ = CreateTexture(kTileWidth, kTileHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
    tile_active_b_ = CreateTexture(kTileWidth, kTileHeight, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, GL_NEAREST);
}

void PowderApp::CreateBuffers() {
    const GLsizeiptr int_bytes = static_cast<GLsizeiptr>(CellCount() * static_cast<int>(sizeof(int)));
    const GLsizeiptr vec2_bytes = static_cast<GLsizeiptr>(CellCount() * static_cast<int>(sizeof(float) * 2));
    const GLsizeiptr uint_bytes = static_cast<GLsizeiptr>(CellCount() * static_cast<int>(sizeof(GLuint)));

    glGenBuffers(1, &desired_buffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, desired_buffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, int_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &proposed_velocity_buffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, proposed_velocity_buffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, vec2_bytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &winner_buffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, winner_buffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, uint_bytes, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void PowderApp::CreatePrograms() {
    const std::filesystem::path shader_root = ResolveShaderRoot();

    init_state_program_ = glutil::CreateComputeProgramFromFile((shader_root / "init_state.comp").string());
    spawn_program_ = glutil::CreateComputeProgramFromFile((shader_root / "spawn.comp").string());
    sand_prepare_program_ = glutil::CreateComputeProgramFromFile((shader_root / "sand_prepare.comp").string());
    sand_desired_program_ = glutil::CreateComputeProgramFromFile((shader_root / "sand_desired.comp").string());
    sand_resolve_program_ = glutil::CreateComputeProgramFromFile((shader_root / "sand_resolve.comp").string());
    sand_commit_program_ = glutil::CreateComputeProgramFromFile((shader_root / "sand_commit.comp").string());
    water_lbm_program_ = glutil::CreateComputeProgramFromFile((shader_root / "water_lbm.comp").string());
    water_pressure_program_ = glutil::CreateComputeProgramFromFile((shader_root / "water_pressure.comp").string());

    smoke_advect_program_ = glutil::CreateComputeProgramFromFile((shader_root / "smoke_advect.comp").string());
    smoke_vorticity_program_ = glutil::CreateComputeProgramFromFile((shader_root / "smoke_vorticity.comp").string());
    smoke_divergence_program_ = glutil::CreateComputeProgramFromFile((shader_root / "smoke_divergence.comp").string());
    smoke_pressure_clear_program_ =
        glutil::CreateComputeProgramFromFile((shader_root / "smoke_pressure_clear.comp").string());
    smoke_pressure_jacobi_program_ =
        glutil::CreateComputeProgramFromFile((shader_root / "smoke_pressure_jacobi.comp").string());
    smoke_project_program_ = glutil::CreateComputeProgramFromFile((shader_root / "smoke_project.comp").string());

    fire_rd_program_ = glutil::CreateComputeProgramFromFile((shader_root / "fire_rd.comp").string());
    heat_diffuse_program_ = glutil::CreateComputeProgramFromFile((shader_root / "heat_diffuse.comp").string());

    coupling_fire_program_ = glutil::CreateComputeProgramFromFile((shader_root / "coupling_fire.comp").string());
    coupling_heat_program_ = glutil::CreateComputeProgramFromFile((shader_root / "coupling_heat.comp").string());
    coupling_smoke_program_ = glutil::CreateComputeProgramFromFile((shader_root / "coupling_smoke.comp").string());
    coupling_sand_drag_program_ =
        glutil::CreateComputeProgramFromFile((shader_root / "coupling_sand_drag.comp").string());
    tile_build_program_ = glutil::CreateComputeProgramFromFile((shader_root / "tile_build.comp").string());
    tile_dilate_program_ = glutil::CreateComputeProgramFromFile((shader_root / "tile_dilate.comp").string());

    render_program_ = glutil::CreateProgramFromFiles((shader_root / "fullscreen.vert").string(),
                                                     (shader_root / "composite.frag").string());
}

void PowderApp::InitializeState() {
    glUseProgram(init_state_program_);
    glUniform2i(glGetUniformLocation(init_state_program_, "gridSize"), kGridWidth, kGridHeight);

    glBindImageTexture(0, material_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16UI);
    glBindImageTexture(1, velocity_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(2, stress_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(3, lbm_a0_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, lbm_a1_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, lbm_a2_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(6, water_pressure_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(7, fire_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glBindImageTexture(0, material_b_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16UI);
    glBindImageTexture(1, velocity_b_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(2, stress_b_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(3, lbm_b0_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, lbm_b1_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, lbm_b2_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(6, water_pressure_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(7, fire_b_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    const GLuint clear_value = 0xFFFFFFFFU;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, winner_buffer_);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &clear_value);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void PowderApp::UpdateInput() {
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    if (glfwGetKey(window_, GLFW_KEY_1) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Sand;
    }
    if (glfwGetKey(window_, GLFW_KEY_2) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Water;
    }
    if (glfwGetKey(window_, GLFW_KEY_3) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Solid;
    }
    if (glfwGetKey(window_, GLFW_KEY_4) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Erase;
    }
    if (glfwGetKey(window_, GLFW_KEY_5) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Smoke;
    }
    if (glfwGetKey(window_, GLFW_KEY_6) == GLFW_PRESS) {
        brush_mode_ = BrushMode::Fire;
    }

    if (glfwGetKey(window_, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        brush_radius_ = std::max(1, brush_radius_ - 1);
    }
    if (glfwGetKey(window_, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        brush_radius_ = std::min(96, brush_radius_ + 1);
    }

    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window_, &cursor_x, &cursor_y);

    int fb_width = 1;
    int fb_height = 1;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);

    const float u = static_cast<float>(cursor_x) / static_cast<float>(std::max(1, fb_width));
    const float v = 1.0F - (static_cast<float>(cursor_y) / static_cast<float>(std::max(1, fb_height)));

    brush_x_ = std::clamp(static_cast<int>(u * static_cast<float>(kGridWidth)), 0, kGridWidth - 1);
    brush_y_ = std::clamp(static_cast<int>(v * static_cast<float>(kGridHeight)), 0, kGridHeight - 1);
}

void PowderApp::RunFrame(float dt) {
    ++frame_index_;
    RunSpawnPass();
    RunTileActivityPass();
    RunSandPass(dt);
    RunWaterPass();
    RunSmokePass(dt);
    RunFireHeatPass(dt);
    RunCouplingPass(dt);
}

void PowderApp::RunSpawnPass() {
    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS) {
        return;
    }

    GLint material_mode = kSpawnMaterialNone;
    GLint spawn_water = 0;
    float density_boost = 0.0F;
    std::array<float, 2> water_velocity{0.0F, 0.0F};

    GLint spawn_smoke = 0;
    float smoke_density = 0.0F;
    std::array<float, 2> smoke_velocity{0.0F, 0.0F};

    GLint spawn_fire = 0;
    std::array<float, 2> fire_seed{1.0F, 0.0F};
    float heat_add = 0.0F;
    GLint clear_effects = 0;

    switch (brush_mode_) {
        case BrushMode::Sand:
            material_mode = static_cast<GLint>(kSandMaterial);
            break;
        case BrushMode::Water:
            spawn_water = 1;
            density_boost = 0.12F;
            water_velocity[1] = -0.02F;
            break;
        case BrushMode::Solid:
            material_mode = static_cast<GLint>(kSolidMaterial);
            break;
        case BrushMode::Erase:
            material_mode = static_cast<GLint>(kEmptyMaterial);
            spawn_water = 1;
            clear_effects = 1;
            break;
        case BrushMode::Smoke:
            spawn_smoke = 1;
            smoke_density = 0.9F;
            smoke_velocity[1] = 0.4F;
            break;
        case BrushMode::Fire:
            spawn_fire = 1;
            spawn_smoke = 1;
            smoke_density = 0.5F;
            fire_seed = {0.35F, 0.95F};
            heat_add = 0.9F;
            break;
    }

    glUseProgram(spawn_program_);
    glUniform2i(glGetUniformLocation(spawn_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(spawn_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(spawn_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(spawn_program_, "brushCenter"), brush_x_, brush_y_);
    glUniform1i(glGetUniformLocation(spawn_program_, "brushRadius"), brush_radius_);

    glUniform1i(glGetUniformLocation(spawn_program_, "materialMode"), material_mode);
    glUniform1i(glGetUniformLocation(spawn_program_, "spawnWater"), spawn_water);
    glUniform1f(glGetUniformLocation(spawn_program_, "waterDensityBoost"), density_boost);
    glUniform2f(glGetUniformLocation(spawn_program_, "waterVelocity"), water_velocity[0], water_velocity[1]);

    glUniform1i(glGetUniformLocation(spawn_program_, "spawnSmoke"), spawn_smoke);
    glUniform1f(glGetUniformLocation(spawn_program_, "smokeDensity"), smoke_density);
    glUniform2f(glGetUniformLocation(spawn_program_, "smokeVelocity"), smoke_velocity[0], smoke_velocity[1]);

    glUniform1i(glGetUniformLocation(spawn_program_, "spawnFire"), spawn_fire);
    glUniform2f(glGetUniformLocation(spawn_program_, "fireSeed"), fire_seed[0], fire_seed[1]);
    glUniform1f(glGetUniformLocation(spawn_program_, "heatAdd"), heat_add);
    glUniform1i(glGetUniformLocation(spawn_program_, "clearEffects"), clear_effects);

    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(1, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(2, CurrentStress(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(3, CurrentLbm0(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    glBindImageTexture(4, CurrentLbm1(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    glBindImageTexture(5, CurrentLbm2(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(6, CurrentFire(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(7, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(8, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(9, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);

    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunTileActivityPass() {
    glUseProgram(tile_build_program_);
    glUniform2i(glGetUniformLocation(tile_build_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(tile_build_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(tile_build_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(tile_build_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, water_pressure_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentFire(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(4, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(6, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(7, CurrentLbm0(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(8, CurrentLbm1(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(9, CurrentLbm2(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(10, tile_active_a_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    DispatchGrid(kTileWidth, kTileHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(tile_dilate_program_);
    glUniform2i(glGetUniformLocation(tile_dilate_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, tile_active_a_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindImageTexture(1, tile_active_b_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    DispatchGrid(kTileWidth, kTileHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void PowderApp::RunSandPass(float dt) {
    glUseProgram(sand_prepare_program_);
    glUniform2i(glGetUniformLocation(sand_prepare_program_, "gridSize"), kGridWidth, kGridHeight);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, CurrentStress(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, NextMaterial(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16UI);
    glBindImageTexture(4, NextVelocity(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(5, NextStress(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(sand_desired_program_);
    glUniform2i(glGetUniformLocation(sand_desired_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(sand_desired_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(sand_desired_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(sand_desired_program_, "gravity"), 240.0F);
    glUniform1f(glGetUniformLocation(sand_desired_program_, "maxSpeed"), 280.0F);
    glUniform1i(glGetUniformLocation(sand_desired_program_, "maxSteps"), 14);
    glUniform1f(glGetUniformLocation(sand_desired_program_, "restitution"), 0.15F);
    glUniform1f(glGetUniformLocation(sand_desired_program_, "damping"), 0.92F);
    glUniform1ui(glGetUniformLocation(sand_desired_program_, "frameIndex"), frame_index_);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, desired_buffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, proposed_velocity_buffer_);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    const GLuint clear_value = 0xFFFFFFFFU;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, winner_buffer_);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &clear_value);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(sand_resolve_program_);
    glUniform2i(glGetUniformLocation(sand_resolve_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(sand_resolve_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1ui(glGetUniformLocation(sand_resolve_program_, "frameIndex"), frame_index_);
    glBindImageTexture(0, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, desired_buffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, winner_buffer_);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(sand_commit_program_);
    glUniform2i(glGetUniformLocation(sand_commit_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(sand_commit_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, CurrentStress(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, NextMaterial(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16UI);
    glBindImageTexture(4, NextVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(5, NextStress(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(6, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, desired_buffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, proposed_velocity_buffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, winner_buffer_);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    SwapMaterialPing();
}

void PowderApp::RunWaterPass() {
    glUseProgram(water_lbm_program_);
    glUniform2i(glGetUniformLocation(water_lbm_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(water_lbm_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(water_lbm_program_, "omega"), 1.08F);
    glUniform1f(glGetUniformLocation(water_lbm_program_, "restMix"), 0.012F);
    glUniform1f(glGetUniformLocation(water_lbm_program_, "maxVelocity"), 0.75F);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, CurrentLbm0(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(2, CurrentLbm1(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(3, CurrentLbm2(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(4, NextLbm0(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, NextLbm1(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, NextLbm2(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(7, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(water_pressure_program_);
    glUniform2i(glGetUniformLocation(water_pressure_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(water_pressure_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, NextLbm0(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, NextLbm1(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(2, NextLbm2(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, water_pressure_, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(4, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapLbmPing();
}

void PowderApp::RunSmokePass(float dt) {
    glUseProgram(smoke_advect_program_);
    glUniform2i(glGetUniformLocation(smoke_advect_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(smoke_advect_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(smoke_advect_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(smoke_advect_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(smoke_advect_program_, "velocityDissipation"), 0.986F);
    glUniform1f(glGetUniformLocation(smoke_advect_program_, "densityDissipation"), 0.976F);
    glUniform1f(glGetUniformLocation(smoke_advect_program_, "buoyancy"), 1.45F);
    glBindImageTexture(0, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(2, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, NextSmokeVel(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(4, NextSmokeDen(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindImageTexture(5, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(smoke_vorticity_program_);
    glUniform2i(glGetUniformLocation(smoke_vorticity_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(smoke_vorticity_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(smoke_vorticity_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(smoke_vorticity_program_, "epsilon"), 0.22F);
    glBindImageTexture(0, NextSmokeVel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, NextSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(2, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindImageTexture(4, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(smoke_divergence_program_);
    glUniform2i(glGetUniformLocation(smoke_divergence_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(smoke_divergence_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, smoke_divergence_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(2, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(smoke_pressure_clear_program_);
    glUniform2i(glGetUniformLocation(smoke_pressure_clear_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(smoke_pressure_clear_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, CurrentSmokePressure(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(1, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    constexpr int kPressureIterations = 20;
    for (int i = 0; i < kPressureIterations; ++i) {
        glUseProgram(smoke_pressure_jacobi_program_);
        glUniform2i(glGetUniformLocation(smoke_pressure_jacobi_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
        glUniform2i(glGetUniformLocation(smoke_pressure_jacobi_program_, "tileSize"), kTileWidth, kTileHeight);
        glBindImageTexture(0, smoke_divergence_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, CurrentSmokePressure(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, NextSmokePressure(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glBindImageTexture(3, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
        DispatchGrid(kSmokeWidth, kSmokeHeight);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        SwapSmokePressurePing();
    }

    glUseProgram(smoke_project_program_);
    glUniform2i(glGetUniformLocation(smoke_project_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(smoke_project_program_, "tileSize"), kTileWidth, kTileHeight);
    glBindImageTexture(0, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
    glBindImageTexture(2, CurrentSmokePressure(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, NextSmokeVel(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(4, NextSmokeDen(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindImageTexture(5, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapSmokePing();
}

void PowderApp::RunFireHeatPass(float dt) {
    glUseProgram(fire_rd_program_);
    glUniform2i(glGetUniformLocation(fire_rd_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(fire_rd_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(fire_rd_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(fire_rd_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(fire_rd_program_, "diffusionU"), 0.00025F);
    glUniform1f(glGetUniformLocation(fire_rd_program_, "diffusionV"), 0.00012F);
    glUniform1f(glGetUniformLocation(fire_rd_program_, "feed"), 0.034F);
    glUniform1f(glGetUniformLocation(fire_rd_program_, "kill"), 0.062F);
    glBindImageTexture(0, CurrentFire(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(1, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, NextFire(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glBindImageTexture(3, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(heat_diffuse_program_);
    glUniform2i(glGetUniformLocation(heat_diffuse_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(heat_diffuse_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(heat_diffuse_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(heat_diffuse_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(heat_diffuse_program_, "diffusion"), 0.22F);
    glUniform1f(glGetUniformLocation(heat_diffuse_program_, "decay"), 0.09F);
    glUniform1f(glGetUniformLocation(heat_diffuse_program_, "fireToHeat"), 1.1F);
    glBindImageTexture(0, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(1, NextFire(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, NextHeat(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(3, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kHeatWidth, kHeatHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    SwapFirePing();
    SwapHeatPing();
}

void PowderApp::RunCouplingPass(float dt) {
    glUseProgram(coupling_fire_program_);
    glUniform2i(glGetUniformLocation(coupling_fire_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(coupling_fire_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(coupling_fire_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(coupling_fire_program_, "dt"), dt);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, water_pressure_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, CurrentFire(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(4, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(coupling_heat_program_);
    glUniform2i(glGetUniformLocation(coupling_heat_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(coupling_heat_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(coupling_heat_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(coupling_heat_program_, "dt"), dt);
    glBindImageTexture(0, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R16F);
    glBindImageTexture(1, CurrentFire(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(2, water_pressure_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(3, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kHeatWidth, kHeatHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(coupling_smoke_program_);
    glUniform2i(glGetUniformLocation(coupling_smoke_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(coupling_smoke_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(coupling_smoke_program_, "heatSize"), kHeatWidth, kHeatHeight);
    glUniform2i(glGetUniformLocation(coupling_smoke_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(coupling_smoke_program_, "dt"), dt);
    glBindImageTexture(0, CurrentSmokeVel(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(1, CurrentSmokeDen(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8);
    glBindImageTexture(2, CurrentFire(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RG16F);
    glBindImageTexture(3, CurrentHeat(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(4, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kSmokeWidth, kSmokeHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(coupling_sand_drag_program_);
    glUniform2i(glGetUniformLocation(coupling_sand_drag_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(coupling_sand_drag_program_, "tileSize"), kTileWidth, kTileHeight);
    glUniform1f(glGetUniformLocation(coupling_sand_drag_program_, "dt"), dt);
    glUniform1f(glGetUniformLocation(coupling_sand_drag_program_, "drag"), 0.18F);
    glBindImageTexture(0, CurrentMaterial(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
    glBindImageTexture(1, water_pressure_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
    glBindImageTexture(2, CurrentVelocity(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
    glBindImageTexture(3, tile_active_b_, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
    DispatchGrid(kGridWidth, kGridHeight);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void PowderApp::Render() {
    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);

    glViewport(0, 0, fb_width, fb_height);
    glClearColor(0.83F, 0.87F, 0.90F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(render_program_);
    glUniform2i(glGetUniformLocation(render_program_, "gridSize"), kGridWidth, kGridHeight);
    glUniform2i(glGetUniformLocation(render_program_, "smokeSize"), kSmokeWidth, kSmokeHeight);
    glUniform2i(glGetUniformLocation(render_program_, "heatSize"), kHeatWidth, kHeatHeight);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CurrentMaterial());
    glUniform1i(glGetUniformLocation(render_program_, "materialTex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, CurrentStress());
    glUniform1i(glGetUniformLocation(render_program_, "stressTex"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, water_pressure_);
    glUniform1i(glGetUniformLocation(render_program_, "waterPressureTex"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, CurrentSmokeDen());
    glUniform1i(glGetUniformLocation(render_program_, "smokeDensityTex"), 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, CurrentFire());
    glUniform1i(glGetUniformLocation(render_program_, "fireTex"), 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, CurrentHeat());
    glUniform1i(glGetUniformLocation(render_program_, "heatTex"), 5);

    glBindVertexArray(fullscreen_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void PowderApp::SwapMaterialPing() {
    material_ping_ = !material_ping_;
}

void PowderApp::SwapLbmPing() {
    lbm_ping_ = !lbm_ping_;
}

void PowderApp::SwapSmokePing() {
    smoke_ping_ = !smoke_ping_;
}

void PowderApp::SwapFirePing() {
    fire_ping_ = !fire_ping_;
}

void PowderApp::SwapHeatPing() {
    heat_ping_ = !heat_ping_;
}

void PowderApp::SwapSmokePressurePing() {
    smoke_pressure_ping_ = !smoke_pressure_ping_;
}

GLuint PowderApp::CurrentMaterial() const {
    return material_ping_ ? material_a_ : material_b_;
}

GLuint PowderApp::CurrentVelocity() const {
    return material_ping_ ? velocity_a_ : velocity_b_;
}

GLuint PowderApp::CurrentStress() const {
    return material_ping_ ? stress_a_ : stress_b_;
}

GLuint PowderApp::NextMaterial() const {
    return material_ping_ ? material_b_ : material_a_;
}

GLuint PowderApp::NextVelocity() const {
    return material_ping_ ? velocity_b_ : velocity_a_;
}

GLuint PowderApp::NextStress() const {
    return material_ping_ ? stress_b_ : stress_a_;
}

GLuint PowderApp::CurrentLbm0() const {
    return lbm_ping_ ? lbm_a0_ : lbm_b0_;
}

GLuint PowderApp::CurrentLbm1() const {
    return lbm_ping_ ? lbm_a1_ : lbm_b1_;
}

GLuint PowderApp::CurrentLbm2() const {
    return lbm_ping_ ? lbm_a2_ : lbm_b2_;
}

GLuint PowderApp::NextLbm0() const {
    return lbm_ping_ ? lbm_b0_ : lbm_a0_;
}

GLuint PowderApp::NextLbm1() const {
    return lbm_ping_ ? lbm_b1_ : lbm_a1_;
}

GLuint PowderApp::NextLbm2() const {
    return lbm_ping_ ? lbm_b2_ : lbm_a2_;
}

GLuint PowderApp::CurrentSmokeVel() const {
    return smoke_ping_ ? smoke_vel_a_ : smoke_vel_b_;
}

GLuint PowderApp::CurrentSmokeDen() const {
    return smoke_ping_ ? smoke_den_a_ : smoke_den_b_;
}

GLuint PowderApp::NextSmokeVel() const {
    return smoke_ping_ ? smoke_vel_b_ : smoke_vel_a_;
}

GLuint PowderApp::NextSmokeDen() const {
    return smoke_ping_ ? smoke_den_b_ : smoke_den_a_;
}

GLuint PowderApp::CurrentSmokePressure() const {
    return smoke_pressure_ping_ ? smoke_pressure_a_ : smoke_pressure_b_;
}

GLuint PowderApp::NextSmokePressure() const {
    return smoke_pressure_ping_ ? smoke_pressure_b_ : smoke_pressure_a_;
}

GLuint PowderApp::CurrentFire() const {
    return fire_ping_ ? fire_a_ : fire_b_;
}

GLuint PowderApp::NextFire() const {
    return fire_ping_ ? fire_b_ : fire_a_;
}

GLuint PowderApp::CurrentHeat() const {
    return heat_ping_ ? heat_a_ : heat_b_;
}

GLuint PowderApp::NextHeat() const {
    return heat_ping_ ? heat_b_ : heat_a_;
}

void PowderApp::DispatchGrid(int width, int height) {
    const GLuint groups_x = static_cast<GLuint>((width + kWorkgroupSize - 1) / kWorkgroupSize);
    const GLuint groups_y = static_cast<GLuint>((height + kWorkgroupSize - 1) / kWorkgroupSize);
    glDispatchCompute(groups_x, groups_y, 1);
}

int PowderApp::CellCount() {
    return kGridWidth * kGridHeight;
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
    } else if (type == GL_HALF_FLOAT) {
        const int channels = (format == GL_RG) ? 2 : (format == GL_RGBA ? 4 : 1);
        const std::vector<std::uint16_t> zeros(static_cast<std::size_t>(texel_count * channels), 0U);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, zeros.data());
    } else {
        const int channels = (format == GL_RG) ? 2 : (format == GL_RGBA ? 4 : 1);
        const std::vector<float> zeros(static_cast<std::size_t>(texel_count * channels), 0.0F);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_FLOAT, zeros.data());
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void PowderApp::DeleteProgram(GLuint& program) {
    if (program != 0U) {
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

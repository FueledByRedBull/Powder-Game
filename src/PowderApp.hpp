#pragma once

#include <cstdint>

#include <glad/glad.h>

struct GLFWwindow;

class PowderApp {
public:
    int Run();

private:
    enum class BrushMode : int {
        Sand = 0,
        Water = 1,
        Solid = 2,
        Erase = 3,
        Smoke = 4,
        Fire = 5,
    };

    static constexpr int kGridWidth = 1000;
    static constexpr int kGridHeight = 1000;
    static constexpr int kHeatWidth = kGridWidth / 2;
    static constexpr int kHeatHeight = kGridHeight / 2;
    static constexpr int kSmokeWidth = kGridWidth / 4;
    static constexpr int kSmokeHeight = kGridHeight / 4;
    static constexpr int kWorkgroupSize = 16;

    GLFWwindow* window_ = nullptr;
    bool gl_ready_ = false;

    bool material_ping_ = true;
    bool lbm_ping_ = true;
    bool smoke_ping_ = true;
    bool fire_ping_ = true;
    bool heat_ping_ = true;
    bool smoke_pressure_ping_ = true;

    BrushMode brush_mode_ = BrushMode::Sand;
    int brush_radius_ = 8;

    int brush_x_ = kGridWidth / 2;
    int brush_y_ = kGridHeight / 2;
    std::uint32_t frame_index_ = 0U;

    GLuint material_a_ = 0;
    GLuint material_b_ = 0;
    GLuint velocity_a_ = 0;
    GLuint velocity_b_ = 0;
    GLuint stress_a_ = 0;
    GLuint stress_b_ = 0;

    GLuint lbm_a0_ = 0;
    GLuint lbm_a1_ = 0;
    GLuint lbm_a2_ = 0;
    GLuint lbm_b0_ = 0;
    GLuint lbm_b1_ = 0;
    GLuint lbm_b2_ = 0;
    GLuint water_pressure_ = 0;

    GLuint smoke_vel_a_ = 0;
    GLuint smoke_vel_b_ = 0;
    GLuint smoke_den_a_ = 0;
    GLuint smoke_den_b_ = 0;
    GLuint smoke_pressure_a_ = 0;
    GLuint smoke_pressure_b_ = 0;
    GLuint smoke_divergence_ = 0;

    GLuint fire_a_ = 0;
    GLuint fire_b_ = 0;

    GLuint heat_a_ = 0;
    GLuint heat_b_ = 0;

    GLuint desired_buffer_ = 0;
    GLuint proposed_velocity_buffer_ = 0;
    GLuint winner_buffer_ = 0;

    GLuint init_state_program_ = 0;
    GLuint spawn_program_ = 0;
    GLuint sand_prepare_program_ = 0;
    GLuint sand_desired_program_ = 0;
    GLuint sand_resolve_program_ = 0;
    GLuint sand_commit_program_ = 0;
    GLuint water_lbm_program_ = 0;
    GLuint water_pressure_program_ = 0;
    GLuint smoke_advect_program_ = 0;
    GLuint smoke_vorticity_program_ = 0;
    GLuint smoke_divergence_program_ = 0;
    GLuint smoke_pressure_clear_program_ = 0;
    GLuint smoke_pressure_jacobi_program_ = 0;
    GLuint smoke_project_program_ = 0;
    GLuint fire_rd_program_ = 0;
    GLuint heat_diffuse_program_ = 0;
    GLuint coupling_fire_program_ = 0;
    GLuint coupling_heat_program_ = 0;
    GLuint coupling_smoke_program_ = 0;
    GLuint coupling_sand_drag_program_ = 0;
    GLuint render_program_ = 0;

    GLuint fullscreen_vao_ = 0;

    bool Initialize();
    void Shutdown();

    void CreateTextures();
    void CreateBuffers();
    void CreatePrograms();
    void InitializeState();

    void UpdateInput();
    void RunFrame(float dt);
    void RunSpawnPass();
    void RunSandPass(float dt);
    void RunWaterPass();
    void RunSmokePass(float dt);
    void RunFireHeatPass(float dt);
    void RunCouplingPass(float dt);
    void Render();

    void SwapMaterialPing();
    void SwapLbmPing();
    void SwapSmokePing();
    void SwapFirePing();
    void SwapHeatPing();
    void SwapSmokePressurePing();

    GLuint CurrentMaterial() const;
    GLuint CurrentVelocity() const;
    GLuint CurrentStress() const;

    GLuint NextMaterial() const;
    GLuint NextVelocity() const;
    GLuint NextStress() const;

    GLuint CurrentLbm0() const;
    GLuint CurrentLbm1() const;
    GLuint CurrentLbm2() const;

    GLuint NextLbm0() const;
    GLuint NextLbm1() const;
    GLuint NextLbm2() const;

    GLuint CurrentSmokeVel() const;
    GLuint CurrentSmokeDen() const;
    GLuint NextSmokeVel() const;
    GLuint NextSmokeDen() const;

    GLuint CurrentSmokePressure() const;
    GLuint NextSmokePressure() const;

    GLuint CurrentFire() const;
    GLuint NextFire() const;

    GLuint CurrentHeat() const;
    GLuint NextHeat() const;

    static void DispatchGrid(int width, int height);
    static int CellCount();

    GLuint CreateTexture(int width,
                         int height,
                         GLenum internal_format,
                         GLenum format,
                         GLenum type,
                         GLint filter) const;
    void DeleteProgram(GLuint& program);
    void DeleteTexture(GLuint& texture);
    void DeleteBuffer(GLuint& buffer);
};

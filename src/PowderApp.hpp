#pragma once

#include "Config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glad/glad.h>

struct GLFWwindow;

class PowderApp {
public:
    int Run();

private:
    friend struct AppLifecycleTests;

    struct BoundaryState {
        GLuint sdf_static = 0;
        GLuint mask_full = 0;
        GLuint mask_static = 0;
        GLuint mask_gas = 0;
        GLuint seed_full_a = 0;
        GLuint seed_full_b = 0;
        bool static_dirty = true;
    };

    struct DebugState {
        GLuint counters = 0;
    };

    struct WaterState {
        int substeps = 2;
        GLuint particles = 0;
        GLuint scratch_particles = 0;
        GLuint spawn_cursor = 0;
        GLuint cell_counts = 0;
        GLuint u_face_weights = 0;
        GLuint v_face_weights = 0;
        GLuint u_face_velocity_sums = 0;
        GLuint v_face_velocity_sums = 0;
        GLuint mac_u = 0;
        GLuint mac_v = 0;
        GLuint mac_u_prev = 0;
        GLuint mac_v_prev = 0;
        GLuint pressure_a = 0;
        GLuint pressure_vectors = 0;
        GLuint pressure_partials = 0;
        GLuint pressure_scalars = 0;
        GLuint divergence = 0;
        GLuint liquid_volume = 0;
        GLuint liquid_phi = 0;
        GLuint amount = 0;
        GLuint render_velocity = 0;
    };

    struct GasState {
        int substeps = 1;
        GLuint mac_u_a = 0;
        GLuint mac_u_b = 0;
        GLuint mac_u_tmp = 0;
        GLuint mac_v_a = 0;
        GLuint mac_v_b = 0;
        GLuint mac_v_tmp = 0;
        GLuint velocity_center_a = 0;
        GLuint velocity_center_b = 0;
        GLuint velocity_center_tmp = 0;
        GLuint pressure_a = 0;
        GLuint pressure_b = 0;
        GLuint pressure_mid_a = 0;
        GLuint pressure_mid_b = 0;
        GLuint pressure_coarse_a = 0;
        GLuint pressure_coarse_b = 0;
        GLuint divergence = 0;
        GLuint rhs_mid = 0;
        GLuint rhs_coarse = 0;
        GLuint pressure_check = 0;
        GLuint smoke_a = 0;
        GLuint smoke_b = 0;
        GLuint smoke_tmp = 0;
        GLuint temperature_a = 0;
        GLuint temperature_b = 0;
        GLuint temperature_tmp = 0;
        GLuint fuel_a = 0;
        GLuint fuel_b = 0;
        GLuint fuel_tmp = 0;
        GLuint oxidizer_a = 0;
        GLuint oxidizer_b = 0;
        GLuint oxidizer_tmp = 0;
        GLuint reaction_a = 0;
        GLuint reaction_b = 0;
        GLuint reaction_tmp = 0;
    };

    struct SandState {
        int substeps = 1;
        GLuint particles = 0;
        GLuint scratch_particles = 0;
        GLuint spawn_cursor = 0;
        GLuint grid_mass = 0;
        GLuint grid_momentum_x = 0;
        GLuint grid_momentum_y = 0;
        GLuint grid_velocity = 0;
        GLuint occupancy = 0;
    };

    struct ParticleMetrics {
        GLuint count = 0;
        double mass = 0.0;
    };

    struct ReplayValidationState {
        double last_water_sum = 0.0;
        double last_water_mass = 0.0;
        double water_reference_mass = -1.0;
        bool water_mass_conserved = true;
        GLuint water_live_particles = 0;
        bool water_monotonic = true;
        bool gas_ignited = false;
        bool gas_extinguished = false;
        double gas_reaction_sum = 0.0;
        double sand_reference_mass = -1.0;
        double sand_final_mass = 0.0;
        bool sand_mass_conserved = true;
        double leakage_total_water = 0.0;
        double leakage_below_barrier = 0.0;
        double boundary_contact_mass = 0.0;
        double boundary_crossed_mass = 0.0;
        std::array<GLuint, 4> accumulated_debug_counters{};
    };

    struct ReplayTimingAggregate {
        float min_ms = 0.0F;
        float max_ms = 0.0F;
        double sum_ms = 0.0;
        int samples = 0;
    };

    struct ReplaySignature {
        double water_mass = 0.0;
        double sand_mass = 0.0;
        double water_amount_sum = 0.0;
        double liquid_volume_sum = 0.0;
        double smoke_sum = 0.0;
        double temperature_sum = 0.0;
        double reaction_sum = 0.0;
        double sand_occupancy_sum = 0.0;
        GLuint water_live_particles = 0;
        GLuint sand_live_particles = 0;
    };

    struct BrushConfig {
        bool replace_material = false;
        std::uint32_t material_value = 0U;
        bool spawn_sand = false;
        int sand_particles = 0;
        std::array<float, 2> sand_velocity{0.0F, 0.0F};
        bool spawn_water = false;
        int water_particles = 0;
        std::array<float, 2> water_velocity{0.0F, 0.0F};
        bool spawn_gas = false;
        float smoke_density = 0.0F;
        std::array<float, 2> smoke_velocity{0.0F, 0.0F};
        float temperature_add = 0.0F;
        float fuel_add = 0.0F;
        float oxidizer_add = 0.0F;
        float reaction_add = 0.0F;
        bool blocked_by_solid = true;
        bool blocked_by_water = false;
        bool clear_water = false;
        bool clear_gas = false;
    };

    struct DispatchRegion {
        int origin_x = 0;
        int origin_y = 0;
        int width = 0;
        int height = 0;

        [[nodiscard]] bool Empty() const {
            return width <= 0 || height <= 0;
        }
    };

    enum class BrushMode : int {
        Sand = 0,
        Water = 1,
        Solid = 2,
        Erase = 3,
        Smoke = 4,
        Fire = 5,
    };

    enum class RenderMode : int {
        Composite = 0,
        Boundary = 1,
        Water = 2,
        WaterVelocity = 3,
        GasVelocity = 4,
        GasPressure = 5,
        Smoke = 6,
        Temperature = 7,
        FuelReaction = 8,
        DebugCounters = 9,
    };

    static constexpr int kGridWidth = 1000;
    static constexpr int kGridHeight = 1000;
    static constexpr int kWorkgroupSize = 16;
    static constexpr int kGasWidth = kGridWidth / 2;
    static constexpr int kGasHeight = kGridHeight / 2;
    static constexpr int kHeatWidth = kGasWidth;
    static constexpr int kHeatHeight = kGasHeight;
    static constexpr int kSmokeWidth = kGasWidth;
    static constexpr int kSmokeHeight = kGasHeight;
    static constexpr int kMaxWaterParticles = 262144;
    static constexpr int kWaterParticleComponents = 12;
    static constexpr int kSandParticleComponents = 16;
    static constexpr int kMaxSandParticles = 262144;
    static constexpr int kGasMidWidth = kGasWidth / 2;
    static constexpr int kGasMidHeight = kGasHeight / 2;
    static constexpr int kGasCoarseWidth = kGasMidWidth / 2;
    static constexpr int kGasCoarseHeight = kGasMidHeight / 2;
    static constexpr int kReplayQueryCount = 7;

    struct ReplayBrushEvent {
        int frame = 0;
        BrushMode mode = BrushMode::Sand;
        int x = 0;
        int y = 0;
        int radius = 0;
        int hold_frames = 1;
    };

    struct ReplayScene {
        std::string name;
        int total_frames = 0;
        std::vector<ReplayBrushEvent> events;
    };

    enum class TimedPass : std::size_t {
        Spawn = 0,
        Boundary = 1,
        Water = 2,
        Gas = 3,
        Sand = 4,
        Coupling = 5,
        Extract = 6,
    };

    GLFWwindow* window_ = nullptr;
    bool gl_ready_ = false;
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> uniform_locations_;

    bool gas_mac_ping_ = true;
    bool smoke_ping_ = true;

    powder_config::SimulationConfig config_{};
    BoundaryState boundary_state_{};
    DebugState debug_state_{};
    WaterState water_state_{};
    GasState gas_state_{};
    SandState sand_state_{};
    ReplayValidationState replay_validation_{};

    BrushMode brush_mode_ = BrushMode::Sand;
    RenderMode render_mode_ = RenderMode::Composite;
    int brush_radius_ = 8;

    int brush_x_ = kGridWidth / 2;
    int brush_y_ = kGridHeight / 2;
    std::uint32_t frame_index_ = 0U;

    GLuint material_a_ = 0;
    GLuint render_smoke_density_ = 0;
    GLuint render_fire_emissive_ = 0;
    GLuint render_temperature_ = 0;
    GLuint render_gas_velocity_ = 0;



    GLuint init_state_program_ = 0;
    GLuint spawn_full_program_ = 0;
    GLuint spawn_gas_program_ = 0;
    GLuint sand_particle_spawn_program_ = 0;
    GLuint sand_particle_compact_program_ = 0;
    GLuint sand_particle_clear_program_ = 0;
    GLuint sand_particle_p2g_program_ = 0;
    GLuint sand_grid_update_program_ = 0;
    GLuint sand_particle_step_program_ = 0;
    GLuint sand_rasterize_program_ = 0;
    GLuint boundary_build_program_ = 0;
    GLuint boundary_downsample_program_ = 0;
    GLuint boundary_seed_program_ = 0;
    GLuint boundary_jumpflood_program_ = 0;
    GLuint boundary_distance_program_ = 0;
    GLuint water_particle_p2g_program_ = 0;
    GLuint water_particle_spawn_program_ = 0;
    GLuint water_particle_compact_program_ = 0;
    GLuint water_particle_clear_program_ = 0;
    GLuint water_particle_step_program_ = 0;
    GLuint water_particle_finalize_program_ = 0;
    GLuint water_mac_build_program_ = 0;
    GLuint water_mac_finalize_program_ = 0;
    GLuint water_divergence_program_ = 0;
    GLuint pressure_cg_program_ = 0;
    GLuint pressure_cg_reduce_program_ = 0;
    GLuint water_project_program_ = 0;
    GLuint smoke_advect_program_ = 0;
    GLuint smoke_correct_program_ = 0;
    GLuint gas_velocity_advect_program_ = 0;
    GLuint gas_velocity_correct_program_ = 0;
    GLuint gas_face_to_center_program_ = 0;
    GLuint smoke_divergence_program_ = 0;
    GLuint smoke_pressure_clear_program_ = 0;
    GLuint gas_pressure_restrict_program_ = 0;
    GLuint gas_pressure_relax_program_ = 0;
    GLuint gas_pressure_prolongate_program_ = 0;
    GLuint smoke_project_program_ = 0;
    GLuint pressure_residual_program_ = 0;
    GLuint fire_rd_program_ = 0;
    GLuint coupling_fire_program_ = 0;
    GLuint coupling_heat_program_ = 0;
    GLuint render_extract_program_ = 0;
    GLuint render_program_ = 0;

    GLuint fullscreen_vao_ = 0;
    std::array<GLuint, kReplayQueryCount> timing_queries_{};
    std::array<GLuint, 2> replay_frame_queries_{};
    std::array<float, kReplayQueryCount> pass_timings_ms_{};
    std::array<ReplayTimingAggregate, kReplayQueryCount> replay_timing_aggregates_{};
    ReplayTimingAggregate replay_frame_timing_{};
    ReplaySignature replay_reference_signature_{};
    ReplaySignature replay_current_signature_{};
    bool replay_has_reference_signature_ = false;
    bool replay_determinism_ok_ = true;
    bool replay_mode_ = false;
    int replay_frame_index_ = 0;
    bool replay_failed_ = false;
    std::string replay_scene_name_;

    bool Initialize();
    void Shutdown();

    void CreateTextures();
    void CreateBuffers();
    void CreatePrograms();
    void InitializeState();

    void UpdateInput();
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    void RunFrame(float dt, const BrushConfig* scripted_brush = nullptr, bool reset_counters = true);
    void RunSpawnPass(const BrushConfig* replay_brush);
    void RunBoundaryPass();
    BrushConfig BuildBrushConfig() const;
    DispatchRegion ComputeDispatchRegion(int scale, int domain_width, int domain_height) const;
    void RunSpawnFullRes(const BrushConfig& config, const DispatchRegion& region);
    void RunSpawnGas(const BrushConfig& config, const DispatchRegion& region);
    void RunSpawnSandParticles(const BrushConfig& config, const DispatchRegion& region);
    void RunSpawnWaterParticles(const BrushConfig& config, const DispatchRegion& region);
    void RunClearSandParticles(const DispatchRegion& region);
    void RunClearWaterParticles(const DispatchRegion& region);
    void RunClearFullRes(const DispatchRegion& region);
    void RunClearGas(const DispatchRegion& region);
    void RunSandPass(float dt);
    void RunWaterPass(float dt);
    void RunWaterProjection();
    void RunGasPass(float dt);
    void RunSmokePass(float dt);
    void RunGasProjection();
    void RunFireHeatPass(float dt);
    void RunCouplingPass(float dt);
    void RunRenderFieldExtractionPass();
    void Render();
    int RunReplay();
    void ApplyReplayBrush(const ReplayBrushEvent& event);
    std::vector<ReplayScene> BuildReplayScenes() const;
    void ResetReplayMetrics();
    void UpdateReplayMetrics(const ReplayScene& scene, int frame_index);
    void AccumulateReplayDebugCounters();
    void BeginTimedPass(TimedPass pass);
    void EndTimedPass(TimedPass pass);
    void ReadBackTimingQueries();
    void ReadBackTimingQueries(bool accumulate);
    bool ReplayMetricsOk() const;
    std::array<GLuint, 4> ReadDebugCounters() const;
    ParticleMetrics ReadParticleMetrics(GLuint buffer, int capacity, int components) const;
    GLuint ReadSandLiveParticles() const;
    GLuint ReadWaterLiveParticles() const;
    ReplaySignature CaptureReplaySignature() const;
    bool ReplaySignaturesClose(const ReplaySignature& a, const ReplaySignature& b) const;

    void SwapGasMacPing();
    void SwapSmokePing();

    GLuint CurrentMaterial() const;

    GLuint CurrentWaterAmount() const;
    GLuint CurrentWaterVelocity() const;
    GLuint CurrentWaterDivergence() const;
    GLuint CurrentWaterPressure() const;

    GLuint CurrentGasVelocityCenter() const;
    GLuint CurrentSmokeDen() const;
    GLuint NextGasVelocityCenter() const;
    GLuint NextSmokeDen() const;
    GLuint CurrentGasU() const;
    GLuint NextGasU() const;
    GLuint CurrentGasV() const;
    GLuint NextGasV() const;

    GLuint CurrentReactionRate() const;
    GLuint NextReactionRate() const;
    GLuint CurrentFuel() const;
    GLuint NextFuel() const;
    GLuint CurrentOxidizer() const;
    GLuint NextOxidizer() const;

    GLuint CurrentTemperature() const;
    GLuint NextTemperature() const;
    GLuint CurrentGasPressure() const;

    static void DispatchGrid(int width, int height);
    static void DispatchRegionGrid(const DispatchRegion& region);
    static int CellCount();
    double ReadScalarTextureSum(GLuint texture, int width, int height) const;
    double ReadReactionTextureSum(GLuint texture, int width, int height) const;

    GLuint CreateTexture(int width,
                         int height,
                         GLenum internal_format,
                         GLenum format,
                         GLenum type,
                         GLint filter) const;
    void DeleteProgram(GLuint& program);
    GLint CachedUniform(GLuint program, const char* name);
    void DeleteTexture(GLuint& texture);
    void DeleteBuffer(GLuint& buffer);
};

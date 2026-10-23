#pragma once

#include "Config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glad/glad.h>

struct GLFWwindow;
class FixedStepClock;

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
        GLuint bins = 0;  // per-bin counts, then slots, of the sorted compaction (water_particle_compact.comp)
        GLuint spawn_cursor = 0;
        GLuint live_count = 0;    // live particles after the last compaction
        // Host upper bound on the slots that may hold a live record (live count plus every spawn since); compaction
        // and erase dispatch it. Slots at or past it are inactive.
        GLuint record_bound = 0;
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
        GLuint pressure_tiles = 0;  // indirect dispatch args, the listed tile count, then the tile ids
        GLuint pressure_sums = 0;   // three vec4 sums per listed tile
        GLuint pressure_levels = 0;  // multigrid indirect arguments, level boxes, then the coarse level cells
        GLuint pressure_state = 0;   // a copy of the solver's running flag, breakdown flag and iteration count
        GLuint pockets = 0;          // uvec4 per cell: sealed-pocket labels and sums (pressure_pockets.comp), water and gas
        int pressure_iterations = 0;  // GPU iterations of the last solve, which sizes the next blind batch
        int pressure_dispatched = 0;  // iterations the host dispatched for the last solve
        GLuint divergence = 0;
        GLuint liquid_volume = 0;
        GLuint liquid_phi = 0;
        GLuint amount = 0;
        GLuint render_velocity = 0;
        GLuint packed = 0;         // R8UI calm water per cell: particles (bits 0-6), wake pending (bit 7) (water_pack.comp)
        GLuint wake_velocity = 0;  // RG16F projected velocity a packed cell wakes with
        GLuint cell_tally = 0;     // per cell: particles, ready particles, pack flag (water_sleep.comp)
    };

    // Gas pressure multigrid levels: 500, 250, 125, 63, 32 and 16 cells across.
    static constexpr int kGasLevels = 6;
    // Red-black sweeps before and after each coarser level of a V-cycle, and the red-black pairs each way that
    // one workgroup runs on the 16x16 bottom level in a single dispatch.
    static constexpr int kGasSmoothSweeps = 2;
    static constexpr int kGasCoarsestSweeps = 16;

    struct GasState {
        int substeps = 1;
        int pressure_iterations = 0;  // CG iterations of the last solve, which places the next solve's first check
        // The pressure solve that spans a tick pair (RunGasTick): begun on the first tick, finished on the second.
        bool solve_pending = false;
        bool solve_running = false;   // the residual check has not yet stopped it
        bool solve_started = false;   // a solve was needed when it began (the iteration count is recorded)
        int solve_iterations = 0;
        int solve_next_check = 0;
        GLuint mask_solve = 0;  // R8UI copy of the gas mask the pending solve was built on, so its operator stays fixed
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
        GLuint divergence = 0;
        // Multigrid levels below the fine grid (pressure_a, divergence): footprint kinds, corrections, residuals.
        // Kind level 0 classifies the fine grid itself.
        std::array<GLuint, kGasLevels> level_kind{};
        std::array<GLuint, kGasLevels - 1> level_pressure{};
        std::array<GLuint, kGasLevels - 1> level_rhs{};
        // Fine residual of each level that restricts, gathered with the transposed prolongation weights.
        std::array<GLuint, kGasLevels - 1> level_residual{};
        // Pre-smoothed pressure of each level that restricts: fused smoothing never smooths in place.
        std::array<GLuint, kGasLevels - 1> level_smoothed{};
        // Preconditioned CG vectors (R32F, gas size): residual s, preconditioned z, direction d, q = L d,
        // per-workgroup partial sums and the shared scalars (gas_cg.comp).
        GLuint cg_s = 0;
        GLuint cg_z = 0;
        GLuint cg_d = 0;
        GLuint cg_q = 0;
        GLuint cg_partials = 0;
        GLuint cg_scalars = 0;
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
        GLuint grid_moisture = 0;  // moisture-weighted node mass of the occupancy transfer (sand_particle_p2g.comp)
        GLuint tile_flags = 0;     // per 16x16 node tile: deposited this substep (sand_grid_update.comp skips the rest)
        GLuint grid_velocity = 0;
        GLuint occupancy = 0;
        GLuint moisture = 0;     // R8 mean moisture of a cell's sand (sand_rasterize.comp), drawn by composite.frag
        GLuint packed = 0;       // R8UI sleeping grains per cell (sand_pack.comp)
        GLuint packed_f = 0;     // RGBA32F mean volume ratio J, hardening fraction and moisture of a cell's sleeping grains
        GLuint cell_tally = 0;   // per cell: grains, ready grains, wake/pack flags (sand_sleep.comp)
        GLuint cell_fsum = 0;    // per cell: summed F of the ready grains, four ints times 4096
        GLuint live_count = 0;   // live grains after the last compaction
        // Host upper bound on the slots that may hold a live grain (the live count plus every spawn since), as for water;
        // the sleep tally, compaction and erase dispatch it. Slots at or past it are inactive.
        GLuint record_bound = 0;
        // Grid node rows/columns [x0, x1) x [y0, y1) the previous tick's grid passes covered; the next tick also clears
        // them, so nodes the moving sand left hold zero mass and velocity.
        std::array<int, 4> node_box{0, 0, kGridWidth + 2, kGridHeight + 2};
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
        std::array<GLuint, 5> accumulated_debug_counters{};
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
        float water_density_limit = 0.0F;
        // Strokes only: centre of the previous stamp of this tick, whose disc the particle spawners skip.
        bool skip_previous_stamp = false;
        std::array<int, 2> previous_stamp{0, 0};
        bool spawn_gas = false;
        float smoke_density = 0.0F;
        std::array<float, 2> smoke_velocity{0.0F, 0.0F};
        float temperature_add = 0.0F;
        float fuel_add = 0.0F;
        float oxidizer_add = 0.0F;
        float reaction_add = 0.0F;
        bool blocked_by_solid = true;
        bool blocked_by_water = false;
        // Smoothed cursor velocity (cells/s) of an interactive stroke, already added to the water and sand spawn
        // velocities; kept for every material so gas brushes can use it too. Zero for scripted brushes.
        std::array<float, 2> brush_velocity{0.0F, 0.0F};
        // Half-width (cells/s) of the random spread added to each spawned water or sand particle's velocity.
        float velocity_spread = 0.0F;
        bool oil = false;  // spawned water records are tagged as oil
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

    // Simulation viewport inside the framebuffer, in pixels with a bottom-left origin.
    struct SimRect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    enum class BrushMode : int {
        Sand = 0,
        Water = 1,
        Solid = 2,
        Erase = 3,
        Smoke = 4,
        Fire = 5,
        Wood = 6,
        Oil = 7,
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
    // Sand grid node tiles per row (and column): the (grid + 2)-node halo grid in workgroup-sized tiles.
    static constexpr int kSandTileStride = (kGridWidth + 2 + kWorkgroupSize - 1) / kWorkgroupSize;
    static constexpr int kGasWidth = kGridWidth / 2;
    static constexpr int kGasHeight = kGridHeight / 2;
    static constexpr std::uint32_t kGasTickStride = 2;  // one gas pressure solve spans this many ticks (RunGasTick)
    static constexpr int kHeatWidth = kGasWidth;
    static constexpr int kHeatHeight = kGasHeight;
    static constexpr int kSmokeWidth = kGasWidth;
    static constexpr int kSmokeHeight = kGasHeight;
    // Emitted-light texture for the composite: one texel per 4x4 cells, with a full mip chain (8 levels at 250x250).
    static constexpr int kLightWidth = kGridWidth / 4;
    static constexpr int kLightHeight = kGridHeight / 4;
    // Cursor lantern reach in cells: a base plus a share of the brush radius.
    static constexpr float kLanternRadius = 130.0F;
    static constexpr float kLanternPerBrushCell = 1.5F;
    // Spark ring: 128 new records a tick for 128 ticks (2.13 s, over the longest ember life of 2.1 s). A source is a gas
    // cell index + 1 in 18 bits (spark_pick.comp).
    static constexpr int kSparkSpawnsPerTick = 128;
    static constexpr int kSparkRingTicks = 128;
    static constexpr int kSparkCapacity = kSparkSpawnsPerTick * kSparkRingTicks;
    static_assert(kGasWidth * kGasHeight < (1 << 18), "spark_pick.comp packs the gas cell index into 18 bits");
    // Preallocated pools (CreateBuffers checks GL_MAX_SHADER_STORAGE_BLOCK_SIZE): 96 MB of water, 64 MB of sand, each
    // twice for compaction. Sleeping sand lives in the packed texture instead, so the sand pool bounds moving grains.
    static constexpr int kMaxWaterParticles = 1 << 21;
    // Slots the water brush leaves free for packed water that wakes (RunSpawnWaterParticles).
    static constexpr int kWaterWakeReserve = kMaxWaterParticles / 16;
    static constexpr int kWaterBinSize = 8;  // cells per side of a compaction bin
    static constexpr int kWaterBinCount = ((kGridWidth + kWaterBinSize - 1) / kWaterBinSize) * ((kGridHeight + kWaterBinSize - 1) / kWaterBinSize);
    static constexpr int kWaterParticleComponents = 12;
    static constexpr int kSandParticleComponents = 16;
    static constexpr int kMaxSandParticles = 1 << 20;
    // Slots the sand brush leaves free for packed sand that wakes (RunSpawnSandParticles).
    static constexpr int kSandWakeReserve = kMaxSandParticles / 16;
    // Sleeping grains one cell may hold (R8UI); more stay moving. Two fill a cell at rest density, but the bottom of a
    // deep bed is compressed to J ~0.25 (seven or eight per cell; benchmark-fill), so the cap leaves room.
    static constexpr int kMaxPackedGrains = 64;
    // Conductance of a water face against sand, as a fraction of an open face: sand is porous, and the leak
    // keeps water sealed inside sand from forming a singular pressure system.
    static constexpr float kSandLeak = 0.005F;
    // Coarse levels of the water pressure multigrid: halve until the coarsest fits one 8x8 workgroup, at least once.
    static constexpr int kWaterMultigridLevels = [] {
        int levels = 0;
        for (int width = kGridWidth, height = kGridHeight; levels == 0 || width > 8 || height > 8; ++levels) {
            width = (width + 1) / 2;
            height = (height + 1) / 2;
        }
        return levels;
    }();
    static_assert(kWaterMultigridLevels < 8, "pressure_mg.comp holds eight level slots");
    // Byte offset in the CG scalars of the restart's indirect dispatch arguments (pressure_cg.comp).
    static constexpr GLintptr kWaterRestartDispatchOffset = 64;
    static constexpr int kWaterMultigridCells = [] {
        int cells = 0;
        for (int l = 1, width = kGridWidth, height = kGridHeight; l <= kWaterMultigridLevels; ++l) {
            width = (width + 1) / 2;
            height = (height + 1) / 2;
            cells += width * height;
        }
        return cells;
    }();
    static constexpr int kReplayQueryCount = 7;
    static constexpr int kMaxStrokeStamps = 64;
    static constexpr std::size_t kMaxCursorTrail = 256;

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
    // Transparent hashing lets CachedUniform look a name up by string_view without building a std::string.
    struct UniformNameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
    };
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint, UniformNameHash, std::equal_to<>>> uniform_locations_;

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
    int brush_radius_ = 12;  // 6 cm, about 28 px across on a 1440p screen: a stream that reads at first touch

    int brush_x_ = kGridWidth / 2;
    int brush_y_ = kGridHeight / 2;
    // Last interactive stamp of the held stroke; inactive until the next press inside the simulation.
    struct Stroke {
        bool active = false;
        BrushMode mode = BrushMode::Sand;
        int x = 0;
        int y = 0;
    } stroke_;
    // Cursor cells reported since the last stamp (event rate, not frame rate), oldest first and bounded.
    std::vector<std::array<int, 2>> cursor_trail_;
    // One entry of the OS mouse-move history (Windows: GetMouseMovePointsEx), in screen pixels and milliseconds.
    struct MovePoint {
        int x = 0;
        int y = 0;
        std::uint32_t time = 0;
        bool operator==(const MovePoint&) const = default;
    };
    // The newest history entry already in cursor_trail_; invalid after a press, so a stroke never reaches back before it.
    MovePoint last_move_{};
    bool last_move_valid_ = false;
    // Cursor velocity in cells/s: the stroke polyline's travel per tick, smoothed (kBrushVelocityTau) and capped.
    std::array<float, 2> brush_velocity_{0.0F, 0.0F};
    static constexpr float kBrushVelocityTau = 0.05F;
    // 4 m/s: a fast flick; faster cursor jumps would fling matter beyond the CFL budget's intent.
    static constexpr float kBrushVelocityMax = 800.0F;
    bool cursor_in_sim_ = false;
    bool painting_ = false;  // left button held over the simulation, as of the last UpdateInput
    bool help_visible_ = true;
    double help_hide_time_ = -1.0;  // glfwGetTime() at which DrawHud hides the help panel; negative is disarmed
    // Lighting is presentation only. Off, composite.frag draws the plain albedo and no lantern, which is what the
    // render-modes oracle and the HUD pixel tests compare against.
    bool lighting_enabled_ = true;
    // Wood was painted since the last reset: burning wood changes the static geometry (RunCouplingPass).
    bool wood_present_ = false;
    // Oil, or a heat source (fire or wood), was painted since the last reset: only then do the oil drift and the
    // phase passes (RunPhasePass) run.
    bool oil_present_ = false;
    bool heat_present_ = false;
    // Phase changes (RunPhasePass): per-cell oil/all tallies, per-gas-cell steam and lit-oil sources, the cumulative
    // sinks (evaporated water, burned oil records), the oil field and the ping-pong steam scalar.
    struct PhaseState {
        GLuint cell_counts = 0;
        GLuint gas_sources = 0;
        GLuint sinks = 0;
        GLuint oil_field = 0;
        GLuint steam_a = 0;
        GLuint steam_b = 0;
        bool steam_ping = true;
    } phase_state_;
    // Sparks and embers (spark_pick.comp, spark_step.comp, spark.vert): a ring of kSparkCapacity records advanced once per
    // tick, kSparkSpawnsPerTick of them replaced each tick, so a slot is reused only after kSparkRingTicks ticks (longer
    // than any life). Drawn over the lit composite by DrawSparks; both buffers are cleared with the rest of the state.
    struct SparkState {
        GLuint particles = 0;  // two vec4 per record: x, y, vx, vy; age, life, drag time, seed * 2 + ember (life 0 = dead)
        GLuint buckets = 0;    // per-tick spawn contest, one key per spawn slot (spark_pick.comp)
        GLuint vao = 0;        // the particle buffer as two per-instance attributes
    } spark_state_;
    // HUD motion, eased toward the state above by DrawHud. Only the interactive loop draws the HUD, so replays and
    // Render() never touch it.
    struct HudMotion {
        double last_time = -1.0;  // clock of the previous DrawHud; negative until the first call, which snaps
        float help = 1.0F;        // help panel visibility
        float painting = 0.0F;
        std::array<float, 8> slot_pop{};  // selection of each inventory slot
    } hud_motion_;
    std::uint32_t frame_index_ = 0U;

    GLuint material_a_ = 0;
    GLuint render_smoke_density_ = 0;
    GLuint render_fire_emissive_ = 0;
    GLuint render_temperature_ = 0;
    GLuint render_gas_velocity_ = 0;
    GLuint light_texture_ = 0;



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
    GLuint sand_sleep_program_ = 0;
    GLuint sand_pack_program_ = 0;
    GLuint water_sleep_program_ = 0;
    GLuint water_pack_program_ = 0;
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
    GLuint pressure_pockets_program_ = 0;
    GLuint pressure_cg_reduce_program_ = 0;
    GLuint pressure_mg_program_ = 0;
    GLuint water_project_program_ = 0;
    GLuint smoke_advect_program_ = 0;
    GLuint smoke_correct_program_ = 0;
    GLuint gas_velocity_advect_program_ = 0;
    GLuint gas_velocity_correct_program_ = 0;
    GLuint gas_face_to_center_program_ = 0;
    GLuint smoke_divergence_program_ = 0;
    GLuint gas_pressure_kind_program_ = 0;
    GLuint gas_cg_program_ = 0;
    GLuint wood_burn_program_ = 0;
    GLuint phase_particles_program_ = 0;
    GLuint oil_field_program_ = 0;
    GLuint steam_update_program_ = 0;
    GLuint gas_pressure_restrict_program_ = 0;
    GLuint gas_pressure_relax_program_ = 0;
    GLuint smoke_project_program_ = 0;
    GLuint pressure_residual_program_ = 0;
    GLuint fire_rd_program_ = 0;
    GLuint coupling_fire_program_ = 0;
    GLuint coupling_heat_program_ = 0;
    GLuint render_extract_program_ = 0;
    GLuint light_build_program_ = 0;
    GLuint spark_pick_program_ = 0;
    GLuint spark_step_program_ = 0;
    GLuint spark_draw_program_ = 0;
    GLuint render_program_ = 0;
    GLuint hud_program_ = 0;
    GLuint hud_font_texture_ = 0;

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

    void Initialize();
    void Shutdown();

    void CreateTextures();
    void CreateBuffers();
    void CreatePrograms();
    void InitializeState();

    void UpdateInput();
    static SimRect FitSimRect(int framebuffer_width, int framebuffer_height);
    static bool MapCursorToGrid(double cursor_x,
                                double cursor_y,
                                int window_width,
                                int window_height,
                                int framebuffer_width,
                                int framebuffer_height,
                                int& grid_x,
                                int& grid_y);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double x, double y);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static std::vector<MovePoint> MovesSince(const std::vector<MovePoint>& newest_first, const MovePoint& last);
    int RunInteractiveFrame(FixedStepClock& clock, double elapsed);
    void RunFrame(float dt, const BrushConfig* scripted_brush = nullptr, bool reset_counters = true);
    void RunSpawnPass(const BrushConfig* replay_brush);
    void StampStroke();
    static std::vector<std::array<int, 2>> StrokePoints(const std::vector<std::array<int, 2>>& path, int radius);
    void StampBrush(const BrushConfig& config);
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
    void RemovePocketMeans(bool gas);
    void BindWaterSolver();
    void SetupWaterMultigrid();
    void ApplyWaterMultigrid(int slot, bool initial);
    void RunGasTick(float dt);
    // Transport substeps for dt; without project the caller owns the pressure solve (RunGasTick).
    void RunGasPass(float dt, bool project = true);
    void RunSmokePass(float dt, bool project = true);
    void RunGasProjection();
    // The projection in two halves: begin builds the right-hand side, the multigrid footprint and the CG state and runs at
    // most iteration_budget iterations (negative: no limit); finish runs the rest to the residual stop or the cap, then
    // checks and projects the velocity.
    void RunGasProjectionStage(bool begin, bool finish, int iteration_budget);
    void RunFireHeatPass(float dt);
    void RunCouplingPass(float dt);
    void RunWoodPass();
    void RunPhasePass(float dt);
    GLuint CurrentSteam() const;
    std::array<GLuint, 2> ReadPhaseSinks() const;  // evaporated water and burned oil records since the last reset
    void RunRenderFieldExtractionPass();
    void RunLightBuildPass();
    void RunSparkPass(float dt);
    void DrawSparks(const SimRect& sim, int fb_width, int fb_height);
    void Render(bool letterbox = false);
    void DrawHud(double now = -1.0);  // now: seconds on the glfwGetTime clock; negative reads it
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
    std::array<GLuint, 5> ReadDebugCounters() const;
    ParticleMetrics ReadParticleMetrics(GLuint buffer, int capacity, int components) const;
    GLuint ReadSandLiveParticles() const;
    std::uint64_t ReadSandPackedSum() const;  // sleeping grains, each of mass one
    GLuint ReadWaterLiveParticles() const;
    std::uint64_t ReadWaterPackedSum() const;  // packed calm-water particles
    ParticleMetrics ReadWaterMetrics() const;    // moving plus packed water
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

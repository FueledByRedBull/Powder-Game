#include "PowderApp.hpp"

#include "FixedStepClock.hpp"
#include "HudFont.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void CheckGl(const char* operation) {
    std::ostringstream errors;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        errors << " 0x" << std::hex << error;
    }
    Require(errors.str().empty(), std::string(operation) + ": OpenGL errors" + errors.str());
}

void WriteScalar(GLuint texture, float value, int x = 0, int y = 0) {
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RED, GL_FLOAT, &value);
}

void WriteInteger(GLuint texture, GLuint value, int x = 0, int y = 0) {
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}

float ReadScalar(GLuint texture, int x = 0, int y = 0) {
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint width = 0;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    std::vector<float> values(static_cast<std::size_t>(width * height));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
    return values[static_cast<std::size_t>(y * width + x)];
}

GLuint ReadInteger(GLuint texture, int x = 0, int y = 0) {
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint width = 0;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    std::vector<GLuint> values(static_cast<std::size_t>(width * height));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, values.data());
    return values[static_cast<std::size_t>(y * width + x)];
}

using Particle = std::array<float, 12>;
using SandParticle = std::array<float, 16>;  // PowderApp::kSandParticleComponents

template <std::size_t Components>
void WriteParticles(GLuint buffer, const std::vector<std::array<float, Components>>& particles) {
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
}

std::vector<SandParticle> SandRecords(const std::vector<Particle>& common) {
    std::vector<SandParticle> result(common.size());
    for (std::size_t i = 0; i < common.size(); ++i) {
        std::copy(common[i].begin(), common[i].end(), result[i].begin());
        result[i][10] = 0.0F;  // no volume correction
        result[i][12] = result[i][15] = 1.0F;
    }
    return result;
}

template <std::size_t Components = 12>
std::vector<std::array<float, Components>> ReadParticles(GLuint buffer, std::size_t count) {
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    std::vector<std::array<float, Components>> particles(count);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
    return particles;
}

PFNGLTEXSUBIMAGE2DPROC native_upload = nullptr;
std::vector<std::string> misaligned_uploads;

void APIENTRY AuditUpload(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, const void* data) {
    GLint alignment = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    const int channels = format == GL_RG || format == GL_RG_INTEGER ? 2 : 1;
    const int bytes = type == GL_UNSIGNED_BYTE ? 1 : type == GL_HALF_FLOAT ? 2 : 4;
    if (height > 1 && (width * channels * bytes) % alignment != 0) {
        misaligned_uploads.push_back(std::to_string(width) + " texels, alignment " + std::to_string(alignment));
    }
    native_upload(target, level, x, y, width, height, format, type, data);
}

struct UploadAudit {
    UploadAudit() {
        misaligned_uploads.clear();
        native_upload = glad_glTexSubImage2D;
        glad_glTexSubImage2D = AuditUpload;
    }
    ~UploadAudit() {
        glad_glTexSubImage2D = native_upload;
    }
};

}  // namespace

struct AppLifecycleTests {
    // A fixture write at slot 0 stands in for a spawn: the pool's cursor and the host's water record bound cover it.
    template <std::size_t Components>
    static void WriteParticles(PowderApp& app, GLuint buffer, const std::vector<std::array<float, Components>>& particles) {
        ::WriteParticles(buffer, particles);
        const bool water = buffer == app.water_state_.particles;
        if (!water && buffer != app.sand_state_.particles) return;
        GLuint cursor = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, water ? app.water_state_.spawn_cursor : app.sand_state_.spawn_cursor);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
        cursor = std::max(cursor, static_cast<GLuint>(particles.size()));
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        GLuint& bound = water ? app.water_state_.record_bound : app.sand_state_.record_bound;
        bound = std::max(bound, cursor);
    }

    // Relaxation dispatches per gas V-cycle: one fused smoothing before and one after each coarser level, plus one
    // dispatch for the whole bottom solve.
    static constexpr int kGasRelaxPerCycle = 2 * (PowderApp::kGasLevels - 1) + 1;
    static void BoundaryLifecycle(PowderApp& app) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        app.InitializeState();
        const auto paint = [&](PowderApp::BrushMode mode) {
            app.brush_mode_ = mode;
            app.brush_x_ = app.brush_y_ = 500;
            app.brush_radius_ = 0;
            const auto brush = app.BuildBrushConfig();
            app.RunSpawnPass(&brush);
        };
        const auto check = [&](const std::vector<std::array<int, 2>>& solids, bool sand) {
            const auto occupied = [&](int x, int y, bool include_sand) {
                return (include_sand && sand && x == 510 && y == 510) ||
                    std::any_of(solids.begin(), solids.end(), [&](const auto& point) {
                        return point[0] == x && point[1] == y;
                    });
            };
            for (const bool dynamic : {false, true}) {
                const GLuint texture = dynamic ? app.boundary_state_.mask_full : app.boundary_state_.mask_static;
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, texture);
                std::vector<unsigned char> mask(PowderApp::kGridWidth * PowderApp::kGridHeight);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                bool exact = true;
                for (int y = 0; y < PowderApp::kGridHeight; ++y) {
                    for (int x = 0; x < PowderApp::kGridWidth; ++x) {
                        // Painted solids are 1; a cell at 0.9 of rest density in sand is 3 (2 is the water-only band above 0.25).
                        const int expected = occupied(x, y, false) ? 1 : (occupied(x, y, dynamic) ? 3 : 0);
                        exact = exact && mask[y * PowderApp::kGridWidth + x] == expected;
                    }
                }
                Require(exact, "boundary full/static mask disagrees with source geometry");
            }
            glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
            std::vector<unsigned char> mask(PowderApp::kGasWidth * PowderApp::kGasHeight);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            bool exact = true;
            for (int y = 0; y < PowderApp::kGasHeight; ++y) {
                for (int x = 0; x < PowderApp::kGasWidth; ++x) {
                    // 1 for a painted solid in the footprint, else 3 for dense sand (porous to the gas pressure solve).
                    bool solid = false, sand = false;
                    for (int sy = std::max(0, y * 2 - 1); sy < std::min(PowderApp::kGridHeight, y * 2 + 3); ++sy)
                        for (int sx = std::max(0, x * 2 - 1); sx < std::min(PowderApp::kGridWidth, x * 2 + 3); ++sx) {
                            solid = solid || occupied(sx, sy, false);
                            sand = sand || occupied(sx, sy, true);
                        }
                    exact = exact && mask[y * PowderApp::kGasWidth + x] == (solid ? 1 : sand ? 3 : 0);
                }
            }
            Require(exact, "gas mask disagrees with the conservative full-resolution footprint");
            glBindTexture(GL_TEXTURE_2D, app.boundary_state_.sdf_static);
            std::vector<float> distances(PowderApp::kGridWidth * PowderApp::kGridHeight);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, distances.data());
            bool signs_match = true;
            for (int y = 0; y < PowderApp::kGridHeight; ++y)
                for (int x = 0; x < PowderApp::kGridWidth; ++x) {
                    const float value = distances[y * PowderApp::kGridWidth + x];
                    signs_match = signs_match && std::isfinite(value) && (value <= 0.0F) == occupied(x, y, false);
                }
            Require(signs_match, "static distance sign is stale or includes dynamic sand");
            if (solids.size() == 1) {
                Require(distances[500 * PowderApp::kGridWidth + 500] == -0.5F &&
                            distances[500 * PowderApp::kGridWidth + 501] == 0.5F &&
                            distances[500 * PowderApp::kGridWidth + 502] == 1.5F,
                        "isolated static obstacle distance/normal samples changed");
            }
            CheckGl("boundary lifecycle readback");
        };
        struct Queries {
            GLuint ids[2]{};
            Queries() { glGenQueries(2, ids); }
            ~Queries() { glDeleteQueries(2, ids); }
        } queries;
        const auto measure = [&](const char* name, bool repaint) {
            constexpr int warmup = 8, samples = 48;
            std::vector<double> milliseconds;
            for (int frame = -warmup; frame < samples; ++frame) {
                if (repaint) paint(PowderApp::BrushMode::Solid);
                glQueryCounter(queries.ids[0], GL_TIMESTAMP);
                app.RunBoundaryPass();
                glQueryCounter(queries.ids[1], GL_TIMESTAMP);
                GLuint64 start = 0, end = 0;
                glGetQueryObjectui64v(queries.ids[0], GL_QUERY_RESULT, &start);
                glGetQueryObjectui64v(queries.ids[1], GL_QUERY_RESULT, &end);
                if (frame >= 0) milliseconds.push_back(static_cast<double>(end - start) / 1.0e6);
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            std::cout << "boundary " << name << " GPU mean/p95 ms="
                      << std::accumulate(milliseconds.begin(), milliseconds.end(), 0.0) / samples << '/'
                      << milliseconds[static_cast<std::size_t>(std::ceil(samples * 0.95)) - 1]
                      << " warmup=" << warmup << " samples=" << samples << '\n';
            CheckGl("boundary timestamp measurement");
        };
        std::cout << "boundary renderer=" << glGetString(GL_RENDERER) << " size=" << PowderApp::kGridWidth
                  << 'x' << PowderApp::kGridHeight << '\n';
        check({}, false);
        measure("empty-repeat", false);
        paint(PowderApp::BrushMode::Solid);
        app.RunBoundaryPass();
        check({{500, 500}}, false);
        measure("static-repeat", false);
        measure("static-repaint", true);
        check({{500, 500}}, false);
        auto replacement = app.BuildBrushConfig();
        replacement.material_value = 0;
        replacement.blocked_by_solid = false;
        app.RunSpawnFullRes(replacement, app.ComputeDispatchRegion(1, PowderApp::kGridWidth, PowderApp::kGridHeight));
        app.RunBoundaryPass();
        check({}, false);
        paint(PowderApp::BrushMode::Solid);
        app.RunBoundaryPass();
        WriteScalar(app.sand_state_.occupancy, 0.9F, 510, 510);
        app.RunBoundaryPass();
        check({{500, 500}}, true);
        paint(PowderApp::BrushMode::Erase);
        app.RunBoundaryPass();
        check({}, true);
        app.ApplyReplayBrush({0, PowderApp::BrushMode::Solid, 500, 500, 1, 1});
        const auto replay_brush = app.BuildBrushConfig();
        app.RunSpawnPass(&replay_brush);
        app.RunBoundaryPass();
        check({{500, 500}, {499, 500}, {501, 500}, {500, 499}, {500, 501}}, true);
        app.InitializeState();
        check({}, false);
        app.RunBoundaryPass();
        check({}, false);
    }

    static void UploadAlignment(PowderApp& app) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        {
            UploadAudit audit;
            app.InitializeState();
        }
        GLint alignment = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        Require(alignment == 4, "InitializeState must restore the caller's unpack alignment");
        std::ostringstream failures;
        for (const auto& upload : misaligned_uploads) {
            failures << " [" << upload << ']';
        }
        Require(misaligned_uploads.empty(), "tightly packed reset uploads use padded rows:" + failures.str());
        for (GLuint texture : {app.gas_state_.mac_u_a, app.gas_state_.level_pressure[1]}) {
            Require(ReadScalar(texture) == 0.0F, "odd-width floating reset texture must contain zero");
        }
    }

    static void Reset(PowderApp& app) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        app.InitializeState();
        const std::array<std::pair<const char*, GLuint>, 6> zero_fields{{
            {"water U", app.water_state_.mac_u}, {"water V", app.water_state_.mac_v},
            {"previous water U", app.water_state_.mac_u_prev},
            {"previous water V", app.water_state_.mac_v_prev},
            {"water pressure A", app.water_state_.pressure_a},
            {"water divergence", app.water_state_.divergence}}};
        for (const auto& [name, texture] : zero_fields) {
            (void)name;
            WriteScalar(texture, 1.0F);
        }
        for (GLuint texture : {app.boundary_state_.mask_full, app.boundary_state_.mask_static,
                               app.boundary_state_.mask_gas}) {
            WriteInteger(texture, 1U);
        }
        WriteInteger(app.CurrentMaterial(), 2U);
        app.gas_mac_ping_ = app.smoke_ping_ = false;
        app.frame_index_ = 123U;
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_radius_ = 11;
        const float original_dt = app.config_.fixed_dt;
        app.InitializeState();

        std::ostringstream failures;
        for (const auto& [name, texture] : zero_fields) {
            if (ReadScalar(texture) != 0.0F) {
                failures << " [stale " << name << ']';
            }
        }
        for (GLuint texture : {app.boundary_state_.mask_full, app.boundary_state_.mask_static,
                               app.boundary_state_.mask_gas}) {
            if (ReadInteger(texture) != 0U) {
                failures << " [stale boundary mask]";
            }
        }
        if (ReadInteger(app.CurrentMaterial()) != 0U) {
            failures << " [stale painted material]";
        }
        if (!(app.gas_mac_ping_ && app.smoke_ping_)) {
            failures << " [ping flags not reset]";
        }
        if (app.frame_index_ != 0U) {
            failures << " [frame index not reset]";
        }
        if (ReadScalar(app.water_state_.liquid_phi) < app.config_.water_liquid_phi_threshold) {
            failures << " [empty reset cell classified as liquid]";
        }
        Require(app.brush_mode_ == PowderApp::BrushMode::Water && app.brush_radius_ == 11 &&
                    app.config_.fixed_dt == original_dt,
                "reset must preserve brush choice and simulation configuration");
        Require(failures.str().empty(), "incomplete scene reset:" + failures.str());
    }

    static void Erase(PowderApp& app) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        app.InitializeState();
        app.brush_x_ = app.brush_y_ = 500;
        app.brush_radius_ = 3;
        WriteInteger(app.CurrentMaterial(), 2U, 500, 500);
        WriteScalar(app.CurrentWaterAmount(), 0.5F, 500, 500);
        WriteScalar(app.CurrentSmokeDen(), 0.5F, 250, 250);
        WriteScalar(app.CurrentFuel(), 0.5F, 250, 250);
        const std::vector<Particle> particle{{500.5F, 500.5F, 0.0F, 0.0F, 1.0F, 1.0F}};
        WriteParticles(app, app.sand_state_.particles, SandRecords(particle));
        WriteParticles(app, app.water_state_.particles, particle);
        app.brush_mode_ = PowderApp::BrushMode::Erase;
        const auto config = app.BuildBrushConfig();
        app.RunSpawnPass(&config);
        Require(ReadInteger(app.CurrentMaterial(), 500, 500) == 0U, "scripted erase must clear painted solids");
        Require(ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, 1)[0][4] == 0.0F, "scripted erase must kill sand particles");
        Require(ReadParticles(app.water_state_.particles, 1)[0][4] == 0.0F, "scripted erase must kill water particles");
        Require(ReadScalar(app.CurrentWaterAmount(), 500, 500) == 0.0F, "scripted erase must clear derived water");
        Require(ReadScalar(app.CurrentSmokeDen(), 250, 250) == 0.0F &&
                    ReadScalar(app.CurrentFuel(), 250, 250) == 0.0F,
                "scripted erase must clear gas scalars");

        // Erase is brush-local: water frozen under distant sand (blocked in mask_full) must survive it.
        app.InitializeState();
        WriteScalar(app.sand_state_.occupancy, 0.9F, 700, 700);
        app.RunBoundaryPass();
        WriteParticles(app, app.water_state_.particles, std::vector<Particle>{{700.5F, 700.5F, 0.0F, 0.0F, 1.0F, 1.0F}});
        app.RunSpawnPass(&config);
        Require(ReadParticles(app.water_state_.particles, 1)[0][4] == 1.0F, "erase must not remove water outside its brush");

        // A solid brush removes the particles of every cell it paints and nothing else. Brush radius 3 at 500,500:
        // cell 503 is painted though a particle at x=503.9 lies beyond the disc; cell 504 is not painted.
        app.InitializeState();
        const std::vector<Particle> particles{{500.5F, 500.5F, 0.0F, 0.0F, 1.0F, 1.0F},
                                              {503.9F, 500.5F, 0.0F, 0.0F, 1.0F, 1.0F},
                                              {504.1F, 500.5F, 0.0F, 0.0F, 1.0F, 1.0F},
                                              {520.5F, 500.5F, 0.0F, 0.0F, 1.0F, 1.0F}};
        WriteParticles(app, app.sand_state_.particles, SandRecords(particles));
        WriteParticles(app, app.water_state_.particles, particles);
        app.brush_mode_ = PowderApp::BrushMode::Solid;
        const auto solid = app.BuildBrushConfig();
        app.RunSpawnPass(&solid);
        const auto sand = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, particles.size());
        const auto water = ReadParticles(app.water_state_.particles, particles.size());
        for (std::size_t i = 0; i < particles.size(); ++i) {
            const float painted = i < 2 ? 0.0F : 1.0F;
            Require(sand[i][4] == painted && water[i][4] == painted,
                    "solid brush must remove sand and water particles in painted cells only: particle " + std::to_string(i));
        }
    }

    static void WaterForce(PowderApp& app) {
        const auto original_config = app.config_;
        app.config_.water_flip_blend = 1.0F;
        app.config_.water_velocity_damping = 1.0F;
        app.config_.water_bulk_damping = 0.0F;
        app.config_.water_max_velocity = 2.0F;
        app.config_.water_min_substeps = app.config_.water_max_substeps = 1;
        const auto run = [&](float gravity) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            app.InitializeState();
            app.RunBoundaryPass();
            app.config_.water_gravity = gravity;
            std::vector<Particle> particles;
            for (int y = 497; y <= 503; ++y) {
                for (int x = 497; x <= 503; ++x) {
                    particles.push_back({x + 0.5F, y + 0.5F, 0.0F, 0.0F, 1.0F, 1.0F});
                }
            }
            WriteParticles(app, app.water_state_.particles, particles);
            app.RunWaterPass(1.0F / 60.0F);
            const auto actual = ReadParticles(app.water_state_.particles, particles.size());
            std::array<double, 2> mean{};
            int count = 0;
            // The central 3x3 particles have full interpolation support inside the 7x7 cluster.
            for (const auto& particle : actual) {
                Require(particle[4] > 0.5F, "interior force fixture lost a live particle");
                if (particle[0] > 499.0F && particle[0] < 502.0F && particle[1] > 499.0F && particle[1] < 502.0F) {
                    Require(std::isfinite(particle[1]) && std::isfinite(particle[3]), "non-finite water force result");
                    mean[0] += particle[3];
                    mean[1] += particle[1];
                    ++count;
                }
            }
            Require(count == 9, "force fixture must retain the central nine particles");
            mean[0] /= count;
            mean[1] /= count;
            return mean;
        };
        const auto control = run(0.0F);
        const auto gravity = run(-60.0F);
        app.config_ = original_config;
        std::cout << "water force: control vy=" << control[0] << " y=" << control[1]
                  << "; gravity vy=" << gravity[0] << " y=" << gravity[1] << '\n';
        Require(std::abs(control[0]) < 0.001 && std::abs(gravity[0] - control[0] + 1.0) < 0.01,
                "pure FLIP must retain gravity*dt=-1; velocity delta=" + std::to_string(gravity[0] - control[0]));
        Require(std::abs(gravity[1] - control[1] + 1.0 / 60.0) < 0.001,
                "gravity must advance the interior particle centroid by -1/60 cell");
    }

    static void WaterForceRange(PowderApp& app) {
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static int dispatches = 0;
        static GLuint project_program = 0, projected_v = 0;
        static float forced_v = 0.0F;
        static PowderApp* observed_app = nullptr;
        static bool observe_additive = false;
        static int projections = 0;
        static float first_raw_v = 0.0F, first_pressure_gradient = 0.0F, first_forced_v = 0.0F;
        struct RestoreDispatch {
            PFNGLDISPATCHCOMPUTEPROC saved;
            ~RestoreDispatch() { glad_glDispatchCompute = saved; }
        } restore{glad_glDispatchCompute};
        native_dispatch = restore.saved;
        project_program = app.water_project_program_;
        projected_v = app.water_state_.mac_v;
        observed_app = &app;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            ++dispatches;
            GLint program = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            const bool first_projection = observe_additive && static_cast<GLuint>(program) == project_program && projections == 0;
            if (first_projection) {
                first_raw_v = ReadScalar(observed_app->water_state_.mac_v_prev, 500, 500);
                first_pressure_gradient = ReadScalar(observed_app->CurrentWaterPressure(), 500, 500) -
                                          ReadScalar(observed_app->CurrentWaterPressure(), 500, 499);
            }
            native_dispatch(x, y, z);
            if (static_cast<GLuint>(program) == project_program) {
                forced_v = ReadScalar(projected_v, 500, 500);
                if (first_projection) first_forced_v = forced_v;
                ++projections;
            }
        };
        std::ostringstream failures;
        for (float gravity : {1e7F, -1e7F, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}) {
            app.config_ = {};
            app.InitializeState();
            app.config_.water_gravity = gravity;
            dispatches = 0;
            bool rejected = false;
            try { app.RunWaterPass(app.config_.fixed_dt); }
            catch (const std::runtime_error& error) {
                const std::string message = error.what();
                rejected = message.find("POWDER_WATER_GRAVITY") != std::string::npos &&
                           message.find("POWDER_FIXED_DT") != std::string::npos;
            }
            std::cout << "water force range gravity=" << gravity << " rejected=" << rejected
                      << " dispatches=" << dispatches;
            if (!rejected) std::cout << " stored V=" << forced_v;
            std::cout << '\n';
            if (!rejected || dispatches != 0) failures << " [unrepresentable gravity impulse was dispatched]";
        }
        for (float gravity : {-140.0F, 1e7F, -1e7F}) {
            app.config_ = {};
            app.InitializeState();
            app.config_.water_gravity = gravity;
            const int steps = gravity == -140.0F ? 2 : 4;
            app.config_.water_min_substeps = app.config_.water_max_substeps = steps;
            app.config_.water_max_velocity = 72.0F;  // the fixture forces its substep count; 72 cells/s fits two
            dispatches = 0;
            app.RunWaterPass(app.config_.fixed_dt);
            const float actual = forced_v;
            const float expected = gravity * (app.config_.fixed_dt / steps);
            const float half_ulp = std::ldexp(1.0F, std::ilogb(std::abs(expected)) - 10);
            Require(dispatches > 0 && app.water_state_.substeps == steps && std::isfinite(actual) &&
                        std::abs(actual - expected) <= half_ulp &&
                        app.ReadDebugCounters() == std::array<GLuint, 5>{},
                    "representable gravity impulse must execute and survive R16F storage within one ULP");
            std::cout << "water force range control gravity=" << gravity << " substeps=" << steps
                      << " expected/stored V=" << expected << '/' << actual << '\n';
        }
        for (float impulse : {65408.0F, -65408.0F, 65472.0F, -65472.0F}) {
            app.config_ = {};
            app.InitializeState();
            const float initial_velocity = std::copysign(72.0F, impulse);
            std::vector<Particle> particles;
            for (int y = 496; y <= 503; ++y) {
                for (int x = 496; x <= 503; ++x) {
                    for (float dy : {0.25F, 0.75F}) {
                        for (float dx : {0.25F, 0.75F})
                            particles.push_back({x + dx, y + dy, 0.0F, initial_velocity, 1.0F, 1.0F});
                    }
                }
            }
            WriteParticles(app, app.water_state_.particles, particles);
            app.config_.water_gravity = impulse / (app.config_.fixed_dt / 2.0F);
            app.config_.water_min_substeps = app.config_.water_max_substeps = 2;  // the impulse above assumes two substeps
            app.config_.water_max_velocity = 72.0F;
            observe_additive = true;
            projections = 0;
            dispatches = 0;
            app.RunWaterPass(app.config_.fixed_dt);
            observe_additive = false;
            const auto counters = app.ReadDebugCounters();
            const bool overflow = std::abs(impulse + initial_velocity) > 65504.0F;
            std::cout << "water additive range impulse=" << impulse << " initial V=" << initial_velocity
                      << " projections=" << projections;
            if (projections != 0) {
                const float expected = first_raw_v + app.config_.water_gravity * (app.config_.fixed_dt / 2.0F) -
                                       first_pressure_gradient;
                std::cout << " raw V=" << first_raw_v << " pressure gradient=" << first_pressure_gradient
                          << " expected/stored V=" << expected << '/' << first_forced_v;
                Require(std::abs(first_raw_v - initial_velocity) < 0.01F && std::isfinite(expected) &&
                            (std::abs(expected) > 65504.0F) == overflow,
                        "measured project inputs must reproduce the intended additive storage range");
                if (!overflow && (!std::isfinite(first_forced_v) || std::abs(first_forced_v - expected) > 16.0F))
                    failures << " [representable additive force was not retained within half a binary16 ULP]";
            }
            std::cout << " counters=" << counters[0] << ',' << counters[1] << ',' << counters[2] << ',' << counters[3]
                      << " clamped=" << counters[4] << '\n';
            if (overflow && counters[0] == 0U)
                failures << " [out-of-range additive force silently saturated without a diagnostic]";
            // The impulse drives velocities far past the cap, so the clamp counter (index 4) is expected to count them.
            if (!overflow && (projections == 0 || counters[0] != 0U || counters[1] != 0U || counters[2] != 0U || counters[3] != 0U))
                failures << " [representable additive-force control failed]";
        }
        app.config_ = {};
        CheckGl("water force range");
        Require(failures.str().empty(), failures.str());
    }

    static void WaterSurface(PowderApp& app) {
        const auto original_config = app.config_;
        app.config_.water_gravity = 0.0F;
        app.config_.water_flip_blend = 1.0F;
        app.config_.water_velocity_damping = 1.0F;
        app.config_.water_bulk_damping = 0.0F;
        app.config_.water_max_velocity = 2.0F;
        app.config_.water_min_substeps = app.config_.water_max_substeps = 1;
        app.InitializeState();
        std::vector<Particle> particles;
        for (int y = 497; y <= 501; ++y) {
            for (int x = 497; x <= 501; ++x) {
                particles.push_back({x + 0.31F, y + 0.67F, 1.0F, -1.0F, 1.0F, 1.0F});
                particles.push_back({x + 0.69F, y + 0.33F, 1.0F, -1.0F, 1.0F, 1.0F});
            }
        }
        WriteParticles(app, app.water_state_.particles, particles);
        app.RunWaterPass(1.0F / 60.0F);
        app.config_ = original_config;

        // Unit velocities share the P2G weight quantization exactly, so this translation has zero pressure RHS.
        std::vector<float> divergence(PowderApp::kGridWidth * PowderApp::kGridHeight);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentWaterDivergence());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, divergence.data());
        float max_divergence = 0.0F;
        for (float value : divergence) {
            Require(std::isfinite(value), "surface transfer produced nonfinite divergence");
            max_divergence = std::max(max_divergence, std::abs(value));
        }
        std::size_t live_count = 0;
        std::size_t surface_count = 0;
        float max_surface_error = 0.0F;
        float max_interior_error = 0.0F;
        for (const auto& particle : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
            if (particle[4] < 0.5F) continue;
            ++live_count;
            for (float value : particle) Require(std::isfinite(value), "surface transfer produced a nonfinite particle");
            const float error = std::max(std::abs(particle[2] - 1.0F), std::abs(particle[3] + 1.0F));
            if (particle[0] < 498.0F || particle[0] > 501.0F || particle[1] < 498.0F || particle[1] > 501.0F) {
                ++surface_count;
                max_surface_error = std::max(max_surface_error, error);
            } else {
                max_interior_error = std::max(max_interior_error, error);
            }
        }
        std::cout << "water surface: live=" << live_count << " surface=" << surface_count
                  << " pressure RHS max=" << max_divergence << " velocity error surface=" << max_surface_error
                  << " interior=" << max_interior_error << '\n';
        Require(live_count == particles.size() && surface_count == 32, "surface fixture must retain all 50 particles");
        Require(ReadScalar(app.water_state_.liquid_phi, 499, 499) < app.config_.water_liquid_phi_threshold,
                "surface fixture center must be liquid");
        // Half-float face storage can round unit velocity by one ULP (2^-11).
        Require(max_divergence < 0.002F, "uniform translation RHS must stay within face quantization error");
        Require(max_surface_error < 0.002F && max_interior_error < 0.002F,
                "pure FLIP must preserve uniform translation across the free surface");
    }

    static void WaterMass(PowderApp& app) {
        const auto original_config = app.config_;
        app.config_ = powder_config::SimulationConfig{};
        app.config_.water_gravity = 0.0F;
        app.InitializeState();
        std::vector<Particle> particles;
        for (int y = 497; y <= 501; ++y) {
            for (int x = 497; x <= 501; ++x) {
                particles.push_back({x + 0.31F, y + 0.67F, 0.0F, 0.0F, 1.0F, app.config_.water_particle_mass});
                particles.push_back({x + 0.69F, y + 0.33F, 0.0F, 0.0F, 1.0F, app.config_.water_particle_mass});
            }
        }
        WriteParticles(app, app.water_state_.particles, particles);
        const auto measure = [&]() {
            std::pair<std::size_t, double> result{};
            for (const auto& particle : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
                if (particle[4] < 0.5F) continue;
                for (float value : particle) Require(std::isfinite(value), "mass fixture produced a nonfinite particle");
                Require(particle[5] > 0.0F, "live water particles must have positive stored mass");
                ++result.first;
                result.second += particle[5];
            }
            return result;
        };
        const auto before = measure();
        app.RunWaterPass(1.0F / 60.0F);
        const float center_phi = ReadScalar(app.water_state_.liquid_phi, 499, 499);
        const float phi_threshold = app.config_.water_liquid_phi_threshold;
        app.config_ = original_config;
        const auto after = measure();
        std::cout << "water mass: live=" << before.first << " -> " << after.first
                  << " stored mass=" << before.second << " -> " << after.second
                  << " center phi=" << center_phi << '\n';
        Require(before.first == particles.size() && after.first > 0, "mass fixture must contain live water particles");
        Require(std::isfinite(center_phi) && center_phi < phi_threshold, "mass fixture center must be liquid");
        Require(std::abs(after.second - before.second) <= before.second * 0.0001,
                "source-free water must conserve total stored particle mass");
    }

    // A water stamp that might not fit below the wake reserve is skipped whole on the host, never cut short on the GPU.
    static void WaterPoolFull(PowderApp& app) {
        app.config_ = {};
        app.InitializeState();
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_x_ = app.brush_y_ = 500;
        app.brush_radius_ = 12;
        const auto stamp_from = [&](GLuint record_bound) {
            app.water_state_.record_bound = record_bound;
            app.StampBrush(app.BuildBrushConfig());
            GLuint cursor = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.spawn_cursor);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
            return std::pair{cursor, app.water_state_.record_bound};
        };
        const auto [full_cursor, full_bound] = stamp_from(PowderApp::kMaxWaterParticles - PowderApp::kWaterWakeReserve);
        const auto [open_cursor, open_bound] = stamp_from(0U);
        app.water_state_.record_bound = 0U;
        std::cout << "water-pool-full: cursor full/open=" << full_cursor << '/' << open_cursor << " bound=" << full_bound << '/'
                  << open_bound << '\n';
        Require(full_cursor == 0U && full_bound == PowderApp::kMaxWaterParticles - PowderApp::kWaterWakeReserve,
                "a stamp that might pass the reserve must spawn nothing");
        Require(open_cursor > 0U && open_cursor <= open_bound, "a stamp below the reserve must spawn");
        Require(app.ReadDebugCounters()[1] == 0U, "a skipped stamp must not count overflow");
    }

    // Likewise for sand: a stamp that might not fit below the wake reserve is skipped whole, never cut short on the GPU.
    static void SandPoolFull(PowderApp& app) {
        app.config_ = {};
        app.InitializeState();
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_x_ = app.brush_y_ = 500;
        app.brush_radius_ = 12;
        const auto stamp_from = [&](GLuint record_bound) {
            app.sand_state_.record_bound = record_bound;
            app.StampBrush(app.BuildBrushConfig());
            GLuint cursor = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.sand_state_.spawn_cursor);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
            return std::pair{cursor, app.sand_state_.record_bound};
        };
        const auto [full_cursor, full_bound] = stamp_from(PowderApp::kMaxSandParticles - PowderApp::kSandWakeReserve);
        const auto [open_cursor, open_bound] = stamp_from(0U);
        app.sand_state_.record_bound = 0U;
        std::cout << "sand-pool-full: cursor full/open=" << full_cursor << '/' << open_cursor << " bound=" << full_bound << '/'
                  << open_bound << '\n';
        Require(full_cursor == 0U && full_bound == PowderApp::kMaxSandParticles - PowderApp::kSandWakeReserve,
                "a stamp that might pass the reserve must spawn nothing");
        Require(open_cursor > 0U && open_cursor <= open_bound, "a stamp below the reserve must spawn");
        Require(app.ReadDebugCounters()[1] == 0U, "a skipped stamp must not count overflow");
    }

    static void BenchmarkPressure(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
        constexpr std::size_t cells = static_cast<std::size_t>(width) * height;
        // POWDER_BENCH_PRESSURE_SNAPSHOT selects another system in the same format (benchmark-interactive's capture).
        const char* snapshot_path = std::getenv("POWDER_BENCH_PRESSURE_SNAPSHOT");
        std::ifstream snapshot(snapshot_path != nullptr ? snapshot_path : "build/goal-baseline/water-pressure-failing-system.bin",
                               std::ios::binary);
        Require(snapshot.is_open(), "pressure benchmark needs the preserved water-pressure-failing-system.bin");
        std::array<std::int32_t, 2> dimensions{};
        snapshot.read(reinterpret_cast<char*>(dimensions.data()), sizeof(dimensions));
        Require(snapshot.good() && dimensions == std::array<std::int32_t, 2>{width, height},
                "pressure snapshot dimensions do not match the app grid");
        std::vector<GLubyte> mask(cells);
        std::vector<float> rhs(cells), saved_pressure(cells), phi(cells), pressure(cells), zero(cells, 0.0F);
        snapshot.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
        for (auto* values : {&rhs, &saved_pressure, &phi}) {
            snapshot.read(reinterpret_cast<char*>(values->data()), static_cast<std::streamsize>(cells * sizeof(float)));
            Require(snapshot.good() && std::all_of(values->begin(), values->end(), [](float x) { return std::isfinite(x); }),
                    "pressure snapshot is truncated or contains nonfinite input");
        }
        Require(snapshot.peek() == std::char_traits<char>::eof() &&
                    std::all_of(mask.begin(), mask.end(), [](GLubyte x) { return x <= 1; }),
                "pressure snapshot has trailing bytes or an invalid mask");
        app.config_ = {};
        app.InitializeState();
        const auto upload = [&](GLuint texture, GLenum format, GLenum type, const void* data) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
        };
        upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
        upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
        struct Queries {
            GLuint ids[2]{};
            Queries() { glGenQueries(2, ids); }
            ~Queries() { glDeleteQueries(2, ids); }
        } queries;
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static int dispatches = 0;
        struct RestoreDispatch {
            PFNGLDISPATCHCOMPUTEPROC saved;
            ~RestoreDispatch() { glad_glDispatchCompute = saved; }
        } restore{glad_glDispatchCompute};
        native_dispatch = restore.saved;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) { ++dispatches; native_dispatch(x, y, z); };
        const auto blocked = [&](int x, int y) {
            return x < 0 || y < 0 || x >= width || y >= height || mask[static_cast<std::size_t>(y * width + x)] != 0;
        };
        // Half a second of cold solves first raises the GPU clock, which the latency-bound solve is sensitive to.
        for (const double warmup = glfwGetTime(); glfwGetTime() - warmup < 0.5;) {
            upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero.data());
            app.water_state_.pressure_iterations = 0;
            app.RunWaterProjection();
            glFinish();
        }
        for (bool warm : {false, true}) for (int sample = 0; sample < 3; ++sample) {
            upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, warm ? saved_pressure.data() : zero.data());
            const GLuint empty = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &empty);
            dispatches = 0;
            const auto start = std::chrono::steady_clock::now();
            glQueryCounter(queries.ids[0], GL_TIMESTAMP);
            app.RunWaterProjection();
            glQueryCounter(queries.ids[1], GL_TIMESTAMP);
            GLuint64 first = 0, last = 0;
            glGetQueryObjectui64v(queries.ids[0], GL_QUERY_RESULT, &first);
            glGetQueryObjectui64v(queries.ids[1], GL_QUERY_RESULT, &last);
            const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, pressure.data());
            Require(std::all_of(pressure.begin(), pressure.end(), [](float x) { return std::isfinite(x); }),
                    "pressure benchmark produced nonfinite pressure");
            double squared = 0.0, maximum = 0.0;
            std::size_t liquid_cells = 0;
            GLuint failures = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto i = static_cast<std::size_t>(y * width + x);
                if (blocked(x, y) || phi[i] >= app.config_.water_liquid_phi_threshold) continue;
                float neighbors = 0.0F, diagonal = 0.0F;
                for (const auto& d : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (blocked(nx, ny)) continue;
                    const auto j = static_cast<std::size_t>(ny * width + nx);
                    if (phi[j] < app.config_.water_liquid_phi_threshold) {
                        ++diagonal;
                        neighbors += pressure[j];
                    } else {
                        diagonal += AirConductance(app.config_, phi[i], phi[j]);
                    }
                }
                const float residual = rhs[i] - (neighbors - diagonal * pressure[i]);
                maximum = std::max(maximum, double(std::abs(residual)));
                squared += double(residual) * residual;
                failures += std::abs(residual) > app.config_.water_pressure_residual_threshold ? 1U : 0U;
                ++liquid_cells;
            }
            std::array<GLuint, 8> state{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(state), state.data());
            const auto counters = app.ReadDebugCounters();
            std::cout << "pressure-only " << (warm ? "snapshot" : "cold") << " sample=" << sample
                      << " GPU/wall ms=" << double(last - first) / 1e6 << '/' << wall_ms
                      << " iterations/dispatches=" << state[6] << '/' << dispatches
                      << " residual max/RMS=" << maximum << '/' << std::sqrt(squared / std::max<std::size_t>(1, liquid_cells))
                      << " liquid cells=" << liquid_cells << " failures=" << failures << '\n';
            Require(liquid_cells > 0 && failures == 0 && counters == std::array<GLuint, 5>{},
                    "frozen pressure benchmark failed its unchanged true-residual or debug gate");
        }
        CheckGl("frozen pressure benchmark");
    }

    static void BenchmarkGasPressure(PowderApp& app, bool slow) {
        constexpr int width = PowderApp::kGasWidth, height = PowderApp::kGasHeight;
        constexpr std::size_t cells = static_cast<std::size_t>(width * height);
        const auto original_config = app.config_;
        app.config_ = powder_config::LoadConfigFromEnvironment();
        std::vector<unsigned char> mask(cells);
        std::vector<float> rhs(cells), pressure(cells);
        std::vector<float> input_u((width + 1) * height), input_v(width * (height + 1));
        const auto load = [&](const char* path, bool mac) {
            std::ifstream file(path, std::ios::binary);
            std::array<int, 2> dimensions{};
            file.read(reinterpret_cast<char*>(dimensions.data()), sizeof(dimensions));
            Require(file.good() && dimensions == std::array<int, 2>{width, height}, "invalid gas snapshot dimensions");
            if (!mac) file.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
            for (auto* values : mac ? std::array{&input_u, &input_v} : std::array{&rhs, &pressure}) {
                file.read(reinterpret_cast<char*>(values->data()), static_cast<std::streamsize>(values->size() * sizeof(float)));
                Require(std::all_of(values->begin(), values->end(), [](float value) { return std::isfinite(value); }),
                        "gas snapshot contains a nonfinite field");
            }
            Require(file.good() && file.peek() == std::ifstream::traits_type::eof(), "invalid gas snapshot length");
        };
        load(slow ? "build/goal-baseline/gas-pressure-slow-system.bin" : "build/goal-baseline/gas-pressure-failing-system.bin", false);
        load(slow ? "build/goal-baseline/gas-pressure-slow-input-mac.bin" : "build/goal-baseline/gas-pressure-failing-input-mac.bin", true);
        Require(std::all_of(mask.begin(), mask.end(), [](unsigned char value) { return value <= 1; }),
                "invalid gas snapshot mask");
        Require(std::any_of(mask.begin(), mask.end(), [](unsigned char value) { return value == 0; }),
                "gas snapshot must contain fluid cells");
        const auto upload = [](GLuint texture, int w, int h, GLenum format, GLenum type, const void* values) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, values);
        };
        const auto read = [](GLuint texture, std::vector<float>& values) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
        };
        const auto blocked = [&](int x, int y) {
            return x < 0 || y < 0 || x >= width || y >= height || mask[y * width + x] != 0;
        };
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static int dispatches = 0;
        native_dispatch = glad_glDispatchCompute;
        struct Restore {
            GLuint queries[2]{};
            Restore() { glGenQueries(2, queries); }
            ~Restore() { glad_glDispatchCompute = native_dispatch; glDeleteQueries(2, queries); }
        } restore;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) { ++dispatches; native_dispatch(x, y, z); };
        std::vector<float> output_u(input_u.size()), output_v(input_v.size()), actual_rhs(cells);
        bool valid = true;
        for (int sample = 0; sample < 3; ++sample) {
            app.InitializeState();
            upload(app.boundary_state_.mask_gas, width, height, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            upload(app.CurrentGasU(), width + 1, height, GL_RED, GL_FLOAT, input_u.data());
            upload(app.CurrentGasV(), width, height + 1, GL_RED, GL_FLOAT, input_v.data());
            dispatches = 0;
            glQueryCounter(restore.queries[0], GL_TIMESTAMP);
            app.RunGasProjection();
            glQueryCounter(restore.queries[1], GL_TIMESTAMP);
            GLuint64 begin = 0, end = 0;
            glGetQueryObjectui64v(restore.queries[0], GL_QUERY_RESULT, &begin);
            glGetQueryObjectui64v(restore.queries[1], GL_QUERY_RESULT, &end);
            read(app.gas_state_.pressure_a, pressure);
            read(app.gas_state_.divergence, actual_rhs);
            Require(actual_rhs == rhs, "captured MAC field must reproduce the saved pressure RHS exactly");
            read(app.CurrentGasU(), output_u);
            read(app.CurrentGasV(), output_v);
            double square_sum = 0, projected_square_sum = 0, maximum = 0, projected_maximum = 0;
            GLuint failures = 0, fluid = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                if (blocked(x, y)) continue;
                const int cell = y * width + x;
                float sum = 0, diagonal = 0;
                for (const auto& offset : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    if (blocked(x + offset.first, y + offset.second)) continue;
                    sum += pressure[(y + offset.second) * width + x + offset.first];
                    diagonal += 1;
                }
                const float residual = rhs[cell] - (sum - diagonal * pressure[cell]);
                const double projected = static_cast<double>(output_u[y * (width + 1) + x + 1]) - output_u[y * (width + 1) + x]
                    + output_v[(y + 1) * width + x] - output_v[y * width + x];
                Require(std::isfinite(residual) && std::isfinite(projected), "gas precision trial produced nonfinite output");
                maximum = std::max(maximum, static_cast<double>(std::abs(residual)));
                projected_maximum = std::max(projected_maximum, std::abs(projected));
                square_sum += static_cast<double>(residual) * residual;
                projected_square_sum += projected * projected;
                ++fluid;
                if (std::abs(residual) > app.config_.gas_pressure_residual_threshold) ++failures;
            }
            const auto counters = app.ReadDebugCounters();
            valid = valid && fluid > 0 && failures == 0 && counters == std::array<GLuint, 5>{} &&
                    projected_maximum <= app.config_.gas_pressure_residual_threshold &&
                    std::sqrt(projected_square_sum / fluid) < 0.004;
            std::cout << "gas captured sample=" << sample << " GPU ms=" << (end - begin) / 1e6
                      << " dispatches=" << dispatches << " residual max/RMS=" << maximum << '/' << std::sqrt(square_sum / fluid)
                      << " projected max/RMS=" << projected_maximum << '/' << std::sqrt(projected_square_sum / fluid)
                      << " failing cells/diagnostic=" << failures << '/' << counters[2] << '\n';
        }
        const auto solved_pressure = pressure;
        for (bool zero_pressure : {true, false}) {
            if (zero_pressure) std::fill(pressure.begin(), pressure.end(), 0.0F);
            else pressure = solved_pressure;
            upload(app.gas_state_.pressure_a, width, height, GL_RED, GL_FLOAT, pressure.data());
            for (float threshold : {0.004F, std::numeric_limits<float>::max()}) {
                const GLuint program = app.pressure_residual_program_;
                glUseProgram(program);
                glUniform2i(glGetUniformLocation(program, "gridSize"), width, height);
                glUniform1i(glGetUniformLocation(program, "boundaryScale"), 1);
                glUniform1i(glGetUniformLocation(program, "useLiquidMask"), 0);
                glUniform1i(glGetUniformLocation(program, "collectStats"), 0);
                glUniform1f(glGetUniformLocation(program, "residualThreshold"), threshold);
                glUniform1i(glGetUniformLocation(program, "pressureTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, app.gas_state_.pressure_a);
                glUniform1i(glGetUniformLocation(program, "wideRhs"), 1);
                glBindImageTexture(4, app.gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
                glBindImageTexture(2, app.boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
                const GLuint zero = 0;
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, app.gas_state_.pressure_check);
                glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                glQueryCounter(restore.queries[0], GL_TIMESTAMP);
                constexpr int repeats = 64;
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    app.DispatchGrid(width, height);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                }
                glQueryCounter(restore.queries[1], GL_TIMESTAMP);
                GLuint64 begin = 0, end = 0;
                glGetQueryObjectui64v(restore.queries[0], GL_QUERY_RESULT, &begin);
                glGetQueryObjectui64v(restore.queries[1], GL_QUERY_RESULT, &end);
                std::array<GLuint, 4> counters{};
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
                glUniform1i(glGetUniformLocation(program, "wideRhs"), 0);  // the water solve's right-hand side is R16F
                Require(counters[0] == 0, "residual timing probe must remain finite");
                std::cout << "gas residual-only " << (zero_pressure ? "initial" : "solved")
                          << " threshold=" << (threshold == 0.004F ? "strict" : "no-fail")
                          << " GPU ms/call=" << (end - begin) / (1e6 * repeats)
                          << " failing cells/call=" << counters[2] / repeats << '\n';
            }
        }
        app.config_ = original_config;
        Require(valid, "captured gas system must meet existing residual and projected-divergence gates");
    }

    // The multigrid-preconditioned solve on a hard system: a 998x300 pool with a sand block, a painted pillar, an air
    // bubble, a strong inflow patch and drops in the air above, under a pseudo-random right-hand side. It must pass the
    // independent true-residual check in few iterations (the 16x16 block-Jacobi CG this replaced needed hundreds; its
    // count on this scene is in VALIDATION), and the V-cycle must be a symmetric positive definite map, which CG needs.
    static void WaterMultigrid(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
        constexpr std::size_t cells = static_cast<std::size_t>(width) * height;
        const auto original_config = app.config_;
        app.config_ = {};
        const float threshold = app.config_.water_liquid_phi_threshold;
        std::vector<unsigned char> mask(cells, 1);
        std::vector<float> phi(cells, 1.0F), rhs(cells, 0.0F), zero(cells, 0.0F);
        std::uint32_t seed = 12345U;
        const auto random = [&seed]() {
            seed = seed * 1664525U + 1013904223U;
            return static_cast<float>(seed >> 8) / static_cast<float>(1U << 24) * 2.0F - 1.0F;
        };
        for (int y = 1; y <= 600; ++y)
            for (int x = 1; x < width - 1; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * width + x);
                mask[i] = x >= 300 && x < 340 && y >= 50 && y < 90 ? 2 : x >= 600 && x < 610 && y < 250 ? 1 : 0;
                const bool bubble = (x - 200) * (x - 200) + (y - 150) * (y - 150) < 400;
                const bool drop = (y >= 400 && y < 403 || y >= 450 && y < 453) && x % 50 < 3;
                if (mask[i] == 0 && ((y <= 300 && !bubble) || drop)) {
                    phi[i] = 0.0F;
                    rhs[i] = random() + (std::abs(x - 500) < 8 && y > 280 && y <= 296 ? -8.0F : 0.0F);
                }
            }
        app.InitializeState();
        const auto upload = [&](GLuint texture, GLenum format, GLenum type, const void* data) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
        };
        upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
        upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
        upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero.data());
        // The stored right-hand side is half precision.
        glBindTexture(GL_TEXTURE_2D, app.CurrentWaterDivergence());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, rhs.data());
        app.water_state_.pressure_iterations = 0;
        app.RunWaterProjection();
        std::vector<float> pressure(cells);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, pressure.data());
        const auto blocked = [&](int x, int y) {
            return x < 0 || y < 0 || x >= width || y >= height || mask[static_cast<std::size_t>(y * width + x)] != 0;
        };
        double maximum = 0.0;
        std::size_t liquid = 0, failures = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto i = static_cast<std::size_t>(y * width + x);
                if (blocked(x, y) || phi[i] >= threshold) continue;
                double sum = 0.0, diagonal = 0.0;
                for (const auto& d : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (blocked(nx, ny)) {
                        if (!(nx < 0 || ny < 0 || nx >= width || ny >= height) && mask[ny * width + nx] >= 2)
                            diagonal += PowderApp::kSandLeak;
                        continue;
                    }
                    if (phi[ny * width + nx] < threshold) {
                        ++diagonal;
                        sum += pressure[ny * width + nx];
                    } else {
                        diagonal += AirConductance(app.config_, phi[i], phi[ny * width + nx]);
                    }
                }
                const double residual = rhs[i] - (sum - diagonal * pressure[i]);
                maximum = std::max(maximum, std::abs(residual));
                failures += std::abs(residual) > app.config_.water_pressure_residual_threshold ? 1U : 0U;
                ++liquid;
            }
        const int iterations = app.water_state_.pressure_iterations;
        std::cout << "water multigrid hard system: liquid cells=" << liquid << " iterations=" << iterations
                  << " dispatched=" << app.water_state_.pressure_dispatched << " true residual max=" << maximum
                  << " failures=" << failures << '\n';
        Require(app.ReadDebugCounters() == std::array<GLuint, 5>{} && failures == 0,
                "the hard system must pass the true-residual check without failure counters");
        Require(iterations > 1 && iterations <= 16, "multigrid PCG must solve the hard system in at most 16 iterations");

        // <M a, b> = <a, M b> and <M a, a> > 0 for pseudo-random a and b over the solve's cells. The solve above left
        // the hierarchy of this mask; restore the running flag and tile dispatch that its stop cleared, then apply
        // one V-cycle to each vector placed as the CG residual.
        std::vector<std::array<float, 4>> vectors(cells);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_vectors);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(cells * sizeof(vectors[0])), vectors.data());
        std::array<GLuint, 8> tiles{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_tiles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(tiles), tiles.data());
        const GLuint listed = tiles[4];
        Require(listed > 0, "the hard system must list tiles");
        const std::array<GLuint, 4> dispatch{listed, 1U, 1U, 0U};
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(dispatch), dispatch.data());
        const GLuint running = 1U;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(float), sizeof(running), &running);
        const auto apply = [&](const std::vector<float>& input) {
            for (std::size_t i = 0; i < cells; ++i) vectors[i][0] = input[i];
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_vectors);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(cells * sizeof(vectors[0])), vectors.data());
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            app.BindWaterSolver();
            app.SetupWaterMultigrid();
            app.ApplyWaterMultigrid(1, false);
            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            std::vector<std::array<float, 4>> result(cells);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_vectors);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(cells * sizeof(result[0])), result.data());
            std::vector<float> output(cells);
            for (std::size_t i = 0; i < cells; ++i) output[i] = result[i][2];
            return output;
        };
        std::vector<float> a(cells, 0.0F), b(cells, 0.0F);
        std::size_t rows = 0;
        for (std::size_t i = 0; i < cells; ++i)
            if (vectors[i][3] > 0.0F) {
                a[i] = random();
                b[i] = random();
                ++rows;
            }
        const std::vector<float> ma = apply(a), mb = apply(b);
        double mab = 0.0, amb = 0.0, maa = 0.0, mbb = 0.0;
        for (std::size_t i = 0; i < cells; ++i) {
            if (vectors[i][3] <= 0.0F) {
                Require(ma[i] == 0.0F && mb[i] == 0.0F, "the V-cycle must leave cells outside the solve at zero");
                continue;
            }
            mab += double(ma[i]) * b[i];
            amb += double(a[i]) * mb[i];
            maa += double(ma[i]) * a[i];
            mbb += double(mb[i]) * b[i];
        }
        const double asymmetry = std::abs(mab - amb) / std::sqrt(maa * mbb);
        std::cout << "water multigrid preconditioner: rows=" << rows << " <Ma,b>=" << mab << " <a,Mb>=" << amb
                  << " relative asymmetry=" << asymmetry << " <Ma,a>=" << maa << " <Mb,b>=" << mbb << '\n';
        Require(maa > 0.0 && mbb > 0.0, "the V-cycle must be positive on the solve's cells");
        Require(asymmetry < 1e-4, "the V-cycle must be symmetric");
        app.config_ = original_config;
        app.InitializeState();
        CheckGl("water multigrid");
    }

    static void WaterPressure(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth;
        constexpr int height = PowderApp::kGridHeight;
        constexpr int liquid_width = 64;
        constexpr int liquid_depth = 32;
        constexpr int liquid_cells = 64 * liquid_depth;
        const auto original_config = app.config_;
        app.config_.water_ghost_theta_min = 0.0F;  // the manufactured pool pins p = 0 at the air cell centre
        const int default_iterations = app.config_.water_pressure_iterations;
        const float tolerance = app.config_.water_pressure_residual_threshold;
        std::vector<int> checkpoints{1, default_iterations, 128};
        std::sort(checkpoints.begin(), checkpoints.end());
        checkpoints.erase(std::unique(checkpoints.begin(), checkpoints.end()), checkpoints.end());
        std::vector<unsigned char> mask(width * height, 1);
        std::vector<float> phi(width * height, 1.0F);
        std::vector<float> rhs(width * height, 0.0F);
        std::vector<float> exact(width * height, 0.0F);
        // A downward face velocity of -4 meets a solid floor. The top air row anchors p=0.
        // The exact pressure is 4*(33-y), so the RHS and solution are representable in R16F.
        for (int y = 1; y <= liquid_depth + 1; ++y) {
            for (int x = 1; x <= liquid_width; ++x) {
                const int cell = y * width + x;
                mask[cell] = 0;
                if (y <= liquid_depth) {
                    phi[cell] = 0.0F;
                    rhs[cell] = y == 1 ? -4.0F : 0.0F;
                    exact[cell] = 4.0F * (liquid_depth + 1 - y);
                }
            }
        }
        app.InitializeState();
        const auto upload = [&](GLuint texture, GLenum format, GLenum type, const void* data) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
        };
        upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
        upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());

        struct Residual {
            double rms = 0.0;
            double maximum = 0.0;
            double mean = 0.0;
            double pressure_maximum = 0.0;
            int above_threshold = 0;
        };
        const auto measure = [&](const std::vector<float>& pressure) {
            Residual result;
            int measured_cells = 0;
            for (float value : pressure) Require(std::isfinite(value), "pool pressure must remain finite");
            for (int y = 1; y <= liquid_depth; ++y) {
                for (int x = 1; x <= liquid_width; ++x) {
                    const int cell = y * width + x;
                    if (mask[cell] != 0 || phi[cell] >= app.config_.water_liquid_phi_threshold) continue;
                    ++measured_cells;
                    double laplacian = 0.0;
                    for (int neighbor : {cell - 1, cell + 1, cell - width, cell + width}) {
                        if (mask[neighbor] == 0)
                            laplacian += (phi[neighbor] < 0.42F ? pressure[neighbor] : 0.0F) - pressure[cell];
                    }
                    const double residual = rhs[cell] - laplacian;
                    result.rms += residual * residual;
                    result.maximum = std::max(result.maximum, std::abs(residual));
                    result.mean += residual;
                    result.pressure_maximum = std::max(result.pressure_maximum, std::abs(double(pressure[cell])));
                    result.above_threshold += std::abs(residual) > tolerance ? 1 : 0;
                }
            }
            result.rms = std::sqrt(result.rms / std::max(1, measured_cells));
            result.mean /= std::max(1, measured_cells);
            return result;
        };
        Require(measure(exact).maximum == 0.0, "manufactured pool pressure must solve the stored RHS exactly");
        const auto initial = measure(std::vector<float>(width * height, 0.0F));
        std::cout << "water pressure pool: 64x32, exact max p=128, initial residual RMS=" << initial.rms
                  << " max=" << initial.maximum << ", threshold=" << tolerance << '\n';
        Residual default_result;
        // The solve runs over listed 16x16 tiles. Every liquid cell of the current mask must lie in a listed tile,
        // each listed tile must hold one, and the host must dispatch its blind batch (the previous solve's iterations),
        // then confirm at a fence; a fence that finds the solve running is followed by 1, 1, 2, then 4 more iterations
        // before the next, until a fence finds the solve stopped, or the cap.
        struct Solve { GLuint iterations = 0; GLuint tiles = 0; };
        const auto counted_projection = [&]() {
            const int previous = app.water_state_.pressure_iterations;
            const int cap = app.config_.water_pressure_iterations;
            app.RunWaterProjection();
            std::array<GLuint, 8> head{};
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_tiles);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(head), head.data());
            std::vector<GLuint> listed(head[4]);
            if (!listed.empty())
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(head), static_cast<GLsizeiptr>(listed.size() * sizeof(GLuint)),
                                   listed.data());
            std::array<GLuint, 8> solver{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(solver), solver.data());
            const Solve result{solver[6], head[4]};
            const int iterations = static_cast<int>(result.iterations);
            int expected = std::min(cap, previous);
            for (int ahead = 0; expected < cap;) {
                const int fence = expected;
                expected += std::min(ahead, cap - expected);
                if (iterations <= fence || expected >= cap) break;
                if (ahead == 0) expected += std::min(1, cap - expected);
                ahead = std::min(4, std::max(1, ahead * 2));
            }
            Require(app.water_state_.pressure_dispatched == expected && app.water_state_.pressure_iterations == iterations,
                    "host must dispatch the blind batch, then stop at the first confirmation after GPU convergence or at the cap");
            if (result.tiles == 0) return result;  // converged at the start: nothing listed, nothing iterated
            constexpr int tiles_across = (width + 15) / 16;
            std::vector<char> is_listed(static_cast<std::size_t>(tiles_across * ((height + 15) / 16)), 0);
            for (GLuint tile : listed) {
                Require(tile < is_listed.size() && is_listed[tile] == 0, "a tile must be listed once");
                is_listed[tile] = 1;
            }
            std::vector<char> holds_liquid(is_listed.size(), 0);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto cell = static_cast<std::size_t>(y * width + x);
                if (mask[cell] != 0 || phi[cell] >= app.config_.water_liquid_phi_threshold) continue;
                const auto tile = static_cast<std::size_t>((y / 16) * tiles_across + x / 16);
                Require(is_listed[tile] != 0, "every liquid cell must lie in a listed tile");
                holds_liquid[tile] = 1;
            }
            Require(holds_liquid == is_listed, "every listed tile must hold a liquid cell");
            return result;
        };
        for (int cap : checkpoints) {
            const GLuint zero = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            app.config_.water_pressure_iterations = cap;
            const std::vector<float> zero_pressure(width * height, 0.0F);
            upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero_pressure.data());
            const Solve solve = counted_projection();
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
            std::vector<float> pressure(width * height);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, pressure.data());
            const auto residual = measure(pressure);
            const auto counters = app.ReadDebugCounters();
            std::array<GLuint, 8> solver{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(solver), solver.data());
            std::cout << "water pressure PCG cap=" << cap << " iterations=" << solver[6]
                      << " dispatched=" << app.water_state_.pressure_dispatched
                      << " tiles=" << solve.tiles
                      << ": RMS=" << residual.rms << " max=" << residual.maximum << " mean=" << residual.mean
                      << " count=" << residual.above_threshold << '/' << liquid_cells
                      << " max|p|=" << residual.pressure_maximum << " breakdown=" << solver[5] << '\n';
            Require(counters[0] == 0 && counters[2] == static_cast<GLuint>(residual.above_threshold),
                    "production residual counter must match the independent pool residual at every cap");
            // The 64x32 pool at x 1..64, y 1..32 spans 5x3 tiles, not the whole domain.
            Require(solve.tiles == 15U, "pressure iterations must cover the pool's tiles, not the whole domain");
            if (cap == 1) Require(residual.above_threshold > 0, "one CG iteration must report insufficient pool convergence");
            if (cap == default_iterations) {
                default_result = residual;
            }
        }
        const Solve unchanged = counted_projection();
        Require(unchanged.tiles == 0U && unchanged.iterations == 0U,
                "an unchanged converged system must reuse pressure without iteration work");
        // Change RHS sign, remove liquid, and insert a wall: the initial residual must use the new operator.
        for (int y = 1; y <= liquid_depth; ++y) for (int x = 1; x <= liquid_width; ++x) {
            const int cell = y * width + x;
            rhs[cell] = -rhs[cell];
            if (y >= 16) phi[cell] = 1.0F;
            if (x == 32) mask[cell] = 1;
        }
        upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
        upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
        const Solve changed = counted_projection();
        Require(changed.iterations > 0U, "changed RHS and mask must trigger a new pressure solve");
        // Liquid now fills y 1..15 only: one tile row of 5 tiles.
        Require(changed.tiles == 5U, "pressure iteration tiles must shrink with the current liquid mask");
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
        std::vector<float> changed_pressure(width * height);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, changed_pressure.data());
        Require(measure(changed_pressure).above_threshold == 0, "warm start must solve the changed RHS and mask");
        for (std::size_t i = 0; i < changed_pressure.size(); ++i)
            if (mask[i] != 0 || phi[i] >= app.config_.water_liquid_phi_threshold)
                Require(changed_pressure[i] == 0.0F, "new air and solid cells must discard previous pressure");
        for (bool full_extent : {false, true}) {
            std::fill(mask.begin(), mask.end(), 1);
            std::fill(phi.begin(), phi.end(), 1.0F);
            std::fill(rhs.begin(), rhs.end(), 0.0F);
            const std::array<int, 2> cells = full_extent ? std::array<int, 2>{0, width * height - 1}
                                                       : std::array<int, 2>{30 * width + 10, 40 * width + 20};
            for (int cell : cells) {
                mask[cell] = mask[cell == width * height - 1 ? cell - 1 : cell + 1] = 0;
                phi[cell] = 0.0F;
                rhs[cell] = 1.0F;
            }
            upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
            upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
            Require(counted_projection().tiles == 2U, "tiles must include both disconnected liquid regions and domain extrema");
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, changed_pressure.data());
            for (int cell : cells)
                Require(std::abs(changed_pressure[cell] + 1.0F) < 0.001F,
                        "each disconnected liquid region must receive its own pressure correction");
        }
        std::fill(phi.begin(), phi.end(), 1.0F);
        std::fill(rhs.begin(), rhs.end(), 0.0F);
        upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
        upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
        const Solve empty = counted_projection();
        Require(empty.tiles == 0U && empty.iterations == 0U, "an empty solve must initialize and validate without iteration work");
        app.config_ = original_config;
        CheckGl("production water pressure pool");
        Require(default_result.above_threshold == 0,
                "default water pressure cap " + std::to_string(default_iterations) + " leaves " +
                    std::to_string(default_result.above_threshold) + " pool cells above the existing residual threshold; max=" +
                    std::to_string(default_result.maximum));
    }

    // The host's schedule of a water solve (the blind batch, the fences and the speculative iterations after them) must
    // not change its result: stops, restarts from the true residual and extra iterations are decided on the GPU and a
    // restart queued while the solve still runs does nothing, so every schedule ends on the same pressure bits and
    // iteration count. A solve whose blind batch covers it waits at one fence and reads nothing back otherwise.
    static void WaterSolveSchedule(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth;
        constexpr int height = PowderApp::kGridHeight;
        // single: the restart from the true residual passes at once, so a solve covered by its blind batch needs one fence.
        struct System { const char* name; int liquid_width, depth; float source, threshold; bool single; };
        // A deep tank's pressure (up to 60,000 here) is where float rounding makes the recursive residual drift; the
        // tighter thresholds make the restart fail and CG iterate on.
        const std::array<System, 7> systems{{{"pool 64x32", 64, 32, -4.0F, 0.12F, true},
                                             {"tank 200x300", 200, 300, -200.0F, 0.12F, true},
                                             {"tank 200x300 threshold 0.07", 200, 300, -200.0F, 0.07F, true},
                                             {"tank 200x300 threshold 0.05", 200, 300, -200.0F, 0.05F, false},
                                             {"tank 200x300 threshold 0.045", 200, 300, -200.0F, 0.045F, false},
                                             {"tank 200x300 threshold 0.04", 200, 300, -200.0F, 0.04F, false},
                                             {"tank 48x800", 48, 800, -50.0F, 0.12F, true}}};
        static PFNGLCLIENTWAITSYNCPROC native_wait = glad_glClientWaitSync;
        static PFNGLGETBUFFERSUBDATAPROC native_get = glad_glGetBufferSubData;
        static int waits = 0, reads = 0;
        glad_glClientWaitSync = [](GLsync sync, GLbitfield flags, GLuint64 timeout) {
            ++waits;
            return native_wait(sync, flags, timeout);
        };
        glad_glGetBufferSubData = [](GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
            ++reads;
            native_get(target, offset, size, data);
        };
        struct Restore {
            ~Restore() {
                glad_glClientWaitSync = native_wait;
                glad_glGetBufferSubData = native_get;
            }
        } restore;
        const auto original_config = app.config_;
        for (const System& system : systems) {
            std::vector<unsigned char> mask(width * height, 1);
            std::vector<float> phi(width * height, 1.0F);
            std::vector<float> rhs(width * height, 0.0F);
            for (int y = 1; y <= system.depth + 1; ++y)
                for (int x = 1; x <= system.liquid_width; ++x) {
                    const int cell = y * width + x;
                    mask[cell] = 0;
                    if (y <= system.depth) {
                        phi[cell] = 0.0F;
                        rhs[cell] = y == 1 ? system.source : 0.0F;
                    }
                }
            app.InitializeState();
            app.config_ = original_config;
            app.config_.water_pressure_residual_threshold = system.threshold;
            const auto upload = [&](GLuint texture, GLenum format, GLenum type, const void* data) {
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
            };
            upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
            upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
            struct Result {
                std::vector<float> pressure;
                std::array<GLuint, 8> solver{};
                GLuint failing = 0;
                int dispatched = 0, waits = 0, reads = 0;
            };
            const std::vector<float> zero_pressure(width * height, 0.0F);
            const auto solve = [&](int blind_batch) {
                const GLuint zero = 0;
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
                glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero_pressure.data());
                app.water_state_.pressure_iterations = blind_batch;
                waits = reads = 0;
                app.RunWaterProjection();
                Result result;
                result.waits = waits;
                result.reads = reads;
                result.dispatched = app.water_state_.pressure_dispatched;
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
                result.pressure.resize(width * height);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, result.pressure.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(result.solver), result.solver.data());
                result.failing = app.ReadDebugCounters()[2];
                return result;
            };
            const Result reference = solve(0);
            const int needed = static_cast<int>(reference.solver[6]);
            std::cout << "water solve schedule " << system.name << ": iterations=" << needed << " restart at "
                      << reference.solver[7] << " failing cells=" << reference.failing << " breakdown=" << reference.solver[5]
                      << " waits from a cold start=" << reference.waits << '\n';
            Require(reference.solver[5] == 0U, std::string(system.name) + ": the solve must not break down");
            if (system.threshold == 0.12F)
                Require(reference.failing == 0U, std::string(system.name) + ": the default threshold must pass the true residual");
            for (int batch : {1, 2, needed - 1, needed, needed + 1, needed + 4, 3 * needed + 7}) {
                if (batch < 0) continue;
                const Result result = solve(batch);
                Require(result.pressure == reference.pressure && result.solver[6] == reference.solver[6] &&
                            result.solver[7] == reference.solver[7] && result.failing == reference.failing,
                        std::string(system.name) + ": a blind batch of " + std::to_string(batch) +
                            " must end on the pressure, iteration count and restart of any other schedule");
                if (batch < needed || needed == 0) continue;
                Require(result.reads == 0, std::string(system.name) + ": a covered solve must read nothing back");
                if (system.single)
                    Require(result.waits == 1, std::string(system.name) + ": a solve covered by its blind batch must wait at one fence, waited " +
                                                   std::to_string(result.waits));
                else
                    Require(result.waits > 1, std::string(system.name) + ": a failing restart must make CG iterate on and wait again");
            }
        }
        app.config_ = original_config;
        CheckGl("water solve schedule");
    }

    // Ghost-fluid free surface: in a hydrostatic column whose top cell reads phi 0.21 under an air cell at 0.63 (the
    // threshold 0.42 half way between them, theta 0.5), the liquid-air face conducts 2, so the pressure is zero at the
    // surface and the top cell holds 4 / 2 = 2: p(y) = 4 / conductance + 4 (32 - y). Without the condition (theta_min 0)
    // the air cell centre holds the zero and the top cell reads 4. The production residual check, which forms its own
    // operator, must pass on both, and the pressure must match the manufactured column.
    static void WaterGhostFluid(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
        constexpr int liquid_width = 8, liquid_depth = 32;
        const auto original_config = app.config_;
        for (const float theta_min : {0.25F, 0.0F}) {
            app.config_ = {};
            app.config_.water_ghost_theta_min = theta_min;
            std::vector<unsigned char> mask(width * height, 1);
            std::vector<float> phi(width * height, 1.0F), rhs(width * height, 0.0F), zero(width * height, 0.0F);
            for (int y = 1; y <= liquid_depth + 1; ++y)
                for (int x = 1; x <= liquid_width; ++x) {
                    const int cell = y * width + x;
                    mask[cell] = 0;
                    phi[cell] = y == liquid_depth + 1 ? 0.63F : y == liquid_depth ? 0.21F : 0.0F;
                    rhs[cell] = y == 1 ? -4.0F : 0.0F;
                }
            app.InitializeState();
            const auto upload = [&](GLuint texture, const void* data, GLenum format, GLenum type) {
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
            };
            upload(app.boundary_state_.mask_full, mask.data(), GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            upload(app.water_state_.liquid_phi, phi.data(), GL_RED, GL_FLOAT);
            upload(app.CurrentWaterDivergence(), rhs.data(), GL_RED, GL_FLOAT);
            upload(app.CurrentWaterPressure(), zero.data(), GL_RED, GL_FLOAT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
            const GLuint cleared = 0;
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &cleared);
            app.RunWaterProjection();
            std::vector<float> pressure(width * height);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentWaterPressure());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, pressure.data());
            // theta from the half-float phi values, as the shaders read them.
            const float conductance = theta_min > 0.0F ? 1.0F / 0.5F : 1.0F;
            double worst = 0.0;
            for (int y = 1; y <= liquid_depth; ++y)
                worst = std::max(worst, static_cast<double>(std::abs(pressure[static_cast<std::size_t>(y * width + 3)] -
                                                                     (4.0F / conductance + 4.0F * (liquid_depth - y)))));
            const auto counters = app.ReadDebugCounters();
            std::cout << "water ghost fluid: theta_min " << theta_min << " top cell p=" << pressure[liquid_depth * width + 3]
                      << " floor cell p=" << pressure[width + 3] << " worst error " << worst << " counters " << counters[0] << '/'
                      << counters[2] << '\n';
            Require(worst < 0.5, "the pressure must match the manufactured hydrostatic column");
            Require(counters[0] == 0U && counters[2] == 0U, "the production residual check must pass on the ghost-fluid operator");
        }
        app.config_ = original_config;
        CheckGl("ghost-fluid column");
    }

    static void GasProjection(PowderApp& app) {
        constexpr int width = PowderApp::kGasWidth;
        constexpr int height = PowderApp::kGasHeight;
        const std::array<int, 3> cycles{1, 4, 24};
        const auto original_config = app.config_;
        std::ostringstream failures;
        const auto expect = [&](bool condition, const std::string& message) {
            if (!condition) failures << " [" << message << ']';
        };
        // These fixtures are closed chambers.
        app.config_.gas_open_top = false;
        app.config_.gas_pressure_residual_threshold = 0.015F;
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static GLuint relaxation_program = 0;
        static int relaxation_dispatches = 0;
        const auto counted_projection = [&]() {
            native_dispatch = glad_glDispatchCompute;
            relaxation_program = app.gas_pressure_relax_program_;
            relaxation_dispatches = 0;
            struct Restore {
                ~Restore() { glad_glDispatchCompute = native_dispatch; }
            } restore;
            glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
                GLint program = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                if (static_cast<GLuint>(program) == relaxation_program) ++relaxation_dispatches;
                native_dispatch(x, y, z);
            };
            app.RunGasProjection();
            Require(relaxation_dispatches % kGasRelaxPerCycle == 0, "gas solve must execute complete multigrid cycles");
            return relaxation_dispatches / kGasRelaxPerCycle;
        };
        const auto read = [](GLuint texture, std::size_t count) {
            std::vector<float> values(count);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
            return values;
        };
        const auto divergence = [&](const std::vector<float>& u, const std::vector<float>& v, int x, int y) {
            return static_cast<double>(u[y * (width + 1) + x + 1]) - u[y * (width + 1) + x] +
                   v[(y + 1) * width + x] - v[y * width + x];
        };
        struct ResidualStats {
            double rms = 0.0;
            double maximum = 0.0;
            double mean = 0.0;
            int count = 0;
        };
        app.InitializeState();
        const int empty_cycles = counted_projection();
        expect(empty_cycles == 0, "zero-RHS gas must skip multigrid cycles");
        expect(app.ReadDebugCounters() == std::array<GLuint, 5>{}, "zero-RHS gas must retain clean diagnostics");
        for (GLuint texture : {app.CurrentGasU(), app.CurrentGasV()}) {
            const auto values = read(texture, static_cast<std::size_t>((width + 1) * height));
            expect(std::all_of(values.begin(), values.end(), [](float value) { return value == 0.0F; }),
                   "zero-RHS gas projection must retain zero MAC faces");
        }
        std::cout << "gas empty executed cycles=" << empty_cycles << '\n';
        // A broad low-amplitude mode meets the maximum limit before solving, but not the RMS limit.
        std::vector<float> rms_u((width + 1) * height, 0.0F), rms_v(width * (height + 1), 0.0F);
        for (int y = 0; y < height; ++y)
            for (int x = 1; x < width; ++x) rms_u[y * (width + 1) + x] = (x + y) % 2 == 0 ? 0.002F : -0.002F;
        for (int y = 1; y < height; ++y)
            for (int x = 0; x < width; ++x) rms_v[y * width + x] = (x + y) % 2 == 0 ? 0.002F : -0.002F;
        glBindTexture(GL_TEXTURE_2D, app.CurrentGasU());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width + 1, height, GL_RED, GL_FLOAT, rms_u.data());
        glBindTexture(GL_TEXTURE_2D, app.CurrentGasV());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height + 1, GL_RED, GL_FLOAT, rms_v.data());
        const int rms_cycles = counted_projection();
        expect(rms_cycles > 0, "small maximum with excessive RMS must still solve");
        expect(app.ReadDebugCounters() == std::array<GLuint, 5>{}, "RMS-limited solve must retain clean diagnostics");
        std::cout << "gas RMS-only control executed cycles=" << rms_cycles << '\n';
        for (const std::string scene : {"wave", "neck", "strip"}) {
            const bool neck = scene == "neck";
            const bool strip = scene == "strip";
            std::vector<unsigned char> mask(width * height, neck || strip ? 1 : 0);
            if (strip) {
                // Five fluid rows lose their last row on both coarse levels.
                // A Neumann correction there invents a component gauge absent from the fine correction space.
                for (int y = 0; y < 5; ++y)
                    for (int x = 52; x < 84; ++x) mask[y * width + x] = 0;
            } else if (neck) {
                for (int y = 100; y < 132; ++y) {
                    for (int x = 100; x < 165; ++x) {
                        if (x != 132) mask[y * width + x] = 0;
                    }
                }
                // This sole connection is fluid on the fine grid and blocked by coarse footprints.
                mask[117 * width + 132] = 0;
            } else {
                for (int y = 180; y < 320; ++y) {
                    for (int x = 210; x < 290; ++x) mask[y * width + x] = 1;
                }
                for (int y = 100; y < 400; ++y) mask[y * width + 100] = 1;
            }
            const auto blocked = [&](int x, int y) {
                return x < 0 || y < 0 || x >= width || y >= height || mask[y * width + x] != 0;
            };
            const auto pressure = [&](int x, int y) {
                if (strip) return static_cast<double>(y);
                if (neck) return x < 132 ? 1.0 : x > 132 ? -1.0 : 0.0;
                return std::cos(6.283185307179586 * (x + 0.5) / 32.0) +
                       0.5 * std::cos(6.283185307179586 * (y + 0.5) / 48.0);
            };
            std::vector<float> initial_u((width + 1) * height, 0.0F);
            std::vector<float> initial_v(width * (height + 1), 0.0F);
            for (int y = 0; y < height; ++y) {
                for (int x = 1; x < width; ++x) {
                    if (!blocked(x - 1, y) && !blocked(x, y))
                        initial_u[y * (width + 1) + x] = static_cast<float>(pressure(x, y) - pressure(x - 1, y));
                }
            }
            for (int y = 1; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    if (!blocked(x, y - 1) && !blocked(x, y))
                        initial_v[y * width + x] = static_cast<float>(pressure(x, y) - pressure(x, y - 1));
                }
            }
            const auto measure = [&](const std::vector<float>& u, const std::vector<float>& v) {
                // Whole domain, wall-adjacent cells, left chamber, right chamber.
                std::array<ResidualStats, 4> stats{};
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        if (blocked(x, y)) continue;
                        const double value = divergence(u, v, x, y);
                        Require(std::isfinite(value), "gas projection produced nonfinite divergence");
                        const bool wall = blocked(x - 1, y) || blocked(x + 1, y) ||
                                          blocked(x, y - 1) || blocked(x, y + 1);
                        const std::array<bool, 4> included{true, wall, neck && x < 132, neck && x > 132};
                        for (std::size_t group = 0; group < stats.size(); ++group) {
                            if (!included[group]) continue;
                            stats[group].rms += value * value;
                            stats[group].maximum = std::max(stats[group].maximum, std::abs(value));
                            stats[group].mean += value;
                            ++stats[group].count;
                        }
                    }
                }
                for (auto& stat : stats) {
                    if (stat.count == 0) continue;
                    stat.rms = std::sqrt(stat.rms / stat.count);
                    stat.mean /= stat.count;
                }
                return stats;
            };
            std::array<std::array<ResidualStats, 4>, 3> errors{};
            for (std::size_t i = 0; i < cycles.size(); ++i) {
                app.InitializeState();
                app.config_.gas_pressure_iterations = cycles[i];
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                glBindTexture(GL_TEXTURE_2D, app.CurrentGasU());
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width + 1, height, GL_RED, GL_FLOAT, initial_u.data());
                glBindTexture(GL_TEXTURE_2D, app.CurrentGasV());
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height + 1, GL_RED, GL_FLOAT, initial_v.data());
                const auto uploaded_u = read(app.CurrentGasU(), initial_u.size());
                const auto uploaded_v = read(app.CurrentGasV(), initial_v.size());
                const auto before = measure(uploaded_u, uploaded_v);
                const int executed_cycles = counted_projection();
                if (cycles[i] == 1) expect(executed_cycles == cycles[i], scene + ": insufficient budget must execute every cycle");
                const auto u = read(app.CurrentGasU(), initial_u.size());
                const auto v = read(app.CurrentGasV(), initial_v.size());
                errors[i] = measure(u, v);

                // Closed-domain flux is compatible before RHS rounding to R16F, which may change its mean.
                const auto rhs = read(app.gas_state_.divergence, static_cast<std::size_t>(width * height));
                const auto stored_pressure = read(app.gas_state_.pressure_a, rhs.size());
                GLuint nonconverged = 0;
                float maximum_stored_residual = 0.0F;
                double stored_residual_square_sum = 0.0;
                double rhs_sum = 0.0;
                double rounding_sum = 0.0;
                double rounding_square_sum = 0.0;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        if (blocked(x, y)) continue;
                        const double value = rhs[y * width + x];
                        Require(std::isfinite(value), "gas projection stored nonfinite RHS");
                        const double rounding = value - divergence(uploaded_u, uploaded_v, x, y);
                        rhs_sum += value;
                        rounding_sum += rounding;
                        rounding_square_sum += rounding * rounding;
                        float pressure_sum = 0.0F, diagonal = 0.0F;
                        for (const auto& offset : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                            if (blocked(x + offset.first, y + offset.second)) continue;
                            pressure_sum += stored_pressure[(y + offset.second) * width + x + offset.first];
                            diagonal += 1.0F;
                        }
                        const float residual = rhs[y * width + x] -
                            (pressure_sum - diagonal * stored_pressure[y * width + x]);
                        maximum_stored_residual = std::max(maximum_stored_residual, std::abs(residual));
                        stored_residual_square_sum += static_cast<double>(residual) * residual;
                        if (std::abs(residual) > app.config_.gas_pressure_residual_threshold) ++nonconverged;
                    }
                }
                const std::string context = scene + " " + std::to_string(cycles[i]) + " cycles";
                expect(executed_cycles <= cycles[i] && (executed_cycles == cycles[i] ||
                       (maximum_stored_residual <= std::min(0.01F, app.config_.gas_pressure_residual_threshold) &&
                        stored_residual_square_sum <= before[0].count * 0.001 * 0.001)),
                       context + ": early exit requires independent maximum and active-cell RMS residual limits");
                const auto counters = app.ReadDebugCounters();
                // One cell may sit within float rounding of the threshold (GPU FMA vs CPU arithmetic).
                expect(counters[0] == 0 && std::abs(static_cast<int>(counters[2]) - static_cast<int>(nonconverged)) <= 1,
                       context + ": final residual counter must match independent stored-field operator (GPU " +
                           std::to_string(counters[2]) + ", CPU " + std::to_string(nonconverged) + ")");
                if (cycles[i] == 1) expect(nonconverged > 0, context + ": insufficient budget must report failure");
                if (cycles[i] == 24) expect(nonconverged == 0, context + ": full budget must meet final residual limit");
                std::cout << "gas projection " << context << ": RMS " << before[0].rms << " -> " << errors[i][0].rms
                          << ", max " << before[0].maximum << " -> " << errors[i][0].maximum
                          << ", wall RMS " << before[1].rms << " -> " << errors[i][1].rms
                          << ", wall max " << before[1].maximum << " -> " << errors[i][1].maximum
                          << ", RHS mean " << rhs_sum / before[0].count
                          << ", R16F mean delta " << rounding_sum / before[0].count
                          << ", R16F RMS delta " << std::sqrt(rounding_square_sum / before[0].count)
                          << ", executed cycles=" << executed_cycles << ", residual failures=" << nonconverged << '\n';
                expect(errors[i][0].rms < before[0].rms, context + ": projection must reduce divergence");
                expect(errors[i][0].maximum < before[0].maximum, context + ": maximum divergence must decrease");
                if (!neck) {
                    expect(errors[i][1].rms < before[1].rms, context + ": wall-adjacent RMS must decrease");
                    expect(errors[i][1].maximum < before[1].maximum, context + ": wall-adjacent maximum must decrease");
                }
                if (neck) {
                    Require(before[2].count == 1024 && before[3].count == 1024 && before[0].count == 2049,
                            "neck fixture must contain two 32x32 chambers and one connecting cell");
                    for (std::size_t group : {2U, 3U}) {
                        const std::string chamber = group == 2 ? "left" : "right";
                        std::cout << "  " << chamber << " chamber: RMS " << before[group].rms << " -> " << errors[i][group].rms
                                  << ", max " << before[group].maximum << " -> " << errors[i][group].maximum
                                  << ", mean " << before[group].mean << " -> " << errors[i][group].mean << '\n';
                        expect(errors[i][group].rms < before[group].rms, context + ": " + chamber + " chamber RMS must decrease");
                        expect(errors[i][group].maximum < before[group].maximum,
                               context + ": " + chamber + " chamber maximum must decrease");
                    }
                }
                bool wall_flux_valid = true;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x <= width; ++x) {
                        if (blocked(x - 1, y) || blocked(x, y))
                            wall_flux_valid = wall_flux_valid && u[y * (width + 1) + x] == 0.0F;
                    }
                }
                for (int y = 0; y <= height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        if (blocked(x, y - 1) || blocked(x, y))
                            wall_flux_valid = wall_flux_valid && v[y * width + x] == 0.0F;
                    }
                }
                expect(wall_flux_valid, context + ": gas projection must retain zero normal velocity at walls");
            }
            // A solve that meets the stop limits within four cycles exits there; past that the R16F RHS sets the floor.
            expect(errors[1][0].rms < errors[0][0].rms && errors[2][0].rms <= errors[1][0].rms * 1.1,
                   scene + ": additional multigrid cycles must reduce this manufactured error");
            expect(errors[2][0].rms < 0.004, scene + ": manufactured pressure must converge to RMS divergence below 0.004");
            if (neck) {
                for (std::size_t group : {2U, 3U}) {
                    const std::string chamber = group == 2 ? "left" : "right";
                    expect(errors[1][group].rms < errors[0][group].rms && errors[2][group].rms < errors[1][group].rms,
                           chamber + " chamber: additional cycles must reduce RMS divergence");
                    expect(errors[2][group].rms < 0.004, chamber + " chamber: final RMS divergence must be below 0.004");
                }
            }
        }
        app.InitializeState();
        const float invalid_velocity = std::numeric_limits<float>::quiet_NaN();
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentGasU());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 10, 10, 1, 1, GL_RED, GL_FLOAT, &invalid_velocity);
        const int invalid_cycles = counted_projection();
        const auto invalid_counters = app.ReadDebugCounters();
        expect(invalid_cycles == 0 && invalid_counters[0] > 0 && invalid_counters[2] > 0,
               "invalid initial gas residual must skip iteration and remain visible in final diagnostics");
        const auto projected = read(app.CurrentGasU(), static_cast<std::size_t>((width + 1) * height));
        expect(std::all_of(projected.begin(), projected.end(), [](float value) { return std::isfinite(value); }),
               "invalid gas input must not escape through the projected MAC field");
        std::cout << "gas invalid executed cycles=" << invalid_cycles << " nan/failures="
                  << invalid_counters[0] << '/' << invalid_counters[2] << '\n';
        app.config_ = original_config;
        Require(failures.str().empty(), failures.str());
    }


    static void ValidateReplayMetrics(PowderApp& app) {
        app.InitializeState();
        std::ostringstream failures;
        const auto expect = [&](bool condition, const char* message) {
            if (!condition) failures << " [" << message << ']';
        };
        const auto reset_metrics = [&](const char* scene) {
            app.ResetReplayMetrics();
            app.replay_scene_name_ = scene;
            app.replay_determinism_ok_ = true;
            for (auto& timing : app.replay_timing_aggregates_) timing.samples = 1;
            app.replay_frame_timing_.samples = 1;
        };
        PowderApp::ReplaySignature reference{};
        reference.water_live_particles = reference.sand_live_particles = 100000;
        reference.water_mass = reference.sand_mass = 100000.0;
        expect(app.ReplaySignaturesClose(reference, reference), "identical replay signatures must pass");
        auto changed = reference;
        --changed.water_live_particles;
        expect(!app.ReplaySignaturesClose(reference, changed), "replay must reject one missing water particle");
        changed = reference;
        --changed.sand_live_particles;
        expect(!app.ReplaySignaturesClose(reference, changed), "replay must reject one missing sand particle");
        changed = reference;
        changed.water_mass -= 1.0;
        expect(!app.ReplaySignaturesClose(reference, changed), "replay must reject water mass drift with unchanged count");
        changed = reference;
        changed.sand_mass -= 1.0;
        expect(!app.ReplaySignaturesClose(reference, changed), "replay must reject sand mass drift with unchanged count");
        reset_metrics("combined");
        expect(!app.ReplayMetricsOk(), "empty combined scene must fail even with fast timings");
        app.replay_current_signature_ = reference;
        app.replay_current_signature_.smoke_sum = 1.0;
        app.replay_validation_.gas_ignited = true;
        expect(app.ReplayMetricsOk(), "populated combined scene must pass with valid frame timings");
        app.replay_frame_timing_.sum_ms = app.config_.combined_target_frame_ms + 1.0;
        expect(!app.ReplayMetricsOk(), "combined frame mean must include work outside individual timed passes");
        app.replay_frame_timing_.sum_ms = 0.0;
        app.replay_frame_timing_.max_ms = app.config_.combined_worst_frame_ms + 1.0F;
        expect(!app.ReplayMetricsOk(), "combined whole-frame maximum must enforce the worst-frame budget");

        const PowderApp::ReplayScene boundary_scene{"boundary", 1, {{0, PowderApp::BrushMode::Water, 500, 920, 10, 1}}};
        WriteScalar(app.CurrentWaterAmount(), 1.0F, 500, 920);
        WriteParticles(app, app.water_state_.particles, std::vector<Particle>{{500.5F, 920.5F, 0.0F, 0.0F, 1.0F, 1.0F}});
        reset_metrics("boundary");
        app.UpdateReplayMetrics(boundary_scene, 0);
        expect(!app.ReplayMetricsOk(), "water that never reaches an obstacle must fail the boundary scene");
        WriteInteger(app.boundary_state_.mask_static, 1, 500, 500);
        for (const float y : {501.5F, 500.5F, 499.5F}) {
            WriteParticles(app, app.water_state_.particles, std::vector<Particle>{{500.5F, y, 0.0F, 0.0F, 1.0F, 1.0F}});
            reset_metrics("boundary");
            app.UpdateReplayMetrics(boundary_scene, 0);
            expect(app.ReplayMetricsOk() == (y == 501.5F), "boundary must accept contact and reject particles inside or across the barrier");
        }
        app.InitializeState();
        std::vector<Particle> particles{{500.5F, 500.5F, 0.0F, 0.0F, 1.0F, 2.0F},
                                        {501.5F, 500.5F, 0.0F, 0.0F, 0.0F, 99.0F},
                                        {502.5F, 500.5F, 0.0F, 0.0F, 1.0F, 3.0F}};
        for (const auto& [buffer, cursor] : {std::pair{app.water_state_.particles, app.water_state_.spawn_cursor},
                                           std::pair{app.sand_state_.particles, app.sand_state_.spawn_cursor}}) {
            if (buffer == app.sand_state_.particles) WriteParticles(app, buffer, SandRecords(particles));
            else WriteParticles(app, buffer, particles);
            const GLuint allocated = 3;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, cursor);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(allocated), &allocated);
        }
        expect(app.ReadWaterLiveParticles() == 2, "water count must exclude inactive allocated particles");
        expect(app.ReadSandLiveParticles() == 2, "sand count must exclude inactive allocated particles");
        reset_metrics("sand");
        const PowderApp::ReplayScene sand_scene{"sand", 220, {}};
        app.UpdateReplayMetrics(sand_scene, 139);
        expect(app.replay_validation_.sand_reference_mass == 5.0, "sand reference must sum stored live mass");
        particles[0][5] = 1.0F;
        WriteParticles(app, app.sand_state_.particles, SandRecords(particles));
        app.UpdateReplayMetrics(sand_scene, 219);
        expect(app.replay_validation_.sand_final_mass == 4.0, "sand final must detect mass loss with unchanged count");
        expect(!app.ReplayMetricsOk(), "unchanged allocation cursor must not hide mass loss");

        reset_metrics("gas");
        const PowderApp::ReplayScene gas_scene{"gas", 240, {}};
        app.UpdateReplayMetrics(gas_scene, 180);
        expect(!app.ReplayMetricsOk(), "gas cannot pass extinguishing without first igniting");
        const auto write_reaction = [&](float value) {
            const std::array<float, 2> reaction{0.0F, value};
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentReactionRate());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RG, GL_FLOAT, reaction.data());
        };
        reset_metrics("gas");
        write_reaction(4.0F);
        app.UpdateReplayMetrics(gas_scene, 100);
        write_reaction(0.0F);
        app.UpdateReplayMetrics(gas_scene, 180);
        expect(app.ReplayMetricsOk(), "gas ignition followed by extinction must pass its metric");

        app.InitializeState();
        const std::array<GLuint, 5> prior_tick_counters{0, 7, 0, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(prior_tick_counters), prior_tick_counters.data());
        app.RunFrame(app.config_.fixed_dt, nullptr, false);
        expect(app.ReadDebugCounters()[1] == 7, "later catchup ticks must preserve earlier debug failures");
        app.RunFrame(app.config_.fixed_dt, nullptr, true);
        expect(app.ReadDebugCounters() == std::array<GLuint, 5>{}, "first tick must start a fresh counter interval");

        int rejected = 0;
        for (int component : {0, 4, 5}) {
            auto invalid = particles;
            invalid[0][component] = std::nanf("");
            WriteParticles(app, app.water_state_.particles, invalid);
            try { app.ReadWaterLiveParticles(); } catch (const std::runtime_error&) { ++rejected; }
        }
        for (int component : {10, 12, 15}) {
            auto invalid = SandRecords(particles);
            invalid[0][component] = std::nanf("");
            WriteParticles(app, app.sand_state_.particles, invalid);
            try { app.ReadSandLiveParticles(); } catch (const std::runtime_error&) { ++rejected; }
        }
        expect(rejected == 6, "particle readback must reject invalid position, active flag, mass, and sand material state");
        std::cout << "particle metric invalid records rejected=" << rejected << "/6\n";
        Require(failures.str().empty(), "replay validation is not measuring actual behavior:" + failures.str());
    }

    static void RenderModes(PowderApp& app) {
        app.InitializeState();
        app.ResetReplayMetrics();
        // The oracle below is the plain albedo; lighting has its own test.
        struct Unlit {
            PowderApp& app;
            bool previous;
            ~Unlit() { app.lighting_enabled_ = previous; }
        } unlit{app, app.lighting_enabled_};
        app.lighting_enabled_ = false;
        const auto fill = [](GLuint texture, std::array<float, 2> value, bool pair = false) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            GLint width = 0, height = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            const int components = pair ? 2 : 1;
            std::vector<float> pixels(static_cast<std::size_t>(width * height * components));
            for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = value[i % components];
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, pair ? GL_RG : GL_RED, GL_FLOAT, pixels.data());
        };
        // A single corner impulse must clamp symmetrically at both domain edges.
        for (int corner : {0, PowderApp::kGasWidth - 1}) {
            fill(app.CurrentGasVelocityCenter(), {0.0F, 0.0F}, true);
            const std::array<float, 2> velocity{1.0F, 0.0F};
            glBindTexture(GL_TEXTURE_2D, app.CurrentGasVelocityCenter());
            glTexSubImage2D(GL_TEXTURE_2D, 0, corner, corner, 1, 1, GL_RG, GL_FLOAT, velocity.data());
            app.RunRenderFieldExtractionPass();
            const int full_corner = corner == 0 ? 0 : PowderApp::kGridWidth - 1;
            const float actual = ReadScalar(app.render_gas_velocity_, full_corner, full_corner);
            Require(actual == 1.0F, "render extraction attenuates a clamped corner impulse: " + std::to_string(actual));
        }
        app.InitializeState();
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        int width = 0, height = 0;
        glfwGetFramebufferSize(app.window_, &width, &height);
        struct Target {
            GLuint framebuffer = 0, color = 0;
            ~Target() {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteTextures(1, &color);
            }
        } target;
        glGenTextures(1, &target.color);
        glBindTexture(GL_TEXTURE_2D, target.color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "render test framebuffer incomplete");
        const auto check_mode = [&](PowderApp::RenderMode mode, std::array<float, 3> expected) {
            app.render_mode_ = mode;
            app.Render();
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                const int channel = static_cast<int>(i % 4);
                const int target_value = channel == 3 ? 255 : static_cast<int>(std::lround(expected[channel] * 255.0F));
                Require(std::abs(static_cast<int>(pixels[i]) - target_value) <= 1,
                        "render mode " + std::to_string(static_cast<int>(mode)) + " pixel mismatch");
            }
            CheckGl("render mode pixels");
        };
        // CPU mirror of composite.frag's albedo for scenes without water, smoke or fire, whose terms all vanish, drawn with
        // lighting off (the light is a separate pass with its own test). The hash and dither are integer-exact; the rest
        // is float math, so the +-1 byte tolerance only absorbs GPU/CPU rounding differences.
        const auto mix = [](float a, float b, float t) { return a * (1.0F - t) + b * t; };
        const auto smoothstep = [](float edge0, float edge1, float value) {
            const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
            return t * t * (3.0F - 2.0F * t);
        };
        const auto hash = [](std::uint32_t x, std::uint32_t y) {  // both PCG2D components as floats in [0, 1)
            x = x * 1664525U + 1013904223U;
            y = y * 1664525U + 1013904223U;
            x += y * 1664525U;
            y += x * 1664525U;
            x ^= x >> 16;
            y ^= y >> 16;
            x += y * 1664525U;
            y += x * 1664525U;
            x ^= x >> 16;
            y ^= y >> 16;
            return std::array<float, 2>{static_cast<float>(x >> 8) * (1.0F / 16777216.0F),
                                        static_cast<float>(y >> 8) * (1.0F / 16777216.0F)};
        };
        // Hash of the block a cell sits in: `shift` is the block size in bits, `salt` the offset added to both lanes.
        const auto block_hash = [&](int cell_x, int cell_y, int shift, std::uint32_t salt) {
            return hash(static_cast<std::uint32_t>(cell_x >> shift) + salt, static_cast<std::uint32_t>(cell_y >> shift) + salt);
        };
        using Rgb = std::array<float, 3>;
        const auto stone_tone = [&](int cell_x, int cell_y) {
            return 0.30F * block_hash(cell_x, cell_y, 0, 0U)[0] + 0.25F * block_hash(cell_x, cell_y, 1, 7U)[0] +
                   0.25F * block_hash(cell_x, cell_y, 3, 101U)[0] + 0.20F * block_hash(cell_x, cell_y, 4, 202U)[1];
        };
        const auto cave_albedo = [&](int cell_x, int cell_y) {
            constexpr Rgb cave{0.125F, 0.118F, 0.150F};
            const float tone = 0.40F * block_hash(cell_x, cell_y, 2, 11U)[0] + 0.35F * block_hash(cell_x, cell_y, 3, 101U)[0] +
                               0.25F * block_hash(cell_x, cell_y, 4, 202U)[1];
            return Rgb{cave[0] * (0.70F + 0.60F * tone), cave[1] * (0.70F + 0.60F * tone), cave[2] * (0.70F + 0.60F * tone)};
        };
        // Open cells next to solids: x is the drop shadow from a solid up to four cells up-left, y a solid 4-neighbour.
        const auto solid_shade = [&](int cell_x, int cell_y, const auto& solid_at) {
            const auto solid = [&](int x, int y) {
                return solid_at(std::clamp(x, 0, PowderApp::kGridWidth - 1), std::clamp(y, 0, PowderApp::kGridHeight - 1));
            };
            const float touch = solid(cell_x + 1, cell_y) || solid(cell_x - 1, cell_y) || solid(cell_x, cell_y + 1) || solid(cell_x, cell_y - 1) ? 1.0F : 0.0F;
            float shadow = touch;
            for (int k = 1; k <= 4; ++k)
                if (solid(cell_x - k, cell_y + k)) {
                    shadow = std::max(shadow, static_cast<float>(5 - k) / 4.0F);
                    break;
                }
            return std::array<float, 2>{shadow, touch};
        };
        const auto rock_albedo = [&](int cell_x, int cell_y, const auto& solid_at) {
            constexpr Rgb dark{0.30F, 0.32F, 0.39F}, mid{0.46F, 0.49F, 0.57F}, light{0.62F, 0.66F, 0.75F}, moss_color{0.36F, 0.40F, 0.18F};
            constexpr Rgb crust_top{0.88F, 0.91F, 0.96F}, crust_side{0.66F, 0.70F, 0.79F};
            const auto fine = block_hash(cell_x, cell_y, 0, 0U);
            const float tone = stone_tone(cell_x, cell_y);
            const float moss = smoothstep(0.70F, 0.78F, 0.6F * block_hash(cell_x, cell_y, 3, 303U)[0] + 0.4F * block_hash(cell_x, cell_y, 1, 404U)[1]);
            // 1 next to open space, falling to 0 over `reach` cells; the domain edge reads as the nearest cell, like the shader's clamp.
            const auto rim = [&](int dx, int dy, int reach) {
                for (int k = 1; k <= reach; ++k) {
                    const int x = std::clamp(cell_x + dx * k, 0, PowderApp::kGridWidth - 1), y = std::clamp(cell_y + dy * k, 0, PowderApp::kGridHeight - 1);
                    if (!solid_at(x, y)) return static_cast<float>(reach + 1 - k) / static_cast<float>(reach);
                }
                return 0.0F;
            };
            const float rim_top = rim(0, 1, 6), rim_bottom = rim(0, -1, 3), rim_side = std::max(rim(-1, 0, 3), rim(1, 0, 3));
            const float jitter = fine[1] * 0.95F, crust_tone = 0.82F + 0.36F * fine[0];
            const float top = rim_top > jitter ? 1.0F : 0.0F, side = rim_side > jitter ? 1.0F : 0.0F, bottom = rim_bottom > jitter ? 1.0F : 0.0F;
            Rgb color{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                float value = mix(mix(dark[channel], mid[channel], smoothstep(0.30F, 0.50F, tone)), light[channel], smoothstep(0.60F, 0.80F, tone));
                value = mix(value, moss_color[channel], 0.7F * moss);
                value *= mix(0.80F, 1.0F, std::max(std::max(rim_top, rim_bottom), rim_side));
                value = mix(value, mix(crust_top[channel], moss_color[channel] * 1.7F, 0.5F * moss) * crust_tone, 0.95F * top);
                value = mix(value, crust_side[channel] * crust_tone, 0.55F * side * (1.0F - top));
                color[channel] = value * (1.0F - 0.35F * bottom * (1.0F - top));
            }
            return color;
        };
        // Wet sand in the third and fifth 200-column bands (R8: 153 is 0.6), dry elsewhere.
        const auto moisture_at = [](int x) { return x / 200 == 2 || x / 200 == 4 ? std::uint8_t{153} : std::uint8_t{0}; };
        const auto set_scene = [&](const auto& solid_at, const auto& raw_sand_at) {
            std::vector<GLuint> cells(PowderApp::kGridWidth * PowderApp::kGridHeight);
            std::vector<float> raw(cells.size());
            std::vector<std::uint8_t> moisture(cells.size());
            for (int y = 0; y < PowderApp::kGridHeight; ++y) for (int x = 0; x < PowderApp::kGridWidth; ++x) {
                const auto index = static_cast<std::size_t>(y * PowderApp::kGridWidth + x);
                cells[index] = solid_at(x, y) ? 2U : 0U;
                raw[index] = raw_sand_at(x);
                moisture[index] = moisture_at(x);
            }
            glBindTexture(GL_TEXTURE_2D, app.sand_state_.moisture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                            GL_RED, GL_UNSIGNED_BYTE, moisture.data());
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                            GL_RED_INTEGER, GL_UNSIGNED_INT, cells.data());
            glBindTexture(GL_TEXTURE_2D, app.sand_state_.occupancy);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                            GL_RED, GL_FLOAT, raw.data());
        };
        struct Coverage {
            int bevel = 0, partial_sand = 0, wet_sand = 0;
            std::array<int, 4> classes{};  // empty, solid, sand over empty, sand over solid
            std::uint64_t hash = 14695981039346656037ULL;
        };
        // Renders a scene given as a solid mask and a per-column constant sand occupancy, then compares every pixel
        // with the oracle except where skip_at(grid_x) marks filter taps that cross an occupancy band edge.
        const auto check_composite = [&](const auto& solid_at, const auto& raw_sand_at, const auto& skip_at) {
            set_scene(solid_at, raw_sand_at);
            app.render_mode_ = PowderApp::RenderMode::Composite;
            app.Render();
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            Coverage coverage;
            for (unsigned char value : pixels) coverage.hash = (coverage.hash ^ value) * 1099511628211ULL;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const float grid_x = (x + 0.5F) / width * PowderApp::kGridWidth;
                const float v = (y + 0.5F) / height;
                const int sx = static_cast<int>(grid_x);
                const int sy = static_cast<int>(v * PowderApp::kGridHeight);
                if (skip_at(grid_x)) continue;
                const bool solid = solid_at(sx, sy);
                Rgb color = solid ? rock_albedo(sx, sy, solid_at) : cave_albedo(sx, sy);
                const auto shade = solid ? std::array<float, 2>{} : solid_shade(sx, sy, solid_at);
                for (float& value : color) value *= 1.0F - 0.60F * shade[0];
                if (solid) {
                    const float top = solid_at(sx, std::min(sy + 1, PowderApp::kGridHeight - 1)) ? 0.0F : 1.0F;
                    const float bottom = solid_at(sx, std::max(sy - 1, 0)) ? 0.0F : 1.0F;
                    coverage.bevel += top + bottom > 0.0F;
                }
                // Occupancy is mass / rest density; the composite doubles it to keep the look tuned on half-density saturation.
                const float sand = std::clamp(2.0F * std::max(raw_sand_at(sx) * 1.02F, raw_sand_at(sx) * 0.78F), 0.0F, 1.0F);
                const float cover = smoothstep(0.04F, 0.11F, sand);
                if (cover > 0.0F) {
                    const float stress = std::clamp(std::max(solid ? 0.65F : 0.0F, sand * 0.68F), 0.0F, 1.0F);
                    const auto grain_hash = hash(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
                    const float grain = 1.0F + (grain_hash[0] - 0.5F) * 0.30F +
                                        (grain_hash[1] > 0.93F ? 0.16F : (grain_hash[1] < 0.08F ? -0.18F : 0.0F));
                    constexpr Rgb relaxed{0.82F, 0.64F, 0.38F}, compressed{0.66F, 0.48F, 0.27F};
                    const float wet = 1.0F - 0.45F * static_cast<float>(moisture_at(sx)) / 255.0F;
                    coverage.wet_sand += wet < 1.0F;
                    for (int channel = 0; channel < 3; ++channel)
                        color[channel] = mix(color[channel], mix(relaxed[channel], compressed[channel], stress) * grain * wet, cover);
                    coverage.partial_sand += cover < 1.0F;
                }
                for (float& value : color) value *= 1.0F - 0.30F * shade[1];
                ++coverage.classes[(cover > 0.0F ? 2U : 0U) + (solid ? 1U : 0U)];
                const float dither = (hash(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))[0] - 0.5F) / 255.0F;
                for (int channel = 0; channel < 4; ++channel) {
                    const int expected = channel == 3 ? 255 : static_cast<int>(std::lround(std::clamp(color[channel] + dither, 0.0F, 1.0F) * 255.0F));
                    const int actual = pixels[static_cast<std::size_t>((y * width + x) * 4 + channel)];
                    Require(std::abs(actual - expected) <= 1,
                            "composite pixel " + std::to_string(x) + "," + std::to_string(y) + " channel " + std::to_string(channel) +
                                " differs from the CPU oracle: expected " + std::to_string(expected) + " got " + std::to_string(actual));
                }
            }
            CheckGl("composite pixels");
            return coverage;
        };
        const auto no_skip = [](float) { return false; };
        const auto no_sand = [](int) { return 0.0F; };
        // Empty cells: the background gradient and dither alone. All solid: no exposed edges, so a flat plate.
        const auto empty = check_composite([](int, int) { return false; }, no_sand, no_skip);
        const auto plate = check_composite([](int, int) { return true; }, no_sand, no_skip);
        Require(empty.classes[0] == width * height && plate.classes[1] == width * height && plate.bevel == 0,
                "flat empty and all-solid scenes must hit only their own oracle class");
        // Sand spans empty and solid cells, including the solid-dependent tint, a level below the soft edge, a
        // partially covered level and fully covered ones. Wide constant bands make the filtered occupancy
        // analytically known away from their edges.
        constexpr std::array<float, 5> levels{0.0F, 0.015625F, 0.03515625F, 0.0625F, 1.0F};
        const auto mixed = check_composite([](int x, int y) { return (x / 125 + y / 125) % 2 != 0; },
                                           [&](int x) { return levels[static_cast<std::size_t>(x / 200)]; },
                                           // Filter taps crossing an occupancy band are covered by the hash, not this constant-band oracle.
                                           [](float grid_x) { return std::min(std::fmod(grid_x, 200.0F), 200.0F - std::fmod(grid_x, 200.0F)) < 2.0F; });
        Require(std::all_of(mixed.classes.begin(), mixed.classes.end(), [](int count) { return count > 0; }) && mixed.partial_sand > 0 &&
                    mixed.wet_sand > 0,
                "mixed composite must exercise empty, solid, sand over empty, sand over solid, a partly covered sand edge and wet sand");
        std::cout << "mixed composite RGBA hash=" << mixed.hash << " checked="
                  << mixed.classes[0] << '/' << mixed.classes[1] << '/' << mixed.classes[2] << '/' << mixed.classes[3] << '\n';
        // A solid band whose edge cells are rows the sampled pixels land on: lit top edge, shaded bottom edge.
        const auto grid_row = [&](int y) { return static_cast<int>((y + 0.5F) / height * PowderApp::kGridHeight); };
        const int band_low = grid_row(height / 8), band_high = grid_row(height / 8 + 1);
        Require(band_high > band_low + 1, "bevel scene needs the sampled rows to be at least two cells apart");
        const auto bevel = check_composite([&](int, int y) { return y >= band_low && y <= band_high; }, no_sand, no_skip);
        Require(bevel.bevel == 2 * width, "the bevel scene must light one sampled row and shade the other");
        // Sparse water must still read as a blue liquid (it used to vanish below about three particles per cell).
        set_scene([](int, int) { return false; }, no_sand);
        app.render_mode_ = PowderApp::RenderMode::Composite;
        for (const float amount : {0.03F, 0.075F, 0.22F}) {
            fill(app.CurrentWaterAmount(), {amount, 0.0F});
            app.Render();
            std::array<unsigned char, 4> pixel{};
            glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            Require(pixel[2] >= pixel[0] + 40 && pixel[2] >= 90,
                    "water amount " + std::to_string(amount) + " must render clearly blue: " + std::to_string(pixel[0]) + "," +
                        std::to_string(pixel[1]) + "," + std::to_string(pixel[2]));
        }
        fill(app.CurrentWaterAmount(), {0.0F, 0.0F});
        // Fire: hot cells glow orange, and their bloom warms cells a few grid cells away but nothing further.
        // The hot block ends 6.6 cells before the centre of the middle pixel column, which therefore sees only the bloom.
        {
            const int near_x = width / 2, far_x = width * 25 / 32;
            const int edge = static_cast<int>((near_x + 0.5) / width * PowderApp::kGridWidth - 6.6);
            const auto column = [&](int pixel_x) {
                std::array<int, 3> sum{};
                for (int y = 0; y < height; ++y) {
                    std::array<unsigned char, 4> pixel{};
                    glReadPixels(pixel_x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
                    for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] += pixel[channel];
                }
                for (int& value : sum) value /= height;
                return sum;
            };
            fill(app.render_temperature_, {0.0F, 0.0F});
            app.Render();
            const auto cold_near = column(near_x), cold_far = column(far_x);
            std::vector<float> heat(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight), 0.0F);
            for (int y = 0; y < PowderApp::kGridHeight; ++y)
                for (int x = 0; x < edge; ++x) heat[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] = 1.0F;
            glBindTexture(GL_TEXTURE_2D, app.render_temperature_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED, GL_FLOAT, heat.data());
            app.Render();
            const auto hot = column(0), near_hot = column(near_x), far_hot = column(far_x);
            Require(hot[0] > 200 && hot[0] > hot[2] + 100, "a hot cell must glow orange: " + std::to_string(hot[0]) + "," + std::to_string(hot[2]));
            Require(near_hot[0] >= cold_near[0] + 10 && near_hot[0] > cold_near[0] + 2 * (near_hot[2] - cold_near[2]),
                    "the fire bloom must warm cells next to hot ones: " + std::to_string(cold_near[0]) + " -> " + std::to_string(near_hot[0]));
            for (std::size_t channel = 0; channel < 3; ++channel)
                Require(std::abs(far_hot[channel] - cold_far[channel]) <= 1, "the fire bloom must stay local");
            fill(app.render_temperature_, {0.0F, 0.0F});
        }
        std::vector<GLuint> material(PowderApp::kGridWidth * PowderApp::kGridHeight);
        fill(app.sand_state_.occupancy, {0.0F, 0.0F});
        std::fill(material.begin(), material.end(), 1U);
        glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_full);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
        fill(app.CurrentWaterAmount(), {0.25F, 0.0F});
        fill(app.CurrentWaterVelocity(), {0.3F, 0.4F}, true);
        fill(app.CurrentGasVelocityCenter(), {0.3F, 0.4F}, true);
        fill(app.CurrentSmokeDen(), {0.25F, 0.0F});
        fill(app.CurrentTemperature(), {0.5F, 0.0F});
        fill(app.CurrentReactionRate(), {0.0F, 0.75F}, true);
        fill(app.CurrentFuel(), {0.25F, 0.0F});
        fill(app.CurrentGasPressure(), {0.25F, 0.0F});
        app.RunRenderFieldExtractionPass();
        for (const auto& [texture, expected] : std::array<std::pair<GLuint, float>, 4>{{
                 {app.render_smoke_density_, 0.225F}, {app.render_temperature_, 0.5F},
                 {app.render_fire_emissive_, 0.625F}, {app.render_gas_velocity_, 0.3F}}}) {
            for (int cell : {0, PowderApp::kGridWidth / 2, PowderApp::kGridWidth - 1}) {
                Require(std::abs(ReadScalar(texture, cell, cell) - expected) <= 0.00025F,
                        "render extraction must preserve a constant field including its edges");
            }
        }
        check_mode(PowderApp::RenderMode::Boundary, {1.0F, 1.0F, 1.0F});
        check_mode(PowderApp::RenderMode::Water, {0.225F, 0.225F, 0.225F});
        check_mode(PowderApp::RenderMode::WaterVelocity, {0.5F, 0.5F, 0.5F});
        check_mode(PowderApp::RenderMode::GasVelocity, {0.5F, 0.5F, 0.5F});
        check_mode(PowderApp::RenderMode::GasPressure, {0.25F, 0.25F, 0.25F});
        check_mode(PowderApp::RenderMode::Smoke, {0.225F, 0.225F, 0.225F});
        check_mode(PowderApp::RenderMode::Temperature, {0.5F, 0.5F, 0.5F});
        check_mode(PowderApp::RenderMode::FuelReaction, {0.25F, 0.75F, 0.5F});
        const std::array<GLuint, 5> counters{8, 16, 4, 4, 0};
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        check_mode(PowderApp::RenderMode::DebugCounters, {0.25F, 0.5F, 0.25F});
        // Water velocity clamps add to the blue channel with the solver and scheduling counters: (4 + 4 + 8) / 32.
        const std::array<GLuint, 5> clamped_counters{8, 16, 4, 4, 8};
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(clamped_counters), clamped_counters.data());
        check_mode(PowderApp::RenderMode::DebugCounters, {0.25F, 0.5F, 0.5F});

        // F10 also draws the pass timings: the interactive loop reads them a frame late, only in that view.
        app.InitializeState();
        const auto timed_frames = [&app](PowderApp::RenderMode mode, int frames) {
            app.ResetReplayMetrics();  // every bar starts at -1, "never read"
            app.render_mode_ = mode;
            FixedStepClock clock;
            for (int frame = 0; frame < frames; ++frame) {
                Require(app.RunInteractiveFrame(clock, app.config_.fixed_dt) == 1, "each frame must run one tick");
                if (app.pass_timings_ms_[0] >= 0.0F) break;  // a frame whose queries were not ready keeps the old values
            }
        };
        timed_frames(PowderApp::RenderMode::Composite, 4);
        for (const float ms : app.pass_timings_ms_) Require(ms == -1.0F, "pass timings must not be read outside the F10 view");
        timed_frames(PowderApp::RenderMode::DebugCounters, 30);
        float total_ms = 0.0F;
        for (const float ms : app.pass_timings_ms_) {
            Require(std::isfinite(ms) && ms >= 0.0F, "the F10 view must read every pass timing");
            total_ms += ms;
        }
        Require(total_ms > 0.0F, "the F10 pass timings must be non-zero after a few ticks");
        app.render_mode_ = PowderApp::RenderMode::Composite;
        app.InitializeState();
        std::cout << "render extraction corners/constants and all ten render modes passed\n";
    }

    // Lighting is presentation only: composite = albedo * (ambient + the fire's glow + the cursor lantern) + emissive.
    // These checks are qualitative (the albedo has its CPU oracle in render-modes): the lit world is dark but not black,
    // a cell near fire is brighter and warmer than before and than the same material far away, the lantern lights the
    // cursor's surroundings only in interactive (letterboxed) frames, and nothing in a frame depends on time.
    static void Lighting(PowderApp& app) {
        app.InitializeState();
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        struct Restore {
            PowderApp& app;
            int window_width, window_height;
            bool lighting, cursor;
            int x, y, radius;
            GLuint framebuffer = 0, color = 0;
            ~Restore() {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteTextures(1, &color);
                glfwSetWindowSize(app.window_, window_width, window_height);
                app.lighting_enabled_ = lighting;
                app.cursor_in_sim_ = cursor;
                app.brush_x_ = x;
                app.brush_y_ = y;
                app.brush_radius_ = radius;
                app.hud_motion_ = {};
                app.render_mode_ = PowderApp::RenderMode::Composite;
            }
        } restore{app, 0, 0, app.lighting_enabled_, app.cursor_in_sim_, app.brush_x_, app.brush_y_, app.brush_radius_};
        glfwGetWindowSize(app.window_, &restore.window_width, &restore.window_height);
        glfwSetWindowSize(app.window_, 1280, 900);
        glfwPollEvents();
        int width = 0, height = 0;
        glfwGetFramebufferSize(app.window_, &width, &height);
        Require(width == 1280 && height == 900, "lighting test needs a 1280x900 framebuffer");
        const PowderApp::SimRect sim = PowderApp::FitSimRect(width, height);
        glGenTextures(1, &restore.color);
        glBindTexture(GL_TEXTURE_2D, restore.color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glGenFramebuffers(1, &restore.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, restore.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, restore.color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "lighting test framebuffer incomplete");
        app.render_mode_ = PowderApp::RenderMode::Composite;
        app.brush_radius_ = 10;
        app.cursor_in_sim_ = false;

        using Image = std::vector<unsigned char>;
        const auto render = [&](bool lit, bool letterbox) {
            app.lighting_enabled_ = lit;
            app.Render(letterbox);
            Image pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            CheckGl("lighting capture");
            return pixels;
        };
        // Mean colour of a 21x21 pixel block around a grid cell of the letterboxed simulation.
        const auto mean_at = [&](const Image& image, int cell_x, int cell_y) {
            const int centre_x = sim.x + static_cast<int>((cell_x + 0.5) * sim.width / PowderApp::kGridWidth);
            const int centre_y = sim.y + static_cast<int>((cell_y + 0.5) * sim.height / PowderApp::kGridHeight);
            std::array<double, 3> sum{};
            for (int y = centre_y - 10; y <= centre_y + 10; ++y) for (int x = centre_x - 10; x <= centre_x + 10; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] += image[static_cast<std::size_t>((y * width + x) * 4) + channel];
            for (double& value : sum) value /= 441.0;
            return sum;
        };
        const auto luma = [](const std::array<double, 3>& c) { return 0.30 * c[0] + 0.59 * c[1] + 0.11 * c[2]; };
        const auto fill_cold = [&] {
            const std::vector<float> zeros(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight), 0.0F);
            for (const GLuint texture : {app.render_temperature_, app.render_fire_emissive_}) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED, GL_FLOAT, zeros.data());
            }
        };
        // All rock, no fire: a plain world to light.
        const std::vector<GLuint> rock(PowderApp::kGridWidth * PowderApp::kGridHeight, 2U);
        glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED_INTEGER, GL_UNSIGNED_INT, rock.data());
        fill_cold();

        // With no light the scene is dark, but not black: the moonlit ambient keeps the composition readable.
        const Image albedo = render(false, true);
        const Image dark = render(true, true);
        const auto albedo_centre = mean_at(albedo, 300, 700), dark_centre = mean_at(dark, 300, 700);
        Require(luma(dark_centre) > 7.0 && luma(dark_centre) < 0.7 * luma(albedo_centre) && luma(dark_centre) < 70.0,
                "an unlit scene must be dark but not black: " + std::to_string(luma(dark_centre)) + " against albedo " + std::to_string(luma(albedo_centre)));
        Require(dark_centre[2] > dark_centre[0], "the ambient must be cool moonlight");
        // The vignette: the corners of the window are darker than its centre.
        Require(luma(mean_at(dark, 40, 40)) < 0.8 * luma(mean_at(dark, 500, 500)), "the corners must fall off to black");

        // A hot, burning block on the left. Hot cells and flame cells light their surroundings, which then are brighter and
        // warmer than the same unlit rock and than rock far from the fire.
        std::vector<float> heat(rock.size(), 0.0F), flame(rock.size(), 0.0F);
        for (int y = 470; y < 530; ++y) for (int x = 170; x < 230; ++x) {
            heat[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] = 1.0F;
            flame[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] = 0.8F;
        }
        glBindTexture(GL_TEXTURE_2D, app.render_temperature_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED, GL_FLOAT, heat.data());
        glBindTexture(GL_TEXTURE_2D, app.render_fire_emissive_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED, GL_FLOAT, flame.data());
        const Image burning = render(true, true);
        // 40 cells from the block: the glow reaches about a mip-4 texel (64 cells) beyond the flame.
        const auto near_fire = mean_at(burning, 270, 500), near_dark = mean_at(dark, 270, 500);
        const auto far_fire = mean_at(burning, 900, 200), far_dark = mean_at(dark, 900, 200);
        const double near_gain = luma(near_fire) / luma(near_dark), far_gain = luma(far_fire) / luma(far_dark);
        Require(near_gain > 1.5 && near_gain > far_gain + 0.25,
                "rock near a fire must be brighter than before and than the same rock far away: gain " +
                    std::to_string(near_gain) + " near, " + std::to_string(far_gain) + " far");
        Require(near_fire[0] / near_fire[2] > 1.3 * (near_dark[0] / near_dark[2]), "the fire's light must be warm");
        // Light only reaches the surroundings when lighting is on: unlit, 100 cells from the block nothing changes.
        const Image unlit_burning = render(false, true);
        const auto unlit_near = mean_at(unlit_burning, 330, 500), albedo_near = mean_at(albedo, 330, 500);
        for (std::size_t channel = 0; channel < 3; ++channel)
            Require(std::abs(unlit_near[channel] - albedo_near[channel]) <= 1.0, "with lighting off the fire must not light its surroundings");
        // The emission texture is mipped: the coarse levels hold the block's light spread out.
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindTexture(GL_TEXTURE_2D, app.light_texture_);
        for (const auto& [level, minimum] : std::array<std::pair<int, float>, 3>{{{0, 1.0F}, {3, 0.2F}, {5, 0.02F}}}) {
            GLint level_width = 0, level_height = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &level_width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &level_height);
            std::vector<float> texels(static_cast<std::size_t>(level_width * level_height * 4));
            glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, texels.data());
            float brightest = 0.0F;
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                Require(std::isfinite(texels[i]) && texels[i] >= 0.0F && texels[i + 1] <= texels[i] + 1e-3F, "emitted light must be finite, non-negative and warm");
                brightest = std::max(brightest, texels[i]);
            }
            Require(brightest > minimum, "emission level " + std::to_string(level) + " must carry the fire's light: " + std::to_string(brightest));
        }

        // A frame depends on the state and the simulation clock only: rendered again it is identical. Lit fire moves with
        // the clock (tongues, sparks, heat shimmer); unlit frames, which the pixel oracles use, do not.
        Require(render(true, true) == burning, "a lit frame must be deterministic");
        {
            const Image unlit_now = render(false, true);
            const std::uint32_t tick = app.frame_index_;
            app.frame_index_ = tick + 7U;
            const Image later = render(true, true), unlit_later = render(false, true);
            app.frame_index_ = tick;
            std::size_t changed = 0;
            for (std::size_t i = 0; i < later.size(); ++i) changed += later[i] != burning[i];
            Require(changed > 2000, "lit fire must animate with the simulation clock: " + std::to_string(changed) + " bytes changed");
            Require(unlit_later == unlit_now, "unlit frames must not depend on the simulation clock");
        }

        // The lantern: interactive (letterboxed) frames with the cursor over the simulation only.
        fill_cold();
        app.brush_x_ = 700;
        app.brush_y_ = 300;
        app.cursor_in_sim_ = true;
        const Image lantern = render(true, true);
        const auto lit_near = mean_at(lantern, 700, 300), lit_mid = mean_at(lantern, 750, 300);
        const auto dark_near = mean_at(dark, 700, 300), dark_mid = mean_at(dark, 750, 300);
        Require(luma(lit_near) > 1.8 * luma(dark_near) && luma(lit_mid) > 1.5 * luma(dark_mid),
                "the lantern must brighten the world around the cursor");
        Require(lit_near[0] / lit_near[2] > dark_near[0] / dark_near[2], "the lantern must be warm");
        for (std::size_t channel = 0; channel < 3; ++channel)
            Require(std::abs(mean_at(lantern, 100, 900)[channel] - mean_at(dark, 100, 900)[channel]) <= 1.0,
                    "the lantern must not reach beyond its radius");
        // It is off in replay frames (not letterboxed), with lighting off, and when the cursor is away.
        const Image stretched_with = render(true, false);
        app.cursor_in_sim_ = false;
        Require(render(true, false) == stretched_with, "replay frames must not show the lantern");
        Require(render(true, true) == dark, "the lantern must go out with the cursor");
        app.cursor_in_sim_ = true;
        const Image unlit_with = render(false, true);
        app.cursor_in_sim_ = false;
        Require(render(false, true) == unlit_with, "lighting off must switch the lantern off too");
        // Painted rock is the foreground: a rock band hanging in open space is much brighter than the back wall
        // beside it under the same moonlight.
        {
            std::vector<GLuint> band(rock.size(), 0U);
            for (int y = 480; y < 520; ++y) for (int x = 200; x < 800; ++x) band[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] = 2U;
            glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight, GL_RED_INTEGER, GL_UNSIGNED_INT, band.data());
            const Image banded = render(true, true);
            const double rock_luma = luma(mean_at(banded, 500, 500)), wall_luma = luma(mean_at(banded, 500, 400));
            Require(rock_luma > 2.5 * wall_luma, "rock must stand out from the back wall: " + std::to_string(rock_luma) + " against " + std::to_string(wall_luma));
        }
        app.InitializeState();
        std::cout << "lighting: dark but not black, vignette, warm glow near fire against far and against unlit, mipped emission, lantern radius/off states and determinism pass\n";
    }

    static void ValidateReadbacks(PowderApp& app) {
        app.InitializeState();
        const GLuint scalar = app.CurrentWaterAmount();
        const GLuint reaction = app.CurrentReactionRate();
        const PowderApp::ReplayScene leakage_scene{"leakage", 1, {}};
        WriteScalar(scalar, 1.5F);
        WriteScalar(scalar, -3.0F, 1, 0);
        Require(app.ReadScalarTextureSum(scalar, PowderApp::kGridWidth, PowderApp::kGridHeight) == 1.5,
                "finite scalar sum must retain its nonnegative clamp");
        std::array<float, 2> components{0.25F, 0.75F};
        glBindTexture(GL_TEXTURE_2D, reaction);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RG, GL_FLOAT, components.data());
        Require(app.ReadReactionTextureSum(reaction, PowderApp::kGasWidth, PowderApp::kGasHeight) == 0.75,
                "finite reaction sum must use the second component");
        app.ResetReplayMetrics();
        app.UpdateReplayMetrics(leakage_scene, 0);
        Require(app.replay_validation_.leakage_total_water == 1.5 &&
                    app.replay_validation_.leakage_below_barrier == 1.5,
                "finite leakage sum must retain its nonnegative clamp");

        std::ostringstream failures;
        int rejected = 0;
        const auto expect_rejection = [&](const char* measurement, const std::string& label, const auto& read) {
            try {
                read();
                failures << " [accepted " << label << ']';
            } catch (const std::runtime_error& error) {
                Require(std::string(error.what()).find(measurement) != std::string::npos,
                        "invalid readback error must name its measurement: " + std::string(error.what()));
                ++rejected;
            }
        };
        for (float invalid : {std::nanf(""), INFINITY, -INFINITY}) {
            const std::string kind = std::isnan(invalid) ? "NaN" : invalid > 0.0F ? "+Inf" : "-Inf";
            WriteScalar(scalar, invalid);
            Require(!std::isfinite(ReadScalar(scalar)), "GPU fixture must retain the injected nonfinite scalar");
            expect_rejection("scalar texture sum", "scalar " + kind, [&] {
                app.ReadScalarTextureSum(scalar, PowderApp::kGridWidth, PowderApp::kGridHeight);
            });
            app.ResetReplayMetrics();
            expect_rejection("leakage water sum", "leakage " + kind, [&] {
                app.UpdateReplayMetrics(leakage_scene, 0);
            });
            for (int component = 0; component < 2; ++component) {
                components = {0.25F, 0.75F};
                components[component] = invalid;
                glBindTexture(GL_TEXTURE_2D, reaction);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RG, GL_FLOAT, components.data());
                expect_rejection("reaction texture sum", "reaction component " + std::to_string(component) + ' ' + kind, [&] {
                    app.ReadReactionTextureSum(reaction, PowderApp::kGasWidth, PowderApp::kGasHeight);
                });
            }
        }
        std::cout << "invalid readbacks rejected=" << rejected << "/12\n";
        Require(failures.str().empty(), "nonfinite validation measurements must fail:" + failures.str());
    }

    static void Benchmark(PowderApp& app, int combined_ticks) {
        using Clock = std::chrono::steady_clock;
        constexpr int warmup = 60;
        constexpr int samples = 240;
        app.config_ = powder_config::LoadConfigFromEnvironment();
        Require(combined_ticks == 1 || app.config_.fixed_dt == 1.0F / 60.0F,
                "benchmark-realtime requires the default POWDER_FIXED_DT=1/60 for its two-tick 30 FPS workload");
        const bool audit_pressure = std::getenv("POWDER_PRESSURE_AUDIT") != nullptr;
        const bool profile_gas = std::getenv("POWDER_GAS_PROFILE") != nullptr;
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static PFNGLUSEPROGRAMPROC native_use_program = nullptr;
        static GLuint tracked_program = 0;
        static std::function<void(GLuint, GLuint, GLuint)> audited_dispatch;
        static std::function<void(GLuint, GLuint, GLuint)> measured_dispatch;
        struct RestoreDispatch {
            ~RestoreDispatch() {
                if (native_dispatch != nullptr) glad_glDispatchCompute = native_dispatch;
                if (native_use_program != nullptr) glad_glUseProgram = native_use_program;
                audited_dispatch = {};
                measured_dispatch = {};
            }
        } restore_dispatch;
        native_dispatch = glad_glDispatchCompute;
        native_use_program = glad_glUseProgram;
        glad_glUseProgram = [](GLuint program) { tracked_program = program; native_use_program(program); };
        int audit_frame = 0, audit_water_step = 0;
        int gas_relaxations = 0, gas_checks = 0;
        bool gas_projection_active = false, captured_slow_gas = false;
        std::vector<int> gas_cycle_samples, gas_check_samples;
        std::array<std::uint64_t, 2> pressure_failures{};
        std::array<bool, 2> captured{};
        bool capture_projected_gas = false;
        const auto save_gas_mac = [&](const char* path, GLuint u, GLuint v) {
            const std::array<int, 2> dimensions{PowderApp::kGasWidth, PowderApp::kGasHeight};
            std::vector<float> faces(static_cast<std::size_t>((dimensions[0] + 1) * dimensions[1]));
            GLint binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(dimensions.data()), sizeof(dimensions));
            for (GLuint texture : {u, v}) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, faces.data());
                file.write(reinterpret_cast<const char*>(faces.data()), static_cast<std::streamsize>(faces.size() * sizeof(float)));
            }
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
            Require(file.good(), "could not preserve gas MAC snapshot");
        };
        if (audit_pressure) {
            native_dispatch = glad_glDispatchCompute;
            audited_dispatch = [&](GLuint x, GLuint y, GLuint z) {
                GLint program = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                if (static_cast<GLuint>(program) != app.pressure_residual_program_) {
                    native_dispatch(x, y, z);
                    if (capture_projected_gas && static_cast<GLuint>(program) == app.smoke_project_program_) {
                        save_gas_mac("build/goal-baseline/gas-pressure-failing-projected-mac.bin", app.NextGasU(), app.NextGasV());
                        capture_projected_gas = false;
                    }
                    return;
                }
                GLint liquid = 0;
                glGetUniformiv(program, glGetUniformLocation(program, "useLiquidMask"), &liquid);
                const auto before = app.ReadDebugCounters();
                native_dispatch(x, y, z);
                const auto after = app.ReadDebugCounters();
                const GLuint failures = after[2] - before[2];
                const std::size_t material = liquid != 0 ? 0 : 1;
                pressure_failures[material] += failures;
                if (liquid != 0) ++audit_water_step;
                if (failures == 0 || captured[material]) return;
                captured[material] = true;
                if (liquid == 0) {
                    save_gas_mac("build/goal-baseline/gas-pressure-failing-input-mac.bin", app.CurrentGasU(), app.CurrentGasV());
                    capture_projected_gas = true;
                }
                const int width = liquid != 0 ? PowderApp::kGridWidth : PowderApp::kGasWidth;
                const int height = liquid != 0 ? PowderApp::kGridHeight : PowderApp::kGasHeight;
                const std::size_t count = static_cast<std::size_t>(width * height);
                std::vector<float> rhs(count), pressure(count), phi(count, 0.0F);
                std::vector<GLubyte> mask(count);
                GLint saved_texture = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
                const auto read_float = [&](GLuint texture, std::vector<float>& values) {
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
                };
                read_float(liquid != 0 ? app.water_state_.divergence : app.gas_state_.divergence, rhs);
                read_float(liquid != 0 ? app.water_state_.pressure_a : app.gas_state_.pressure_a, pressure);
                if (liquid != 0) read_float(app.water_state_.liquid_phi, phi);
                glBindTexture(GL_TEXTURE_2D, liquid != 0 ? app.boundary_state_.mask_full : app.boundary_state_.mask_gas);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_texture));
                {
                    const char* path = liquid != 0 ? "build/goal-baseline/water-pressure-failing-system.bin"
                                                  : "build/goal-baseline/gas-pressure-failing-system.bin";
                    std::ofstream snapshot(path, std::ios::binary);
                    snapshot.write(reinterpret_cast<const char*>(&width), sizeof(width));
                    snapshot.write(reinterpret_cast<const char*>(&height), sizeof(height));
                    snapshot.write(reinterpret_cast<const char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
                    snapshot.write(reinterpret_cast<const char*>(rhs.data()), static_cast<std::streamsize>(rhs.size() * sizeof(float)));
                    snapshot.write(reinterpret_cast<const char*>(pressure.data()), static_cast<std::streamsize>(pressure.size() * sizeof(float)));
                    if (liquid != 0) snapshot.write(reinterpret_cast<const char*>(phi.data()), static_cast<std::streamsize>(phi.size() * sizeof(float)));
                    Require(snapshot.good(), "could not preserve failing pressure system");
                }
                const auto blocked = [&](int px, int py) {
                    return px < 0 || py < 0 || px >= width || py >= height || mask[static_cast<std::size_t>(py * width + px)] != 0;
                };
                double square_sum = 0.0, rhs_sum = 0.0;
                float max_residual = 0.0F, max_pressure = 0.0F, max_rhs = 0.0F;
                int maximum_x = 0, maximum_y = 0;
                std::size_t cells = 0, bad = 0;
                const float threshold = liquid != 0 ? app.config_.water_pressure_residual_threshold : app.config_.gas_pressure_residual_threshold;
                for (int py = 0; py < height; ++py) for (int px = 0; px < width; ++px) {
                    const std::size_t i = static_cast<std::size_t>(py * width + px);
                    if (blocked(px, py) || (liquid != 0 && phi[i] >= app.config_.water_liquid_phi_threshold)) continue;
                    float sum = 0.0F, diagonal = 0.0F;
                    for (const auto& offset : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                        const int nx = px + offset[0], ny = py + offset[1];
                        if (blocked(nx, ny)) continue;
                        const auto j = static_cast<std::size_t>(ny * width + nx);
                        if (liquid == 0 || phi[j] < app.config_.water_liquid_phi_threshold) {
                            ++diagonal;
                            sum += pressure[j];
                        } else {
                            diagonal += AirConductance(app.config_, phi[i], phi[j]);
                        }
                    }
                    const float residual = rhs[i] - (sum - diagonal * pressure[i]);
                    square_sum += static_cast<double>(residual) * residual;
                    rhs_sum += rhs[i];
                    if (std::abs(residual) > max_residual) {
                        max_residual = std::abs(residual);
                        maximum_x = px;
                        maximum_y = py;
                    }
                    max_pressure = std::max(max_pressure, std::abs(pressure[i]));
                    max_rhs = std::max(max_rhs, std::abs(rhs[i]));
                    ++cells;
                    if (!std::isfinite(residual) || std::abs(residual) > threshold) ++bad;
                }
                std::array<float, 4> values{};
                std::array<GLuint, 4> flags{};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(values), values.data());
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(values), sizeof(flags), flags.data());
                std::cout << "pressure audit material=" << (liquid != 0 ? "water" : "gas") << " frame=" << audit_frame
                          << " water-step=" << audit_water_step << " GPU/CPU failures=" << failures << '/' << bad
                          << " residual max/RMS=" << max_residual << '/' << std::sqrt(square_sum / std::max<std::size_t>(1, cells))
                          << " max-cell=" << maximum_x << ',' << maximum_y
                          << " pressure/rhs max=" << max_pressure << '/' << max_rhs << " rhs mean=" << rhs_sum / std::max<std::size_t>(1, cells)
                          << " water recursive/iterations/breakdown=" << values[3] << '/' << flags[2] << '/' << flags[1] << '\n';
            };
            std::cout << "pressure audit enabled: instrumented times are not performance evidence\n";
        }
        measured_dispatch = [&](GLuint x, GLuint y, GLuint z) {
            if (tracked_program == app.smoke_divergence_program_) {
                gas_projection_active = true;
                gas_relaxations = gas_checks = 0;
            }
            if (tracked_program == app.gas_pressure_relax_program_) ++gas_relaxations;
            if (gas_projection_active && tracked_program == app.pressure_residual_program_) ++gas_checks;
            if (tracked_program == app.smoke_project_program_) {
                const int cycles = gas_relaxations / kGasRelaxPerCycle;
                if (audit_frame >= warmup) {
                    gas_cycle_samples.push_back(cycles);
                    gas_check_samples.push_back(gas_checks);
                }
                if (profile_gas && !captured_slow_gas && audit_frame >= warmup && cycles >= 20) {
                    captured_slow_gas = true;
                    save_gas_mac("build/goal-baseline/gas-pressure-slow-input-mac.bin", app.CurrentGasU(), app.CurrentGasV());
                    const std::array<int, 2> dimensions{PowderApp::kGasWidth, PowderApp::kGasHeight};
                    const std::size_t cells = static_cast<std::size_t>(dimensions[0] * dimensions[1]);
                    std::vector<unsigned char> mask(cells);
                    std::vector<float> values(cells);
                    GLint binding = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
                    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                    glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                    std::ofstream file("build/goal-baseline/gas-pressure-slow-system.bin", std::ios::binary);
                    file.write(reinterpret_cast<const char*>(dimensions.data()), sizeof(dimensions));
                    file.write(reinterpret_cast<const char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
                    for (GLuint texture : {app.gas_state_.divergence, app.gas_state_.pressure_a}) {
                        glBindTexture(GL_TEXTURE_2D, texture);
                        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
                        file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
                    }
                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
                    Require(file.good(), "could not preserve slow gas system");
                    std::cout << "gas slow snapshot frame=" << audit_frame << " cycles/checks=" << cycles << '/' << gas_checks << '\n';
                }
                gas_projection_active = false;
            }
            if (audited_dispatch) audited_dispatch(x, y, z);
            else native_dispatch(x, y, z);
        };
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) { measured_dispatch(x, y, z); };
        if (profile_gas) std::cout << "gas snapshot capture enabled: instrumented times are not performance evidence\n";
        app.replay_mode_ = true;
        glfwSetWindowSize(app.window_, 1000, 1000);
        glfwPollEvents();
        int render_width = 0, render_height = 0;
        glfwGetFramebufferSize(app.window_, &render_width, &render_height);
        Require(render_width > 0 && render_height > 0, "benchmark needs a drawable framebuffer size");
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        struct Targets {
            GLuint framebuffer = 0, color = 0;
            std::array<GLuint, 2> queries{};
            ~Targets() {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteTextures(1, &color);
                glDeleteQueries(2, queries.data());
            }
        } target;
        glGenTextures(1, &target.color);
        glBindTexture(GL_TEXTURE_2D, target.color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, render_width, render_height);
        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "benchmark framebuffer incomplete");
        glGenQueries(2, target.queries.data());
        std::cout << "benchmark renderer=" << glGetString(GL_RENDERER) << " driver=" << glGetString(GL_VERSION)
                  << " simulation=1000x1000 gas=500x500 render=" << render_width << 'x' << render_height
                  << " dt=" << app.config_.fixed_dt << " warmup=" << warmup << " samples=" << samples << '\n';
        std::cout << "benchmark water pressure cap=" << app.config_.water_pressure_iterations << '\n';
        std::ostringstream failures;
        for (bool combined : {false, true}) {
            if (audit_pressure && !combined) continue;
            app.InitializeState();
            gas_cycle_samples.clear();
            gas_check_samples.clear();
            app.ResetReplayMetrics();
            const int ticks_per_render = combined ? combined_ticks : 1;
            std::cout << "benchmark " << (combined ? "combined" : "idle") << " ticks per render=" << ticks_per_render << '\n';
            if (combined) {
                std::vector<Particle> water;
                water.reserve(100000);
                for (int y = 200; y < 600; ++y)
                    for (int x = 600; x < 850; ++x)
                        water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                const std::vector<Particle> sand = BrushLikeSand(100, 200, 300, 325);
                WriteParticles(app, app.water_state_.particles, water);
                WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
            }
            const auto initial_water = app.ReadWaterMetrics();
            const GLuint initial_evaporated = app.ReadPhaseSinks()[0];
            const auto initial_sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                              PowderApp::kSandParticleComponents);
            Require(initial_water.count == (combined ? 100000U : 0U) && initial_sand.count == (combined ? 50000U : 0U),
                    "benchmark initial particle load does not match its declared workload");
            std::vector<double> gpu_ms, wall_ms;
            std::array<double, PowderApp::kReplayQueryCount> last_tick_pass_ms{};
            double last_water_iterations = 0.0;
            GLuint max_water_iterations = 0;
            std::array<std::uint64_t, 5> counters{};
            double largest_mass_drift = 0.0;
            double substep_sum = 0.0;
            std::array<int, 9> substep_histogram{};
            int substep_max = 0, substep_ticks = 0;
            double peak_smoke = 0.0, peak_reaction = 0.0;
            for (int frame = 0; frame < warmup + samples; ++frame) {
                audit_frame = frame;
                audit_water_step = 0;
                app.brush_mode_ = frame % 2 == 0 ? PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke;
                app.brush_x_ = 500;
                app.brush_y_ = 120;
                app.brush_radius_ = 18;
                const auto source = app.BuildBrushConfig();
                const auto start = Clock::now();
                glQueryCounter(target.queries[0], GL_TIMESTAMP);
                for (int tick = 0; tick < ticks_per_render; ++tick) {
                    app.RunFrame(app.config_.fixed_dt, combined ? &source : nullptr, tick == 0);
                    if (frame >= warmup) {
                        substep_sum += app.water_state_.substeps;
                        ++substep_histogram[static_cast<std::size_t>(std::min(app.water_state_.substeps, 8))];
                        substep_max = std::max(substep_max, app.water_state_.substeps);
                        ++substep_ticks;
                    }
                }
                app.Render();
                glQueryCounter(target.queries[1], GL_TIMESTAMP);
                std::array<GLuint64, 2> timestamps{};
                glGetQueryObjectui64v(target.queries[0], GL_QUERY_RESULT, &timestamps[0]);
                glGetQueryObjectui64v(target.queries[1], GL_QUERY_RESULT, &timestamps[1]);
                const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                Require(timestamps[1] > timestamps[0], "benchmark GPU timestamps must advance");
                if (frame >= warmup) {
                    gpu_ms.push_back(static_cast<double>(timestamps[1] - timestamps[0]) / 1000000.0);
                    wall_ms.push_back(elapsed_ms);
                }
                // Validation readback is outside both measured intervals; rendering is inside them.
                if (frame >= warmup) {
                    for (std::size_t i = 0; i < last_tick_pass_ms.size(); ++i) {
                        GLuint64 elapsed = 0;
                        glGetQueryObjectui64v(app.timing_queries_[i], GL_QUERY_RESULT, &elapsed);
                        last_tick_pass_ms[i] += static_cast<double>(elapsed) / 1000000.0;
                    }
                    std::array<GLuint, 4> flags{};
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
                    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * 4, sizeof(flags), flags.data());
                    last_water_iterations += flags[2];
                    max_water_iterations = std::max(max_water_iterations, flags[2]);
                }
                const auto frame_counters = app.ReadDebugCounters();
                if (audit_pressure && frame_counters[2] != 0) {
                    std::cout << "pressure audit total water/gas failing cells=" << pressure_failures[0] << '/' << pressure_failures[1] << '\n';
                    throw std::runtime_error("pressure audit captured first failing frame; benchmark acceptance not evaluated");
                }
                if (frame_counters[2] != 0 && counters[2] == 0) {
                    std::array<float, 4> solver_values{};
                    std::array<GLuint, 4> solver_flags{};
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
                    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(solver_values), solver_values.data());
                    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(solver_values), sizeof(solver_flags), solver_flags.data());
                    std::cout << "benchmark first pressure failure frame=" << frame
                              << " last-solve residual=" << solver_values[3] << " iterations=" << solver_flags[2]
                              << " breakdown=" << solver_flags[1] << '\n';
                }
                for (std::size_t i = 0; i < counters.size(); ++i) counters[i] += frame_counters[i];
                if (frame % 30 == 0 || frame == warmup + samples - 1) {
                    const auto water = app.ReadWaterMetrics();
                    const auto sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                              PowderApp::kSandParticleComponents);
                    largest_mass_drift = std::max({largest_mass_drift,
                        // Water the fire evaporated (counted by phase_particles.comp) left as steam, not drift.
                        std::abs(water.mass + (app.ReadPhaseSinks()[0] - initial_evaporated) - initial_water.mass) /
                            std::max(1.0, initial_water.mass),
                        // Packed grains are sand too (mass one each): moving plus packed is the conserved total.
                        std::abs(sand.mass + static_cast<double>(app.ReadSandPackedSum()) - initial_sand.mass) /
                            std::max(1.0, initial_sand.mass)});
                    peak_smoke = std::max(peak_smoke, app.ReadScalarTextureSum(app.CurrentSmokeDen(),
                                                                            PowderApp::kGasWidth, PowderApp::kGasHeight));
                    peak_reaction = std::max(peak_reaction, app.ReadReactionTextureSum(app.CurrentReactionRate(),
                                                                                     PowderApp::kGasWidth, PowderApp::kGasHeight));
                }
                CheckGl("full-frame benchmark");
                glfwPollEvents();
            }
            std::sort(gpu_ms.begin(), gpu_ms.end());
            const double average = std::accumulate(gpu_ms.begin(), gpu_ms.end(), 0.0) / samples;
            const double wall_average = std::accumulate(wall_ms.begin(), wall_ms.end(), 0.0) / samples;
            const double p95 = gpu_ms[static_cast<std::size_t>(std::ceil(samples * 0.95)) - 1];
            const char* name = combined ? "combined" : "idle";
            std::cout << "benchmark " << name << " GPU mean/p95/max ms=" << average << '/' << p95 << '/' << gpu_ms.back()
                      << " wall mean ms=" << wall_average << " GPU frames/s=" << 1000.0 / average
                      << " wall frames/s=" << 1000.0 / wall_average << " smoke/reaction peaks=" << peak_smoke << '/' << peak_reaction
                      << " sampled mass drift=" << largest_mass_drift << " debug="
                      << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '/' << counters[4] << '\n';
            std::cout << "benchmark " << name << " last tick mean ms spawn/boundary/water/gas/sand/coupling/extract=";
            for (double total : last_tick_pass_ms) std::cout << total / samples << '/';
            std::cout << " last water solve iterations mean/max=" << last_water_iterations / samples << '/' << max_water_iterations
                      << " water substeps mean/max=" << substep_sum / std::max(1, substep_ticks) << '/' << substep_max
                      << " histogram";
            for (std::size_t i = 1; i < substep_histogram.size(); ++i)
                if (substep_histogram[i] != 0) std::cout << ' ' << i << ':' << substep_histogram[i];
            std::cout << '\n';
            if (!gas_cycle_samples.empty()) {
                double cycles_sum = 0, checks_sum = 0;
                for (int value : gas_cycle_samples) cycles_sum += value;
                for (int value : gas_check_samples) checks_sum += value;
                std::sort(gas_cycle_samples.begin(), gas_cycle_samples.end());
                std::cout << "benchmark gas projections=" << gas_cycle_samples.size()
                          << " cycles mean/max=" << cycles_sum / gas_cycle_samples.size() << '/' << gas_cycle_samples.back()
                          << " checks mean=" << checks_sum / gas_check_samples.size() << " cycle histogram=";
                for (std::size_t first = 0; first < gas_cycle_samples.size();) {
                    std::size_t last = first + 1;
                    while (last < gas_cycle_samples.size() && gas_cycle_samples[last] == gas_cycle_samples[first]) ++last;
                    std::cout << gas_cycle_samples[first] << ':' << last - first << ' ';
                    first = last;
                }
                std::cout << '\n';
            }
            const double frame_budget = combined ? 1000.0 / 30.0 : 1000.0 / 60.0;
            if (average > frame_budget || wall_average > frame_budget || p95 > 50.0 ||
                (combined && (peak_smoke < 2.5 || peak_reaction < 2.5)) ||
                largest_mass_drift > 0.005 || counters[0] + counters[1] + counters[2] + counters[3] != 0) {
                failures << " [" << name << " full-frame acceptance failed]";
            }
        }
        app.replay_mode_ = false;
        if (audit_pressure) std::cout << "pressure audit total water/gas failing cells=" << pressure_failures[0] << '/' << pressure_failures[1] << '\n';
        Require(failures.str().empty(), failures.str());
    }

    // Deterministic per-pass GPU profile of four scripted scenes, one tick plus Render(true) and DrawHud per frame in a
    // 1766x1210 window: sweep (water brush 24 at 400 cells/s), pour (water brush 48 at 800 cells/s across the grid),
    // combined (the benchmark's 100,000 water and 50,000 sand with its fire/smoke source) and user (sand bed under a water
    // pool, rock strokes, two wood bowls of burning oil). Not a CTest case. POWDER_PROFILE_SCENE selects one scene,
    // POWDER_PROFILE_FRAMES the measured frames (default 600, after 60 warm-up frames).
    // Takes the cursor, mouse button and key callbacks off a window for the scope, so a real cursor over it cannot change a
    // scripted run; puts them back on destruction.
    struct DetachedInput {
        GLFWwindow* window;
        GLFWcursorposfun cursor = glfwSetCursorPosCallback(window, nullptr);
        GLFWmousebuttonfun button = glfwSetMouseButtonCallback(window, nullptr);
        GLFWkeyfun key = glfwSetKeyCallback(window, nullptr);
        explicit DetachedInput(GLFWwindow* owner) : window(owner) {}
        DetachedInput(const DetachedInput&) = delete;
        DetachedInput& operator=(const DetachedInput&) = delete;
        ~DetachedInput() {
            glfwSetCursorPosCallback(window, cursor);
            glfwSetMouseButtonCallback(window, button);
            glfwSetKeyCallback(window, key);
        }
    };

    static void BenchmarkProfile(PowderApp& app) {
        app.config_ = powder_config::LoadConfigFromEnvironment();
        const char* scene_env = std::getenv("POWDER_PROFILE_SCENE");
        const std::string filter = scene_env != nullptr ? scene_env : "";
        const char* frames_env = std::getenv("POWDER_PROFILE_FRAMES");
        const int measured = frames_env != nullptr ? std::atoi(frames_env) : 600;
        constexpr int warmup = 60;
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = glad_glDispatchCompute;
        static PFNGLDISPATCHCOMPUTEINDIRECTPROC native_indirect = glad_glDispatchComputeIndirect;
        static PFNGLUSEPROGRAMPROC native_use = glad_glUseProgram;
        static int dispatches = 0;
        static GLuint current_program = 0;
        // POWDER_PROFILE_DISPATCH: a timestamp pair around every dispatch (perturbing), summed per program.
        static std::vector<GLuint> pool;
        static std::vector<std::pair<GLuint, std::size_t>> pending;  // program, first query of its pair
        static std::vector<GLuint> pending_groups;                   // workgroups of each timed dispatch, 0 indirect
        static GLuint dispatch_groups = 0;
        static const bool by_groups = std::getenv("POWDER_PROFILE_GROUPS") != nullptr;
        static const bool per_dispatch = std::getenv("POWDER_PROFILE_DISPATCH") != nullptr;
        if (per_dispatch && pool.empty()) {
            pool.resize(1 << 15);
            glGenQueries(static_cast<GLsizei>(pool.size()), pool.data());
        }
        static const auto timed = [](const auto& dispatch) {
            ++dispatches;
            if (!per_dispatch || 2 * pending.size() + 2 > pool.size()) return dispatch();
            const std::size_t first = 2 * pending.size();
            glQueryCounter(pool[first], GL_TIMESTAMP);
            dispatch();
            glQueryCounter(pool[first + 1], GL_TIMESTAMP);
            pending.push_back({current_program, first});
            pending_groups.push_back(dispatch_groups);
        };
        // CPU time blocked in readbacks and fences, so the tick's CPU submission time is its wall time minus this.
        static PFNGLGETBUFFERSUBDATAPROC native_get_buffer = glad_glGetBufferSubData;
        static PFNGLCLIENTWAITSYNCPROC native_wait = glad_glClientWaitSync;
        static PFNGLMAPBUFFERRANGEPROC native_map = glad_glMapBufferRange;
        static PFNGLGETTEXIMAGEPROC native_get_tex = glad_glGetTexImage;
        static double blocked_ms = 0.0;
        static int blocking_calls = 0, blocking_syncs = 0;  // every blocking call, and those that wait (not the maps after a fence)
        static const auto blocked = [](const auto& call) {
            const auto begin = std::chrono::steady_clock::now();
            const auto result = call();
            blocked_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            ++blocking_calls;
            return result;
        };
        glad_glGetBufferSubData = [](GLenum t, GLintptr o, GLsizeiptr s, void* d) {
            ++blocking_syncs;
            blocked([&] { native_get_buffer(t, o, s, d); return 0; });
        };
        glad_glClientWaitSync = [](GLsync sync, GLbitfield flags, GLuint64 timeout) {
            ++blocking_syncs;
            return blocked([&] { return native_wait(sync, flags, timeout); });
        };
        glad_glMapBufferRange = [](GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a) {
            return blocked([&] { return native_map(t, o, l, a); });
        };
        glad_glGetTexImage = [](GLenum t, GLint l, GLenum f, GLenum ty, void* d) {
            ++blocking_syncs;
            blocked([&] { native_get_tex(t, l, f, ty, d); return 0; });
        };
        glad_glUseProgram = [](GLuint p) { current_program = p; native_use(p); };
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            dispatch_groups = x * y * z;
            timed([&] { native_dispatch(x, y, z); });
        };
        glad_glDispatchComputeIndirect = [](GLintptr offset) {
            dispatch_groups = 0;
            timed([&] { native_indirect(offset); });
        };
        struct Restore {
            ~Restore() {
                glad_glDispatchCompute = native_dispatch;
                glad_glDispatchComputeIndirect = native_indirect;
                glad_glUseProgram = native_use;
                glad_glGetBufferSubData = native_get_buffer;
                glad_glClientWaitSync = native_wait;
                glad_glMapBufferRange = native_map;
                glad_glGetTexImage = native_get_tex;
            }
        } restore;
        // Scripted input only: glfwPollEvents delivered the real cursor over the shown window into the stroke trail, so
        // pour and sweep spawned extra water in some runs (920k-2.1M records where every run now matches).
        const DetachedInput detached(app.window_);
        const std::map<GLuint, const char*> program_names{{app.init_state_program_, "init_state"}, {app.spawn_full_program_, "spawn_full"}, {app.spawn_gas_program_, "spawn_gas"}, {app.sand_particle_spawn_program_, "sand_particle_spawn"}, {app.sand_particle_compact_program_, "sand_particle_compact"}, {app.sand_particle_clear_program_, "sand_particle_clear"}, {app.sand_particle_p2g_program_, "sand_particle_p2g"}, {app.sand_grid_update_program_, "sand_grid_update"}, {app.sand_particle_step_program_, "sand_particle_step"}, {app.sand_rasterize_program_, "sand_rasterize"}, {app.sand_sleep_program_, "sand_sleep"}, {app.sand_pack_program_, "sand_pack"}, {app.water_sleep_program_, "water_sleep"}, {app.water_pack_program_, "water_pack"}, {app.boundary_build_program_, "boundary_build"}, {app.boundary_downsample_program_, "boundary_downsample"}, {app.boundary_seed_program_, "boundary_seed"}, {app.boundary_jumpflood_program_, "boundary_jumpflood"}, {app.boundary_distance_program_, "boundary_distance"}, {app.water_particle_p2g_program_, "water_particle_p2g"}, {app.water_particle_spawn_program_, "water_particle_spawn"}, {app.water_particle_compact_program_, "water_particle_compact"}, {app.water_particle_clear_program_, "water_particle_clear"}, {app.water_particle_step_program_, "water_particle_step"}, {app.water_particle_finalize_program_, "water_particle_finalize"}, {app.water_mac_build_program_, "water_mac_build"}, {app.water_mac_finalize_program_, "water_mac_finalize"}, {app.water_divergence_program_, "water_divergence"}, {app.pressure_cg_program_, "pressure_cg"}, {app.pressure_pockets_program_, "pressure_pockets"}, {app.pressure_cg_reduce_program_, "pressure_cg_reduce"}, {app.pressure_mg_program_, "pressure_mg"}, {app.water_project_program_, "water_project"}, {app.smoke_advect_program_, "smoke_advect"}, {app.smoke_correct_program_, "smoke_correct"}, {app.gas_velocity_advect_program_, "gas_velocity_advect"}, {app.gas_velocity_correct_program_, "gas_velocity_correct"}, {app.gas_face_to_center_program_, "gas_face_to_center"}, {app.smoke_divergence_program_, "smoke_divergence"}, {app.gas_pressure_kind_program_, "gas_pressure_kind"}, {app.gas_cg_program_, "gas_cg"}, {app.wood_burn_program_, "wood_burn"}, {app.phase_particles_program_, "phase_particles"}, {app.oil_field_program_, "oil_field"}, {app.steam_update_program_, "steam_update"}, {app.gas_pressure_restrict_program_, "gas_pressure_restrict"}, {app.gas_pressure_relax_program_, "gas_pressure_relax"}, {app.smoke_project_program_, "smoke_project"}, {app.pressure_residual_program_, "pressure_residual"}, {app.fire_rd_program_, "fire_rd"}, {app.coupling_fire_program_, "coupling_fire"}, {app.coupling_heat_program_, "coupling_heat"}, {app.render_extract_program_, "render_extract"}, {app.light_build_program_, "light_build"}, {app.spark_pick_program_, "spark_pick"}, {app.spark_step_program_, "spark_step"}, {app.spark_draw_program_, "spark_draw"}, {app.render_program_, "render"}, {app.hud_program_, "hud"}};
        std::ostringstream failures;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glfwSetWindowSize(app.window_, 1766, 1210);
        glfwShowWindow(app.window_);
        glfwPollEvents();
        glfwSwapInterval(0);
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(app.window_, &fb_width, &fb_height);
        std::cout << "profile renderer=" << glGetString(GL_RENDERER) << " framebuffer=" << fb_width << 'x' << fb_height
                  << " frames=" << measured << '\n';
        std::array<GLuint, 4> stamps{};
        glGenQueries(4, stamps.data());
        const float dt = app.config_.fixed_dt;
        const auto stamp = [&](PowderApp::BrushMode mode, int x, int y, int radius) {
            app.brush_mode_ = mode;
            app.brush_x_ = x;
            app.brush_y_ = y;
            app.brush_radius_ = radius;
            app.StampBrush(app.BuildBrushConfig());
        };
        const auto line = [&](PowderApp::BrushMode mode, int x0, int y0, int x1, int y1, int radius) {
            const int steps = std::max(1, static_cast<int>(std::hypot(x1 - x0, y1 - y0) / std::max(1, radius / 2)));
            for (int i = 0; i <= steps; ++i)
                stamp(mode, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, radius);
        };
        const auto bowl = [&](int cx, int cy, int r) {
            for (int a = 0; a <= 36; ++a) {
                const double t = 3.14159265358979 * (1.0 + a / 36.0);
                stamp(PowderApp::BrushMode::Wood, cx + static_cast<int>(r * std::cos(t)), cy + static_cast<int>(r * std::sin(t)), 5);
            }
        };
        for (const std::string scene : {"sweep", "pour", "combined", "user"}) {
            if (!filter.empty() && filter != scene) continue;
            app.InitializeState();
            // Every tick's counters (each RunFrame resets them), so a transient failure cannot hide behind a clean last tick.
            std::array<std::uint64_t, 5> counter_sum{};
            std::array<GLuint, 5> counter_max{};
            int ticks = 0, failing_ticks = 0, first_failing = -1;
            const auto tally = [&] {
                const auto c = app.ReadDebugCounters();
                for (std::size_t i = 0; i < c.size(); ++i) {
                    counter_sum[i] += c[i];
                    counter_max[i] = std::max(counter_max[i], c[i]);
                }
                if (c[0] != 0 || c[1] != 0 || c[2] != 0 || c[3] != 0) {
                    if (failing_ticks++ == 0) first_failing = ticks;
                }
                ++ticks;
            };
            app.replay_mode_ = false;
            app.render_mode_ = PowderApp::RenderMode::Composite;
            app.stroke_ = {};
            app.cursor_trail_.clear();
            app.help_visible_ = false;
            app.help_hide_time_ = -1.0;
            app.painting_ = app.cursor_in_sim_ = false;
            if (scene == "combined" || scene == "user") {
                std::vector<Particle> water;
                const bool user = scene == "user";
                for (int y = user ? 90 : 200; y < (user ? 190 : 600); ++y)
                    for (int x = user ? 20 : 600; x < (user ? 980 : 850); ++x) water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                const std::vector<Particle> sand = user ? BrushLikeSand(20, 10, 980, 90) : BrushLikeSand(100, 200, 300, 325);
                WriteParticles(app, app.water_state_.particles, water);
                WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
            }
            if (scene == "user") {
                line(PowderApp::BrushMode::Solid, 60, 420, 380, 320, 7);
                line(PowderApp::BrushMode::Solid, 620, 360, 940, 470, 7);
                line(PowderApp::BrushMode::Solid, 150, 700, 420, 760, 6);
                bowl(300, 560, 90);
                bowl(700, 640, 100);
                // Fill the bowls with oil and light them, simulating as the user would have.
                for (int tick = 0; tick < 90; ++tick) {
                    if (tick % 30 == 0) {
                        stamp(PowderApp::BrushMode::Oil, 300, 520, 25);
                        stamp(PowderApp::BrushMode::Oil, 700, 600, 25);
                    }
                    app.RunFrame(dt, nullptr, true);
                    tally();
                }
                for (int tick = 0; tick < 60; ++tick) {
                    stamp(PowderApp::BrushMode::Fire, 300, 520, 20);
                    stamp(PowderApp::BrushMode::Fire, 700, 600, 20);
                    app.RunFrame(dt, nullptr, true);
                    tally();
                }
            }
            std::vector<double> frame_ms, tick_ms, render_ms;
            std::array<double, PowderApp::kReplayQueryCount> pass_ms{};
            double hud_sum = 0.0, water_substeps = 0.0, sand_substeps = 0.0, water_iterations = 0.0, gas_iterations = 0.0;
            double dispatch_sum = 0.0, wall_sum = 0.0, blocked_sum = 0.0, blocking_sum = 0.0, sync_sum = 0.0, frame_wall_sum = 0.0;
            std::vector<double> frame_wall_ms;
            // Gas pass and whole-tick GPU ms by tick parity (odd: the first tick after InitializeState, which starts a gas step).
            std::array<double, 2> gas_parity_ms{}, tick_parity_ms{};
            std::array<int, 2> parity_ticks{};
            std::map<GLuint, std::pair<double, int>> program_ms;
            std::map<std::pair<GLuint, GLuint>, std::pair<double, int>> group_ms;  // by program and workgroups
            for (int frame = 0; frame < warmup + measured; ++frame) {
                const double t = frame * static_cast<double>(dt);
                PowderApp::BrushConfig source{};
                const PowderApp::BrushConfig* scripted = nullptr;
                if (scene == "sweep" || scene == "pour") {
                    const bool pour = scene == "pour";
                    const double span = pour ? 900.0 : 600.0, phase = std::fmod(t * (pour ? 800.0 : 400.0), 2.0 * span);
                    const int x = (pour ? 50 : 200) + static_cast<int>(phase < span ? phase : 2.0 * span - phase);
                    app.brush_mode_ = PowderApp::BrushMode::Water;
                    app.brush_radius_ = pour ? 48 : 24;
                    app.cursor_trail_.push_back({x, pour ? 900 : 850});
                    app.brush_x_ = x;
                    app.brush_y_ = pour ? 900 : 850;
                    app.cursor_in_sim_ = app.painting_ = true;
                } else if (scene == "combined") {
                    app.brush_mode_ = frame % 2 == 0 ? PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke;
                    app.brush_x_ = 500;
                    app.brush_y_ = 120;
                    app.brush_radius_ = 18;
                    source = app.BuildBrushConfig();
                    scripted = &source;
                }
                dispatches = 0;
                blocked_ms = 0.0;
                blocking_calls = blocking_syncs = 0;
                glQueryCounter(stamps[0], GL_TIMESTAMP);
                const auto tick_begin = std::chrono::steady_clock::now();
                app.RunFrame(dt, scripted, true);
                const double tick_wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_begin).count();
                const double tick_blocked = blocked_ms;
                const int tick_blocking = blocking_calls, tick_syncs = blocking_syncs;
                glQueryCounter(stamps[1], GL_TIMESTAMP);
                app.Render(true);
                glQueryCounter(stamps[2], GL_TIMESTAMP);
                app.DrawHud(t);
                glQueryCounter(stamps[3], GL_TIMESTAMP);
                glfwSwapBuffers(app.window_);
                glfwPollEvents();
                const double frame_wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_begin).count();
                std::array<GLuint64, 4> at{};
                for (int i = 0; i < 4; ++i) glGetQueryObjectui64v(stamps[i], GL_QUERY_RESULT, &at[i]);
                for (std::size_t p = 0; p < pending.size(); ++p) {
                    const auto& [timed_program, first] = pending[p];
                    GLuint64 a = 0, b = 0;
                    glGetQueryObjectui64v(pool[first], GL_QUERY_RESULT, &a);
                    glGetQueryObjectui64v(pool[first + 1], GL_QUERY_RESULT, &b);
                    if (frame < warmup) continue;
                    program_ms[timed_program].first += static_cast<double>(b - a) / 1e6;
                    ++program_ms[timed_program].second;
                    auto& by = group_ms[{timed_program, pending_groups[p]}];
                    by.first += static_cast<double>(b - a) / 1e6;
                    ++by.second;
                }
                pending.clear();
                pending_groups.clear();
                tally();
                if (frame < warmup) continue;
                frame_ms.push_back(static_cast<double>(at[3] - at[0]) / 1e6);
                tick_ms.push_back(static_cast<double>(at[1] - at[0]) / 1e6);
                render_ms.push_back(static_cast<double>(at[2] - at[1]) / 1e6);
                hud_sum += static_cast<double>(at[3] - at[2]) / 1e6;
                for (std::size_t i = 0; i < pass_ms.size(); ++i) {
                    GLuint64 value = 0;
                    glGetQueryObjectui64v(app.timing_queries_[i], GL_QUERY_RESULT, &value);
                    pass_ms[i] += static_cast<double>(value) / 1e6;
                    if (i == static_cast<std::size_t>(PowderApp::TimedPass::Gas))
                        gas_parity_ms[app.frame_index_ % 2U] += static_cast<double>(value) / 1e6;
                }
                tick_parity_ms[app.frame_index_ % 2U] += static_cast<double>(at[1] - at[0]) / 1e6;
                ++parity_ticks[app.frame_index_ % 2U];
                water_substeps += app.water_state_.substeps;
                sand_substeps += app.sand_state_.substeps;
                water_iterations += app.water_state_.pressure_iterations;
                gas_iterations += app.gas_state_.pressure_iterations;
                dispatch_sum += dispatches;
                wall_sum += tick_wall;
                blocked_sum += tick_blocked;
                blocking_sum += tick_blocking;
                sync_sum += tick_syncs;
                frame_wall_sum += frame_wall;
                frame_wall_ms.push_back(frame_wall);
            }
            CheckGl("profile frame");
            const auto water = app.ReadWaterMetrics();
            const auto sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                      PowderApp::kSandParticleComponents);
            const double n = static_cast<double>(frame_ms.size());
            const auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
            const auto p95 = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * 0.95))];
            };
            const std::vector<double> tail(frame_ms.end() - std::min<std::size_t>(frame_ms.size(), 120), frame_ms.end());
            std::cout << std::fixed << std::setprecision(2) << "profile " << scene << " water=" << water.count
                      << " sand=" << sand.count << " counters sum=" << counter_sum[0] << '/' << counter_sum[1] << '/' << counter_sum[2]
                      << '/' << counter_sum[3] << '/' << counter_sum[4] << " max=" << counter_max[0] << '/' << counter_max[1] << '/'
                      << counter_max[2] << '/' << counter_max[3] << '/' << counter_max[4] << " over " << ticks << " ticks, failing ticks "
                      << failing_ticks << " (first " << first_failing << ")" << std::endl;
            std::cout << "profile " << scene << " GPU frame ms mean/p95/max/last120=" << mean(frame_ms) << '/' << p95(frame_ms)
                      << '/' << *std::max_element(frame_ms.begin(), frame_ms.end()) << '/' << mean(tail)
                      << " tick=" << mean(tick_ms) << " render=" << mean(render_ms) << " hud=" << hud_sum / n << '\n';
            std::cout << "profile " << scene << " pass ms spawn/boundary/water/gas/sand/coupling/extract=";
            for (std::size_t i = 0; i < pass_ms.size(); ++i) std::cout << (i ? "/" : "") << pass_ms[i] / n;
            std::cout << "\nprofile " << scene << " by tick parity odd/even: gas ms="
                      << gas_parity_ms[1] / std::max(1, parity_ticks[1]) << '/' << gas_parity_ms[0] / std::max(1, parity_ticks[0])
                      << " tick ms=" << tick_parity_ms[1] / std::max(1, parity_ticks[1]) << '/'
                      << tick_parity_ms[0] / std::max(1, parity_ticks[0]);
            std::cout << "\nprofile " << scene << " per tick: water substeps=" << water_substeps / n
                      << " last water CG=" << water_iterations / n << " sand substeps=" << sand_substeps / n
                      << " last gas CG=" << gas_iterations / n << " dispatches=" << dispatch_sum / n << '\n';
            std::cout << "profile " << scene << " per tick CPU wall ms=" << wall_sum / n << " blocked ms=" << blocked_sum / n
                      << " (calls " << blocking_sum / n << ", waits " << sync_sum / n << ") submitting ms=" << (wall_sum - blocked_sum) / n << '\n';
            std::cout << "profile " << scene << " wall frame ms mean/p95=" << frame_wall_sum / n << '/' << p95(frame_wall_ms) << '\n';
            if (per_dispatch) {
                std::vector<std::pair<double, GLuint>> order;
                for (const auto& [program, total] : program_ms) order.push_back({total.first, program});
                std::sort(order.rbegin(), order.rend());
                std::cout << "profile " << scene << " dispatch ms/frame x dispatches/frame:";
                for (const auto& [total, program] : order) {
                    const auto found = program_names.find(program);
                    std::cout << ' ' << (found != program_names.end() ? found->second : "?") << '=' << total / n << 'x'
                              << program_ms[program].second / n;
                }
                std::cout << '\n';
                if (by_groups) {
                    std::cout << "profile " << scene << " us/dispatch x dispatches/frame by program:workgroups:";
                    for (const auto& [key, total] : group_ms) {
                        const auto found = program_names.find(key.first);
                        if (total.first / n < 0.05) continue;
                        std::cout << ' ' << (found != program_names.end() ? found->second : "?") << ':' << key.second << '='
                                  << total.first * 1e3 / total.second << 'x' << total.second / n;
                    }
                    std::cout << '\n';
                }
            }
            if (counter_sum[0] != 0 || counter_sum[2] != 0) failures << scene << " nonfinite/nonconverged " << counter_sum[0] << '/' << counter_sum[2] << " over " << failing_ticks << " ticks; ";
        }
        glDeleteQueries(4, stamps.data());
        Require(failures.str().empty(), "profile scenes reported failures: " + failures.str());
    }

    // Live-window profile: the interactive frame body (clock, ticks, Render(true), DrawHud, swap) in a visible
    // 1280x900 window, with the water brush held by script instead of the mouse. Not a CTest case. Environment:
    // POWDER_BENCH_VSYNC (swap interval, default 1), POWDER_BENCH_SCENE (stream|sweep, default both; or erase-flick|fire-flick),
    // POWDER_BENCH_SECONDS (simulated seconds per scene, default 30), POWDER_BENCH_WALL_LIMIT (default 90 s),
    // POWDER_BENCH_DETAIL (adds synchronous per-pass and per-solve reads, which perturb the frame times),
    // POWDER_BENCH_HZ (emulates a slower display by waiting for the next refresh after each swap).
    static void BenchmarkInteractive(PowderApp& app) {
        const auto env_or = [](const char* name, double fallback) {
            const char* value = std::getenv(name);
            return value != nullptr ? std::atof(value) : fallback;
        };
        const int vsync = static_cast<int>(env_or("POWDER_BENCH_VSYNC", 1.0));
        const double display_hz = env_or("POWDER_BENCH_HZ", 0.0);
        const double sim_limit = env_or("POWDER_BENCH_SECONDS", 30.0);
        const double wall_limit = env_or("POWDER_BENCH_WALL_LIMIT", 90.0);
        const bool detail = std::getenv("POWDER_BENCH_DETAIL") != nullptr;
        const char* scene_env = std::getenv("POWDER_BENCH_SCENE");
        const std::string scene_filter = scene_env != nullptr ? scene_env : "";
        app.config_ = powder_config::LoadConfigFromEnvironment();

        // Counted per frame: blocking readbacks the app issues, compute dispatches and water CG iterations dispatched.
        struct Hooks {
            PFNGLDISPATCHCOMPUTEPROC dispatch = glad_glDispatchCompute;
            PFNGLDISPATCHCOMPUTEINDIRECTPROC dispatch_indirect = glad_glDispatchComputeIndirect;
            PFNGLUSEPROGRAMPROC use_program = glad_glUseProgram;
            PFNGLUNIFORM1IPROC uniform1i = glad_glUniform1i;
            PFNGLGETBUFFERSUBDATAPROC get_buffer = glad_glGetBufferSubData;
            PFNGLGETTEXIMAGEPROC get_tex = glad_glGetTexImage;
            PFNGLGETQUERYOBJECTUI64VPROC get_query = glad_glGetQueryObjectui64v;
            PFNGLCLIENTWAITSYNCPROC wait_sync = glad_glClientWaitSync;
            PFNGLCLEARBUFFERDATAPROC clear_buffer = glad_glClearBufferData;
            ~Hooks() {
                glad_glDispatchCompute = dispatch;
                glad_glDispatchComputeIndirect = dispatch_indirect;
                glad_glUseProgram = use_program;
                glad_glUniform1i = uniform1i;
                glad_glGetBufferSubData = get_buffer;
                glad_glGetTexImage = get_tex;
                glad_glGetQueryObjectui64v = get_query;
                glad_glClientWaitSync = wait_sync;
                glad_glClearBufferData = clear_buffer;
            }
        };
        static Hooks* hooks = nullptr;
        static PowderApp* target = nullptr;
        static GLuint program = 0;
        static int cg_phase = -1, mg_phase = -1, mg_level = -1, readbacks = 0, dispatches = 0, cg_iterations = 0;
        static GLint phase_location = -1, mg_phase_location = -1, mg_level_location = -1;
        static bool counting = false;
        static std::vector<std::array<GLuint, 3>>* phase_timer = nullptr;  // phase, start and end timestamp queries
        // Detail scenes: start and end timestamps of each water pressure solve, CG phase 0 to its residual check.
        static std::vector<std::array<GLuint, 2>>* solve_timer = nullptr;
        static GLuint solve_start = 0;
        Hooks installed;
        hooks = &installed;
        target = &app;
        phase_location = glGetUniformLocation(app.pressure_cg_program_, "phase");
        mg_phase_location = glGetUniformLocation(app.pressure_mg_program_, "phase");
        mg_level_location = glGetUniformLocation(app.pressure_mg_program_, "level");
        glad_glUseProgram = [](GLuint p) { program = p; hooks->use_program(p); };
        glad_glUniform1i = [](GLint location, GLint value) {
            if (program == target->pressure_cg_program_ && location == phase_location) cg_phase = value;
            if (program == target->pressure_mg_program_ && location == mg_phase_location) mg_phase = value;
            if (program == target->pressure_mg_program_ && location == mg_level_location) mg_level = value;
            hooks->uniform1i(location, value);
        };
        // Solver micro-benchmark: GPU time of each water solver dispatch between timestamps around it, keyed by CG phase,
        // 100 + 10 * multigrid phase + level, 200 + reduction phase, or 300 for the residual check.
        static const auto timed = [](const auto& dispatch) {
            const bool solver = program == target->pressure_cg_program_ || program == target->pressure_mg_program_ ||
                                program == target->pressure_cg_reduce_program_ || program == target->pressure_residual_program_;
            if (phase_timer == nullptr || !solver) return dispatch();
            std::array<GLuint, 2> pair{};
            glGenQueries(2, pair.data());
            glQueryCounter(pair[0], GL_TIMESTAMP);
            dispatch();
            glQueryCounter(pair[1], GL_TIMESTAMP);
            GLint reduce_phase = 0;
            if (program == target->pressure_cg_reduce_program_)
                glGetUniformiv(program, glGetUniformLocation(program, "phase"), &reduce_phase);
            const int key = program == target->pressure_mg_program_        ? 100 + 10 * mg_phase + mg_level
                            : program == target->pressure_cg_reduce_program_ ? 200 + reduce_phase
                            : program == target->pressure_residual_program_  ? 300
                                                                             : cg_phase;
            phase_timer->push_back({static_cast<GLuint>(key), pair[0], pair[1]});
        };
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            if (counting) {
                ++dispatches;
                if (program == target->pressure_cg_program_ && cg_phase == 1) ++cg_iterations;
            }
            if (solve_timer != nullptr && program == target->pressure_cg_program_ && cg_phase == 0) {
                glGenQueries(1, &solve_start);
                glQueryCounter(solve_start, GL_TIMESTAMP);
            }
            timed([&] { hooks->dispatch(x, y, z); });
            if (solve_timer != nullptr && solve_start != 0 && program == target->pressure_residual_program_) {
                GLuint end = 0;
                glGenQueries(1, &end);
                glQueryCounter(end, GL_TIMESTAMP);
                solve_timer->push_back({solve_start, end});
                solve_start = 0;
            }
        };
        glad_glDispatchComputeIndirect = [](GLintptr offset) {
            if (counting) {
                ++dispatches;
                if (program == target->pressure_cg_program_ && cg_phase == 1) ++cg_iterations;
            }
            timed([&] { hooks->dispatch_indirect(offset); });
        };
        glad_glGetBufferSubData = [](GLenum t, GLintptr o, GLsizeiptr s, void* d) {
            if (counting) ++readbacks;
            hooks->get_buffer(t, o, s, d);
        };
        glad_glGetTexImage = [](GLenum t, GLint l, GLenum f, GLenum ty, void* d) {
            if (counting) ++readbacks;
            hooks->get_tex(t, l, f, ty, d);
        };
        glad_glGetQueryObjectui64v = [](GLuint id, GLenum pname, GLuint64* value) {
            if (counting && pname == GL_QUERY_RESULT) ++readbacks;
            hooks->get_query(id, pname, value);
        };
        glad_glClientWaitSync = [](GLsync sync, GLbitfield flags, GLuint64 timeout) {
            if (counting && timeout != 0) ++readbacks;
            return hooks->wait_sync(sync, flags, timeout);
        };
        // Debug counters accumulate over the whole scene on the GPU and are read once at the end.
        glad_glClearBufferData = [](GLenum t, GLenum i, GLenum f, GLenum ty, const void* d) {
            GLint bound = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &bound);
            if (counting && static_cast<GLuint>(bound) == target->debug_state_.counters) return;
            hooks->clear_buffer(t, i, f, ty, d);
        };

        // The scenes script their own cursor; a real one over the shown window would add strokes to them.
        const DetachedInput detached(app.window_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glfwSetWindowSize(app.window_, 1280, 900);
        glfwShowWindow(app.window_);
        glfwPollEvents();
        glfwSwapInterval(vsync);
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(app.window_, &fb_width, &fb_height);
        std::cout << "interactive renderer=" << glGetString(GL_RENDERER) << " driver=" << glGetString(GL_VERSION)
                  << " framebuffer=" << fb_width << 'x' << fb_height << " swap interval=" << vsync
                  << " dt=" << app.config_.fixed_dt << (detail ? " detail reads on (perturbing)" : "") << '\n';

        // Mean GPU us and count of each timed solver dispatch kind (keys at the phase timer); deletes the queries.
        const auto print_timed = [](const char* label, std::vector<std::array<GLuint, 3>>& timed) {
            std::map<GLuint, std::pair<double, int>> phase_us;  // total us and dispatch count
            for (const auto& [timed_phase, first, last] : timed) {
                GLuint64 a = 0, b = 0;
                glGetQueryObjectui64v(first, GL_QUERY_RESULT, &a);
                glGetQueryObjectui64v(last, GL_QUERY_RESULT, &b);
                glDeleteQueries(1, &first);
                glDeleteQueries(1, &last);
                phase_us[timed_phase].first += static_cast<double>(b - a) / 1e3;
                ++phase_us[timed_phase].second;
            }
            timed.clear();
            std::cout << label << " GPU us/dispatch x dispatches (keys at the timer):";
            for (const auto& [timed_phase, total] : phase_us)
                std::cout << ' ' << timed_phase << '=' << total.first / total.second << 'x' << total.second;
            std::cout << '\n';
        };
        if (detail) {
            // Solver cost on frozen systems, each from a cold start: a 998x64 pool (252 tiles, near the stream scene's), a
            // 998x400 tank with a stream-like impact (a 16x16 patch of strong inflow under its surface), and the pool
            // under a spray of 2x2 drops every 40 cells in open air, which spreads the solve over the whole grid.
            constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
            std::vector<unsigned char> mask(width * height);
            std::vector<float> phi(width * height), rhs(width * height), zero(width * height, 0.0F);
            const auto upload = [&](GLuint texture, GLenum format, GLenum type, const void* data) {
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
            };
            std::array<GLuint, 2> stamps{};
            glGenQueries(2, stamps.data());
            const int default_cap = app.config_.water_pressure_iterations;
            const auto solve_ms = [&](int prediction, bool cold) {
                app.water_state_.pressure_iterations = prediction;
                if (cold) upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero.data());
                glQueryCounter(stamps[0], GL_TIMESTAMP);
                app.RunWaterProjection();
                glQueryCounter(stamps[1], GL_TIMESTAMP);
                GLuint64 begin = 0, end = 0;
                glGetQueryObjectui64v(stamps[0], GL_QUERY_RESULT, &begin);
                glGetQueryObjectui64v(stamps[1], GL_QUERY_RESULT, &end);
                return static_cast<double>(end - begin) / 1e6;
            };
            // POWDER_BENCH_PRESSURE_SNAPSHOT adds a captured system (see the capture below) as the last, timed one.
            const char* snapshot_path = std::getenv("POWDER_BENCH_PRESSURE_SNAPSHOT");
            std::vector<std::string> scenes{"pool 998x64", "tank 998x400", "spray 998x64+drops"};
            if (snapshot_path != nullptr) scenes.push_back("snapshot");
            for (const std::string& scene : scenes) {
                const int depth = scene.starts_with("tank") ? 400 : 64;
                const bool spray = scene.starts_with("spray");
                if (scene == "snapshot") {
                    std::ifstream file(snapshot_path, std::ios::binary);
                    std::array<std::int32_t, 2> dimensions{};
                    std::vector<float> saved(width * height);
                    file.read(reinterpret_cast<char*>(dimensions.data()), sizeof(dimensions));
                    file.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
                    for (auto* values : {&rhs, &saved, &phi})
                        file.read(reinterpret_cast<char*>(values->data()), static_cast<std::streamsize>(values->size() * sizeof(float)));
                    Require(file.good() && dimensions == std::array<std::int32_t, 2>{width, height}, "invalid pressure snapshot");
                } else {
                    std::fill(mask.begin(), mask.end(), 1);
                    std::fill(phi.begin(), phi.end(), 1.0F);
                    std::fill(rhs.begin(), rhs.end(), 0.0F);
                    for (int y = 1; y < (spray ? height - 1 : depth + 2); ++y)
                        for (int x = 1; x < width - 1; ++x) {
                            mask[y * width + x] = 0;
                            const bool drop = spray && y >= 120 && x % 40 < 2 && (y - 120) % 40 < 2;
                            if (y <= depth || drop) phi[y * width + x] = 0.0F;
                            if (y == 1) rhs[y * width + x] = -4.0F;
                            if (drop) rhs[y * width + x] = -2.0F;
                            if (depth == 400 && y > depth - 24 && y <= depth - 8 && std::abs(x - 500) < 8) rhs[y * width + x] = -8.0F;
                        }
                }
                app.InitializeState();
                upload(app.boundary_state_.mask_full, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                upload(app.water_state_.liquid_phi, GL_RED, GL_FLOAT, phi.data());
                upload(app.CurrentWaterDivergence(), GL_RED, GL_FLOAT, rhs.data());
                // Half a second of cold solves first raises the GPU clock, which these latency-bound solves are sensitive
                // to; then five cold solves predicted from the previous one, as in the app, then five warm converged
                // ones: the fixed cost of a solve.
                for (const double warmup = glfwGetTime(); glfwGetTime() - warmup < 0.5;) solve_ms(0, true);
                for (bool cold : {true, false}) {
                    std::vector<double> samples;
                    for (int sample = 0; sample < 5; ++sample)
                        samples.push_back(solve_ms(cold ? app.water_state_.pressure_iterations : 0, cold));
                    std::sort(samples.begin(), samples.end());
                    std::cout << std::setprecision(4) << "interactive solver " << scene
                              << (cold ? " cold" : " warm") << " iterations=" << app.water_state_.pressure_iterations
                              << " dispatched=" << app.water_state_.pressure_dispatched << " GPU ms median/min="
                              << samples[2] << '/' << samples[0] << '\n';
                }
            }
            // Empty blind iterations after a converged start, from a 300-iteration prediction.
            const double empty = solve_ms(300, false);
            std::cout << "interactive solver empty dispatched=" << app.water_state_.pressure_dispatched
                      << " GPU ms per dispatched iteration=" << empty / app.water_state_.pressure_dispatched << '\n';
            glDeleteQueries(2, stamps.data());
            // Four more cold solves of the last system with a timestamp pair around every solver dispatch (perturbing).
            std::vector<std::array<GLuint, 3>> timed;
            app.config_.water_pressure_iterations = default_cap;
            for (int sample = 0; sample < 4; ++sample) {
                app.water_state_.pressure_iterations = 0;
                upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero.data());
                phase_timer = &timed;
                app.RunWaterProjection();
                phase_timer = nullptr;
            }
            print_timed("interactive solver", timed);
        }

        std::ostringstream failures;
        // The flick scenes (only when selected) jump the cursor across the grid every frame: 64 stamps per tick.
        for (const std::string scene : {"stream", "sweep", "erase-flick", "fire-flick"}) {
            const bool flick = scene.ends_with("flick");
            if (scene_filter.empty() ? flick : scene_filter != scene) continue;
            app.InitializeState();
            app.replay_mode_ = false;
            app.render_mode_ = PowderApp::RenderMode::Composite;
            app.brush_mode_ = scene == "erase-flick" ? PowderApp::BrushMode::Erase
                            : scene == "fire-flick"  ? PowderApp::BrushMode::Fire
                                                     : PowderApp::BrushMode::Water;
            app.brush_radius_ = scene == "stream" ? 8 : 24;
            app.stroke_ = {};
            app.cursor_trail_.clear();
            app.help_visible_ = false;
            app.help_hide_time_ = -1.0;
            FixedStepClock clock;
            struct Sample { double interval_ms; int ticks, readbacks, dispatches, cg_iterations, substeps; };
            std::vector<Sample> frames;
            std::array<double, PowderApp::kReplayQueryCount> pass_ms{};
            int pass_samples = 0;
            std::vector<std::array<double, 2>> water_fit;  // one-tick frames: CG iterations dispatched, water GPU ms
            std::vector<std::array<GLuint, 2>> solve_queries;
            std::vector<double> pressure_tick_ms, solve_ms, last_iterations;  // one-tick frames, solves, ticking frames
            int solves = 0, detail_ticks = 0;
            double next_live_sample = 20.0;
            solve_timer = detail ? &solve_queries : nullptr;
            double sim_time = 0.0;
            const double start = glfwGetTime();
            double previous = start;
            double window_start = start, window_worst = 0.0;
            std::size_t window_first = 0;
            long long window_cg = 0;
            while (sim_time < sim_limit && glfwGetTime() - start < wall_limit) {
                glfwPollEvents();
                // Scripted input in place of UpdateInput: the cursor over the simulation with the button held.
                if (scene == "stream") {
                    app.brush_x_ = 500;
                    app.brush_y_ = 900;
                } else if (flick) {
                    app.brush_x_ = frames.size() % 2 == 0 ? 100 : 900;
                    app.brush_y_ = 500;
                    app.cursor_trail_.push_back({app.brush_x_, app.brush_y_});
                } else {
                    // A horizontal sweep at 400 cells/s between x 200 and 800, reported as cursor events.
                    const double phase = std::fmod(sim_time * 400.0, 1200.0);
                    const int x = 200 + static_cast<int>(phase < 600.0 ? phase : 1200.0 - phase);
                    app.cursor_trail_.push_back({x, 850});
                    app.brush_x_ = x;
                    app.brush_y_ = 850;
                }
                app.cursor_in_sim_ = app.painting_ = true;
                const double now = glfwGetTime();
                const double elapsed = now - previous;
                previous = now;
                readbacks = dispatches = cg_iterations = 0;
                int substeps = 0;
                counting = true;
                const auto ticks_before = app.frame_index_;
                // Detail: every 20 simulated seconds, time each solver dispatch of one frame (perturbing).
                std::vector<std::array<GLuint, 3>> live_timed;
                const bool sample_live = detail && sim_time >= next_live_sample;
                if (sample_live) phase_timer = &live_timed;
                const int ticks = app.RunInteractiveFrame(clock, elapsed);
                phase_timer = nullptr;
                // POWDER_BENCH_CAPTURE=<prefix> also saves the last solve's system at that frame for benchmark-pressure
                // (POWDER_BENCH_PRESSURE_SNAPSHOT): mask, right-hand side, pressure, and the liquid level set after the
                // tick's last particle step, a nearby system rather than the solved one.
                const char* capture = std::getenv("POWDER_BENCH_CAPTURE");
                if (sample_live && capture != nullptr) {
                    constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
                    std::vector<GLubyte> mask(width * height);
                    std::vector<float> rhs(width * height), pressure(width * height), phi(width * height);
                    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                    for (const auto& [texture, values] : std::array<std::pair<GLuint, std::vector<float>*>, 3>{
                             {{app.CurrentWaterDivergence(), &rhs}, {app.CurrentWaterPressure(), &pressure},
                              {app.water_state_.liquid_phi, &phi}}}) {
                        glBindTexture(GL_TEXTURE_2D, texture);
                        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values->data());
                    }
                    glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_full);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                    const std::string path = std::string(capture) + "-" + scene + "-" + std::to_string(static_cast<int>(sim_time)) + ".bin";
                    std::ofstream file(path, std::ios::binary);
                    const std::array<std::int32_t, 2> dimensions{width, height};
                    file.write(reinterpret_cast<const char*>(dimensions.data()), sizeof(dimensions));
                    file.write(reinterpret_cast<const char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
                    for (const auto* values : {&rhs, &pressure, &phi})
                        file.write(reinterpret_cast<const char*>(values->data()), static_cast<std::streamsize>(values->size() * sizeof(float)));
                    Require(file.good(), "could not write the captured pressure system");
                    std::cout << "interactive " << scene << " captured " << path << '\n';
                }
                if (sample_live && !live_timed.empty()) {
                    next_live_sample += 20.0;
                    std::cout << std::fixed << std::setprecision(1) << "interactive " << scene << " live t=" << sim_time
                              << "s last-solve iterations=" << app.water_state_.pressure_iterations << " dispatched="
                              << app.water_state_.pressure_dispatched << " substeps=" << app.water_state_.substeps;
                    print_timed("", live_timed);
                }
                // A slower display: wait for the next refresh boundary after the swap, as vsync would.
                if (display_hz > 0.0) {
                    const double next = start + std::ceil((glfwGetTime() - start) * display_hz) / display_hz;
                    while (glfwGetTime() < next) std::this_thread::yield();
                }
                counting = false;
                Require(app.frame_index_ - ticks_before == static_cast<std::uint32_t>(ticks), "ticks must match the clock");
                substeps = app.water_state_.substeps;
                if (detail && ticks > 0) {
                    std::array<double, PowderApp::kReplayQueryCount> tick_ms{};
                    for (std::size_t i = 0; i < pass_ms.size(); ++i) {
                        GLuint64 value = 0;
                        glGetQueryObjectui64v(app.timing_queries_[i], GL_QUERY_RESULT, &value);
                        tick_ms[i] = static_cast<double>(value) / 1e6;
                        pass_ms[i] += tick_ms[i];
                    }
                    ++pass_samples;
                    const auto water_pass = static_cast<std::size_t>(PowderApp::TimedPass::Water);
                    if (ticks == 1) water_fit.push_back({static_cast<double>(cg_iterations), tick_ms[water_pass]});
                    double pressure = 0.0;
                    for (const auto& [first, last] : solve_queries) {
                        GLuint64 a = 0, b = 0;
                        glGetQueryObjectui64v(first, GL_QUERY_RESULT, &a);
                        glGetQueryObjectui64v(last, GL_QUERY_RESULT, &b);
                        glDeleteQueries(1, &first);
                        glDeleteQueries(1, &last);
                        solve_ms.push_back(static_cast<double>(b - a) / 1e6);
                        pressure += solve_ms.back();
                    }
                    solves += static_cast<int>(solve_queries.size());
                    detail_ticks += ticks;
                    solve_queries.clear();
                    if (ticks == 1) pressure_tick_ms.push_back(pressure);
                    last_iterations.push_back(app.water_state_.pressure_iterations);
                }
                sim_time += ticks * app.config_.fixed_dt;
                // The interval recorded for frame n is the time the user waited for it: from the previous frame's start.
                if (!frames.empty()) frames.back().interval_ms = elapsed * 1000.0;
                frames.push_back({0.0, ticks, readbacks, dispatches, cg_iterations, substeps});
                // A time series every five wall seconds shows whether frames degrade as the pool grows.
                if (now - window_start >= 5.0) {
                    std::cout << std::fixed << std::setprecision(1) << "interactive " << scene << " t=" << now - start
                              << "s sim=" << sim_time << "s FPS=" << (frames.size() - window_first) / (now - window_start)
                              << " worst ms=" << window_worst << " CG/frame="
                              << static_cast<double>(window_cg) / (frames.size() - window_first) << '\n';
                    window_start = now;
                    window_first = frames.size();
                    window_worst = 0.0;
                    window_cg = 0;
                }
                window_worst = std::max(window_worst, elapsed * 1000.0);
                window_cg += cg_iterations;
            }
            frames.back().interval_ms = (glfwGetTime() - previous) * 1000.0;
            solve_timer = nullptr;
            for (const auto& pair : solve_queries) glDeleteQueries(2, pair.data());
            const double wall = glfwGetTime() - start;
            CheckGl("interactive frame");
            const auto counters = app.ReadDebugCounters();
            const auto water = app.ReadWaterMetrics();
            // Skip the first half second of frames (window and shader warm-up).
            std::vector<Sample> measured(frames.begin() + std::min<std::size_t>(frames.size() - 1, 30), frames.end());
            std::vector<double> intervals;
            double tick_sum = 0, readback_sum = 0, dispatch_sum = 0, cg_sum = 0;
            int max_ticks = 0, max_cg = 0;
            std::array<int, 5> tick_histogram{};
            for (const Sample& s : measured) {
                intervals.push_back(s.interval_ms);
                tick_sum += s.ticks;
                readback_sum += s.readbacks;
                dispatch_sum += s.dispatches;
                cg_sum += s.cg_iterations;
                max_ticks = std::max(max_ticks, s.ticks);
                max_cg = std::max(max_cg, s.cg_iterations);
                ++tick_histogram[static_cast<std::size_t>(std::min(s.ticks, 4))];
            }
            std::sort(intervals.begin(), intervals.end());
            const double n = static_cast<double>(measured.size());
            const double mean_ms = std::accumulate(intervals.begin(), intervals.end(), 0.0) / n;
            const double p99_ms = intervals[std::min(intervals.size() - 1, static_cast<std::size_t>(std::ceil(n * 0.99)) - 1)];
            const double median_ms = intervals[intervals.size() / 2];
            std::cout << std::fixed << std::setprecision(2) << "interactive " << scene << " radius=" << app.brush_radius_
                      << " frames=" << frames.size() << " wall s=" << wall << " sim s=" << sim_time
                      << " sim/wall=" << sim_time / wall << '\n';
            std::cout << "interactive " << scene << " FPS mean=" << 1000.0 / mean_ms << " median=" << 1000.0 / median_ms
                      << " p1-low=" << 1000.0 / p99_ms << " worst-frame ms=" << intervals.back()
                      << " frame ms mean/median/p99=" << mean_ms << '/' << median_ms << '/' << p99_ms << '\n';
            std::cout << "interactive " << scene << " ticks/frame mean/max=" << tick_sum / n << '/' << max_ticks
                      << " histogram 0:" << tick_histogram[0] << " 1:" << tick_histogram[1] << " 2:" << tick_histogram[2]
                      << " 3:" << tick_histogram[3] << " 4+:" << tick_histogram[4]
                      << " readbacks/frame=" << readback_sum / n << " dispatches/frame=" << dispatch_sum / n
                      << " CG iterations dispatched/frame mean/max=" << cg_sum / n << '/' << max_cg
                      << " last water substeps=" << frames.back().substeps << '\n';
            std::cout << "interactive " << scene << " water particles=" << water.count << " mass=" << water.mass
                      << " debug=" << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '/'
                      << counters[4] << '\n';
            if (pass_samples > 0) {
                std::cout << "interactive " << scene << " detail last-tick GPU ms spawn/boundary/water/gas/sand/coupling/extract=";
                for (double total : pass_ms) std::cout << total / pass_samples << '/';
                std::cout << '\n';
            }
            if (!pressure_tick_ms.empty()) {
                const auto summary = [](std::vector<double> values) {
                    std::sort(values.begin(), values.end());
                    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
                    std::ostringstream text;
                    text << mean << '/' << values[values.size() / 2] << '/' << values[std::min(values.size() - 1, values.size() * 99 / 100)]
                         << '/' << values.back();
                    return text.str();
                };
                std::cout << "interactive " << scene << " detail pressure GPU ms per one-tick frame mean/median/p99/max="
                          << summary(pressure_tick_ms) << " per solve=" << summary(solve_ms)
                          << " solves per tick=" << static_cast<double>(solves) / std::max(1, detail_ticks)
                          << " last-solve iterations=" << summary(last_iterations) << '\n';
            }
            if (water_fit.size() > 2) {
                // Least squares water GPU ms = base + per_iteration * CG iterations over one-tick frames.
                double sx = 0, sy = 0, sxx = 0, sxy = 0, worst = 0;
                for (const auto& [x, y] : water_fit) {
                    sx += x;
                    sy += y;
                    sxx += x * x;
                    sxy += x * y;
                    worst = std::max(worst, y);
                }
                const double m = static_cast<double>(water_fit.size());
                const double slope = (m * sxy - sx * sy) / std::max(1e-12, m * sxx - sx * sx);
                std::cout << "interactive " << scene << " detail water GPU ms per CG iteration=" << slope
                          << " base=" << (sy - slope * sx) / m << " worst one-tick water ms=" << worst << '\n';
            }
            // A held brush can fill the particle pool; overflow_blocked (counter 1) is reported above, not a solver failure.
            if (counters[0] + counters[2] + counters[3] != 0) failures << " [" << scene << " failure counters]";
        }
        glfwSwapInterval(0);
        glfwHideWindow(app.window_);
        app.InitializeState();
        Require(failures.str().empty(), failures.str());
    }

    static void SandSubsteps(PowderApp& app) {
        app.config_ = {};
        app.InitializeState();
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static int dispatches = 0;
        struct RestoreDispatch {
            PFNGLDISPATCHCOMPUTEPROC saved;
            ~RestoreDispatch() { glad_glDispatchCompute = saved; }
        } restore{glad_glDispatchCompute};
        native_dispatch = restore.saved;
        dispatches = 0;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            ++dispatches;
            native_dispatch(x, y, z);
        };
        WriteParticles(app, app.sand_state_.particles, SandRecords({Particle{500.5F, 500.5F, 0, 0, 1, 1}}));
        app.RunSandPass(1.0F / 60.0F);
        // Three substep passes each, then compaction, the final P2G and the raster, plus the sleep tally and two pack phases.
        // Still sand: the stress-wave limit alone, ceil(dt sqrt((lambda + 2 mu) / rho) / CFL) = 46 (it was 23 at the 196000 moduli).
        Require(app.sand_state_.substeps == 46 && dispatches == 3 * 46 + 6,
                "default sand step must dispatch 46 complete material substeps");
        const auto rejected_without_dispatch = [&]() {
            dispatches = 0;
            bool rejected = false;
            try {
                app.RunSandPass(1.0F / 60.0F);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("Sand timestep") != std::string::npos;
            }
            Require(rejected && dispatches == 0, "sand wave-speed budget must reject before GPU dispatch");
        };
        // The budget must hold the advective limit at the speed cap (40 substeps) before anything is dispatched.
        app.config_.sand_max_substeps = 39;
        rejected_without_dispatch();
        app.config_ = {};
        app.config_.sand_shear_modulus = std::numeric_limits<float>::max();
        app.config_.sand_lame_lambda = std::numeric_limits<float>::max();
        app.config_.sand_reference_density = std::numeric_limits<float>::min();
        app.config_.sand_cfl_limit = std::numeric_limits<float>::min();
        rejected_without_dispatch();
        app.config_ = {};
        int material_failures = 0;
        const auto reject_material = [&](float mu, float lambda, float density, float alpha, const char* label) {
            app.config_ = {};
            app.config_.sand_shear_modulus = mu;
            app.config_.sand_lame_lambda = lambda;
            app.config_.sand_reference_density = density;
            app.config_.sand_friction_alpha = alpha;
            dispatches = 0;
            bool rejected = false;
            try { app.RunSandPass(1.0F / 60.0F); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("Sand material coefficients") != std::string::npos;
            }
            if (!rejected || dispatches != 0) {
                ++material_failures;
                std::cerr << "FAIL material preflight: " << label << " rejected=" << rejected
                          << " dispatches=" << dispatches << '\n';
            }
        };
        reject_material(3.0e38F, 0.0F, 3.0e38F, 0.5F, "twice shear modulus");
        reject_material(1.0e-37F, 12000.0F, 2.0F, 0.5F, "yield ratio");
        reject_material(1.0e38F, 3.0e38F, 3.0e38F, 0.5F, "Lame sum");
        reject_material(1.0F, 1.0e20F, 1.0e20F, 1.0e20F, "yield slope");
        for (float scale : {1.0e-30F, 1.0e38F}) {
            app.config_ = {};
            app.InitializeState();
            WriteParticles(app, app.sand_state_.particles, SandRecords({Particle{500.5F, 500.5F, 0, 0, 1, 1}}));
            app.config_.sand_shear_modulus = scale;
            app.config_.sand_lame_lambda = scale;
            app.config_.sand_reference_density = scale;
            app.config_.sand_friction_alpha = 0.0F;
            app.config_.sand_viscosity = 0.0F;  // its diffusion limit grows as 1/density; this case isolates the elastic one
            dispatches = 0;
            app.RunSandPass(1.0F / 60.0F);
            // Wave speed sqrt(3) cells/s and no grain moving: one substep (the advective limit only binds moving sand).
            Require(app.sand_state_.substeps == 1 && dispatches == 3 * 1 + 6,
                    "representable extreme material coefficients must remain accepted");
        }
        app.config_ = {};
        app.InitializeState();
        app.config_.sand_shear_modulus = 1.0e-30F;
        WriteParticles(app, app.sand_state_.particles, SandRecords({Particle{500.5F, 500.5F, 0, 0, 1, 1}}));
        dispatches = 0;
        app.RunSandPass(1.0F / 60.0F);
        Require(app.sand_state_.substeps == 27 && dispatches == 3 * 27 + 6,  // wave speed sqrt(784000 / 2) = 626 cells/s
                "large finite yield ratio must remain accepted");
        int force_failures = 0;
        for (float gravity : {-2.0e8F, 2.0e8F, -6.0e7F, 6.0e7F}) {  // 2e8 / 60 / 46 substeps passes 65504
            app.config_ = {};
            app.InitializeState();
            app.config_.sand_gravity = gravity;
            WriteParticles(app, app.sand_state_.particles, SandRecords({Particle{500.5F, 500.5F, 0, 0, 1, 1}}));
            dispatches = 0;
            bool rejected = false;
            try { app.RunSandPass(1.0F / 60.0F); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("sand_gravity") != std::string::npos;
            }
            const bool unsafe = std::abs(gravity) == 2.0e8F;
            const auto particle = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, 1)[0];
            const auto counters = app.ReadDebugCounters();
            std::cout << "sand gravity=" << gravity << " rejected=" << rejected << " dispatches=" << dispatches
                      << " alive/mass=" << particle[4] << '/' << particle[5] << " position=" << particle[0] << '/' << particle[1]
                      << " velocity=" << particle[2] << '/' << particle[3] << " debug="
                      << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '\n';
            if (unsafe && !rejected) {
                // Record the grid outcome when the velocity bound was not rejected up front.
                std::vector<float> grid(static_cast<std::size_t>((PowderApp::kGridWidth + 2) * (PowderApp::kGridHeight + 2) * 2));
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, app.sand_state_.grid_velocity);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, grid.data());
                float peak = 0.0F;
                int nonfinite = 0;
                for (float value : grid) {
                    if (!std::isfinite(value)) ++nonfinite;
                    else peak = std::max(peak, std::abs(value));
                }
                std::cout << "  unchecked sand grid peak=" << peak << " nonfinite=" << nonfinite << '\n';
            }
            const bool valid = unsafe ? rejected && dispatches == 0 && particle[0] == 500.5F && particle[1] == 500.5F :
                !rejected && dispatches == 3 * 46 + 6 && std::all_of(particle.begin(), particle.end(), [](float x) { return std::isfinite(x); });
            // state1.x is 1 plus the sleep timer (sand_sleep.comp), so a live still grain reads 2.
            if (!valid || particle[4] < 1.0F || particle[5] != 1.0F || counters != std::array<GLuint, 5>{}) {
                ++force_failures;
                std::cerr << "FAIL sand force preflight for gravity=" << gravity << '\n';
            }
        }
        int additive_failures = 0;
        for (float sign : {-1.0F, 1.0F}) {
            constexpr float dt = 0.00025F;  // one wave substep at the default moduli
            app.config_ = {};
            app.InitializeState();
            app.config_.sand_damping = 1.0F;
            app.config_.sand_drag = 0.0F;
            app.config_.sand_gravity = -sign * (65400.0F / dt);
            const float initial_velocity = sign * 280.0F;
            const float impulse = -app.config_.sand_gravity * dt;
            const float expected = initial_velocity + impulse;
            WriteParticles(app, app.sand_state_.particles,
                           SandRecords({Particle{500.5F, 500.5F, 0, initial_velocity, 1, 1}}));
            dispatches = 0;
            bool rejected = false;
            try { app.RunSandPass(dt); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("sand_gravity") != std::string::npos;
            }
            const auto counters = app.ReadDebugCounters();
            const auto particle = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, 1)[0];
            float stored = 0.0F;
            if (!rejected) {
                Require(app.sand_state_.substeps == 1 && dispatches == 9,
                        "sand additive range fixture requires one complete material substep");
                // One step retains this exact centered-particle node after its grid update.
                std::vector<float> grid(static_cast<std::size_t>((PowderApp::kGridWidth + 2) * (PowderApp::kGridHeight + 2) * 2));
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, app.sand_state_.grid_velocity);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, grid.data());
                stored = grid[static_cast<std::size_t>((501 * (PowderApp::kGridWidth + 2) + 501) * 2 + 1)];
            }
            std::cout << "sand additive initial/impulse/expected/stored=" << initial_velocity << '/' << impulse
                      << '/' << expected << '/' << stored << " rejected=" << rejected << " dispatches=" << dispatches
                      << " alive/mass=" << particle[4] << '/' << particle[5] << " debug="
                      << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '\n';
            const bool reported = (rejected && dispatches == 0) || counters[0] > 0;
            if (!reported || particle[4] < 1.0F || particle[5] != 1.0F) ++additive_failures;
        }
        app.config_ = {};
        std::cout << "sand material substeps=20 dispatches=63; budget/extreme guards, material failures="
                  << material_failures << ", force failures=" << force_failures << '\n';
        Require(material_failures == 0, "sand derived material coefficients must reject before GPU dispatch");
        Require(force_failures == 0, "sand unrepresentable gravity must reject before GPU dispatch");
        Require(additive_failures == 0, "finite sand force beyond the grid velocity bound must reject or report");
    }

    static void BenchmarkCoupling(PowderApp& app) {
        constexpr int width = PowderApp::kGasWidth, height = PowderApp::kGasHeight;
        constexpr int warmup = 16, samples = 48;
        const auto original_config = app.config_;
        struct Queries {
            std::array<GLuint, 2> ids{};
            Queries() { glGenQueries(2, ids.data()); }
            ~Queries() { glDeleteQueries(2, ids.data()); }
        } queries;
        std::cout << "coupling benchmark renderer=" << glGetString(GL_RENDERER)
                  << " full=1000x1000 gas=500x500 warmup=" << warmup << " samples=" << samples
                  << " GPU timestamps; setup/readback excluded\n";
        for (int scene = 0; scene < 3; ++scene) {
            app.config_ = {};
            app.InitializeState();
            if (scene != 0) {
                const int full_size = scene == 1 ? 64 : PowderApp::kGridWidth;
                const int origin = scene == 1 ? 400 : 0;
                const std::vector<float> water(static_cast<std::size_t>(full_size * full_size), 0.01F);
                glBindTexture(GL_TEXTURE_2D, app.water_state_.liquid_volume);
                glTexSubImage2D(GL_TEXTURE_2D, 0, origin, origin, full_size, full_size, GL_RED, GL_FLOAT, water.data());
                const int gas_size = full_size / 2;
                const std::vector<float> scalars(static_cast<std::size_t>(gas_size * gas_size), 0.5F);
                for (GLuint texture : {app.CurrentTemperature(), app.CurrentFuel(), app.CurrentOxidizer()}) {
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, origin / 2, origin / 2, gas_size, gas_size,
                                    GL_RED, GL_FLOAT, scalars.data());
                }
                std::vector<float> reactions(static_cast<std::size_t>(gas_size * gas_size * 2), 0.0F);
                for (std::size_t i = 1; i < reactions.size(); i += 2) reactions[i] = 0.5F;
                glBindTexture(GL_TEXTURE_2D, app.CurrentReactionRate());
                glTexSubImage2D(GL_TEXTURE_2D, 0, origin / 2, origin / 2, gas_size, gas_size,
                                GL_RG, GL_FLOAT, reactions.data());
            }
            for (int i = 0; i < warmup; ++i) app.RunCouplingPass(1.0F / 60.0F);
            std::vector<double> timings;
            for (int i = 0; i < samples; ++i) {
                glQueryCounter(queries.ids[0], GL_TIMESTAMP);
                app.RunCouplingPass(1.0F / 60.0F);
                glQueryCounter(queries.ids[1], GL_TIMESTAMP);
                std::array<GLuint64, 2> stamps{};
                for (std::size_t j = 0; j < stamps.size(); ++j)
                    glGetQueryObjectui64v(queries.ids[j], GL_QUERY_RESULT, &stamps[j]);
                Require(stamps[1] > stamps[0], "coupling timestamps must advance");
                timings.push_back(static_cast<double>(stamps[1] - stamps[0]) / 1000000.0);
            }
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            for (GLuint texture : {app.CurrentTemperature(), app.CurrentReactionRate(), app.CurrentOxidizer(), app.CurrentFuel()}) {
                const bool reaction = texture == app.CurrentReactionRate();
                std::vector<float> values(static_cast<std::size_t>(width * height * (reaction ? 2 : 1)));
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, reaction ? GL_RG : GL_RED, GL_FLOAT, values.data());
                Require(std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }),
                        "coupling benchmark output must be finite");
            }
            const double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / samples;
            std::sort(timings.begin(), timings.end());
            std::cout << "coupling scene=" << (scene == 0 ? "idle" : scene == 1 ? "sparse" : "dense")
                      << " mode=direct mean_ms=" << mean << " median_ms=" << timings[samples / 2]
                      << " p95_ms=" << timings[45] << '\n';
            CheckGl("coupling benchmark");
        }
        app.config_ = original_config;
    }

    static void CouplingLifecycle(PowderApp& app) {
        constexpr int width = PowderApp::kGasWidth, height = PowderApp::kGasHeight;
        constexpr float dt = 1.0F / 60.0F;
        const auto original_config = app.config_;
        const auto original_brush = app.brush_mode_;
        const int original_x = app.brush_x_, original_y = app.brush_y_, original_radius = app.brush_radius_;
        std::ostringstream failures;
        const auto expect = [&](bool condition, const std::string& message) {
            if (!condition) failures << " [" << message << ']';
        };
        const auto seed = [&](int x, int y, float water, float scalar) {
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    WriteScalar(app.water_state_.liquid_volume, water, 2 * x + dx, 2 * y + dy);
            WriteScalar(app.CurrentTemperature(), scalar, x, y);
            WriteScalar(app.CurrentFuel(), scalar, x, y);
            WriteScalar(app.CurrentOxidizer(), 0.5F, x, y);
            const std::array<float, 2> reaction{0.0F, scalar};
            glBindTexture(GL_TEXTURE_2D, app.CurrentReactionRate());
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RG, GL_FLOAT, reaction.data());
        };
        const auto read = [](GLuint texture, int count, GLenum format) {
            std::vector<float> values(static_cast<std::size_t>(count));
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, format, GL_FLOAT, values.data());
            return values;
        };
        const auto snapshot = [&]() {
            return std::array<std::vector<float>, 4>{
                read(app.CurrentTemperature(), width * height, GL_RED),
                read(app.CurrentReactionRate(), width * height * 2, GL_RG),
                read(app.CurrentOxidizer(), width * height, GL_RED),
                read(app.CurrentFuel(), width * height, GL_RED)};
        };
        for (int scenario = 0; scenario < 3; ++scenario) {
            app.config_ = {};
            app.InitializeState();
            std::array<std::vector<float>, 4> before;
            std::vector<float> water;
            if (scenario == 0) {
                seed(128, 128, 0.05F, 0.5F);
                seed(159, 128, 0.002F, 0.005F);
                seed(160, 128, 0.002F, 0.005F);
            } else if (scenario == 1) {
                app.config_.water_gravity = 0.0F;
                app.config_.water_velocity_damping = app.config_.water_flip_blend = 1.0F;
                app.config_.water_bulk_damping = 0.0F;
                app.config_.gas_buoyancy_temperature = app.config_.gas_buoyancy_smoke = 0.0F;
                app.config_.gas_velocity_dissipation = app.config_.gas_scalar_dissipation = 1.0F;
                app.config_.gas_combustion_rate = app.config_.gas_temperature_diffusion = 0.0F;
                app.config_.gas_reaction_decay = app.config_.gas_temperature_cooling = 0.0F;
                WriteParticles(app, app.water_state_.particles,
                               std::vector<Particle>{{511.875F, 511.5F, 12.0F, 0.0F, 1.0F, 1.0F}});
                app.RunWaterPass(0.0F);
                seed(255, 255, 0.05F, 0.5F);
                const std::array<float, 2> gas_velocity{6.0F, 0.0F};
                for (int y = 253; y <= 257; ++y) {
                    for (int x = 253; x <= 257; ++x) {
                        WriteScalar(app.CurrentGasU(), 6.0F, x, y);
                        glBindTexture(GL_TEXTURE_2D, app.CurrentGasVelocityCenter());
                        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RG, GL_FLOAT, gas_velocity.data());
                    }
                }
                app.RunWaterPass(dt);
                app.RunGasPass(dt);
                expect(ReadParticles(app.water_state_.particles, 1)[0][0] >= 512.0F,
                       "water must cross the cell edge during RunWaterPass");
                expect(ReadScalar(app.CurrentTemperature(), 256, 255) > 0.0F,
                       "gas must transport heat into the next cell during RunGasPass");
            } else {
                seed(256, 256, 0.0F, 0.005F);
                app.config_.water_gravity = 0.0F;
                app.config_.gas_buoyancy_temperature = app.config_.gas_buoyancy_smoke = 0.0F;
                app.brush_mode_ = PowderApp::BrushMode::Water;
                app.brush_x_ = app.brush_y_ = 512;
                app.brush_radius_ = 0;
                PowderApp::BrushConfig brush;
                brush.spawn_water = true;
                brush.water_particles = 1;
                static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
                static std::function<void()> capture;
                struct RestoreDispatch {
                    PFNGLDISPATCHCOMPUTEPROC saved;
                    ~RestoreDispatch() { glad_glDispatchCompute = saved; capture = {}; }
                } restore{glad_glDispatchCompute};
                native_dispatch = restore.saved;
                int captured = 0;
                capture = [&]() {
                    GLint program = 0;
                    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                    if (static_cast<GLuint>(program) == app.coupling_fire_program_) {
                        before = snapshot();
                        water = read(app.water_state_.liquid_volume, PowderApp::CellCount(), GL_RED);
                        ++captured;
                    }
                };
                glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) { capture(); native_dispatch(x, y, z); };
                app.RunFrame(dt, &brush);
                Require(captured == 1, "RunFrame must execute one real coupling pass");
                expect(ReadParticles(app.water_state_.particles, 1)[0][4] > 0.5F &&
                           ReadScalar(app.water_state_.liquid_volume, 512, 512) > 0.0F,
                       "RunFrame must spawn water into initially dry weak gas");
            }
            if (scenario != 2) {
                before = snapshot();
                water = read(app.water_state_.liquid_volume, PowderApp::CellCount(), GL_RED);
                app.RunCouplingPass(dt);
            }
            const auto actual = snapshot();
            const std::array<const char*, 4> names{"temperature", "reaction", "oxidizer", "fuel"};
            std::array<std::size_t, 4> mismatches{};
            std::array<bool, 4> changed{};
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int full = 2 * y * PowderApp::kGridWidth + 2 * x;
                    const float liquid = 0.25F * (water[full] + water[full + 1] +
                        water[full + PowderApp::kGridWidth] + water[full + PowderApp::kGridWidth + 1]);
                    const float suppression = std::clamp(liquid * app.config_.gas_water_cooling, 0.0F, 1.0F);
                    const std::array<float, 4> retention{
                        1.0F - std::clamp(liquid * app.config_.gas_water_cooling * 0.7F, 0.0F, 1.0F),
                        std::max(1.0F - suppression * 2.2F, 0.0F),
                        std::max(1.0F - suppression * 1.2F, 0.0F), 1.0F - suppression * 0.85F};
                    const std::size_t cell = static_cast<std::size_t>(y * width + x);
                    for (std::size_t field = 0; field < actual.size(); ++field) {
                        const std::size_t i = field == 1 ? 2 * cell + 1 : cell;
                        const float expected = before[field][i] * retention[field];
                        // One R16F output store: use a value-scaled ULP bound so weak-tail misses cannot hide.
                        const float ulp = std::ldexp(1.0F, expected >= std::ldexp(1.0F, -14) ? std::ilogb(expected) - 10 : -24);
                        if (!std::isfinite(actual[field][i]) || std::abs(actual[field][i] - expected) > 1.1F * ulp + 1.0e-7F)
                            ++mismatches[field];
                        changed[field] = changed[field] || actual[field][i] != before[field][i];
                    }
                    if (actual[1][2 * cell] != before[1][2 * cell]) ++mismatches[1];
                }
            }
            for (std::size_t field = 0; field < actual.size(); ++field) {
                const std::string label = "scenario " + std::to_string(scenario) + " " + names[field];
                std::cout << "coupling lifecycle " << label << " mismatches=" << mismatches[field] << '\n';
                expect(changed[field] && mismatches[field] == 0, label + " must match consumed-water cooling");
            }
            CheckGl("coupling lifecycle");
        }
        app.config_ = original_config;
        app.brush_mode_ = original_brush;
        app.brush_x_ = original_x;
        app.brush_y_ = original_y;
        app.brush_radius_ = original_radius;
        Require(failures.str().empty(), failures.str());
    }

    // Not a CTest case. The fraction of 16x16 gas tiles whose scalar fields are not the trivial state (smoke, temperature,
    // fuel, flame and burn rate zero, oxidizer 1 in open cells and 0 in blocked ones), alone and dilated by one tile, in an
    // idle world, after a small fire held for 1.5 s and under the benchmark's fire and smoke source: the share of the
    // transport passes a tile-skipping scheme could have skipped.
    static void GasActivity(PowderApp& app) {
        constexpr int w = PowderApp::kGasWidth, h = PowderApp::kGasHeight, tiles = (w + 15) / 16;
        app.config_ = powder_config::LoadConfigFromEnvironment();
        const auto read = [&](GLuint tex, GLenum format, GLenum type, std::size_t count, std::size_t bytes) {
            std::vector<unsigned char> v(count * bytes);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glBindTexture(GL_TEXTURE_2D, tex);
            glGetTexImage(GL_TEXTURE_2D, 0, format, type, v.data());
            return v;
        };
        for (const std::string scene : {"idle", "sparse", "full"}) {
            app.InitializeState();
            app.replay_mode_ = true;
            double own_sum = 0, dilated_sum = 0;
            int samples = 0;
            for (int tick = 0; tick < 600; ++tick) {
                PowderApp::BrushConfig source{};
                const PowderApp::BrushConfig* brush = nullptr;
                if (scene == "full" || (scene == "sparse" && tick < 90)) {
                    app.brush_mode_ = scene == "full" ? (tick % 2 == 0 ? PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke)
                                                      : PowderApp::BrushMode::Fire;
                    app.brush_x_ = 500;
                    app.brush_y_ = scene == "full" ? 120 : 60;
                    app.brush_radius_ = scene == "full" ? 18 : 6;
                    source = app.BuildBrushConfig();
                    brush = &source;
                }
                app.RunFrame(app.config_.fixed_dt, brush, true);
                if (tick < 30 || tick % 6 != 0) continue;
                const auto fields = {app.CurrentSmokeDen(), app.CurrentTemperature(), app.CurrentFuel(), app.CurrentOxidizer()};
                std::vector<std::vector<unsigned char>> half;
                for (GLuint tex : fields) half.push_back(read(tex, GL_RED, GL_HALF_FLOAT, w * h, 2));
                const auto reaction = read(app.CurrentReactionRate(), GL_RG, GL_HALF_FLOAT, w * h, 4);
                const auto mask = read(app.boundary_state_.mask_gas, GL_RED_INTEGER, GL_UNSIGNED_BYTE, w * h, 1);
                std::vector<char> own(tiles * tiles, 0);
                const auto h16 = [](const std::vector<unsigned char>& v, std::size_t i) {
                    return static_cast<unsigned>(v[2 * i]) | static_cast<unsigned>(v[2 * i + 1]) << 8;
                };
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y * w + x);
                        const bool open = mask[i] == 0;
                        const unsigned ox = h16(half[3], i);  // 0x3c00 is half 1.0
                        bool nontrivial = h16(half[0], i) != 0 || h16(half[1], i) != 0 || h16(half[2], i) != 0 ||
                                          h16(reaction, 2 * i) != 0 || h16(reaction, 2 * i + 1) != 0 ||
                                          (open ? ox != 0x3c00U : ox != 0U);
                        if (nontrivial) own[static_cast<std::size_t>((y / 16) * tiles + x / 16)] = 1;
                    }
                int active = 0, dilated = 0;
                for (int ty = 0; ty < tiles; ++ty)
                    for (int tx = 0; tx < tiles; ++tx) {
                        active += own[static_cast<std::size_t>(ty * tiles + tx)];
                        bool any = false;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = tx + dx, ny = ty + dy;
                                if (nx >= 0 && ny >= 0 && nx < tiles && ny < tiles) any = any || own[static_cast<std::size_t>(ny * tiles + nx)] != 0;
                            }
                        dilated += any ? 1 : 0;
                    }
                own_sum += active / double(tiles * tiles);
                dilated_sum += dilated / double(tiles * tiles);
                ++samples;
            }
            std::cout << "gas activity " << scene << ": tiles nontrivial " << own_sum / samples << ", with one-tile dilation "
                      << dilated_sum / samples << " over " << samples << " samples\n";
        }
        app.replay_mode_ = false;
    }

    static void GasDiffusionSubsteps(PowderApp& app) {
        app.config_ = {};
        // Isolate diffusion: at the default 3 m/s cap, transport alone needs two substeps.
        app.config_.gas_max_velocity = 100.0F;
        app.InitializeState();
        static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
        static int dispatches = 0;
        struct RestoreDispatch {
            PFNGLDISPATCHCOMPUTEPROC saved;
            ~RestoreDispatch() { glad_glDispatchCompute = saved; }
        } restore{glad_glDispatchCompute};
        native_dispatch = restore.saved;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            ++dispatches;
            native_dispatch(x, y, z);
        };
        dispatches = 0;
        app.RunGasPass(1.0F / 64.0F);
        const int single_step_dispatches = dispatches;
        Require(app.gas_state_.substeps == 1 && single_step_dispatches > 0,
                "default gas diffusion must need one real substep");
        int failures = 0;
        const auto check = [&](bool passed, const char* message) {
            if (!passed) { ++failures; std::cerr << "FAIL diffusion: " << message << '\n'; }
        };
        app.config_.gas_temperature_diffusion = 160.0F;
        app.config_.gas_max_substeps = 3;
        dispatches = 0;
        app.RunGasPass(1.0F / 64.0F);
        check(app.gas_state_.substeps == 3 && dispatches == 3 * single_step_dispatches,
              "D*dt=2.5 must execute three complete gas substeps");
        const auto rejected_without_dispatch = [&]() {
            dispatches = 0;
            bool rejected = false;
            try { app.RunGasPass(1.0F / 64.0F); }
            catch (const std::runtime_error& error) {
                const std::string message = error.what();
                rejected = message.find("POWDER_GAS_TEMPERATURE_DIFFUSION") != std::string::npos &&
                           message.find("POWDER_GAS_MAX_SUBSTEPS") != std::string::npos;
            }
            check(rejected && dispatches == 0, "diffusion budget must reject before dispatch and name both settings");
        };
        app.config_.gas_max_substeps = 2;
        rejected_without_dispatch();
        app.config_.gas_temperature_diffusion = std::numeric_limits<float>::max();
        rejected_without_dispatch();
        app.config_ = {};
        CheckGl("gas diffusion substeps");
        std::cout << "gas diffusion default=1, D*dt=2.5 needs 3; budget/extreme rejection failures=" << failures << '\n';
        Require(failures == 0, "gas diffusion timestep regressions failed");
    }

    static void GasQuenching(PowderApp& app, bool omit_water) {
        const auto scenes = app.BuildReplayScenes();
        const auto found = std::find_if(scenes.begin(), scenes.end(), [](const auto& scene) { return scene.name == "gas"; });
        Require(found != scenes.end(), "gas replay scene must exist");
        auto scene = *found;
        if (omit_water) {
            std::erase_if(scene.events, [](const auto& event) { return event.mode == PowderApp::BrushMode::Water; });
        }
        std::cout << "gas quenching " << (omit_water ? "no-water control" : "production replay geometry") << '\n';
        app.replay_mode_ = true;
        app.InitializeState();
        const auto read = [](GLuint texture, int width, int height, int channels) {
            std::vector<float> values(static_cast<std::size_t>(width * height * channels));
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, channels == 2 ? GL_RG : GL_RED, GL_FLOAT, values.data());
            return values;
        };
        double maximum_formula_error = 0.0;
        double maximum_wet_reaction = 0.0, maximum_coupling_reduction = 0.0;
        double final_reaction = 0.0, poured_reaction = 0.0;
        GLuint expected_particles = 0;
        bool ignited = false;
        for (int frame = 0; frame < scene.total_frames; ++frame) {
            glfwPollEvents();
            ++app.frame_index_;
            const GLuint zero = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            for (const auto& event : scene.events) {
                if (frame < event.frame || frame >= event.frame + event.hold_frames) continue;
                app.ApplyReplayBrush(event);
                const auto brush = app.BuildBrushConfig();
                if (brush.spawn_water) {
                    GLuint source_cells = 0;
                    for (int y = -event.radius; y <= event.radius; ++y)
                        for (int x = -event.radius; x <= event.radius; ++x)
                            if (x * x + y * y <= event.radius * event.radius) ++source_cells;
                    expected_particles += source_cells * static_cast<GLuint>(brush.water_particles);
                    Require(expected_particles <= static_cast<GLuint>(PowderApp::kMaxWaterParticles),
                            "gas quench source must stay within the existing particle capacity");
                }
                app.RunSpawnPass(&brush);
            }
            app.RunBoundaryPass();
            app.RunWaterPass(app.config_.fixed_dt);
            app.RunGasPass(app.config_.fixed_dt);
            app.RunSandPass(app.config_.fixed_dt);
            const bool sample = frame == 119 || frame == 120 || frame == 143 || frame == 144 ||
                                frame == 179 || frame == 180 || frame == 219 || frame == 239;
            std::vector<float> before, water;
            if (sample) {
                before = read(app.CurrentReactionRate(), PowderApp::kGasWidth, PowderApp::kGasHeight, 2);
                water = read(app.water_state_.liquid_volume, PowderApp::kGridWidth, PowderApp::kGridHeight, 1);
            }
            app.RunCouplingPass(app.config_.fixed_dt);
            const double reaction = app.ReadReactionTextureSum(app.CurrentReactionRate(), PowderApp::kGasWidth, PowderApp::kGasHeight);
            // Fire now burns out about 0.5 s after its brush stops (frame 144), so compare the flame just before the pour
            // (frame 119) with the flame the still-burning brush keeps up 23 frames into it.
            if (frame == 119) poured_reaction = reaction;
            if (frame == 143) final_reaction = reaction;
            if (frame < 120 && reaction >= 2.5) ignited = true;
            if (sample) {
                const auto after = read(app.CurrentReactionRate(), PowderApp::kGasWidth, PowderApp::kGasHeight, 2);
                double pre_sum = 0.0, wet_reaction = 0.0, predicted_sum = 0.0, center_x = 0.0, center_y = 0.0;
                int min_x = PowderApp::kGridWidth, min_y = PowderApp::kGridHeight, max_x = -1, max_y = -1;
                for (int y = 0; y < PowderApp::kGasHeight; ++y) for (int x = 0; x < PowderApp::kGasWidth; ++x) {
                    const auto gas = static_cast<std::size_t>(y * PowderApp::kGasWidth + x);
                    const auto full = static_cast<std::size_t>(2 * y * PowderApp::kGridWidth + 2 * x);
                    const float wet = 0.25F * (water[full] + water[full + 1] +
                                               water[full + PowderApp::kGridWidth] + water[full + PowderApp::kGridWidth + 1]);
                    const float suppression = std::clamp(wet * app.config_.gas_water_cooling, 0.0F, 1.0F);
                    const double expected = before[2 * gas + 1] *
                        std::pow(std::max(1.0F - suppression * 2.2F, 0.0F), app.config_.fixed_dt * 60.0F);
                    maximum_formula_error = std::max(maximum_formula_error, std::abs(after[2 * gas + 1] - expected));
                    pre_sum += before[2 * gas + 1];
                    if (wet > 0.0F) wet_reaction += before[2 * gas + 1];
                    predicted_sum += expected;
                    center_x += after[2 * gas + 1] * (2 * x + 1);
                    center_y += after[2 * gas + 1] * (2 * y + 1);
                    if (after[2 * gas + 1] > 0.001F) {
                        min_x = std::min(min_x, 2 * x); max_x = std::max(max_x, 2 * x + 1);
                        min_y = std::min(min_y, 2 * y); max_y = std::max(max_y, 2 * y + 1);
                    }
                }
                const auto particles = app.ReadWaterMetrics();
                maximum_wet_reaction = std::max(maximum_wet_reaction, wet_reaction);
                maximum_coupling_reduction = std::max(maximum_coupling_reduction, pre_sum - reaction);
                // Water touching the flame evaporates into steam: an explicit, counted sink.
                const GLuint kept = expected_particles - app.ReadPhaseSinks()[0];
                Require(particles.count == kept && std::abs(particles.mass - kept * app.config_.water_particle_mass) <= 0.001,
                        "gas quench source must conserve the independently counted particle mass less the evaporated water");
                std::cout << "gas replay frame=" << frame << " reaction before/wet/predicted/after=" << pre_sum << '/'
                          << wet_reaction << '/' << predicted_sum << '/' << reaction
                          << " reaction bounds=" << min_x << ',' << min_y << ':' << max_x << ',' << max_y
                          << " center=" << center_x / std::max(reaction, 1.0e-12) << ',' << center_y / std::max(reaction, 1.0e-12)
                          << " water mass=" << particles.mass << " evaporated=" << expected_particles - kept
                          << " max coupling rounding=" << maximum_formula_error << '\n';
            }
            // The first four counters are failures; a splash faster than the water speed cap only counts in the fifth.
            const auto counters = app.ReadDebugCounters();
            Require(counters[0] == 0 && counters[1] == 0 && counters[2] == 0 && counters[3] == 0,
                    "gas quenching must keep clean diagnostics");
        }
        Require(ignited, "gas replay must actually ignite before quenching");
        Require(omit_water ? maximum_wet_reaction == 0.0 && maximum_coupling_reduction == 0.0 :
                            maximum_wet_reaction >= 2.5 && maximum_coupling_reduction >= 2.5,
                "extinction must include measured water/reaction overlap and actual coupling suppression");
        Require(maximum_formula_error <= 1.0 / 1024.0, "actual coupling must follow the existing formula within one R16F ULP");
        Require(omit_water ? final_reaction >= 0.8 * poured_reaction : final_reaction < 0.2 * poured_reaction,
                "water must cut the fed flame below a fifth while the no-water control keeps burning; before=" +
                    std::to_string(poured_reaction) + " after=" + std::to_string(final_reaction));
        std::cout << "gas quenching threshold 2.5 passed\n";
    }

    static void BenchmarkGas(PowderApp& app) {
        constexpr int width = PowderApp::kGasWidth, height = PowderApp::kGasHeight;
        constexpr std::size_t cells = static_cast<std::size_t>(width) * height;
        constexpr int samples = 3, warmup_ticks = 120;
        struct Restore {
            PowderApp& app;
            powder_config::SimulationConfig config;
            bool replay;
            PowderApp::BrushMode brush;
            int x, y, radius;
            ~Restore() {
                app.config_ = config;
                app.replay_mode_ = replay;
                app.brush_mode_ = brush;
                app.brush_x_ = x; app.brush_y_ = y; app.brush_radius_ = radius;
            }
        } restore{app, app.config_, app.replay_mode_, app.brush_mode_, app.brush_x_, app.brush_y_, app.brush_radius_};
        app.config_ = powder_config::LoadConfigFromEnvironment();
        const auto source_config = app.config_;
        app.replay_mode_ = true;
        struct Queries {
            GLuint ids[2]{};
            Queries() { glGenQueries(2, ids); }
            ~Queries() { glDeleteQueries(2, ids); }
        } queries;
        const auto read = [](GLuint texture, std::size_t count) {
            std::vector<float> values(count);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
            Require(std::all_of(values.begin(), values.end(), [](float x) { return std::isfinite(x); }),
                    "gas characterization read nonfinite texture values");
            return values;
        };
        const auto upload = [](GLuint texture, int w, int h, GLenum format, GLenum type, const void* data) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, data);
        };
        const auto divergence = [](const std::vector<float>& u, const std::vector<float>& v, int x, int y) {
            return static_cast<double>(u[y * (width + 1) + x + 1]) - u[y * (width + 1) + x] +
                   v[(y + 1) * width + x] - v[y * width + x];
        };
        std::cout << "gas characterization renderer=" << glGetString(GL_RENDERER) << " driver=" << glGetString(GL_VERSION)
                  << " gas=" << width << 'x' << height << " dt=" << app.config_.fixed_dt
                  << " samples=" << samples << " source warmup ticks=" << warmup_ticks
                  << " (capture first projection of final tick; timing excludes uploads/readbacks)\n";
        for (const std::string scene : {"wave", "neck", "strip", "sparsefire", "combined", "zero"}) {
            app.config_ = source_config;
            app.InitializeState();
            const bool neck = scene == "neck", strip = scene == "strip";
            const bool sourced = scene == "sparsefire" || scene == "combined";
            std::vector<GLubyte> mask(cells, neck || strip ? 1 : 0);
            std::vector<float> initial_u((width + 1) * height, 0.0F), initial_v(width * (height + 1), 0.0F);
            if (sourced) {
                if (scene == "combined") {
                    std::vector<Particle> water;
                    water.reserve(100000);
                    for (int y = 200; y < 600; ++y) for (int x = 600; x < 850; ++x)
                        water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                    const std::vector<Particle> sand = BrushLikeSand(100, 200, 300, 325);
                    WriteParticles(app, app.water_state_.particles, water);
                    WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
                }
                static PFNGLDISPATCHCOMPUTEPROC native_dispatch = nullptr;
                static std::function<void()> capture;
                struct RestoreDispatch {
                    PFNGLDISPATCHCOMPUTEPROC saved;
                    ~RestoreDispatch() { glad_glDispatchCompute = saved; capture = {}; }
                } restore_dispatch{glad_glDispatchCompute};
                native_dispatch = restore_dispatch.saved;
                bool captured = false;
                capture = [&] {
                    GLint program = 0;
                    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                    if (captured || static_cast<GLuint>(program) != app.smoke_divergence_program_) return;
                    GLint bound_texture = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_texture);
                    initial_u = read(app.CurrentGasU(), initial_u.size());
                    initial_v = read(app.CurrentGasV(), initial_v.size());
                    glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound_texture));
                    captured = true;
                };
                for (int tick = 0; tick < warmup_ticks; ++tick) {
                    app.brush_mode_ = scene == "sparsefire" || (tick / 2) % 2 == 0 ?
                        PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke;
                    app.brush_x_ = 500; app.brush_y_ = 120; app.brush_radius_ = 18;
                    const auto source = app.BuildBrushConfig();
                    if (tick == warmup_ticks - 1)
                        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) { capture(); native_dispatch(x, y, z); };
                    app.RunFrame(app.config_.fixed_dt, &source, true);
                }
                Require(captured, "gas characterization did not capture the actual host projection input");
                const auto water = app.ReadWaterMetrics();
                const auto sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                          PowderApp::kSandParticleComponents);
                const double smoke = app.ReadScalarTextureSum(app.CurrentSmokeDen(), width, height);
                const double reaction = app.ReadReactionTextureSum(app.CurrentReactionRate(), width, height);
                Require(smoke > 0.0 && reaction > 0.0 && water.count == (scene == "combined" ? 100000U : 0U) &&
                            sand.count == (scene == "combined" ? 50000U : 0U), "gas source workload did not remain populated");
                std::cout << "gas source " << scene << " water/sand=" << water.count << '/' << sand.count
                          << " mass=" << water.mass << '/' << sand.mass << " smoke/reaction=" << smoke << '/' << reaction << '\n';
            } else {
                if (strip) {
                    for (int y = 0; y < 5; ++y) for (int x = 52; x < 84; ++x) mask[y * width + x] = 0;
                } else if (neck) {
                    for (int y = 100; y < 132; ++y) for (int x = 100; x < 165; ++x)
                        if (x != 132) mask[y * width + x] = 0;
                    mask[117 * width + 132] = 0;
                } else if (scene == "wave") {
                    for (int y = 180; y < 320; ++y) for (int x = 210; x < 290; ++x) mask[y * width + x] = 1;
                    for (int y = 100; y < 400; ++y) mask[y * width + 100] = 1;
                }
                const auto blocked = [&](int x, int y) {
                    return x < 0 || y < 0 || x >= width || y >= height || mask[y * width + x] != 0;
                };
                const auto pressure = [&](int x, int y) {
                    if (scene == "zero") return 0.0;
                    if (strip) return static_cast<double>(y);
                    if (neck) return x < 132 ? 1.0 : x > 132 ? -1.0 : 0.0;
                    return std::cos(6.283185307179586 * (x + 0.5) / 32.0) +
                           0.5 * std::cos(6.283185307179586 * (y + 0.5) / 48.0);
                };
                for (int y = 0; y < height; ++y) for (int x = 1; x < width; ++x)
                    if (!blocked(x - 1, y) && !blocked(x, y))
                        initial_u[y * (width + 1) + x] = static_cast<float>(pressure(x, y) - pressure(x - 1, y));
                for (int y = 1; y < height; ++y) for (int x = 0; x < width; ++x)
                    if (!blocked(x, y - 1) && !blocked(x, y))
                        initial_v[y * width + x] = static_cast<float>(pressure(x, y) - pressure(x, y - 1));
            }
            // Quantize once through the actual face storage. Every variant then receives identical R16F input.
            upload(app.CurrentGasU(), width + 1, height, GL_RED, GL_FLOAT, initial_u.data());
            upload(app.CurrentGasV(), width, height + 1, GL_RED, GL_FLOAT, initial_v.data());
            initial_u = read(app.CurrentGasU(), initial_u.size());
            initial_v = read(app.CurrentGasV(), initial_v.size());
            const auto blocked = [&](int x, int y) {
                return x < 0 || y < 0 || x >= width || y >= height || mask[y * width + x] != 0;
            };
            for (int cycles : {0, 1, 2, 4, 8, 12, 24}) {
                std::vector<float> u = initial_u, v = initial_v, pressure(cells, 0.0F), rhs(cells, 0.0F);
                double gpu_ms = 0.0, wall_ms = 0.0;
                std::array<GLuint, 5> counters{};
                if (cycles != 0) {
                    app.config_.gas_pressure_iterations = cycles;
                    for (int sample = -1; sample < samples; ++sample) {
                        upload(app.boundary_state_.mask_gas, width, height, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                        upload(app.CurrentGasU(), width + 1, height, GL_RED, GL_FLOAT, initial_u.data());
                        upload(app.CurrentGasV(), width, height + 1, GL_RED, GL_FLOAT, initial_v.data());
                        const GLuint zero = 0;
                        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
                        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
                        glFinish();
                        const auto start = std::chrono::steady_clock::now();
                        glQueryCounter(queries.ids[0], GL_TIMESTAMP);
                        app.RunGasProjection();
                        glQueryCounter(queries.ids[1], GL_TIMESTAMP);
                        std::array<GLuint64, 2> timestamps{};
                        glGetQueryObjectui64v(queries.ids[0], GL_QUERY_RESULT, &timestamps[0]);
                        glGetQueryObjectui64v(queries.ids[1], GL_QUERY_RESULT, &timestamps[1]);
                        const double elapsed = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();
                        Require(timestamps[1] > timestamps[0], "gas projection timestamps must advance");
                        if (sample >= 0) {
                            gpu_ms += static_cast<double>(timestamps[1] - timestamps[0]) / 1000000.0 / samples;
                            wall_ms += elapsed / samples;
                        }
                    }
                    u = read(app.CurrentGasU(), u.size()); v = read(app.CurrentGasV(), v.size());
                    pressure = read(app.gas_state_.pressure_a, cells); rhs = read(app.gas_state_.divergence, cells);
                    counters = app.ReadDebugCounters();
                    Require(counters[0] == 0 && counters[1] == 0 && counters[3] == 0,
                            "gas characterization raised a non-pressure debug error");
                }
                double residual_square = 0.0, residual_max = 0.0, stored_max = 0.0;
                double div_square = 0.0, div_max = 0.0, wall_max = 0.0, wall_flux = 0.0, center_speed = 0.0;
                int fluid = 0;
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    if (blocked(x, y)) continue;
                    const int index = y * width + x;
                    double laplacian = 0.0;
                    for (const auto& offset : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                        const int nx = x + offset[0], ny = y + offset[1];
                        if (!blocked(nx, ny)) laplacian += static_cast<double>(pressure[ny * width + nx]) - pressure[index];
                    }
                    const double source = divergence(initial_u, initial_v, x, y);
                    const double residual = source - laplacian;
                    const double actual = divergence(u, v, x, y);
                    Require(std::isfinite(residual) && std::isfinite(actual), "gas characterization residual must be finite");
                    residual_square += residual * residual; residual_max = std::max(residual_max, std::abs(residual));
                    stored_max = std::max(stored_max, std::abs((cycles == 0 ? source : rhs[index]) - laplacian));
                    div_square += actual * actual; div_max = std::max(div_max, std::abs(actual));
                    if (blocked(x - 1, y) || blocked(x + 1, y) || blocked(x, y - 1) || blocked(x, y + 1))
                        wall_max = std::max(wall_max, std::abs(actual));
                    const double vx = 0.5 * (u[y * (width + 1) + x] + u[y * (width + 1) + x + 1]);
                    const double vy = 0.5 * (v[y * width + x] + v[(y + 1) * width + x]);
                    center_speed = std::max(center_speed, std::hypot(vx, vy));
                    ++fluid;
                }
                for (int y = 0; y < height; ++y) for (int x = 0; x <= width; ++x)
                    if (blocked(x - 1, y) || blocked(x, y)) wall_flux = std::max(wall_flux, std::abs(static_cast<double>(u[y * (width + 1) + x])));
                for (int y = 0; y <= height; ++y) for (int x = 0; x < width; ++x)
                    if (blocked(x, y - 1) || blocked(x, y)) wall_flux = std::max(wall_flux, std::abs(static_cast<double>(v[y * width + x])));
                Require(fluid > 0 && wall_flux == 0.0, "gas characterization requires fluid cells and zero normal wall flux");
                if (scene == "zero") Require(residual_max == 0.0 && div_max == 0.0, "zero gas state must remain exactly zero");
                std::cout << "gas frozen " << scene << " cycles=" << cycles << " fluid=" << fluid
                          << " GPU/wall ms=" << gpu_ms << '/' << wall_ms
                          << " true residual RMS/max=" << std::sqrt(residual_square / fluid) << '/' << residual_max
                          << " stored RHS residual max=" << stored_max
                          << " actual div RMS/max=" << std::sqrt(div_square / fluid) << '/' << div_max
                          << " nearwall max=" << wall_max << " wallflux=" << wall_flux << " center speed=" << center_speed
                          << " debug=" << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '\n';
            }
        }
    }

    static void BrushInput(PowderApp& app) {
        const GLFWkeyfun callback = glfwSetKeyCallback(app.window_, nullptr);
        glfwSetKeyCallback(app.window_, callback);
        Require(callback != nullptr && glfwGetWindowUserPointer(app.window_) == &app,
                "brush input requires an installed callback and app window user pointer");
        const int original_radius = app.brush_radius_;
        struct RestoreRadius {
            PowderApp& app;
            int radius;
            bool replay;
            PowderApp::BrushMode brush;
            PowderApp::RenderMode render;
            int closing;
            bool help;
            ~RestoreRadius() {
                app.brush_radius_ = radius;
                app.replay_mode_ = replay;
                app.brush_mode_ = brush;
                app.render_mode_ = render;
                app.help_visible_ = help;
                glfwSetWindowShouldClose(app.window_, closing);
            }
        } restore{app, original_radius, app.replay_mode_, app.brush_mode_, app.render_mode_, glfwWindowShouldClose(app.window_),
                  app.help_visible_};
        app.replay_mode_ = false;
        // Coalesced mouse moves: the history entries between the last one taken and the current event, oldest first.
        {
            using Move = PowderApp::MovePoint;
            const std::vector<Move> history{{50, 9, 105}, {40, 8, 104}, {30, 7, 103}, {20, 6, 102}, {10, 5, 101}};
            Require(PowderApp::MovesSince(history, {20, 6, 102}) == std::vector<Move>{{30, 7, 103}, {40, 8, 104}},
                    "mouse history must yield the moves since the last one taken, oldest first");
            Require(PowderApp::MovesSince(history, {40, 8, 104}).empty() && PowderApp::MovesSince(history, {50, 9, 105}).empty(),
                    "no move lies between the last one taken and the current event");
            Require(PowderApp::MovesSince(history, {10, 5, 999}).empty() && PowderApp::MovesSince({}, {10, 5, 101}).empty(),
                    "a last move missing from the history must add nothing");
        }
        const auto event = [&](int key, int action) { callback(app.window_, key, 0, action, 0); };
        for (int polls : {60, 144}) {
            for (const auto& [key, change] : {std::pair{GLFW_KEY_LEFT_BRACKET, -1}, std::pair{GLFW_KEY_RIGHT_BRACKET, 1}}) {
                app.brush_radius_ = 8;
                event(key, GLFW_PRESS);
                Require(app.brush_radius_ == 8 + change, "bracket PRESS must change radius once");
                event(key, GLFW_REPEAT);
                Require(app.brush_radius_ == 8 + 2 * change, "bracket REPEAT must change radius once");
                event(key, GLFW_RELEASE);
                Require(app.brush_radius_ == 8 + 2 * change, "bracket RELEASE must not change radius");
                for (int poll = 0; poll < polls; ++poll) app.UpdateInput();
                Require(app.brush_radius_ == 8 + 2 * change, "extra UpdateInput calls must not resize a released brush");
            }
        }
        app.brush_radius_ = 1;
        event(GLFW_KEY_LEFT_BRACKET, GLFW_PRESS);
        event(GLFW_KEY_LEFT_BRACKET, GLFW_REPEAT);
        Require(app.brush_radius_ == 1, "minimum brush radius must remain one");
        app.brush_radius_ = 96;
        event(GLFW_KEY_RIGHT_BRACKET, GLFW_PRESS);
        event(GLFW_KEY_RIGHT_BRACKET, GLFW_REPEAT);
        Require(app.brush_radius_ == 96, "maximum brush radius must remain 96");
        app.brush_radius_ = 8;
        event(GLFW_KEY_A, GLFW_PRESS);
        event(GLFW_KEY_A, GLFW_REPEAT);
        Require(app.brush_radius_ == 8, "unrelated key events must not resize the brush");
        using Brush = PowderApp::BrushMode;
        using View = PowderApp::RenderMode;
        for (const auto& [key, expected] : std::array<std::pair<int, Brush>, 6>{{
                 {GLFW_KEY_1, Brush::Sand}, {GLFW_KEY_2, Brush::Water}, {GLFW_KEY_3, Brush::Solid},
                 {GLFW_KEY_4, Brush::Erase}, {GLFW_KEY_5, Brush::Smoke}, {GLFW_KEY_6, Brush::Fire}}}) {
            app.brush_mode_ = expected == Brush::Sand ? Brush::Fire : Brush::Sand;
            event(key, GLFW_PRESS);
            event(key, GLFW_RELEASE);
            app.UpdateInput();
            Require(app.brush_mode_ == expected, "brush tap must survive PRESS+RELEASE before polling: key " + std::to_string(key));
        }
        for (const auto& [key, expected] : std::array<std::pair<int, View>, 10>{{
                 {GLFW_KEY_F1, View::Composite}, {GLFW_KEY_F2, View::Boundary}, {GLFW_KEY_F3, View::Water},
                 {GLFW_KEY_F4, View::WaterVelocity}, {GLFW_KEY_F5, View::GasVelocity}, {GLFW_KEY_F6, View::GasPressure},
                 {GLFW_KEY_F7, View::Smoke}, {GLFW_KEY_F8, View::Temperature}, {GLFW_KEY_F9, View::FuelReaction},
                 {GLFW_KEY_F10, View::DebugCounters}}}) {
            app.render_mode_ = expected == View::Composite ? View::Boundary : View::Composite;
            event(key, GLFW_PRESS);
            event(key, GLFW_RELEASE);
            app.UpdateInput();
            Require(app.render_mode_ == expected, "view tap must survive PRESS+RELEASE before polling: key " + std::to_string(key));
        }
        for (const auto& keys : {std::pair{GLFW_KEY_6, GLFW_KEY_1}, std::pair{GLFW_KEY_1, GLFW_KEY_6}}) {
            event(keys.first, GLFW_PRESS);
            event(keys.first, GLFW_RELEASE);
            event(keys.second, GLFW_PRESS);
            event(keys.second, GLFW_RELEASE);
            app.UpdateInput();
            Require(app.brush_mode_ == (keys.second == GLFW_KEY_1 ? Brush::Sand : Brush::Fire),
                    "last brush PRESS must win in event order");
        }
        event(GLFW_KEY_F10, GLFW_PRESS);
        event(GLFW_KEY_F1, GLFW_PRESS);
        event(GLFW_KEY_F10, GLFW_RELEASE);
        event(GLFW_KEY_F10, GLFW_REPEAT);
        event(GLFW_KEY_1, GLFW_RELEASE);
        event(GLFW_KEY_1, GLFW_REPEAT);
        Require(app.render_mode_ == View::Composite && app.brush_mode_ == Brush::Fire,
                "selection RELEASE/REPEAT must not replace the most recent PRESS");
        glfwSetWindowShouldClose(app.window_, GLFW_FALSE);
        event(GLFW_KEY_ESCAPE, GLFW_RELEASE);
        Require(glfwWindowShouldClose(app.window_) == GLFW_FALSE, "Escape RELEASE must not close");
        event(GLFW_KEY_ESCAPE, GLFW_PRESS);
        event(GLFW_KEY_ESCAPE, GLFW_RELEASE);
        app.UpdateInput();
        Require(glfwWindowShouldClose(app.window_) == GLFW_TRUE, "Escape tap must close even when released before polling");
        glfwSetWindowShouldClose(app.window_, GLFW_FALSE);
        app.help_visible_ = true;
        event(GLFW_KEY_H, GLFW_PRESS);
        Require(!app.help_visible_, "H PRESS must hide the help panel");
        event(GLFW_KEY_H, GLFW_REPEAT);
        event(GLFW_KEY_H, GLFW_RELEASE);
        Require(!app.help_visible_, "H REPEAT and RELEASE must not toggle the help panel");
        event(GLFW_KEY_H, GLFW_PRESS);
        Require(app.help_visible_, "a second H PRESS must show the help panel again");
        app.replay_mode_ = true;
        event(GLFW_KEY_RIGHT_BRACKET, GLFW_PRESS);
        event(GLFW_KEY_LEFT_BRACKET, GLFW_REPEAT);
        event(GLFW_KEY_1, GLFW_PRESS);
        event(GLFW_KEY_F2, GLFW_PRESS);
        event(GLFW_KEY_ESCAPE, GLFW_PRESS);
        event(GLFW_KEY_H, GLFW_PRESS);
        Require(app.brush_radius_ == 8 && app.brush_mode_ == Brush::Fire && app.render_mode_ == View::Composite &&
                    app.help_visible_ && glfwWindowShouldClose(app.window_) == GLFW_FALSE,
                "live key events must not change scripted replay input state");

        // Window points must land on the grid cell the HUD ring and spawn shaders address, through the
        // letterbox margins and a 2x DPI framebuffer; points in a margin are outside the simulation.
        const auto rect_is = [](const PowderApp::SimRect& rect, int x, int y, int width, int height) {
            return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
        };
        Require(rect_is(PowderApp::FitSimRect(1280, 900), 190, 0, 900, 900) &&
                    rect_is(PowderApp::FitSimRect(600, 1000), 0, 200, 600, 600) &&
                    rect_is(PowderApp::FitSimRect(32, 32), 0, 0, 32, 32),
                "the simulation viewport must be the centred grid-aspect square, covering square framebuffers");
        struct Mapping {
            int window_width, window_height, framebuffer_width, framebuffer_height;
            double x, y;
            bool inside;
            int cell_x, cell_y;
        };
        for (const Mapping& m : std::array<Mapping, 9>{{
                 {1280, 900, 1280, 900, 640, 450, true, 500, 499},
                 {1280, 900, 1280, 900, 190, 0, true, 0, 999},
                 {1280, 900, 1280, 900, 189, 450, false, 0, 499},
                 {1280, 900, 1280, 900, 1090, 450, false, 999, 499},
                 {640, 450, 1280, 900, 320, 225, true, 500, 499},
                 {640, 450, 1280, 900, 95, 100, true, 0, 777},
                 {600, 1000, 600, 1000, 300, 500, true, 500, 499},
                 {600, 1000, 600, 1000, 300, 100, false, 500, 999},
                 {32, 32, 32, 32, 16, 16, true, 500, 499}}}) {
            int cell_x = -1;
            int cell_y = -1;
            const bool inside = PowderApp::MapCursorToGrid(m.x, m.y, m.window_width, m.window_height,
                                                           m.framebuffer_width, m.framebuffer_height, cell_x, cell_y);
            Require(inside == m.inside && cell_x == m.cell_x && cell_y == m.cell_y,
                    "cursor " + std::to_string(m.x) + "," + std::to_string(m.y) + " in a " +
                        std::to_string(m.window_width) + "x" + std::to_string(m.window_height) + " window mapped to " +
                        std::to_string(cell_x) + "," + std::to_string(cell_y) + (inside ? " inside" : " outside"));
        }
        int unused = 0;
        Require(!PowderApp::MapCursorToGrid(1.0, 1.0, 0, 0, 0, 0, unused, unused), "a zero-sized window has no simulation viewport");

        // A held stroke bridges the previous stamp to the cursor with stamps at most radius/2 apart.
        const auto points = PowderApp::StrokePoints({{100, 100}, {300, 160}}, 8);
        Require(points.size() == 53 && points.back() == std::array<int, 2>{300, 160},
                "a 209-cell segment at radius 8 needs 53 stamps ending at the cursor");
        double previous_x = 100.0;
        double previous_y = 100.0;
        for (const auto& point : points) {
            Require(std::hypot(point[0] - previous_x, point[1] - previous_y) <= 4.0 + std::sqrt(2.0),
                    "stroke stamps must stay within radius/2 of each other, plus rounding");
            previous_x = point[0];
            previous_y = point[1];
        }
        Require(PowderApp::StrokePoints({{5, 5}, {5, 5}}, 8) == std::vector<std::array<int, 2>>{{5, 5}} &&
                    PowderApp::StrokePoints({{5, 5}}, 8) == std::vector<std::array<int, 2>>{{5, 5}},
                "a stationary cursor must stamp only itself");
        Require(PowderApp::StrokePoints({{0, 0}, {900, 900}}, 1).size() == PowderApp::kMaxStrokeStamps &&
                    PowderApp::StrokePoints({{0, 0}, {900, 900}}, 1).back() == std::array<int, 2>{900, 900},
                "a long jump must stay bounded and still reach the cursor");
        // A bent path is followed, not cut across or rounded: a right angle stays a corner, stamps stay on both legs,
        // evenly spaced, one beside the corner.
        const auto bent = PowderApp::StrokePoints({{100, 100}, {300, 100}, {300, 300}}, 24);
        Require(bent.size() == 34 && bent.back() == std::array<int, 2>{300, 300}, "a 400-cell bent path at radius 24 needs 34 stamps");
        bool near_corner = false;
        std::array<int, 2> before{100, 100};
        for (const auto& point : bent) {
            Require((point[1] == 100 && point[0] > 100 && point[0] <= 300) || (point[0] == 300 && point[1] >= 100),
                    "stamps of a bent path must stay on its legs");
            Require(std::hypot(point[0] - before[0], point[1] - before[1]) <= 12.0 + std::sqrt(2.0),
                    "stamps of a bent path must stay within radius/2 of each other, plus rounding");
            near_corner = near_corner || std::hypot(point[0] - 300.0, point[1] - 100.0) <= 6.5;
            before = point;
        }
        Require(near_corner, "a stamp of a bent path must land beside its corner");
        // Eight cursor samples of a radius-100 circle: a polyline cuts 7.6 cells inside at the chord middles, the
        // centripetal spline stays within 2 cells of the circle and turns smoothly.
        std::vector<std::array<int, 2>> circle;
        for (int k = 0; k <= 8; ++k)
            circle.push_back({500 + static_cast<int>(std::lround(100.0 * std::cos(k * 0.78539816))),
                              500 + static_cast<int>(std::lround(100.0 * std::sin(k * 0.78539816)))});
        const auto round = PowderApp::StrokePoints(circle, 8);
        double worst = 0.0, sharpest = 0.0;
        for (std::size_t i = 0; i < round.size(); ++i) {
            // Away from the open ends (whose outer control points are reflections).
            const double angle = std::atan2(round[i][1] - 500.0, round[i][0] - 500.0);
            if (std::abs(angle) > 0.8)
                worst = std::max(worst, std::abs(std::hypot(round[i][0] - 500.0, round[i][1] - 500.0) - 100.0));
            if (i < 2) continue;
            const double ax = round[i - 1][0] - round[i - 2][0], ay = round[i - 1][1] - round[i - 2][1];
            const double bx = round[i][0] - round[i - 1][0], by = round[i][1] - round[i - 1][1];
            sharpest = std::max(sharpest, std::acos(std::clamp((ax * bx + ay * by) / std::max(1e-9, std::hypot(ax, ay) * std::hypot(bx, by)), -1.0, 1.0)));
        }
        std::cout << "circle stroke: " << round.size() << " stamps, radius error " << worst << ", sharpest turn "
                  << sharpest * 57.2958 << " deg\n";
        Require(worst <= 2.0 && sharpest * 57.2958 <= 30.0, "a circle stroke must stay round, with no sharp corners");
        const auto bridged = [&](bool active, Brush mode) {
            app.InitializeState();
            app.brush_mode_ = Brush::Solid;
            app.brush_radius_ = 8;
            app.stroke_ = {active, mode, 100, 100};
            app.brush_x_ = 300;
            app.brush_y_ = 100;
            app.StampStroke();
            Require(app.brush_x_ == 300 && app.brush_y_ == 100 && app.stroke_.active && app.stroke_.mode == Brush::Solid &&
                        app.stroke_.x == 300 && app.stroke_.y == 100,
                    "a stroke must end at the cursor and remember it");
            std::vector<GLuint> material(static_cast<std::size_t>(PowderApp::kGridWidth) * PowderApp::kGridHeight);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
            bool contiguous = true;
            for (int x = 100; x <= 300; ++x) {
                contiguous = contiguous && material[static_cast<std::size_t>(100 * PowderApp::kGridWidth + x)] != 0U;
            }
            return contiguous;
        };
        Require(bridged(true, Brush::Solid), "a fast solid stroke must leave a gap-free wall");
        Require(!bridged(false, Brush::Solid), "a new press must stamp only its own cell");
        Require(!bridged(true, Brush::Water), "changing brush must restart the stroke");

        // A held water brush skips cells that are already as dense as it emits; the four neighbours still fill.
        app.InitializeState();
        app.brush_mode_ = Brush::Water;
        app.brush_radius_ = 1;
        app.brush_x_ = app.brush_y_ = 500;
        app.stroke_ = {};
        WriteScalar(app.water_state_.liquid_volume, 0.5F, 500, 500);
        WriteScalar(app.water_state_.liquid_volume, 0.5F * 4.0F * app.config_.water_amount_per_particle, 501, 500);
        app.StampStroke();
        const auto spawned = ReadParticles(app.water_state_.particles, 32);
        Require(std::count_if(spawned.begin(), spawned.end(), [](const Particle& p) { return p[4] > 0.5F; }) == 16,
                "a held water brush must skip dense cells only: dense centre skipped, half-dense and empty cells filled");

        // Stamps along a fast stroke overlap about four deep, but a swept cell must still receive exactly the particles
        // one held tick emits; the stale density gates cannot see the stamps of the same tick. Water records are born a
        // random part of a tick early and have already moved (water_particle_spawn.comp), so water is checked as the
        // total over the cells the stamps cover.
        for (const auto& [mode, per_cell] : {std::pair{Brush::Water, 4}, std::pair{Brush::Sand, 2}}) {
            app.InitializeState();
            app.brush_mode_ = mode;
            app.brush_radius_ = 8;
            app.stroke_ = {true, mode, 100, 100};
            app.brush_x_ = 300;
            app.brush_y_ = 100;
            app.StampStroke();
            std::vector<int> counts(static_cast<std::size_t>(PowderApp::kGridWidth) * PowderApp::kGridHeight);
            const auto tally = [&](const auto& particles) {
                for (const auto& p : particles) {
                    if (p[4] > 0.5F) ++counts[static_cast<std::size_t>(static_cast<int>(p[1]) * PowderApp::kGridWidth + static_cast<int>(p[0]))];
                }
            };
            if (mode == Brush::Water) tally(ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles));
            else tally(ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles));
            if (mode == Brush::Water) {
                std::set<std::pair<int, int>> covered;
                for (const auto& [cx, cy] : PowderApp::StrokePoints({{100, 100}, {300, 100}}, 8))
                    for (int y = cy - 8; y <= cy + 8; ++y)
                        for (int x = cx - 8; x <= cx + 8; ++x)
                            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 64) covered.insert({x, y});
                const long long total = std::accumulate(counts.begin(), counts.end(), 0LL);
                Require(total == 4LL * static_cast<long long>(covered.size()),
                        "a fast water stroke must emit one held tick of 4 particles per covered cell: " + std::to_string(total) +
                            " for " + std::to_string(covered.size()) + " cells");
                continue;
            }
            int wrong = 0;
            int deepest = 0;
            for (int y = 94; y <= 106; ++y) {
                for (int x = 112; x <= 292; ++x) {
                    const int count = counts[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
                    wrong += count != per_cell;
                    deepest = std::max(deepest, count);
                }
            }
            Require(wrong == 0, "every cell of a fast stroke must receive one held tick of " + std::to_string(per_cell) +
                                    " particles; " + std::to_string(wrong) + " cells differ, deepest " + std::to_string(deepest));
        }

        // Cursor events between ticks form the stroke, in order, from the press: a flick that starts and bends before
        // the first tick (a high-refresh frame without a tick) still paints its whole path.
        app.replay_mode_ = false;
        const auto cursor_event = [&](int cell_x, int cell_y) {
            int window_width = 1, window_height = 1, framebuffer_width = 1, framebuffer_height = 1;
            glfwGetWindowSize(app.window_, &window_width, &window_height);
            glfwGetFramebufferSize(app.window_, &framebuffer_width, &framebuffer_height);
            const PowderApp::SimRect rect = PowderApp::FitSimRect(framebuffer_width, framebuffer_height);
            PowderApp::CursorPosCallback(
                app.window_,
                (rect.x + (cell_x + 0.5) * rect.width / PowderApp::kGridWidth) * window_width / framebuffer_width,
                (framebuffer_height - rect.y - (cell_y + 0.5) * rect.height / PowderApp::kGridHeight) * window_height / framebuffer_height);
        };
        const auto solid_at = [&](int x, int y) {
            std::vector<GLuint> material(static_cast<std::size_t>(PowderApp::kGridWidth) * PowderApp::kGridHeight);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
            return material[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] == 2U;
        };
        app.InitializeState();
        app.brush_mode_ = Brush::Solid;
        app.brush_radius_ = 8;
        app.stroke_ = {};
        app.cursor_trail_.clear();
        for (const auto& cell : {std::array<int, 2>{100, 100}, {300, 100}, {300, 300}}) cursor_event(cell[0], cell[1]);
        app.brush_x_ = 300;
        app.brush_y_ = 300;
        app.StampStroke();
        Require(app.cursor_trail_.empty() && app.stroke_.active && app.stroke_.x == 300 && app.stroke_.y == 300,
                "stamping a stroke must consume its cursor events and remember the cursor");
        Require(solid_at(100, 100) && solid_at(200, 100) && solid_at(300, 100) && solid_at(300, 200) && solid_at(300, 300),
                "a stroke must start at the press and follow the cursor events through the bend");
        Require(!solid_at(200, 200), "a stroke must not cut across the bend between ticks");

        // Events: repeats of one cell collapse, the trail is bounded and keeps the newest cell, and an event outside the
        // simulation ends the stroke so leaving and re-entering between ticks does not connect.
        app.cursor_trail_.clear();
        cursor_event(10, 10);
        cursor_event(10, 10);
        Require(app.cursor_trail_.size() == 1, "repeated events in one cell must collapse");
        for (int x = 0; x < 300; ++x) cursor_event(x, 20);
        Require(app.cursor_trail_.size() == PowderApp::kMaxCursorTrail && app.cursor_trail_.back() == std::array<int, 2>{299, 20},
                "the cursor trail must stay bounded and keep the newest cell");
        app.stroke_ = {true, Brush::Solid, 5, 5};
        PowderApp::CursorPosCallback(app.window_, -50.0, -50.0);
        Require(!app.stroke_.active && app.cursor_trail_.empty(), "an event outside the simulation must end the stroke");
        app.replay_mode_ = true;
        cursor_event(10, 10);
        Require(app.cursor_trail_.empty(), "scripted replays must ignore live cursor events");
        app.replay_mode_ = false;

        // Only a left press restarts the stroke, at the cursor.
        app.stroke_ = {true, Brush::Solid, 5, 5};
        app.cursor_trail_ = {{1, 2}};
        PowderApp::MouseButtonCallback(app.window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0);
        PowderApp::MouseButtonCallback(app.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
        Require(app.stroke_.active && app.cursor_trail_.size() == 1 && app.cursor_trail_.front() == std::array<int, 2>{1, 2},
                "a right press or a release must not restart the stroke");
        PowderApp::MouseButtonCallback(app.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        double cursor_x = 0.0, cursor_y = 0.0;
        glfwGetCursorPos(app.window_, &cursor_x, &cursor_y);
        int window_width = 1, window_height = 1, framebuffer_width = 1, framebuffer_height = 1;
        glfwGetWindowSize(app.window_, &window_width, &window_height);
        glfwGetFramebufferSize(app.window_, &framebuffer_width, &framebuffer_height);
        std::array<int, 2> pressed{};
        const bool pressed_inside = PowderApp::MapCursorToGrid(cursor_x, cursor_y, window_width, window_height, framebuffer_width,
                                                              framebuffer_height, pressed[0], pressed[1]);
        Require(!app.stroke_.active && app.cursor_trail_.size() == (pressed_inside ? 1U : 0U) &&
                    (!pressed_inside || app.cursor_trail_.front() == pressed),
                "a left press must restart the stroke at the cursor");
        app.cursor_trail_.clear();
        app.stroke_ = {};
        app.InitializeState();
        std::cout << "brush callback radius/bounds, 60/144 polls, all6brushes/10views, tap/order/release/Escape/H/replay, letterbox/DPI cursor mapping, cursor-event polyline strokes and one-tick particle emission pass; native mouse and held input remain manual\n";
    }

    static void HudRender(PowderApp& app) {
        app.InitializeState();
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        struct Restore {
            PowderApp& app;
            int window_width, window_height;
            bool help, cursor, painting, lighting;
            double help_hide;
            PowderApp::BrushMode brush;
            int radius, x, y;
            GLuint framebuffer = 0, color = 0;
            ~Restore() {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteTextures(1, &color);
                glfwSetWindowSize(app.window_, window_width, window_height);
                app.help_visible_ = help;
                app.help_hide_time_ = help_hide;
                app.cursor_in_sim_ = cursor;
                app.painting_ = painting;
                app.lighting_enabled_ = lighting;
                app.brush_mode_ = brush;
                app.brush_radius_ = radius;
                app.brush_x_ = x;
                app.brush_y_ = y;
                app.hud_motion_ = {};
            }
        } restore{app, 0, 0, app.help_visible_, app.cursor_in_sim_, app.painting_, app.lighting_enabled_, app.help_hide_time_,
                  app.brush_mode_, app.brush_radius_, app.brush_x_, app.brush_y_};
        glfwGetWindowSize(app.window_, &restore.window_width, &restore.window_height);
        int width = 0, height = 0;
        PowderApp::SimRect sim{};
        // The target is recreated for each window size the test visits.
        const auto resize = [&](int window_width, int window_height) {
            glfwSetWindowSize(app.window_, window_width, window_height);
            glfwPollEvents();
            glfwGetFramebufferSize(app.window_, &width, &height);
            Require(width == window_width && height == window_height, "HUD test needs the window to take its requested size");
            sim = PowderApp::FitSimRect(width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &restore.framebuffer);
            glDeleteTextures(1, &restore.color);
            glGenTextures(1, &restore.color);
            glBindTexture(GL_TEXTURE_2D, restore.color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glGenFramebuffers(1, &restore.framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, restore.framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, restore.color, 0);
            Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "HUD test framebuffer incomplete");
        };
        resize(1280, 900);
        Require(sim.x == 190 && sim.width == sim.height, "HUD test needs the default letterboxed framebuffer");

        // Lighting and the lantern have their own test; the HUD pixels are compared against plain albedo here.
        app.lighting_enabled_ = false;
        app.render_mode_ = PowderApp::RenderMode::Composite;
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_radius_ = 50;
        app.brush_x_ = app.brush_y_ = 500;
        app.help_visible_ = false;
        app.help_hide_time_ = -1.0;
        app.cursor_in_sim_ = false;
        app.painting_ = false;
        app.hud_motion_ = {};

        using Image = std::vector<unsigned char>;
        // Every HUD frame is a second after the last, so each eased value snaps to its target; the motion test
        // below drives the clock explicitly.
        double clock = 100.0;
        const auto capture = [&](bool hud, bool letterbox = true) {
            app.Render(letterbox);
            if (hud) app.DrawHud(clock += 1.0);
            Image pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            CheckGl("hud capture");
            return pixels;
        };
        // Pixels are addressed from the top-left, like the plates are laid out; glReadPixels rows come from the bottom.
        const auto at = [&](const Image& image, int x, int top_y) { return &image[static_cast<std::size_t>(((height - 1 - top_y) * width + x) * 4)]; };
        const auto mean_color = [&](const Image& image, int x0, int y0, int x1, int y1) {
            std::array<float, 3> sum{};
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] += static_cast<float>(at(image, x, y)[channel]);
            for (float& value : sum) value /= static_cast<float>((x1 - x0) * (y1 - y0));
            return sum;
        };
        const auto set_material = [&](GLuint value) {
            const std::vector<GLuint> material(PowderApp::kGridWidth * PowderApp::kGridHeight, value);
            glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                            GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
        };
        const auto changed_pixels = [&](const Image& a, const Image& b, const auto& admit) {
            int count = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                if (std::equal(at(a, x, y), at(a, x, y) + 4, at(b, x, y))) continue;
                Require(admit(x, y), "HUD pixel at " + std::to_string(x) + "," + std::to_string(y) + " is outside its region");
                ++count;
            }
            return count;
        };

        // The letterbox margin is the same cave wall as the simulation's empty cells: the mean colour on either side of
        // the edge matches, so there is no seam, and an all-solid grid contrasts with it.
        set_material(0U);
        const Image empty = capture(false);
        for (int y = 30; y < height - 100; y += 97) {
            const auto margin_mean = mean_color(empty, sim.x - 64, y, sim.x, y + 64);
            const auto sim_mean = mean_color(empty, sim.x, y, sim.x + 64, y + 64);
            for (std::size_t channel = 0; channel < 3; ++channel)
                Require(std::abs(margin_mean[channel] - sim_mean[channel]) <= 4.0F,
                        "the letterbox margin must continue the cave wall without a seam at row " + std::to_string(y));
        }
        set_material(2U);
        const Image plain = capture(false);
        const auto rock_mean = mean_color(plain, sim.x + 2, 400, sim.x + 66, 464);
        const auto wall_mean = mean_color(plain, sim.x - 66, 400, sim.x - 2, 464);
        Require((rock_mean[2] - rock_mean[0]) > (wall_mean[2] - wall_mean[0]) + 8.0F,
                "Render(true) must letterbox the simulation square (cool rock) over the cave wall: rock " + std::to_string(rock_mean[0]) + "," +
                    std::to_string(rock_mean[2]) + " wall " + std::to_string(wall_mean[0]) + "," + std::to_string(wall_mean[2]));
        const Image stretched = capture(false, false);
        for (std::size_t channel = 0; channel < 3; ++channel)
            Require(std::abs(mean_color(stretched, 2, 400, 66, 464)[channel] - rock_mean[channel]) <= 6.0F &&
                        std::abs(mean_color(stretched, width - 66, 400, width - 2, 464)[channel] - rock_mean[channel]) <= 6.0F,
                    "Render() must keep stretching the simulation over the whole framebuffer");

        const int scale = std::max(1, (height + 225) / 450);
        const double px_per_cell = static_cast<double>(sim.width) / PowderApp::kGridWidth;
        // Everything the stack draws lies in its own top-left box; with the help panel hidden the hint is one text row.
        const int stack_right = (hud::kEdge + hud::kPanelWidth) * scale;
        const auto in_inventory = [&](int x, int y) { return x < stack_right && y < (hud::kEdge + hud::kInventoryHeight) * scale; };
        const auto in_help = [&](int x, int y) {
            return x < stack_right && y >= (hud::kEdge + hud::kInventoryHeight) * scale &&
                   y < (hud::kEdge + hud::kInventoryHeight + hud::kPanelGap + hud::kHelpPanelHeight) * scale;
        };
        const auto in_hint = [&](int x, int y) {
            return x < stack_right && y >= (hud::kEdge + hud::kInventoryHeight) * scale &&
                   y < (hud::kEdge + hud::kInventoryHeight + hud::kPanelGap + 1 + hud::kGlyphHeight + 1) * scale;
        };

        // In a wide window the whole stack lives in the left margin: nothing it draws reaches the play area.
        Require(hud::FitsInMargin(sim.x, scale), "the default window's margin must hold the HUD");
        const Image base = capture(true);
        const int inventory_and_hint = changed_pixels(plain, base, [&](int x, int y) {
            Require(x < sim.x, "the HUD must stay out of the play area when the margin is wide enough");
            return in_inventory(x, y) || in_hint(x, y);
        });
        Require(inventory_and_hint > 1500, "the inventory plate and hint must draw");

        // Help: a compact panel under the inventory, opened with a warm key column; hidden, the hint takes its place.
        app.help_visible_ = true;
        const Image help = capture(true);
        int text_pixels = 0;
        const int help_pixels = changed_pixels(base, help, [&](int x, int y) {
            Require(x < sim.x, "the help panel must stay in the margin");
            return in_help(x, y);
        });
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            if (!std::equal(at(base, x, y), at(base, x, y) + 4, at(help, x, y))) text_pixels += at(help, x, y)[0] > 200 && at(help, x, y)[1] > 150;
        Require(help_pixels > 3000 && text_pixels > 100, "the help panel must draw a plate and warm key labels");
        app.help_visible_ = false;

        // Slots: the selected one wears a warm outline and a bright key number; the others a dim outline.
        const auto slot_edge = [&](const Image& image, int slot) {
            return at(image, (hud::SlotLeft(slot) + hud::kSlotSize / 2) * scale, hud::kSlotTop * scale);
        };
        const auto digit_brightness = [&](const Image& image, int slot) {
            int bright = 0;
            const int left = (hud::SlotLeft(slot) + (hud::kSlotSize - (hud::kAdvance - 1)) / 2) * scale;
            const int top = (hud::kSlotTop + hud::kSlotSize + hud::kDigitGap) * scale;
            for (int y = top; y < top + hud::kGlyphHeight * scale; ++y)
                for (int x = left; x < left + hud::kGlyphWidth * scale; ++x) bright += at(image, x, y)[0] > 200 && at(image, x, y)[1] > 150;
            return bright;
        };
        using Brush = PowderApp::BrushMode;
        for (int selected = 0; selected < hud::kSlotCount; ++selected) {
            app.brush_mode_ = static_cast<Brush>(selected);
            const Image image = capture(true);
            for (int slot = 0; slot < hud::kSlotCount; ++slot) {
                const unsigned char* edge = slot_edge(image, slot);
                if (slot == selected)
                    Require(edge[0] > 200 && edge[0] > edge[2] + 90 && digit_brightness(image, slot) > 8,
                            "the selected slot must have a warm outline and a bright key number: slot " + std::to_string(slot));
                else
                    Require(edge[0] < 140 && digit_brightness(image, slot) == 0,
                            "an unselected slot must stay dim: slot " + std::to_string(slot) + " with " + std::to_string(selected) + " selected");
            }
        }
        // Each slot's pixel-art motif keeps its material's hue.
        app.brush_mode_ = Brush::Water;
        const Image motifs = capture(true);
        const auto icon_mean = [&](int slot) {
            const int left = (hud::SlotLeft(slot) + (hud::kSlotSize - hud::kIconSize) / 2) * scale;
            const int top = (hud::kSlotTop + (hud::kSlotSize - hud::kIconSize) / 2) * scale;
            return mean_color(motifs, left, top, left + hud::kIconSize * scale, top + hud::kIconSize * scale);
        };
        const auto sand_icon = icon_mean(0), water_icon = icon_mean(1), fire_icon = icon_mean(5), oil_icon = icon_mean(7);
        Require(sand_icon[0] > sand_icon[2] + 25.0F && water_icon[2] > water_icon[0] + 15.0F && fire_icon[0] > fire_icon[2] + 40.0F &&
                    oil_icon[0] > oil_icon[2] + 25.0F && oil_icon[0] < sand_icon[0],
                "slot motifs must keep their material hues: sand, water, fire and the darker amber oil");
        int red_cross = 0;
        for (int y = hud::kSlotTop * scale; y < (hud::kSlotTop + hud::kSlotSize) * scale; ++y)
            for (int x = hud::SlotLeft(3) * scale; x < (hud::SlotLeft(3) + hud::kSlotSize) * scale; ++x)
                red_cross += at(motifs, x, y)[0] > 100 && at(motifs, x, y)[0] > 2 * at(motifs, x, y)[1];
        Require(red_cross >= 20, "the erase slot must show a red cross");
        // The selected material's name and the radius readout sit on the info row.
        const int info_top = (hud::kSlotTop + hud::kSlotSize + hud::kDigitGap + hud::kGlyphHeight + hud::kInfoGap) * scale;
        int info_pixels = 0;
        for (int y = info_top; y < info_top + hud::kGlyphHeight * scale; ++y)
            for (int x = hud::kEdge * scale; x < stack_right; ++x) info_pixels += at(motifs, x, y)[0] > 150;
        Require(info_pixels > 60, "the info row must show the material name and radius");

        // Brush ring: thin, tinted by material, drawn only over the simulation, and still when nothing is held.
        app.brush_mode_ = Brush::Water;
        const Image ringless = capture(true);
        app.cursor_in_sim_ = true;
        const Image ringed = capture(true);
        const double ring_x = sim.x + 500.5 * px_per_cell, ring_y = sim.y + 500.5 * px_per_cell;  // from the bottom
        const auto on_ring = [&](int x, int top_y) {
            const double distance = std::hypot(x + 0.5 - ring_x, (height - 1 - top_y) + 0.5 - ring_y);
            return std::abs(distance - 50 * px_per_cell) <= 3.0;
        };
        const int ring_pixels = changed_pixels(ringless, ringed, on_ring);
        Require(ring_pixels > 100, "brush ring must be drawn while the cursor is over the simulation");
        // What the ring adds to the pixel under it carries its tint: teal-blue for water.
        int water_tinted = 0;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            if (on_ring(x, y)) water_tinted += (at(ringed, x, y)[2] - at(ringless, x, y)[2]) - (at(ringed, x, y)[0] - at(ringless, x, y)[0]) > 10;
        Require(water_tinted > ring_pixels / 4, "the water ring must be tinted teal-blue");
        Require(capture(true) == ringed, "a resting brush ring must not animate");
        // Painting thickens the ring and holds it steady; the selected slot also lights up a little (the only change
        // elsewhere).
        app.painting_ = true;
        const Image painting_a = capture(true);
        const Image painting_b = capture(true);
        int slot_changed = 0;
        const int painting_pixels = changed_pixels(ringless, painting_a, [&](int x, int y) {
            if (in_inventory(x, y)) {
                ++slot_changed;
                return true;
            }
            const double distance = std::hypot(x + 0.5 - ring_x, (height - 1 - y) + 0.5 - ring_y);
            return std::abs(distance - 50 * px_per_cell) <= 5.0;
        });
        Require(painting_pixels - slot_changed > ring_pixels && painting_a == painting_b,
                "painting must grow the ring and hold it steady: " + std::to_string(painting_pixels) + " pixels vs " + std::to_string(ring_pixels));
        Require(slot_changed > 30, "painting must light the selected slot");
        app.painting_ = false;
        // Erase is a distinct reddish ring: dashed, with a small cross at its centre.
        app.brush_mode_ = Brush::Erase;
        app.cursor_in_sim_ = false;
        const Image erase_base = capture(true);
        app.cursor_in_sim_ = true;
        const Image erase_ringed = capture(true);
        int erase_red = 0;
        const int erase_pixels = changed_pixels(erase_base, erase_ringed, [&](int x, int y) {
            const double distance = std::hypot(x + 0.5 - ring_x, (height - 1 - y) + 0.5 - ring_y);
            erase_red += (at(erase_ringed, x, y)[0] - at(erase_base, x, y)[0]) - (at(erase_ringed, x, y)[2] - at(erase_base, x, y)[2]) > 20;
            return on_ring(x, y) || distance <= 6.0 * scale;
        });
        Require(erase_pixels > 60 && erase_pixels < ring_pixels && erase_red > erase_pixels / 3,
                "the erase ring must be reddish and dashed, not a full ring: " + std::to_string(erase_pixels) + " changed (" +
                    std::to_string(erase_red) + " red) against " + std::to_string(ring_pixels) + " for a full ring");
        app.cursor_in_sim_ = false;
        app.brush_mode_ = Brush::Water;

        // Narrow or no margin: the same stack overlays the corner of the simulation instead of leaving the window.
        for (const auto& [window_width, window_height] : {std::pair{900, 900}, std::pair{1000, 900}}) {
            resize(window_width, window_height);
            Require(!hud::FitsInMargin(sim.x, scale) && sim.x < window_width / 2, "this window has no margin wide enough");
            set_material(2U);
            const Image overlay_plain = capture(false);
            const Image overlay_hud = capture(true);
            int in_play_area = 0;
            const int overlay_pixels = changed_pixels(overlay_plain, overlay_hud, [&](int x, int y) {
                in_play_area += x >= sim.x;
                return in_inventory(x, y) || in_hint(x, y);
            });
            Require(overlay_pixels > 1500 && in_play_area > 500,
                    "with no wide margin the stack must overlay the top-left corner of the simulation");
        }
        resize(1280, 900);
        set_material(2U);

        // Help timeout: armed, DrawHud hides the panel once its time has passed, and only then; H shows it and arms a
        // fresh timeout, or hides it and disarms.
        app.hud_motion_ = {};
        const double t_help = 300.0;
        app.help_visible_ = true;
        app.help_hide_time_ = t_help + hud::kHelpSeconds;
        app.DrawHud(t_help);
        Require(app.help_visible_ && app.hud_motion_.help == 1.0F, "the help panel must be up before its timeout");
        app.DrawHud(t_help + hud::kHelpSeconds - 0.1);
        Require(app.help_visible_ && app.help_hide_time_ >= 0.0, "the help panel must stay up until its time");
        app.DrawHud(t_help + hud::kHelpSeconds + 0.1);
        Require(!app.help_visible_ && app.help_hide_time_ < 0.0 && app.hud_motion_.help < 1.0F,
                "the help panel must start hiding once its time has passed");
        app.DrawHud(t_help + hud::kHelpSeconds + 1.0);
        Require(app.hud_motion_.help == 0.0F, "the hidden help panel must settle out");
        app.replay_mode_ = false;
        PowderApp::KeyCallback(app.window_, GLFW_KEY_H, 0, GLFW_PRESS, 0);
        Require(app.help_visible_ && app.help_hide_time_ > glfwGetTime() && app.help_hide_time_ <= glfwGetTime() + hud::kHelpSeconds,
                "H must show the help panel and arm a fresh timeout");
        PowderApp::KeyCallback(app.window_, GLFW_KEY_H, 0, GLFW_PRESS, 0);
        Require(!app.help_visible_ && app.help_hide_time_ < 0.0, "H must hide the help panel and disarm the timeout");
        app.help_hide_time_ = -1.0;

        // Motion: the selection pop and the help panel ease out of the previous state, interruptibly, in about
        // a tenth of a second; the first frame after a stall snaps.
        app.hud_motion_ = {};
        app.brush_mode_ = Brush::Water;
        app.help_visible_ = false;
        const double t0 = 500.0;
        app.DrawHud(t0);
        Require(app.hud_motion_.slot_pop[1] == 1.0F && app.hud_motion_.slot_pop[5] == 0.0F && app.hud_motion_.help == 0.0F,
                "the first HUD frame must snap to the current state");
        app.brush_mode_ = Brush::Fire;
        app.help_visible_ = true;
        app.DrawHud(t0 + 0.03);
        const float rising = app.hud_motion_.slot_pop[5], falling = app.hud_motion_.slot_pop[1];
        Require(rising > 0.3F && rising < 0.7F && std::abs(rising + falling - 1.0F) < 0.001F && app.hud_motion_.help > 0.3F && app.hud_motion_.help < 0.8F,
                "30 ms into a switch the selection and help panel must be part-way: " + std::to_string(rising));
        app.brush_mode_ = Brush::Water;  // interrupt: head back from where it is, with no jump
        app.DrawHud(t0 + 0.04);
        Require(app.hud_motion_.slot_pop[5] < rising && app.hud_motion_.slot_pop[1] > falling, "reversing mid-switch must retarget smoothly");
        app.DrawHud(t0 + 0.9);
        Require(app.hud_motion_.slot_pop[1] == 1.0F && app.hud_motion_.slot_pop[5] == 0.0F && app.hud_motion_.help == 1.0F,
                "the HUD must settle on the final state");
        Require(glIsEnabled(GL_BLEND) == GL_FALSE, "the HUD pass must not leave blending enabled");
        std::cout << "hud margin placement and overlay fallback, cave-wall letterbox, slot highlight and motifs, info row, help panel and hint, "
                     "tinted/dashed/painting brush ring, help timeout and H, and eased motion pass; legibility, feel and native DPI remain manual\n";
    }

    static void UniformCache(PowderApp& app) {
        app.InitializeState();
        static PFNGLDELETEPROGRAMPROC native_delete = nullptr;
        static PFNGLGETUNIFORMLOCATIONPROC native_location = nullptr;
        static GLuint retained_program = 0;
        static int dt_queries = 0;
        struct RestoreProgram {
            PowderApp& app;
            GLuint original;
            PFNGLDELETEPROGRAMPROC erase;
            PFNGLGETUNIFORMLOCATIONPROC locate;
            ~RestoreProgram() {
                glad_glDeleteProgram = erase;
                glad_glGetUniformLocation = locate;
                app.coupling_heat_program_ = original;
                glUseProgram(0);
                if (retained_program != 0) app.DeleteProgram(retained_program);
            }
        } restore{app, app.coupling_heat_program_, glad_glDeleteProgram, glad_glGetUniformLocation};
        native_delete = restore.erase;
        native_location = restore.locate;
        retained_program = glCreateProgram();
        Require(retained_program != 0, "create uniform-cache test program");
        app.coupling_heat_program_ = retained_program;
        const auto link = [&](int location) {
            const std::string source = "#version 430\nlayout(local_size_x=16,local_size_y=16) in;\n"
                "layout(location=" + std::to_string(location) + ") uniform float dt;\n"
                "layout(r16f,binding=0) uniform writeonly image2D result;\n"
                "void main(){if(all(equal(gl_GlobalInvocationID,uvec3(0)))) imageStore(result,ivec2(0),vec4(dt));}\n";
            struct Shader {
                GLuint id = glCreateShader(GL_COMPUTE_SHADER);
                ~Shader() { glDeleteShader(id); }
            } shader;
            const char* text = source.c_str();
            glShaderSource(shader.id, 1, &text, nullptr);
            glCompileShader(shader.id);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader.id, GL_COMPILE_STATUS, &compiled);
            Require(compiled == GL_TRUE, "compile real uniform-cache test shader");
            glAttachShader(retained_program, shader.id);
            glLinkProgram(retained_program);
            glDetachShader(retained_program, shader.id);
            GLint linked = GL_FALSE;
            glGetProgramiv(retained_program, GL_LINK_STATUS, &linked);
            Require(linked == GL_TRUE, "link real uniform-cache test program");
            Require(native_location(retained_program, "dt") == location, "driver must honor explicit uniform location");
        };
        link(3);
        dt_queries = 0;
        glad_glGetUniformLocation = [](GLuint program, const GLchar* name) -> GLint {
            if (program == retained_program && std::string(name) == "dt") ++dt_queries;
            return native_location(program, name);
        };
        app.RunCouplingPass(0.25F);
        app.RunCouplingPass(0.25F);
        Require(ReadScalar(app.CurrentTemperature()) == 0.25F && dt_queries == 1,
                "repeated hot lookup must cache the actual uniform location");
        // Retain this real object once so the deleted application handle is reused deterministically.
        glad_glDeleteProgram = [](GLuint program) { if (program != retained_program) native_delete(program); };
        app.DeleteProgram(app.coupling_heat_program_);
        glad_glDeleteProgram = native_delete;
        Require(app.coupling_heat_program_ == 0 && glIsProgram(retained_program), "test must preserve only the retired GPU object");
        link(7);
        app.coupling_heat_program_ = retained_program;
        app.RunCouplingPass(0.5F);
        const float value = ReadScalar(app.CurrentTemperature());
        int gl_errors = 0;
        while (glGetError() != GL_NO_ERROR) ++gl_errors;
        Require(value == 0.5F && dt_queries == 2 && gl_errors == 0,
                "reused program must query location 7 and write 0.5; got " + std::to_string(value) +
                ", dt queries=" + std::to_string(dt_queries) + ", GL errors=" + std::to_string(gl_errors));
        app.DeleteProgram(app.coupling_heat_program_);
        retained_program = 0;
    }

    // Sand that meets water displaces it and is slowed by it: no water is frozen inside the sand or lost, and the
    // same block sinks slower in a pool than in the dry.
    static void SandWater(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        constexpr int left_wall = 459, right_wall = 540, pool_depth = 60, block_bottom = 45, ticks = 8;
        struct Outcome {
            double drop = 0.0;
            std::size_t water = 0, frozen_early = 0, frozen_late = 0;
            std::array<GLuint, 5> counters{};
        };
        const auto run = [&](bool wet) {
            app.InitializeState();
            for (const int x : {left_wall, right_wall}) {
                const std::vector<GLushort> wall(160, 2);
                glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
                glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, 160, GL_RED_INTEGER, GL_UNSIGNED_SHORT, wall.data());
            }
            app.boundary_state_.static_dirty = true;
            std::vector<Particle> water, sand;
            for (int y = 0; y < (wet ? pool_depth : 0); ++y)
                for (int x = left_wall + 1; x < right_wall; ++x)
                    for (int i = 0; i < 4; ++i) water.push_back({x + 0.25F + 0.5F * (i & 1), y + 0.25F + 0.5F * (i >> 1), 0, 0, 1, 1});
            WriteParticles(app, app.water_state_.particles, water);
            const auto tick = [&](Outcome& outcome) {
                app.RunFrame(app.config_.fixed_dt, nullptr, true);
                const auto counters = app.ReadDebugCounters();
                for (std::size_t i = 0; i < 5; ++i) outcome.counters[i] += counters[i];
            };
            Outcome outcome;
            for (int i = 0; i < 60; ++i) tick(outcome);
            // A dense 8x8 block at rest density starts fully inside the pool (or at the same height in the dry).
            for (int y = block_bottom; y < block_bottom + 8; ++y)
                for (int x = 496; x < 504; ++x)
                    for (int i = 0; i < 2; ++i) sand.push_back({x + 0.25F + 0.5F * i, y + 0.25F + 0.5F * i, 0, 0, 1, 1});
            WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
            const auto mean_y = [&]() {
                double sum = 0.0;
                std::size_t count = 0;
                for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, sand.size())) {
                    sum += p[1];
                    count += p[4] > 0.5F;
                }
                Require(count == sand.size(), "the sand block must keep every particle");
                return sum / static_cast<double>(count);
            };
            const auto frozen = [&]() {
                std::vector<unsigned char> mask(PowderApp::kGridWidth * PowderApp::kGridHeight);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_full);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                std::size_t count = 0;
                for (const auto& p : ReadParticles(app.water_state_.particles, water.size())) {
                    const int x = std::clamp(static_cast<int>(p[0]), 0, PowderApp::kGridWidth - 1);
                    const int y = std::clamp(static_cast<int>(p[1]), 0, PowderApp::kGridHeight - 1);
                    count += p[4] > 0.5F && mask[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] != 0;
                }
                return count;
            };
            const double start = mean_y();
            for (int i = 0; i < 3; ++i) tick(outcome);
            outcome.frozen_early = frozen();
            for (int i = 3; i < ticks; ++i) tick(outcome);
            outcome.frozen_late = frozen();
            outcome.drop = start - mean_y();
            outcome.water = app.ReadWaterMetrics().count;
            return outcome;
        };
        const Outcome dry = run(false);
        const Outcome wet = run(true);
        std::cout << "sand-water drop dry/wet=" << dry.drop << '/' << wet.drop << " water=" << wet.water
                  << " frozen after 3 ticks/at end=" << wet.frozen_early << '/' << wet.frozen_late << " counters="
                  << wet.counters[0] << '/' << wet.counters[1] << '/' << wet.counters[2] << '/' << wet.counters[3] << '\n';
        Require(wet.water == static_cast<std::size_t>((right_wall - left_wall - 1) * pool_depth * 4), "water particles must not be lost");
        Require(wet.frozen_early == 0 && wet.frozen_late == 0, "no water may stay frozen inside the sand");
        const auto failures = [](const auto& c) { return c[0] + c[1] + c[2] + c[3]; };  // index 4 only counts speed clamps
        Require(failures(wet.counters) == 0 && failures(dry.counters) == 0, "the coupled run must report no failures");
        Require(dry.drop > 5.0 && wet.drop > 0.0 && wet.drop < 0.65 * dry.drop, "a block must sink slower in water than in the dry");
        app.replay_mode_ = false;
    }

    // A poured sand pile settles at a sane angle of repose. alpha 0.5 is a 45 degree nominal friction angle
    // (sin(phi) = sqrt(2) alpha in 2D); the grid dissipation leaves the measured pile near 30 degrees.
    static void SandRepose(PowderApp& app) {
        app.config_ = {};
        // The slope of the moving grains' physics, read from their records; sand-sleep-bias compares the packed pile.
        app.config_.sand_sleep_ticks = 0;
        app.replay_mode_ = true;
        app.InitializeState();
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_x_ = 500;
        app.brush_y_ = 200;
        app.brush_radius_ = 3;
        const auto pour = app.BuildBrushConfig();
        const auto tick = [&](const PowderApp::BrushConfig* brush) {
            app.RunSpawnPass(brush);
            app.RunBoundaryPass();
            app.RunSandPass(app.config_.fixed_dt);
        };
        for (int i = 0; i < 900; ++i) tick(&pour);
        const auto poured = Compaction(ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles));
        for (int i = 0; i < 800; ++i) tick(nullptr);
        const auto sand = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles);
        const auto settled = Compaction(sand);
        std::vector<std::vector<float>> columns(PowderApp::kGridWidth);
        double speed_squared = 0.0, hardening = 0.0;
        std::size_t live = 0;
        for (const auto& p : sand) {
            if (p[4] < 0.5F) continue;
            ++live;
            // Klar hardening: the plastic strain q in state1.x's fraction q / (q + 4) (sand_particle_step.comp).
            const double fraction = p[4] - std::floor(p[4]);
            hardening += 4.0 * fraction / (1.0 - fraction);
            speed_squared += p[2] * p[2] + p[3] * p[3];
            columns[static_cast<std::size_t>(std::clamp(static_cast<int>(p[0]), 0, PowderApp::kGridWidth - 1))].push_back(p[1]);
        }
        // Surface height per column (95th percentile), then a least-squares slope on each flank between 20% and 80% of the peak.
        std::vector<double> top(PowderApp::kGridWidth, -1.0);
        double peak = 0.0;
        int first = -1, last = -1, apex = 0;
        for (int x = 0; x < PowderApp::kGridWidth; ++x) {
            auto& column = columns[static_cast<std::size_t>(x)];
            if (column.size() < 2) continue;
            std::sort(column.begin(), column.end());
            top[static_cast<std::size_t>(x)] = column[static_cast<std::size_t>(static_cast<double>(column.size() - 1) * 0.95)] + 0.5;
            if (top[static_cast<std::size_t>(x)] >= peak) { peak = top[static_cast<std::size_t>(x)]; apex = x; }
            if (first < 0) first = x;
            last = x;
        }
        Require(first >= 0 && peak > 10.0, "the poured sand must form a pile");
        const auto flank = [&](int from, int to) {
            double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int x = from; x <= to; ++x) {
                const double h = top[static_cast<std::size_t>(x)];
                if (h < 0.2 * peak || h > 0.8 * peak) continue;
                ++n; sx += x; sy += h; sxx += static_cast<double>(x) * x; sxy += x * h;
            }
            Require(n >= 3, "a pile flank needs at least three measurable columns");
            return std::atan(std::abs((n * sxy - sx * sy) / (n * sxx - sx * sx))) * 180.0 / 3.14159265358979;
        };
        const double angle = 0.5 * (flank(first, apex) + flank(apex, last));
        SandFrames(app).Save("repose", 380, 0, 240, 80);
        double area = 0.0;
        for (double h : top) area += std::max(h, 0.0);
        // Grains per cell under the surface over the two of rest density.
        const double packing = static_cast<double>(live) / area / 2.0;
        std::cout << "sand-repose angle=" << angle << " deg, width=" << last - first + 1 << " peak=" << peak
                  << " rms speed=" << std::sqrt(speed_squared / static_cast<double>(live)) << " particles=" << live
                  << " packing=" << packing << '\n';
        std::cout << "sand-repose end of pour mean J=" << poured[0] << " dilation=" << poured[1] << " dilated share=" << poured[2]
                  << "; settled mean J=" << settled[0] << " dilation=" << settled[1] << " dilated share=" << settled[2] << '\n';
        // Volume correction: grains that landed loose (a remembered dilation) compact as the pile settles, and the pile packs
        // near rest density (an unbounded dilation memory let MPM volume drift overpack it fourfold).
        Require(std::isfinite(poured[0]) && std::isfinite(settled[0]), "grain volumes must stay finite");
        Require(settled[1] < poured[1] && settled[0] > poured[0] - 0.02 && settled[0] < 1.05,
                "a poured pile must compact as it settles: its remembered dilation falls and its volume ratio rises toward 1");
        Require(packing > 0.6 && packing < 1.3, "a settled pile must pack between loose and dense sand, near rest density");
        Require(std::sqrt(speed_squared / static_cast<double>(live)) < 1.0, "the pile must come to rest before its slope is measured");
        hardening /= static_cast<double>(live);
        std::cout << "sand-repose mean hardening strain q=" << hardening << '\n';
        Require(hardening > 1.0 && hardening < 100.0, "poured grains must harden by the plastic strain they flowed through");
        // Dry sand 30-35 degrees (Al-Hashemi and Al-Amoudi 2018); before the stiffer moduli it sagged to 26.6.
        Require(angle > 30.0 && angle < 35.0, "poured sand must rest at an angle of repose between 30 and 35 degrees");
        app.replay_mode_ = false;
    }

    // Mean volume ratio J = det(F) exp(volume correction), mean volume correction and the share of grains holding one.
    static std::array<double, 3> Compaction(const std::vector<SandParticle>& sand) {
        double j = 0.0, dilation = 0.0, dilated = 0.0, n = 0.0;
        for (const auto& p : sand) {
            if (p[4] < 0.5F) continue;
            j += (p[12] * p[15] - p[13] * p[14]) * std::exp(p[10]);
            dilation += p[10];
            dilated += p[10] > 0.0F;
            ++n;
        }
        return {j / n, dilation / n, dilated / n};
    }

    // The brush's bed: two grains per cell at hashed positions inside each cell of [x0, x1) x [y0, y1). A one-grain
    // cell-centred lattice (the benchmarks' sand block before) is half the rest density and a degenerate MPM bed whose
    // grains creep at a few cells/s for ever and never pack (7270 moving at the end against 488).
    static std::vector<Particle> BrushLikeSand(int x0, int y0, int x1, int y1) {
        std::vector<Particle> sand;
        sand.reserve(static_cast<std::size_t>(2 * (x1 - x0) * (y1 - y0)));
        std::uint32_t state = 7U;
        const auto jitter = [&]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<float>(state >> 8) / 16777216.0F;
        };
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                for (int i = 0; i < 2; ++i) {
                    const float px = x + jitter();
                    sand.push_back({px, y + jitter(), 0, 0, 1, 1});
                }
        return sand;
    }

    // A rest-density block of grains at two per cell in [x0, x1) x [y0, y1), all with the given moisture.
    static std::vector<SandParticle> SandBlock(int x0, int y0, int x1, int y1, float moisture) {
        std::vector<Particle> common;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                for (int i = 0; i < 2; ++i) common.push_back({x + 0.25F + 0.5F * i, y + 0.25F + 0.5F * i, 0, 0, 1, 1});
        auto sand = SandRecords(common);
        for (auto& p : sand) p[11] = moisture;  // friction trait 0 plus the moisture fraction
        return sand;
    }

    // mu(I) rheology: a flat-bottomed silo drains through its orifice at a steady rate (Beverloo: independent of the
    // fill height once it exceeds the orifice), so the grains passing per quarter second barely vary.
    static void SandSilo(PowderApp& app) {
        app.config_ = {};
        app.config_.sand_sleep_ticks = 0;
        app.replay_mode_ = true;
        app.InitializeState();
        constexpr int floor_y = 298, orifice = 40;
        PaintRock(app, 398, floor_y, 2, 500);
        PaintRock(app, 600, floor_y, 2, 500);
        PaintRock(app, 400, floor_y, 100 - orifice / 2, 2);
        PaintRock(app, 500 + orifice / 2, floor_y, 100 - orifice / 2, 2);
        WriteParticles(app, app.sand_state_.particles, SandBlock(400, floor_y + 2, 600, 560, 0.0F));
        SandFrames frames(app);
        const auto drained = [&]() {
            std::size_t below = 0;
            for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles))
                below += p[4] > 0.5F && p[1] < floor_y;
            return static_cast<double>(below);
        };
        std::vector<double> flow;
        double previous = 0.0;
        for (int t = 1; t <= 180; ++t) {
            app.RunBoundaryPass();
            app.RunSandPass(app.config_.fixed_dt);
            Require(CleanCounters(app.ReadDebugCounters()), "the silo must report no failures");
            if (t % 15 == 0) {
                const double now = drained();
                if (t > 45) flow.push_back(now - previous);  // after the opening transient
                previous = now;
            }
            if (t % 60 == 0) frames.Save("silo_" + std::to_string(t), 350, 0, 300, 600);
        }
        double mean = 0.0, square = 0.0;
        for (double f : flow) {
            mean += f / static_cast<double>(flow.size());
            square += f * f / static_cast<double>(flow.size());
        }
        const double cv = std::sqrt(std::max(0.0, square - mean * mean)) / mean;
        std::cout << "sand-silo grains per 0.25 s:";
        for (double f : flow) std::cout << ' ' << f;
        std::cout << " mean=" << mean << " cv=" << cv << " drained=" << previous << '\n';
        Require(mean > 100.0, "the silo must drain");
        Require(cv < 0.15, "a silo must drain at a steady rate");
        app.replay_mode_ = false;
    }

    // Wet-sand cohesion: a damp column slumps less than a dry one and keeps steeper sides; its moisture only dries
    // (no water here), stays bounded, reaches the moisture field and survives sleep-packing.
    static void SandWet(PowderApp& app) {
        struct Outcome {
            std::array<double, 4> cone{};
            double shed = 0.0;  // share of grains that left the column's footprint
            double moisture_min = 1.0, moisture_mean = 0.0, moisture_max = 0.0, field = 0.0;
            double packed_share = 0.0;  // grains sleeping in the packed texture at the end
        };
        constexpr int ticks = 240;
        const auto run = [&](const std::string& name, float moisture, int sleep_ticks) {
            app.config_ = {};
            app.config_.sand_sleep_ticks = sleep_ticks;
            app.replay_mode_ = true;
            app.InitializeState();
            WriteParticles(app, app.sand_state_.particles, SandBlock(480, 1, 520, 81, moisture));
            for (int t = 0; t < ticks; ++t) {
                app.RunBoundaryPass();
                app.RunSandPass(app.config_.fixed_dt);
                Require(CleanCounters(app.ReadDebugCounters()), "the wet sand column must report no failures");
                if (t == 0) SandFrames(app).Save(name + "_0", 400, 0, 200, 100);
            }
            SandFrames(app).Save(name + "_" + std::to_string(ticks), 400, 0, 200, 100);
            Outcome outcome;
            const SandCount count = CountSand(app);
            outcome.packed_share =
                static_cast<double>(count.packed) / static_cast<double>(std::max<std::uint64_t>(count.Total(), 1));
            const auto sand = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles);
            outcome.cone = SandCone(sand, 380, 619);
            double n = 0.0;
            for (const auto& p : sand) {
                if (p[4] < 0.5F) continue;
                outcome.shed += p[0] < 478.0F || p[0] >= 522.0F;
                const double m = p[11] - std::floor(p[11]);
                outcome.moisture_min = std::min(outcome.moisture_min, m);
                outcome.moisture_max = std::max(outcome.moisture_max, m);
                outcome.moisture_mean += m;
                ++n;
            }
            outcome.moisture_mean /= std::max(n, 1.0);
            outcome.shed /= std::max(n, 1.0);
            std::vector<std::uint8_t> field(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, app.sand_state_.moisture);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, field.data());
            outcome.field = field[static_cast<std::size_t>(5 * PowderApp::kGridWidth + 500)] / 255.0;
            app.replay_mode_ = false;
            return outcome;
        };
        const auto dry = run("dry", 0.0F, 0);
        const auto wet = run("wet", 0.5F, 0);
        const auto packed = run("wet_packed", 0.5F, powder_config::SimulationConfig{}.sand_sleep_ticks);
        const double expected =
            0.5 * std::exp(-static_cast<double>(powder_config::SimulationConfig{}.sand_drying_rate) * ticks / 60.0);
        for (const auto& [name, o] : {std::pair{"dry", dry}, std::pair{"wet", wet}, std::pair{"wet packed", packed}}) {
            std::cout << "sand-wet " << name << " left/right deg=" << o.cone[0] << '/' << o.cone[1] << " peak=" << o.cone[3]
                      << " shed=" << o.shed << " moisture min/mean/max=" << o.moisture_min << '/' << o.moisture_mean << '/' << o.moisture_max
                      << " field at the base=" << o.field << " packed share=" << o.packed_share << '\n';
        }
        std::cout << "sand-wet expected moisture after drying=" << expected << '\n';
        Require(wet.cone[3] > 1.15 * dry.cone[3], "a damp column must slump less than a dry one");
        // Damp sand fails locally: a steep core stands while cracked-off crumbs shed around it (before: the column stood
        // intact, 0.3 cohesion without softening, or toppled over as one block).
        Require(wet.cone[3] > 0.75 * 80.0 && wet.shed > 0.05, "a damp column must keep a steep core and shed crumbs");
        Require(packed.cone[3] > 0.85 * wet.cone[3], "sleep-packing must not collapse a damp column");
        // Intact damp grains pack too (before, bonded grains stayed moving and a damp clump cost as much as a moving one).
        Require(packed.packed_share > 0.5, "a settled damp column must be mostly packed");
        Require(0.5 * (wet.cone[0] + wet.cone[1]) > 0.5 * (dry.cone[0] + dry.cone[1]) + 5.0, "damp sand must hold steeper sides");
        Require(dry.moisture_max == 0.0 && dry.field == 0.0, "dry sand must stay dry");
        Require(wet.moisture_min >= 0.0 && wet.moisture_max < 1.0 && std::abs(wet.moisture_mean - expected) < 0.01,
                "moisture must only dry, at the drying rate, and stay in [0, 1)");
        Require(std::abs(wet.field - expected) < 0.02, "the moisture field must carry the grains' moisture");
        // Bonded damp grains do not pack, so dry the packed moisture directly (after the sleep, the rest of the 240 ticks drying).
        Require(std::abs(packed.field - expected) < 0.02, "packed sand must dry at the drying rate");
    }

    // A mixed damp clump wakes with the split it packed with. The packed cell keeps the mean bond (the mean of
    // exp(-q / softening), sand_sleep.comp) next to the mean hardening, and its grains wake as intact ones (q 0) and cracked
    // ones carrying the rest of the hardening (sand_pack.comp). Before, every grain woke at the cell's mean hardening, whose
    // bond is the exponential of the mean q, near zero for a cell holding both kinds. Three bands of a damp 40 x 2 bed on
    // rock, two grains per cell: intact (q 0), one of each, cracked (q 10). The bed is two cells high, so one tick of the
    // support climbing wakes all of it at once (a taller one pulls apart as its rows wake in turn, which breaks bonds).
    static void SandWetMixed(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 440, 0, 120, 20);
        constexpr float cracked = 5.0F / 7.0F;  // q 10 = 4 h / (1 - h)
        auto sand = SandBlock(480, 20, 520, 22, 0.5F);
        for (std::size_t i = 0; i < sand.size(); ++i) {
            const int x = 480 + static_cast<int>(i / 2) % 40;
            const bool is_cracked = x >= 506 || (x >= 494 && i % 2 == 1);
            sand[i][4] = 1.0F + (is_cracked ? cracked : 0.0F);
        }
        WriteParticles(app, app.sand_state_.particles, sand);
        const auto run = [&](int ticks) {
            for (int t = 0; t < ticks; ++t) {
                app.RunBoundaryPass();
                app.RunSandPass(app.config_.fixed_dt);
                Require(CleanCounters(app.ReadDebugCounters()), "the mixed clump must report no failures");
            }
        };
        // Bands away from the clump's edges and from each other (grains drift up to half a cell while it settles).
        const std::array<std::pair<int, int>, 3> bands{{{485, 492}, {496, 504}, {508, 515}}};
        const char* names[] = {"intact", "mixed", "cracked"};
        run(180);
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
        std::vector<float> means(packed.size() * 4);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, app.sand_state_.packed);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, packed.data());
        glBindTexture(GL_TEXTURE_2D, app.sand_state_.packed_f);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, means.data());
        std::array<std::array<double, 3>, 3> packed_mean{};  // grains, bond sum, hardening sum
        for (std::size_t b = 0; b < bands.size(); ++b)
            for (int y = 20; y < 22; ++y)
                for (int x = bands[b].first; x < bands[b].second; ++x) {
                    const std::size_t cell = static_cast<std::size_t>(y * PowderApp::kGridWidth + x);
                    packed_mean[b][0] += packed[cell];
                    packed_mean[b][1] += packed[cell] * means[4 * cell + 3];
                    packed_mean[b][2] += packed[cell] * means[4 * cell + 1];
                }
        for (std::size_t b = 0; b < bands.size(); ++b)
            std::cout << "sand-wet-mixed " << names[b] << " packed grains=" << packed_mean[b][0] << " mean bond="
                      << packed_mean[b][1] / std::max(packed_mean[b][0], 1.0) << " mean hardening="
                      << packed_mean[b][2] / std::max(packed_mean[b][0], 1.0) << '\n';
        for (std::size_t b = 0; b < bands.size(); ++b)
            Require(packed_mean[b][0] > 0.8 * 2.0 * 2.0 * (bands[b].second - bands[b].first), "a damp band must pack");
        const auto bond = [&](std::size_t b) { return packed_mean[b][1] / packed_mean[b][0]; };
        const auto hardening = [&](std::size_t b) { return packed_mean[b][2] / packed_mean[b][0]; };
        Require(bond(0) > 0.9 && bond(1) > 0.35 && bond(1) < 0.65 && bond(2) < 0.05,
                "a packed cell must keep the mean bond of its grains");
        Require(hardening(1) > 0.28 && hardening(1) < 0.43, "a packed cell must keep the mean hardening of its grains");

        // Take the rock away: the strip wakes in one tick and starts to fall.
        PaintRock(app, 440, 0, 120, 20, 0);
        run(1);
        std::array<std::array<double, 3>, 3> woken{};  // grains, intact (h < 0.03), mean hardening sum
        std::uint64_t still_packed = 0;
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.sand_state_.packed);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, packed.data());
        for (const auto& band : bands)
            for (int y = 0; y < 30; ++y)
                for (int x = band.first; x < band.second; ++x)
                    still_packed += packed[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
        for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles)) {
            if (p[4] < 0.5F) continue;
            for (std::size_t b = 0; b < bands.size(); ++b) {
                if (p[0] < static_cast<float>(bands[b].first) || p[0] >= static_cast<float>(bands[b].second)) continue;
                const double h = p[4] - std::floor(p[4]);
                ++woken[b][0];
                woken[b][1] += h < 0.03;
                woken[b][2] += h;
            }
        }
        for (std::size_t b = 0; b < bands.size(); ++b)
            std::cout << "sand-wet-mixed " << names[b] << " woken grains=" << woken[b][0] << " intact share="
                      << woken[b][1] / std::max(woken[b][0], 1.0) << " mean hardening=" << woken[b][2] / std::max(woken[b][0], 1.0)
                      << '\n';
        std::cout << "sand-wet-mixed still packed in the bands=" << still_packed << '\n';
        Require(still_packed == 0, "the clump must wake once its support is gone");
        const auto intact = [&](std::size_t b) { return woken[b][1] / std::max(woken[b][0], 1.0); };
        const auto mean_h = [&](std::size_t b) { return woken[b][2] / std::max(woken[b][0], 1.0); };
        Require(intact(0) > 0.9 && intact(2) < 0.1, "all-intact and all-cracked cells must wake as they packed");
        Require(intact(1) > 0.35 && intact(1) < 0.65, "a mixed cell must wake with about half its grains intact");
        Require(mean_h(1) > 0.28 && mean_h(1) < 0.43, "a mixed cell must wake with its mean hardening");
        app.replay_mode_ = false;
    }

    // The domain edge is a wall like painted rock: grains rest on its outer face (before: clamped half a cell inside it,
    // and the grid separated them a node early, while painted walls stop grains at their face).
    static void SandDomainEdge(PowderApp& app) {
        app.config_ = {};
        app.config_.sand_sleep_ticks = 0;
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 0, 100, 1000, 4);    // a painted floor, its face at y = 104
        PaintRock(app, 900, 104, 4, 100);   // a painted wall, its face at x = 900
        auto sand = SandBlock(0, 0, 20, 20, 0.0F);  // in the domain's bottom-left corner
        const auto walled = SandBlock(880, 104, 900, 124, 0.0F);
        sand.insert(sand.end(), walled.begin(), walled.end());
        WriteParticles(app, app.sand_state_.particles, sand);
        for (int t = 0; t < 120; ++t) {
            app.RunBoundaryPass();
            app.RunSandPass(app.config_.fixed_dt);
            Require(CleanCounters(app.ReadDebugCounters()), "the edge scene must report no failures");
        }
        // Gaps between the grains and the face they rest on: domain floor and left edge, painted floor and wall.
        std::array<double, 4> gap{1e9, 1e9, 1e9, 1e9};
        for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles)) {
            if (p[4] < 0.5F) continue;
            if (p[0] < 500.0F) {
                gap[0] = std::min(gap[0], static_cast<double>(p[1]));
                gap[1] = std::min(gap[1], static_cast<double>(p[0]));
            } else {
                gap[2] = std::min(gap[2], p[1] - 104.0);
                gap[3] = std::min(gap[3], 900.0 - p[0]);
            }
        }
        std::cout << "sand-domain-edge gaps domain floor/left=" << gap[0] << '/' << gap[1] << " painted floor/wall=" << gap[2]
                  << '/' << gap[3] << '\n';
        for (double g : gap) Require(g >= 0.0 && g < 0.25, "grains must rest against the face of the wall that holds them");
        Require(std::abs(gap[0] - gap[2]) < 0.1 && std::abs(gap[1] - gap[3]) < 0.1,
                "the domain edge must hold grains like a painted wall");
        app.replay_mode_ = false;
    }

    // Painted rock is material 2; the next boundary pass turns it into the full and static masks.
    static void PaintRock(PowderApp& app, int x, int y, int width, int height, GLushort material = 2) {
        const std::vector<GLushort> rock(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), material);
        if (material == 3) app.wood_present_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RED_INTEGER, GL_UNSIGNED_SHORT, rock.data());
        app.boundary_state_.static_dirty = true;
    }

    // Four particles per cell at random places inside each cell of [x0, x1) x [y0, y1): the density noise a settled pool has.
    static std::vector<Particle> RandomWater(int x0, int y0, int x1, int y1, std::uint32_t seed, float vx = 0.0F, float vy = 0.0F) {
        std::vector<Particle> particles;
        std::uint32_t state = seed;
        const auto uniform = [&]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<float>(state >> 8) / 16777216.0F;
        };
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                for (int i = 0; i < 4; ++i) particles.push_back({x + uniform(), y + uniform(), vx, vy, 1.0F, 1.0F});
        return particles;
    }

    struct WaterProbe {
        std::size_t count = 0;
        double mass = 0.0, level = 0.0, level_spread = 0.0, bulk_mean = 0.0, bulk_std = 0.0, bulk_min = 0.0, bulk_max = 0.0;
        double void_fraction = 0.0, dense_fraction = 0.0, mean_speed = 0.0, max_speed = 0.0;
    };

    // The bilinear cell density the solver sees (particle mass per cell over rest_mass), the surface height of each
    // column in [x_min, x_max] and the density statistics of the bulk at least four cells below it.
    static std::vector<std::uint8_t> ReadWaterPacked(PowderApp& app) {
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, app.water_state_.packed);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, packed.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        return packed;
    }
    static WaterProbe ProbeWater(PowderApp& app, int x_min, int x_max, double rest_mass = 4.0) {
        constexpr int rows = 440;
        constexpr int width = PowderApp::kGridWidth;
        WaterProbe probe;
        std::vector<float> density(static_cast<std::size_t>(width) * rows, 0.0F);
        double speed_sum = 0.0;
        for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
            if (p[4] < 0.5F) continue;
            ++probe.count;
            probe.mass += p[5];
            const double speed = std::hypot(p[2], p[3]);
            speed_sum += speed;
            probe.max_speed = std::max(probe.max_speed, speed);
            const float sx = p[0] - 0.5F, sy = p[1] - 0.5F;
            const int bx = static_cast<int>(std::floor(sx)), by = static_cast<int>(std::floor(sy));
            const float fx = sx - static_cast<float>(bx), fy = sy - static_cast<float>(by);
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const int cx = bx + i, cy = by + j;
                    if (cx < 0 || cx >= width || cy < 0 || cy >= rows) continue;
                    density[static_cast<std::size_t>(cy * width + cx)] += p[5] * (i ? fx : 1.0F - fx) * (j ? fy : 1.0F - fy);
                }
        }
        // Packed calm water counts its particles and adds the rest-density deposit water_mac_build.comp gives the solver.
        const auto packed = ReadWaterPacked(app);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < width; ++x) {
                const int n = packed[static_cast<std::size_t>(y * width + x)] & 127;
                if (n == 0) continue;
                probe.count += static_cast<std::size_t>(n);
                probe.mass += n * app.config_.water_particle_mass;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const int cx = x + i, cy = y + j;
                        if (cx < 0 || cx >= width || cy < 0 || cy >= rows) continue;
                        density[static_cast<std::size_t>(cy * width + cx)] +=
                            static_cast<float>(app.config_.water_rest_density * (i == 0 ? 0.75 : 0.125) * (j == 0 ? 0.75 : 0.125));
                    }
            }
        probe.mean_speed = probe.count > 0 ? speed_sum / static_cast<double>(probe.count) : 0.0;
        double level_sum = 0.0, level_square = 0.0, sum = 0.0, square = 0.0, minimum = 1e30, maximum = -1e30;
        std::size_t columns = 0, bulk = 0, voids = 0, dense = 0;
        for (int x = x_min + 4; x <= x_max - 4; ++x) {
            int top = -1;
            for (int y = rows - 1; y >= 0; --y)
                if (density[static_cast<std::size_t>(y * width + x)] >= 0.5 * rest_mass) { top = y; break; }
            if (top < 0) continue;
            ++columns;
            level_sum += top + 1.0;
            level_square += (top + 1.0) * (top + 1.0);
            for (int y = 3; y <= top - 4; ++y) {
                const double d = density[static_cast<std::size_t>(y * width + x)] / rest_mass;
                ++bulk;
                sum += d;
                square += d * d;
                minimum = std::min(minimum, d);
                maximum = std::max(maximum, d);
                voids += d < 0.5;
                dense += d > 1.5;
            }
        }
        if (columns > 0) {
            probe.level = level_sum / static_cast<double>(columns);
            probe.level_spread = std::sqrt(std::max(0.0, level_square / static_cast<double>(columns) - probe.level * probe.level));
        }
        if (bulk > 0) {
            probe.bulk_mean = sum / static_cast<double>(bulk);
            probe.bulk_std = std::sqrt(std::max(0.0, square / static_cast<double>(bulk) - probe.bulk_mean * probe.bulk_mean));
            probe.bulk_min = minimum;
            probe.bulk_max = maximum;
            probe.void_fraction = static_cast<double>(voids) / static_cast<double>(bulk);
            probe.dense_fraction = static_cast<double>(dense) / static_cast<double>(bulk);
        }
        return probe;
    }

    static void PrintProbe(const char* scene, int tick, const WaterProbe& probe) {
        std::cout << "  " << scene << " tick " << tick << ": particles=" << probe.count << " mass=" << probe.mass
                  << " level=" << probe.level << "+-" << probe.level_spread << " bulk density mean/std/min/max="
                  << probe.bulk_mean << '/' << probe.bulk_std << '/' << probe.bulk_min << '/' << probe.bulk_max
                  << " voids(<0.5)=" << probe.void_fraction << " dense(>1.5)=" << probe.dense_fraction
                  << " speed mean/max=" << probe.mean_speed << '/' << probe.max_speed << '\n';
    }

    // Renders the simulation into an offscreen target and saves a crop as a binary PPM (row 0 on top).
    struct FrameTarget {
        GLuint framebuffer = 0, color = 0;
        int width = 0, height = 0;
        FrameTarget() = default;
        FrameTarget(const FrameTarget&) = delete;
        FrameTarget& operator=(const FrameTarget&) = delete;
        ~FrameTarget() {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &color);
        }
    };

    static void OpenFrameTarget(PowderApp& app, FrameTarget& target) {
        glfwSetWindowSize(app.window_, PowderApp::kGridWidth, PowderApp::kGridHeight);
        glfwPollEvents();
        glfwGetFramebufferSize(app.window_, &target.width, &target.height);
        Require(target.width == PowderApp::kGridWidth && target.height == PowderApp::kGridHeight,
                "frame capture needs a framebuffer of one pixel per cell");
        if (app.fullscreen_vao_ == 0) glGenVertexArrays(1, &app.fullscreen_vao_);
        glGenTextures(1, &target.color);
        glBindTexture(GL_TEXTURE_2D, target.color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, target.width, target.height);
        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "frame capture framebuffer incomplete");
    }

    static void SaveFrame(PowderApp& app, const FrameTarget& target, const std::filesystem::path& path,
                          int x0, int y0, int width, int height) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        app.Render();
        std::vector<unsigned char> pixels(static_cast<std::size_t>(target.width) * target.height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        for (int row = height - 1; row >= 0; --row)
            for (int col = 0; col < width; ++col)
                out.write(reinterpret_cast<const char*>(&pixels[static_cast<std::size_t>((y0 + row) * target.width + x0 + col) * 4]), 3);
        Require(out.good(), "could not write frame " + path.string());
    }

    // With POWDER_SAND_FRAMES set, saves unlit PPM frames of the sand scenes there.
    struct SandFrames {
        PowderApp& app;
        const char* directory = std::getenv("POWDER_SAND_FRAMES");
        FrameTarget target;
        explicit SandFrames(PowderApp& owner) : app(owner) {
            if (directory == nullptr) return;
            std::filesystem::create_directories(directory);
            OpenFrameTarget(app, target);
        }
        void Save(const std::string& name, int x0, int y0, int width, int height) {
            if (directory == nullptr) return;
            const bool lighting = app.lighting_enabled_;
            app.lighting_enabled_ = false;
            SaveFrame(app, target, std::filesystem::path(directory) / (name + ".ppm"), x0, y0, width, height);
            app.lighting_enabled_ = lighting;
        }
    };

    // Conductance of a liquid-air face of the water pressure operator (pressure_cg.comp), for the CPU oracles: 1 / theta
    // with theta the position of the phi threshold between the two cell centres, clamped below by water_ghost_theta_min.
    static float AirConductance(const powder_config::SimulationConfig& config, float phi_liquid, float phi_air) {
        if (config.water_ghost_theta_min <= 0.0F) return 1.0F;
        const float inside = phi_liquid - config.water_liquid_phi_threshold, outside = phi_air - config.water_liquid_phi_threshold;
        if (!(inside < 0.0F && outside > 0.0F)) return 1.0F;
        return 1.0F / std::clamp(inside / (inside - outside), config.water_ghost_theta_min, 1.0F);
    }

    // Counters 0..3 are failures; index 4 counts water particles cut by the speed cap, a diagnostic.
    static bool CleanCounters(const std::array<GLuint, 5>& counters) {
        return counters[0] == 0 && counters[1] == 0 && counters[2] == 0 && counters[3] == 0;
    }

    // Density control: a randomly seeded pool stays at rest density with its level at the particle volume, and a dam
    // break settles into the same band. Without it the pool kept its seeding noise (std 0.15, a tenth of a percent of
    // cells over 1.5 times rest) and the settled dam break had 3% voids, 6% over-dense cells and std 0.29.
    static void WaterDensity(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const auto tick = [&]() {
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            Require(CleanCounters(app.ReadDebugCounters()), "water density scenes must report no failures");
        };
        std::ostringstream failures;
        const auto check = [&](const char* scene, const WaterProbe& probe, double mass, double level, double std_limit) {
            PrintProbe(scene, 0, probe);
            if (std::abs(probe.mass - mass) > 0.005 * mass) failures << " [" << scene << " lost or gained mass]";
            if (std::abs(probe.level - level) > 0.6) failures << " [" << scene << " level " << probe.level << " is not near " << level << ']';
            if (std::abs(probe.bulk_mean - 1.0) > 0.02) failures << " [" << scene << " bulk density mean " << probe.bulk_mean << ']';
            if (probe.bulk_std > std_limit) failures << " [" << scene << " bulk density std " << probe.bulk_std << ']';
            if (probe.void_fraction > 0.005) failures << " [" << scene << " has " << probe.void_fraction << " voids]";
            if (probe.dense_fraction > 0.005) failures << " [" << scene << " has " << probe.dense_fraction << " over-dense cells]";
        };
        app.InitializeState();
        PaintRock(app, 399, 0, 1, 300);
        PaintRock(app, 600, 0, 1, 300);
        const auto pool = RandomWater(400, 0, 600, 60, 12345U);
        WriteParticles(app, app.water_state_.particles, pool);
        for (int t = 0; t < 300; ++t) tick();
        check("settled pool", ProbeWater(app, 400, 599), static_cast<double>(pool.size()), 60.0, 0.08);

        app.InitializeState();
        PaintRock(app, 299, 0, 1, 400);
        PaintRock(app, 700, 0, 1, 400);
        const auto column = RandomWater(300, 0, 360, 150, 777U);
        WriteParticles(app, app.water_state_.particles, column);
        for (int t = 0; t < 1200; ++t) tick();
        // 36,000 particles at rest density fill 9,000 cells: a level of 22.5 across the 400-cell box.
        // Undamped water at real gravity still sloshes after 20 s; with consistent sparse-face momentum (water-sparse-face)
        // the bulk density spread is back under the original 0.12 (measured 0.097).
        check("settled dam break", ProbeWater(app, 300, 699), static_cast<double>(column.size()), 22.5, 0.12);
        app.replay_mode_ = false;
        Require(failures.str().empty(), "water density left the rest-density band:" + failures.str());
    }

    // A painted wall is free-slip like the domain edge: two identical slabs sliding at 20 cells/s, one over the domain
    // floor and one over a painted floor, lose speed alike (only the damping acts on either). The painted floor used
    // to read zero velocity from the faces inside it and dragged the slab to 0.935 of the domain slab.
    static void WaterFreeSlip(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // At real gravity a 6-cell slab collapses sideways at sqrt(g h) ~ 110 cells/s; the old weak gravity isolates the
        // wall friction this test is about.
        app.config_.water_gravity = -140.0F;
        app.config_.water_bulk_damping = 0.0F;  // supported slabs would lose speed to it alike; the test isolates the walls
        app.InitializeState();
        PaintRock(app, 250, 0, 160, 100);
        auto slabs = RandomWater(100, 0, 140, 6, 31U, 20.0F, 0.0F);
        const auto painted = RandomWater(300, 100, 340, 106, 31U, 20.0F, 0.0F);
        slabs.insert(slabs.end(), painted.begin(), painted.end());
        WriteParticles(app, app.water_state_.particles, slabs);
        for (int t = 0; t < 30; ++t) {
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            Require(CleanCounters(app.ReadDebugCounters()), "the sliding slabs must report no failures");
        }
        std::array<double, 2> bottom{}, bottom_count{};
        for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
            if (p[4] < 0.5F) continue;
            const std::size_t slab = p[0] > 250.0F ? 1 : 0;
            if (p[1] - (slab == 1 ? 100.0F : 0.0F) < 2.0F) {
                bottom[slab] += p[2];
                ++bottom_count[slab];
            }
        }
        const double domain = bottom[0] / bottom_count[0], rock = bottom[1] / bottom_count[1];
        // Half a second of damping alone: 20 * 0.992^60.
        const double damped = 20.0 * std::pow(static_cast<double>(app.config_.water_velocity_damping), 60.0);
        std::cout << "water free slip: bottom-row vx domain floor=" << domain << " painted floor=" << rock << " damping only=" << damped << '\n';
        app.replay_mode_ = false;
        Require(std::abs(rock / domain - 1.0) < 0.02, "a painted floor must slow a sliding slab like the domain floor does");
        Require(rock > 0.95 * damped, "a painted floor must not drag the slab below its damping-only speed");
    }

    // Free fall passes the old 72 cells/s cap, the substep count follows the speed, and nothing is clipped; a cap
    // below the fall speed clips, and every clipped particle is counted.
    static void WaterFreeFall(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const auto fall = [&](int ticks) {
            app.InitializeState();
            WriteParticles(app, app.water_state_.particles, RandomWater(480, 900, 520, 910, 5U));
            std::uint64_t clamped = 0;
            int first_substeps = 0, last_substeps = 0;
            for (int t = 0; t < ticks; ++t) {
                app.RunFrame(app.config_.fixed_dt, nullptr, true);
                const auto counters = app.ReadDebugCounters();
                Require(CleanCounters(counters), "free fall must report no failures");
                clamped += counters[4];
                if (t == 0) first_substeps = app.water_state_.substeps;
                last_substeps = app.water_state_.substeps;
            }
            return std::array<double, 4>{ProbeWater(app, 400, 600).max_speed, static_cast<double>(clamped),
                                         static_cast<double>(first_substeps), static_cast<double>(last_substeps)};
        };
        // 45 ticks: 0.75 s of fall from y 900, about 1470 cells/s, before the water reaches the floor.
        const auto fell = fall(45);
        std::cout << "water free fall: max speed=" << fell[0] << " clamped=" << fell[1] << " substeps first/last=" << fell[2]
                  << '/' << fell[3] << '\n';
        app.config_.water_max_velocity = 60.0F;
        app.config_.water_max_substeps = 3;
        const auto capped = fall(120);
        std::cout << "water free fall with a 60 cells/s cap: max speed=" << capped[0] << " clamped=" << capped[1] << '\n';
        app.config_ = {};
        app.replay_mode_ = false;
        Require(fell[0] > 1000.0 && fell[0] < app.config_.water_max_velocity, "free fall must reach real-gravity speeds uncapped");
        Require(fell[1] == 0.0, "free fall under the cap must not count a clamp");
        Require(fell[2] == 1.0 && fell[3] >= 2.0, "still water takes one substep and falling water more");
        Require(capped[0] <= 60.0 + 0.5 && capped[1] > 0.0, "a cap below the fall speed must clip and count");
    }

    // Diagnostic scenes behind the water-solver review: a settled pool, a dam break, a stream into a pool and a slide
    // down a painted ramp. Prints metrics; with POWDER_WATER_FRAMES set it also saves PPM frames there.
    static void WaterScenes(PowderApp& app) {
        app.config_ = powder_config::LoadConfigFromEnvironment();
        app.replay_mode_ = true;
        const bool lighting = app.lighting_enabled_;
        app.lighting_enabled_ = false;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const char* frames_env = std::getenv("POWDER_WATER_FRAMES");
        const std::filesystem::path frames = frames_env != nullptr ? std::filesystem::path(frames_env) : std::filesystem::path();
        if (!frames.empty()) std::filesystem::create_directories(frames);
        FrameTarget target;
        if (!frames.empty()) OpenFrameTarget(app, target);
        std::array<std::uint64_t, 5> counters{};
        int substep_total = 0, substep_max = 0, ticks_run = 0;
        double wall_ms = 0.0, iteration_total = 0.0;
        GLuint iteration_max = 0;
        const auto tick = [&](const PowderApp::BrushConfig* brush) {
            const auto begin = std::chrono::steady_clock::now();
            app.RunFrame(app.config_.fixed_dt, brush, true);
            const auto frame_counters = app.ReadDebugCounters();
            wall_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::array<GLuint, 8> solver{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.pressure_scalars);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(solver), solver.data());
            iteration_total += solver[6];
            iteration_max = std::max(iteration_max, solver[6]);
            for (std::size_t i = 0; i < frame_counters.size() && i < counters.size(); ++i) counters[i] += frame_counters[i];
            substep_total += app.water_state_.substeps;
            substep_max = std::max(substep_max, app.water_state_.substeps);
            ++ticks_run;
        };
        const auto shot = [&](const char* scene, int tick_index, int x0, int y0, int width, int height) {
            if (frames.empty()) return;
            SaveFrame(app, target, frames / (std::string(scene) + "_" + std::to_string(tick_index) + ".ppm"), x0, y0, width, height);
        };
        const auto finish = [&](const char* scene) {
            std::cout << "  " << scene << ": water substeps mean/max=" << static_cast<double>(substep_total) / std::max(1, ticks_run)
                      << '/' << substep_max << " wall ms/tick=" << wall_ms / std::max(1, ticks_run)
                      << " last-solve CG iterations mean/max=" << iteration_total / std::max(1, ticks_run) << '/' << iteration_max
                      << " debug counters=";
            for (std::uint64_t value : counters) std::cout << value << ' ';
            std::cout << '\n';
            counters = {};
            substep_total = substep_max = ticks_run = 0;
            wall_ms = iteration_total = 0.0;
            iteration_max = 0;
        };

        // POWDER_WATER_SCENES=pool,dam,... runs only the named scenes.
        const char* scene_filter = std::getenv("POWDER_WATER_SCENES");
        const auto wanted = [&](const char* name) {
            return scene_filter == nullptr || std::string(scene_filter).find(name) != std::string::npos;
        };
        std::cout << "water scene: settled pool 200x60 cells, random 4 particles per cell\n";
        if (wanted("pool")) {
            app.InitializeState();
            PaintRock(app, 399, 0, 1, 300);
            PaintRock(app, 600, 0, 1, 300);
            const auto pool = RandomWater(400, 0, 600, 60, 12345U);
            WriteParticles(app, app.water_state_.particles, pool);
            const double mass0 = static_cast<double>(pool.size());
            for (int t = 1; t <= 600; ++t) {
                tick(nullptr);
                if (t == 1 || t == 120 || t == 600) shot("pool", t, 380, 0, 240, 110);
                if (t == 1 || t == 30 || t == 60 || t == 120 || t == 240 || t == 400 || t == 600) {
                    const auto probe = ProbeWater(app, 400, 599);
                    PrintProbe("pool", t, probe);
                    if (t == 600) std::cout << "  pool mass drift=" << (probe.mass - mass0) / mass0 << '\n';
                }
            }
            finish("pool");
        }

        std::cout << "water scene: dam break, 60x150 column in a painted box\n";
        if (wanted("dam")) {
            app.InitializeState();
            PaintRock(app, 299, 0, 1, 400);
            PaintRock(app, 700, 0, 1, 400);
            const auto column = RandomWater(300, 0, 360, 150, 777U);
            WriteParticles(app, app.water_state_.particles, column);
            const double mass0 = static_cast<double>(column.size());
            for (int t = 1; t <= 600; ++t) {
                tick(nullptr);
                if (t == 1 || t == 15 || t == 30 || t == 60 || t == 120 || t == 240 || t == 600) shot("dam", t, 280, 0, 440, 200);
                if (t == 30 || t == 60 || t == 120 || t == 240 || t == 360 || t == 480 || t == 600) {
                    double front = 0.0;
                    std::vector<float> xs;
                    for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                        if (p[4] > 0.5F) xs.push_back(p[0]);
                    std::sort(xs.begin(), xs.end());
                    front = xs[static_cast<std::size_t>(static_cast<double>(xs.size() - 1) * 0.99)];
                    const auto probe = ProbeWater(app, 300, 699);
                    std::cout << "  dam front(p99 x)=" << front << " mass drift=" << (probe.mass - mass0) / mass0 << '\n';
                    PrintProbe("dam", t, probe);
                }
            }
            {
                auto fastest = ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles);
                std::erase_if(fastest, [](const auto& p) { return p[4] < 0.5F; });
                std::sort(fastest.begin(), fastest.end(), [](const auto& a, const auto& b) { return std::hypot(a[2], a[3]) > std::hypot(b[2], b[3]); });
                for (std::size_t i = 0; i < 5 && i < fastest.size(); ++i)
                    std::cout << "  dam fastest p=(" << fastest[i][0] << ',' << fastest[i][1] << ") v=(" << fastest[i][2] << ',' << fastest[i][3] << ")\n";
            }
            finish("dam");
        }

        std::cout << "water scene: stream from y=600 into a 40-cell pool\n";
        if (wanted("stream")) {
            app.InitializeState();
            PaintRock(app, 399, 0, 1, 300);
            PaintRock(app, 600, 0, 1, 300);
            WriteParticles(app, app.water_state_.particles, RandomWater(400, 0, 600, 40, 99U));
            app.brush_mode_ = PowderApp::BrushMode::Water;
            app.brush_x_ = 500;
            app.brush_y_ = 600;
            app.brush_radius_ = 4;
            const auto stream = app.BuildBrushConfig();
            double fastest = 0.0;
            for (int t = 1; t <= 600; ++t) {
                tick(t <= 200 ? &stream : nullptr);
                if (t == 100 || t == 200 || t == 360 || t == 600) shot("stream", t, 380, 0, 240, 640);
                if (t % 100 == 0) {
                    const auto probe = ProbeWater(app, 400, 599);
                    fastest = std::max(fastest, probe.max_speed);
                    PrintProbe("stream", t, probe);
                }
            }
            std::cout << "  stream fastest particle=" << fastest << '\n';
            finish("stream");
        }

        std::cout << "water scene: slide down a painted 0.4-slope ramp\n";
        if (wanted("ramp")) {
            app.InitializeState();
            const auto ramp_top = [](int x) { return 300 - static_cast<int>(0.4 * (x - 200)); };
            for (int x = 200; x < 700; ++x) PaintRock(app, x, 0, 1, ramp_top(x));
            std::vector<Particle> water;
            for (int x = 205; x < 245; ++x) {
                const auto column = RandomWater(x, ramp_top(x) + 1, x + 1, ramp_top(x) + 11, 4242U + static_cast<std::uint32_t>(x));
                water.insert(water.end(), column.begin(), column.end());
            }
            WriteParticles(app, app.water_state_.particles, water);
            for (int t = 1; t <= 240; ++t) {
                tick(nullptr);
                if (t == 1 || t == 60 || t == 120 || t == 180 || t == 240) shot("ramp", t, 190, 80, 400, 250);
                if (t % 40 == 0) {
                    double sum_x = 0.0, sum_vx = 0.0, sum_vy = 0.0;
                    std::size_t live = 0;
                    for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                        if (p[4] > 0.5F) { sum_x += p[0]; sum_vx += p[2]; sum_vy += p[3]; ++live; }
                    std::cout << "  ramp tick " << t << ": particles=" << live << " mean x=" << sum_x / live
                              << " mean v=(" << sum_vx / live << ',' << sum_vy / live << ")\n";
                }
            }
            finish("ramp");
        }

        std::cout << "water scene: slabs sliding at 20 cells/s over a painted floor and the domain floor\n";
        if (wanted("slide")) {
            app.InitializeState();
            PaintRock(app, 250, 0, 160, 100);
            auto domain = RandomWater(100, 0, 140, 6, 31U, 20.0F, 0.0F);
            const auto painted = RandomWater(300, 100, 340, 106, 31U, 20.0F, 0.0F);
            domain.insert(domain.end(), painted.begin(), painted.end());
            WriteParticles(app, app.water_state_.particles, domain);
            for (int t = 1; t <= 30; ++t) {
                tick(nullptr);
                if (t == 10 || t == 20 || t == 30) {
                    std::array<double, 2> sum{}, count{}, all_sum{}, all_count{}, height{};
                    for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
                        if (p[4] < 0.5F) continue;
                        const auto slab = static_cast<std::size_t>(p[0] > 250.0F ? 1 : 0);
                        all_sum[slab] += p[2];
                        ++all_count[slab];
                        height[slab] += p[1] - (slab == 1 ? 100.0F : 0.0F);
                        if (p[1] - (slab == 1 ? 100.0F : 0.0F) < 2.0F) { sum[slab] += p[2]; ++count[slab]; }
                    }
                    std::cout << "  slide tick " << t << ": mean vx of the bottom two rows, domain floor=" << sum[0] / count[0]
                              << " painted floor=" << sum[1] / count[1] << "; whole slab " << all_sum[0] / all_count[0] << " / "
                              << all_sum[1] / all_count[1] << "; mean height " << height[0] / all_count[0] << " / "
                              << height[1] / all_count[1] << '\n';
                }
            }
            finish("slide");
        }

        std::cout << "water scene: free fall from y=900\n";
        if (wanted("fall")) {
            app.InitializeState();
            WriteParticles(app, app.water_state_.particles, RandomWater(480, 900, 520, 910, 5U));
            double fastest = 0.0;
            for (int t = 1; t <= 150; ++t) {
                tick(nullptr);
                if (t % 25 == 0) {
                    const auto probe = ProbeWater(app, 400, 600);
                    fastest = std::max(fastest, probe.max_speed);
                    double mean_y = 0.0;
                    std::size_t live = 0;
                    for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                        if (p[4] > 0.5F) { mean_y += p[1]; ++live; }
                    std::cout << "  fall tick " << t << ": mean y=" << mean_y / live << " speed mean/max=" << probe.mean_speed
                              << '/' << probe.max_speed << '\n';
                }
            }
            finish("fall");
        }
        app.lighting_enabled_ = lighting;
        app.replay_mode_ = false;
    }

    // Surface slopes of a sand pile between x_min and x_max: per-column 95th-percentile top, least squares on each flank
    // between 20% and 80% of the peak. Returns {left deg, right deg, mass centroid x, peak}; angles are -1 if unmeasurable.
    static std::array<double, 4> SandCone(const std::vector<SandParticle>& sand, int x_min, int x_max) {
        std::vector<std::vector<float>> columns(static_cast<std::size_t>(x_max - x_min + 1));
        double sum_x = 0.0, count = 0.0;
        for (const auto& p : sand) {
            if (p[4] < 0.5F || p[0] < x_min || p[0] >= x_max + 1) continue;
            columns[static_cast<std::size_t>(static_cast<int>(p[0]) - x_min)].push_back(p[1]);
            sum_x += p[0];
            ++count;
        }
        std::vector<double> top(columns.size(), -1.0);
        double peak = 0.0;
        int first = -1, last = -1, apex = 0;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            auto& column = columns[i];
            if (column.size() < 2) continue;
            std::sort(column.begin(), column.end());
            top[i] = column[static_cast<std::size_t>(static_cast<double>(column.size() - 1) * 0.95)] + 0.5;
            if (top[i] >= peak) { peak = top[i]; apex = static_cast<int>(i); }
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
        }
        const auto flank = [&](int from, int to) {
            double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int x = from; x <= to; ++x) {
                const double h = top[static_cast<std::size_t>(x)];
                if (h < 0.2 * peak || h > 0.8 * peak) continue;
                ++n; sx += x; sy += h; sxx += static_cast<double>(x) * x; sxy += x * h;
            }
            if (n < 3) return -1.0;
            return std::atan(std::abs((n * sxy - sx * sy) / (n * sxx - sx * sx))) * 180.0 / 3.14159265358979;
        };
        if (first < 0) return {-1.0, -1.0, 0.0, 0.0};
        return {flank(first, apex), flank(apex, last), count > 0 ? sum_x / count : 0.0, peak};
    }

    // Water surface per column between x_min and x_max (highest cell whose three-column sum holds half the rest mass):
    // mean, std and max-min.
    static std::array<double, 3> WaterSurface(PowderApp& app, int x_min, int x_max) {
        constexpr int rows = 400;
        std::vector<float> density(static_cast<std::size_t>(PowderApp::kGridWidth) * rows, 0.0F);
        for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles)) {
            if (p[4] < 0.5F) continue;
            const int x = static_cast<int>(p[0]), y = static_cast<int>(p[1]);
            if (x < 0 || x >= PowderApp::kGridWidth || y < 0 || y >= rows) continue;
            density[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)] += p[5];
        }
        double sum = 0, square = 0, lo = 1e30, hi = -1e30, n = 0;
        for (int x = x_min; x <= x_max; ++x) {
            int top = 0;
            for (int y = rows - 1; y >= 0; --y) {
                double d = 0;
                for (int dx = -1; dx <= 1; ++dx) d += density[static_cast<std::size_t>(y * PowderApp::kGridWidth + x + dx)];
                if (d >= 0.5 * 4.0 * 3.0) { top = y + 1; break; }
            }
            sum += top; square += static_cast<double>(top) * top; lo = std::min<double>(lo, top); hi = std::max<double>(hi, top); ++n;
        }
        const double mean = sum / n;
        return {mean, std::sqrt(std::max(0.0, square / n - mean * mean)), hi - lo};
    }

    // Look-and-feel scenes behind the realism pass, driven through the interactive stroke path (density gate, cursor
    // velocity). Prints metrics; with POWDER_FEEL_FRAMES set it saves PPM frames there. POWDER_FEEL_SCENES filters.
    static void FeelScenes(PowderApp& app) {
        app.config_ = powder_config::LoadConfigFromEnvironment();
        const bool lighting = app.lighting_enabled_;
        app.lighting_enabled_ = false;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const char* frames_env = std::getenv("POWDER_FEEL_FRAMES");
        const std::filesystem::path frames = frames_env != nullptr ? std::filesystem::path(frames_env) : std::filesystem::path();
        if (!frames.empty()) std::filesystem::create_directories(frames);
        FrameTarget target;
        if (!frames.empty()) OpenFrameTarget(app, target);
        const char* scene_filter = std::getenv("POWDER_FEEL_SCENES");
        const auto wanted = [&](const char* name) { return scene_filter == nullptr || std::string(scene_filter).find(name) != std::string::npos; };
        std::array<std::uint64_t, 5> counters{};
        int water_substeps = 0, sand_substeps = 0;
        double wall_ms = 0.0;
        int ticks = 0;
        const auto start = [&](PowderApp::BrushMode mode, int radius) {
            app.InitializeState();
            app.replay_mode_ = false;
            app.brush_mode_ = mode;
            app.brush_radius_ = radius;
            app.stroke_ = {};
            app.cursor_trail_.clear();
            counters = {};
            water_substeps = sand_substeps = ticks = 0;
            wall_ms = 0.0;
        };
        // One tick; painting at (x, y) when paint is set, reported as a cursor event like the interactive loop.
        const auto tick = [&](bool paint, int x, int y) {
            app.painting_ = app.cursor_in_sim_ = paint;
            if (paint) {
                app.brush_x_ = x;
                app.brush_y_ = y;
                app.cursor_trail_.push_back({x, y});
            }
            const auto begin = std::chrono::steady_clock::now();
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            const auto c = app.ReadDebugCounters();
            wall_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            for (std::size_t i = 0; i < c.size(); ++i) counters[i] += c[i];
            water_substeps = std::max(water_substeps, app.water_state_.substeps);
            sand_substeps = std::max(sand_substeps, app.sand_state_.substeps);
            ++ticks;
        };
        const auto shot = [&](const char* scene, int t, int x0, int y0, int w, int h) {
            if (!frames.empty()) SaveFrame(app, target, frames / (std::string(scene) + "_" + std::to_string(t) + ".ppm"), x0, y0, w, h);
        };
        const auto finish = [&](const char* scene) {
            std::cout << "  " << scene << ": water substeps max=" << water_substeps << " sand substeps max=" << sand_substeps
                      << " wall ms/tick=" << wall_ms / std::max(1, ticks) << " water=" << app.ReadWaterLiveParticles()
                      << " sand=" << app.ReadSandLiveParticles() << " counters=";
            for (auto v : counters) std::cout << v << ' ';
            std::cout << '\n';
            Require(counters[0] == 0 && counters[2] == 0 && counters[3] == 0, std::string(scene) + " reported failures");
        };
        const auto top_particle = [&]() {
            double highest = 0.0;
            for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                if (p[4] > 0.5F && p[1] < 400.0F && std::abs(p[0] - 500.0F) > 12.0F) highest = std::max<double>(highest, p[1]);
            return highest;
        };

        if (wanted("water-pour")) {
            // Water poured on one spot from y 400 for 1.5 s between painted walls 300 cells apart, then 4.5 s free.
            start(PowderApp::BrushMode::Water, 8);
            PaintRock(app, 349, 0, 1, 400);
            PaintRock(app, 650, 0, 1, 400);
            double splash = 0.0;
            for (int t = 1; t <= 360; ++t) {
                const bool pouring = t <= 90;
                tick(pouring, 500, 400);
                if (pouring && t > 30) splash = std::max(splash, top_particle() - WaterSurface(app, 360, 640)[0]);
                if (t == 30 || t == 60 || t == 90 || t == 105 || t == 120 || t == 150 || t == 180 || t == 240 || t == 360)
                    shot("pour", t, 340, 0, 320, 420);
                if (t >= 90 && t % 15 == 0) {
                    const auto s = WaterSurface(app, 360, 640);
                    const auto left = WaterSurface(app, 360, 420), right = WaterSurface(app, 580, 640);
                    std::cout << "  pour t=" << t / 60.0 << "s level=" << s[0] << " std=" << s[1] << " max-min=" << s[2]
                              << " outer mean L/R=" << left[0] << '/' << right[0] << " outer std L/R=" << left[1] << '/' << right[1] << '\n';
                }
            }
            std::cout << "  pour splash crown height above level=" << splash << '\n';
            finish("pour");
        }
        if (wanted("sandpour")) {
            // Three piles, each poured on one spot from y 300 for 5 s, then settled for 3 s.
            start(PowderApp::BrushMode::Sand, 3);
            for (int t = 1; t <= 480; ++t) {
                if (t <= 300) {
                    for (int x : {200, 500, 800}) {
                        app.brush_x_ = x;
                        app.brush_y_ = 300;
                        app.StampBrush(app.BuildBrushConfig());
                    }
                }
                tick(false, 0, 0);
                if (t == 100 || t == 300 || t == 480) shot("sandpour", t, 0, 0, 1000, 320);
            }
            const auto sand = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles);
            double left = 0, right = 0, offset = 0;
            for (int x : {200, 500, 800}) {
                const auto cone = SandCone(sand, x - 150, x + 149);
                left += cone[0] / 3;
                right += cone[1] / 3;
                offset += (cone[2] - (x + 0.5)) / 3;
                std::cout << "  sandpour x=" << x << " left/right deg=" << cone[0] << '/' << cone[1] << " centroid offset="
                          << cone[2] - (x + 0.5) << " peak=" << cone[3] << '\n';
            }
            std::cout << "  sandpour mean left/right deg=" << left << '/' << right << " mean centroid offset=" << offset << '\n';
            finish("sandpour");
        }
        for (const char* scene : {"stroke-water", "stroke-sand"}) {
            if (!wanted(scene)) continue;
            // The cursor sweeps right at 600 cells/s for half a second at y 600 (x 210 to 500), then the matter lands.
            const bool water = std::string(scene) == "stroke-water";
            start(water ? PowderApp::BrushMode::Water : PowderApp::BrushMode::Sand, 4);
            for (int t = 1; t <= 120; ++t) {
                tick(t <= 30, 200 + t * 10, 600);
                if (t == 15 || t == 30 || t == 45 || t == 60 || t == 120) shot(scene, t, 100, 0, 900, 700);
            }
            double sum = 0, n = 0;
            if (water) {
                for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                    if (p[4] > 0.5F) { sum += p[0]; ++n; }
            } else {
                for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles))
                    if (p[4] > 0.5F) { sum += p[0]; ++n; }
            }
            std::cout << "  " << scene << " landed centroid x=" << sum / std::max(1.0, n) << " (stroke centre 355)\n";
            finish(scene);
        }
        for (const char* scene : {"fall-water", "fall-sand"}) {
            if (!wanted(scene)) continue;
            const bool water = std::string(scene) == "fall-water";
            start(water ? PowderApp::BrushMode::Water : PowderApp::BrushMode::Sand, 4);
            for (int t = 1; t <= 90; ++t) {
                tick(true, 500, 950);
                if (t == 20 || t == 40 || t == 60 || t == 90) shot(scene, t, 350, 0, 300, 1000);
            }
            finish(scene);
        }
        if (wanted("sandwater")) {
            start(PowderApp::BrushMode::Sand, 4);
            PaintRock(app, 299, 0, 1, 300);
            PaintRock(app, 700, 0, 1, 300);
            WriteParticles(app, app.water_state_.particles, RandomWater(300, 0, 700, 80, 4321U));
            for (int t = 1; t <= 360; ++t) {
                tick(t <= 120, 500, 400);
                if (t == 30 || t == 60 || t == 120 || t == 240 || t == 360) shot("sandwater", t, 290, 0, 420, 420);
            }
            const auto sand = ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles, PowderApp::kMaxSandParticles);
            const auto cone = SandCone(sand, 300, 699);
            std::cout << "  sandwater pile left/right deg=" << cone[0] << '/' << cone[1] << " peak=" << cone[3] << '\n';
            finish("sandwater");
        }
        // Fire and smoke scenes render lit, as the interactive view does.
        app.lighting_enabled_ = true;
        const auto gas_report = [&](const char* scene, int t) {
            const auto stats = FireStats(app);
            std::cout << "  " << scene << " t=" << t / 60.0 << "s reaction=" << stats[0] << " heat=" << stats[1]
                      << " heat centroid y=" << stats[2] * 2.0 << " smoke=" << stats[3] << " smoke centroid y=" << stats[4] * 2.0 << " max=" << stats[5] << " gas substeps=" << app.gas_state_.substeps
                      << '\n';
        };
        if (wanted("fire-circle")) {
            // One cursor revolution of radius 120 cells in 1 s (about 750 cells/s), then 1 s free.
            start(PowderApp::BrushMode::Fire, 10);
            for (int t = 1; t <= 120; ++t) {
                const double a = 2.0 * 3.14159265358979 * t / 60.0;
                tick(t <= 60, 500 + static_cast<int>(std::lround(120.0 * std::cos(a))), 350 + static_cast<int>(std::lround(120.0 * std::sin(a))));
                if (t == 20 || t == 40 || t == 60 || t == 75 || t == 90 || t == 120) shot("fire-circle", t, 250, 150, 500, 500);
                if (t % 15 == 0) gas_report("fire-circle", t);
            }
            finish("fire-circle");
        }
        if (wanted("fire-time")) {
            // A fire brush held 1 s on a rock ledge, then released: rise, flicker and burn-out.
            start(PowderApp::BrushMode::Fire, 12);
            PaintRock(app, 380, 90, 240, 10);
            for (int t = 1; t <= 240; ++t) {
                tick(t <= 60, 500, 115);
                if (t == 10 || t == 20 || t == 30 || t == 45 || t == 60 || t == 70 || t == 90 || t == 120 || t == 180 || t == 240)
                    shot("fire-time", t, 300, 50, 400, 600);
                if (t % 10 == 0) gas_report("fire-time", t);
            }
            finish("fire-time");
        }
        if (wanted("smoke-column")) {
            start(PowderApp::BrushMode::Smoke, 10);
            for (int t = 1; t <= 240; ++t) {
                tick(t <= 120, 500, 120);
                if (t == 30 || t == 60 || t == 120 || t == 180 || t == 240) shot("smoke-column", t, 250, 50, 500, 900);
                if (t % 30 == 0) gas_report("smoke-column", t);
            }
            finish("smoke-column");
        }
        if (wanted("fire-water")) {
            // Fire burns on a basin floor; from 0.5 s water pours onto it while the fire is still fed (the two brushes
            // alternate ticks) until 2 s.
            start(PowderApp::BrushMode::Fire, 14);
            PaintRock(app, 380, 90, 240, 10);
            for (int t = 1; t <= 210; ++t) {
                const bool water_tick = t > 30 && t <= 120 && t % 2 == 0;
                app.brush_mode_ = water_tick ? PowderApp::BrushMode::Water : PowderApp::BrushMode::Fire;
                app.brush_radius_ = water_tick ? 10 : 14;
                app.stroke_ = {};
                app.cursor_trail_.clear();
                if (water_tick) tick(true, 500, 300);
                else tick(t <= 120, 500, 115);
                if (t == 60 || t == 80 || t == 100 || t == 120 || t == 150 || t == 210) shot("fire-water", t, 300, 50, 400, 500);
                if (t % 15 == 0) gas_report("fire-water", t);
            }
            finish("fire-water");
        }
        if (wanted("wood-burn")) {
            // A wood beam on a rock ledge and a wooden post, lit at the beam's left end for 0.5 s.
            start(PowderApp::BrushMode::Fire, 10);
            PaintRock(app, 300, 90, 400, 10);
            PaintRock(app, 300, 100, 400, 12, 3);
            PaintRock(app, 640, 112, 14, 160, 3);
            for (int t = 1; t <= 900; ++t) {
                tick(t <= 30, 310, 125);
                if (t == 30 || t == 120 || t == 300 || t == 480 || t == 660 || t == 900) shot("wood-burn", t, 250, 50, 500, 400);
                if (t % 60 == 0) {
                    const auto beam = WoodCensus(app, 300, 100, 700, 112);
                    std::cout << "  wood-burn t=" << t / 60.0 << "s beam intact/burning/charred/gone=" << beam[0] << '/' << beam[1]
                              << '/' << beam[2] << '/' << beam[3] << '\n';
                }
            }
            finish("wood-burn");
        }
        app.painting_ = app.cursor_in_sim_ = false;
        app.lighting_enabled_ = lighting;
    }

    // ---- Sleep-packing (sand_sleep.comp / sand_pack.comp) ----
    struct SandCount {
        std::uint64_t active = 0, packed = 0;
        std::uint64_t Total() const { return active + packed; }
    };
    static SandCount CountSand(PowderApp& app) {
        return {app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                        PowderApp::kSandParticleComponents).count,
                app.ReadSandPackedSum()};
    }
    static void SandTick(PowderApp& app, const PowderApp::BrushConfig* brush) {
        ++app.frame_index_;
        app.RunSpawnPass(brush);
        app.RunBoundaryPass();
        app.RunSandPass(app.config_.fixed_dt);
    }
    static std::vector<float> ReadOccupancy(PowderApp& app) {
        std::vector<float> occupancy(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, app.sand_state_.occupancy);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, occupancy.data());
        return occupancy;
    }
    // Mirror symmetry about the centre column 500 (the brush disc's axis) of the drawn sand.
    struct PileShape {
        double asymmetry = 0.0, centroid_offset = 0.0, mirror_rms = 0.0, mass = 0.0, peak = 0.0;
        std::vector<double> height;
    };
    static PileShape MeasurePile(PowderApp& app) {
        const auto occupancy = ReadOccupancy(app);
        PileShape shape;
        shape.height.assign(PowderApp::kGridWidth, 0.0);
        double left = 0.0, right = 0.0, moment = 0.0;
        for (int x = 0; x < PowderApp::kGridWidth; ++x) {
            double column = 0.0;
            for (int y = 0; y < PowderApp::kGridHeight; ++y) {
                const double value = occupancy[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
                column += value;
                if (value >= 0.5) shape.height[static_cast<std::size_t>(x)] = y + 1.0;
            }
            (x < 500 ? left : right) += x == 500 ? 0.0 : column;
            moment += (x + 0.5) * column;
            shape.mass += column;
            shape.peak = std::max(shape.peak, shape.height[static_cast<std::size_t>(x)]);
        }
        shape.asymmetry = std::abs(left - right) / std::max(1e-9, left + right);
        shape.centroid_offset = moment / std::max(1e-9, shape.mass) - 500.5;
        double squared = 0.0;
        int columns = 0;
        for (int k = 1; k < 500; ++k) {
            const double a = shape.height[static_cast<std::size_t>(500 - k)], b = shape.height[static_cast<std::size_t>(500 + k)];
            if (a == 0.0 && b == 0.0) continue;
            squared += (a - b) * (a - b);
            ++columns;
        }
        shape.mirror_rms = std::sqrt(squared / std::max(1, columns));
        return shape;
    }
    static void PrintShape(const PileShape& shape, const std::string& label) {
        std::cout << label << " L/R asymmetry=" << shape.asymmetry << " centroid offset=" << shape.centroid_offset
                  << " mirror RMS=" << shape.mirror_rms << " peak=" << shape.peak << " occupancy mass=" << shape.mass << '\n';
    }
    // Mean flank slope (degrees) of a drawn pile: least squares on each side between 20% and 80% of the peak height.
    static double PileAngle(const PileShape& shape) {
        int first = -1, last = -1, apex = 0;
        for (int x = 0; x < PowderApp::kGridWidth; ++x) {
            const double h = shape.height[static_cast<std::size_t>(x)];
            if (h <= 0.0) continue;
            if (first < 0) first = x;
            last = x;
            if (h >= shape.height[static_cast<std::size_t>(apex)]) apex = x;
        }
        const auto flank = [&](int from, int to) {
            double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int x = from; x <= to; ++x) {
                const double h = shape.height[static_cast<std::size_t>(x)];
                if (h < 0.2 * shape.peak || h > 0.8 * shape.peak) continue;
                ++n; sx += x; sy += h; sxx += static_cast<double>(x) * x; sxy += x * h;
            }
            Require(n >= 3, "a pile flank needs at least three measurable columns");
            return std::atan(std::abs((n * sxy - sx * sy) / (n * sxx - sx * sx))) * 180.0 / 3.14159265358979;
        };
        Require(first >= 0, "the pile must hold sand");
        return 0.5 * (flank(first, apex) + flank(apex, last));
    }
    // The moving grains are not exactly symmetric themselves (random spawn positions and friction traits), so packing
    // is held to the same pours without it, averaged over seeds (one pour is chaotic: single seeds strayed 1.2 cells): at
    // most 2% more L/R mass imbalance, 0.5 cells more centroid offset and 1.5 cells more mirror RMS.
    static void RequireUnbiased(const std::vector<PileShape>& packed, const std::vector<PileShape>& moving,
                                const std::string& label) {
        std::array<std::array<double, 3>, 2> mean{};
        for (std::size_t s = 0; s < packed.size(); ++s) {
            std::cout << label << " seed " << s << " heights moving|packed every 8 columns:";
            for (int x = 380; x <= 620; x += 8)
                std::cout << ' ' << moving[s].height[static_cast<std::size_t>(x)] << '|' << packed[s].height[static_cast<std::size_t>(x)];
            std::cout << std::endl;
            PrintShape(moving[s], label + " moving");
            PrintShape(packed[s], label + " packed");
            for (const bool p : {false, true}) {
                const PileShape& shape = p ? packed[s] : moving[s];
                mean[p][0] += shape.asymmetry / static_cast<double>(packed.size());
                mean[p][1] += std::abs(shape.centroid_offset) / static_cast<double>(packed.size());
                mean[p][2] += shape.mirror_rms / static_cast<double>(packed.size());
            }
        }
        std::cout << label << " mean moving/packed L/R asymmetry=" << mean[0][0] << '/' << mean[1][0] << " |centroid offset|="
                  << mean[0][1] << '/' << mean[1][1] << " mirror RMS=" << mean[0][2] << '/' << mean[1][2] << '\n';
        // Neither side may gain (sand-sleep-bias also gates the centre pile's height and slope).
        Require(mean[1][0] < mean[0][0] + 0.02 && mean[1][1] < mean[0][1] + 0.5 && mean[1][2] < mean[0][2] + 1.5,
                label + ": packing must not bias the pile sideways beyond the same pour without it");
    }
    // A centre pour from (500, 300), then settling.
    static void PourCentrePile(PowderApp& app, int pour_ticks, int settle_ticks, bool print_speeds) {
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_x_ = 500;
        app.brush_y_ = 300;
        app.brush_radius_ = 3;
        const auto pour = app.BuildBrushConfig();
        for (int i = 0; i < pour_ticks; ++i) SandTick(app, &pour);
        for (int i = 0; i < settle_ticks; ++i) {
            SandTick(app, nullptr);
            if (print_speeds && (i + 1) % 60 == 0) {
                std::vector<float> speeds;
                for (const auto& p : ReadParticles<PowderApp::kSandParticleComponents>(app.sand_state_.particles,
                                                                                      app.sand_state_.live_count)) {
                    if (p[4] >= 0.5F) speeds.push_back(std::hypot(p[2], p[3]));
                }
                std::sort(speeds.begin(), speeds.end());
                const auto at = [&](double q) {
                    return speeds.empty() ? 0.0F : speeds[static_cast<std::size_t>(q * static_cast<double>(speeds.size() - 1))];
                };
                std::cout << "settle tick " << i + 1 << " grains=" << speeds.size() << " speed p50/p90/p99/max cells/s="
                          << at(0.5) << '/' << at(0.9) << '/' << at(0.99) << '/' << at(1.0)
                          << " below 4 cells/s=" << std::count_if(speeds.begin(), speeds.end(), [](float s) { return s < 4.0F; })
                          << '\n';
            }
        }
    }
    static void EraseAt(PowderApp& app, int x, int y, int radius) {
        app.brush_mode_ = PowderApp::BrushMode::Erase;
        app.brush_x_ = x;
        app.brush_y_ = y;
        app.brush_radius_ = radius;
        const auto erase = app.BuildBrushConfig();
        ++app.frame_index_;
        app.RunSpawnPass(&erase);
    }

    // Grain count is exact: moving plus packed equals the poured total every tick, through packing and an erase that
    // pulls a shelf out from under a settled pile.
    static void SandSleepConservation(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 400, 300, 200, 20);
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_x_ = 500;
        app.brush_y_ = 600;
        app.brush_radius_ = 4;
        const auto pour = app.BuildBrushConfig();
        for (int i = 0; i < 300; ++i) SandTick(app, &pour);
        const SandCount poured = CountSand(app);
        std::array<GLuint, 5> counters{};
        std::uint64_t most_packed = 0;
        const auto run = [&](int ticks, std::uint64_t expected, const char* phase) {
            for (int i = 0; i < ticks; ++i) {
                SandTick(app, nullptr);
                const auto tick_counters = app.ReadDebugCounters();
                for (std::size_t c = 0; c < counters.size(); ++c) counters[c] += tick_counters[c];
                const SandCount count = CountSand(app);
                most_packed = std::max(most_packed, count.packed);
                if (count.Total() != expected) {
                    throw std::runtime_error(std::string(phase) + " tick " + std::to_string(i) + ": moving " +
                                             std::to_string(count.active) + " + packed " + std::to_string(count.packed) +
                                             " != " + std::to_string(expected));
                }
            }
        };
        run(400, poured.Total(), "settle");
        const SandCount settled = CountSand(app);
        EraseAt(app, 500, 310, 14);
        const SandCount after_erase = CountSand(app);
        run(400, after_erase.Total(), "collapse");
        const SandCount final_count = CountSand(app);
        std::cout << "sand-sleep-conservation poured=" << poured.Total() << " settled moving/packed=" << settled.active << '/'
                  << settled.packed << " erased=" << settled.Total() - after_erase.Total() << " after collapse moving/packed="
                  << final_count.active << '/' << final_count.packed << " counters=" << counters[0] << '/' << counters[1] << '/'
                  << counters[2] << '/' << counters[3] << '\n';
        Require(settled.packed > settled.Total() / 2, "a settled pile must be mostly packed");
        Require(after_erase.Total() < settled.Total(), "the erase must remove sand");
        Require(counters[0] == 0 && counters[1] == 0, "sleep-packing must report no invalid state or overflow");
        // The collapse moves sand: the shelf's sand falls through the hole to the floor.
        const auto occupancy = ReadOccupancy(app);
        double floor_sand = 0.0;
        for (int y = 0; y < 100; ++y)
            for (int x = 450; x < 550; ++x) floor_sand += occupancy[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
        std::cout << "sand-sleep-conservation sand on the floor below the hole=" << floor_sand << '\n';
        Require(floor_sand > 20.0, "sand above the erased shelf must fall to the floor");
        app.replay_mode_ = false;
    }

    // Packing must not bias the pile: a centre pour is as symmetric packed as moving, and so is the collapse into a
    // central hole erased under it. Also logs the settling speed spread that sets sand_sleep_speed.
    // Eight seeds (the spawn and wake hashes follow frame_index_): one pour's flank varies by +-3 degrees.
    static void SandSleepBias(PowderApp& app) {
        constexpr int kSeeds = 8;
        std::array<std::vector<PileShape>, 2> pile, collapse;
        std::array<std::array<double, 3>, 2> mean{};  // peak, flank degrees, collapse peak
        for (int seed = 0; seed < kSeeds; ++seed) {
            for (const bool sleeping : {false, true}) {
                app.config_ = {};
                if (!sleeping) app.config_.sand_sleep_ticks = 0;
                app.replay_mode_ = true;
                app.InitializeState();
                app.frame_index_ = static_cast<GLuint>(seed) * 100003U;
                PourCentrePile(app, 600, 360, !sleeping && seed == 0);
                pile[sleeping].push_back(MeasurePile(app));
                if (seed == 0) SandFrames(app).Save(sleeping ? "bias_packed" : "bias_moving", 400, 0, 200, 70);
                const SandCount count = CountSand(app);
                if (sleeping) {
                    std::cout << "sand-sleep-bias seed " << seed << " moving/packed=" << count.active << '/' << count.packed << '\n';
                    Require(count.packed > count.Total() / 2, "the settled centre pile must be mostly packed");
                }
                EraseAt(app, 500, 0, 15);
                for (int i = 0; i < 360; ++i) SandTick(app, nullptr);
                collapse[sleeping].push_back(MeasurePile(app));
                const double angle = PileAngle(pile[sleeping].back());
                std::cout << "sand-sleep-bias seed " << seed << (sleeping ? " packed" : " moving") << " peak="
                          << pile[sleeping].back().peak << " flank deg=" << angle << " collapse peak=" << collapse[sleeping].back().peak
                          << '\n';
                mean[sleeping][0] += pile[sleeping].back().peak / kSeeds;
                mean[sleeping][1] += angle / kSeeds;
                mean[sleeping][2] += collapse[sleeping].back().peak / kSeeds;
            }
        }
        std::cout << "sand-sleep-bias mean moving/packed peak=" << mean[0][0] << '/' << mean[1][0] << " flank deg=" << mean[0][1]
                  << '/' << mean[1][1] << " collapse peak=" << mean[0][2] << '/' << mean[1][2] << '\n';
        RequireUnbiased(pile[1], pile[0], "sand-sleep-bias centre pile");
        // Packed cells push on the grains above them like the frozen grains they hold (sand_grid_update.comp): before, the
        // first moving layer on a packed surface bore its load alone, compacted, and the packed pile came out 20% denser
        // and flatter (38 cells tall against 48). Grains pack after 1 s still (sand_sleep_ticks):
        // at 0.2 s a relaxing pile packed mid-motion and packed flanks stood 5 degrees steeper (37.4 against 32.5).
        Require(std::abs(mean[1][0] - mean[0][0]) <= 0.05 * mean[0][0] && std::abs(mean[1][1] - mean[0][1]) <= 2.0 &&
                    mean[1][1] >= 30.0 && mean[1][1] <= 35.0,
                "a packed pile must stand as tall and as steep (within 2 degrees, 30-35) as the same pour without packing");
        // Support lost under a packed pile climbs it three rows a tick (sand_pack.comp), so it collapses into the hole like
        // the moving one: it bridged 1.75 cells more when each row waited for the one below to fall away (6 at the old
        // readiness), and a front of 12 rows a tick collapsed it 1.4 cells lower and denser.
        Require(std::abs(mean[1][2] - mean[0][2]) <= 1.0, "a packed pile must collapse into a hole like a moving one");
        RequireUnbiased(collapse[1], collapse[0], "sand-sleep-bias central-hole collapse");
        app.replay_mode_ = false;
    }

    // A settled pile stays packed: no grain wakes over 300 quiet ticks.
    static void SandSleepNoFlicker(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        // 420 settle ticks: grains pack after 60 still ticks (sand_sleep_ticks), 48 more than when this scene was set up.
        PourCentrePile(app, 600, 420, false);
        const SandCount settled = CountSand(app);
        std::uint64_t most_active = 0;
        for (int i = 0; i < 300; ++i) {
            SandTick(app, nullptr);
            const SandCount count = CountSand(app);
            Require(count.Total() == settled.Total(), "a quiet pile must keep every grain");
            most_active = std::max(most_active, count.active);
        }
        std::cout << "sand-sleep-no-flicker grains=" << settled.Total() << " moving at start=" << settled.active
                  << " most moving over 300 ticks=" << most_active << '\n';
        Require(most_active <= settled.active, "a settled pile must not wake by itself");
        Require(settled.active * 100 <= settled.Total(), "a settled pile must leave at most 1% of its grains moving");
        app.replay_mode_ = false;
    }

    // Fast water (a jet hitting the pile) wakes and carries packed sand; water filling around the pile slowly does not.
    static void SandSleepWater(PowderApp& app) {
        const auto run = [&](bool jet) {
            app.config_ = {};
            app.replay_mode_ = true;
            app.InitializeState();
            app.brush_mode_ = PowderApp::BrushMode::Sand;
            app.brush_x_ = 500;
            app.brush_y_ = 300;
            app.brush_radius_ = 3;
            const auto pour = app.BuildBrushConfig();
            const auto tick = [&](const PowderApp::BrushConfig* brush) {
                app.RunFrame(app.config_.fixed_dt, brush, true);
            };
            for (int i = 0; i < 600; ++i) tick(&pour);
            for (int i = 0; i < 360; ++i) tick(nullptr);
            const SandCount settled = CountSand(app);
            if (jet) {
                // A spout above the pile's flank: water lands at well over sand_wake_water_speed.
                app.brush_mode_ = PowderApp::BrushMode::Water;
                app.brush_x_ = 470;
                app.brush_y_ = 200;
                app.brush_radius_ = 4;
            } else {
                // Water at rest, a thin layer laid at the far wall, spreads toward the pile.
                WriteParticles(app, app.water_state_.particles, RandomWater(0, 0, 60, 6, 7U));
            }
            const auto water = app.BuildBrushConfig();
            std::uint64_t most_woken = 0;
            for (int i = 0; i < 120; ++i) {
                tick(jet ? &water : nullptr);
                if (i % 10 == 9) most_woken = std::max(most_woken, settled.packed - std::min(settled.packed, CountSand(app).packed));
            }
            std::cout << (jet ? "jet" : "slow fill") << " settled packed=" << settled.packed << " most woken=" << most_woken << '\n';
            return std::pair{settled.packed, most_woken};
        };
        const auto [jet_packed, jet_woken] = run(true);
        const auto [fill_packed, fill_woken] = run(false);
        Require(jet_woken > jet_packed / 50, "a water jet must wake and erode the packed pile");
        Require(fill_woken * 100 <= fill_packed, "slowly arriving water must leave the packed pile asleep");
        app.replay_mode_ = false;
    }

    // A calm submerged bed packs and stays packed: a basin 300 cells wide holding a 60-cell sand bed (two grains per cell)
    // under a 150-cell water pool, both placed at rest, run with the full coupled frame.
    static void SandSleepSubmerged(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 340, 0, 10, 260);
        PaintRock(app, 650, 0, 10, 260);
        std::vector<Particle> sand;
        std::uint32_t state = 11U;
        const auto uniform = [&]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<float>(state >> 8) / 16777216.0F;
        };
        for (int y = 0; y < 60; ++y)
            for (int x = 350; x < 650; ++x)
                for (int i = 0; i < 2; ++i) sand.push_back({x + uniform(), y + uniform(), 0, 0, 1, 1});
        WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
        WriteParticles(app, app.water_state_.particles, RandomWater(350, 60, 650, 210, 5U));
        SandCount count;
        std::uint64_t settled_moving = 0, most_moving = 0;
        for (int i = 0; i < 900; ++i) {
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            Require(CleanCounters(app.ReadDebugCounters()), "a submerged bed must report no failures");
            if (i % 60 == 59) {
                count = CountSand(app);
                Require(count.Total() == sand.size(), "a submerged bed must keep every grain");
                std::cout << "sand-sleep-submerged tick " << i + 1 << " moving/packed=" << count.active << '/' << count.packed << '\n';
                if (i + 1 == 600) settled_moving = count.active;
                if (i + 1 > 600) most_moving = std::max(most_moving, count.active);
            }
        }
        Require(settled_moving * 100 <= sand.size(), "a calm submerged bed must leave at most 1% of its grains moving");
        Require(most_moving <= settled_moving + sand.size() / 1000, "a calm submerged bed must not wake by itself");
        app.replay_mode_ = false;
    }

    // ---- Calm-water packing (water_sleep.comp / water_pack.comp) ----
    struct WaterCount {
        std::uint64_t active = 0, packed = 0;
        std::uint64_t Total() const { return active + packed; }
    };
    static WaterCount CountWater(PowderApp& app) {
        return {app.ReadParticleMetrics(app.water_state_.particles, PowderApp::kMaxWaterParticles,
                                        PowderApp::kWaterParticleComponents).count,
                app.ReadWaterPackedSum()};
    }
    // Ticks, requiring clean counters and (expected > 0) an exact moving + packed total every tick.
    static WaterCount WaterTicks(PowderApp& app, int ticks, std::uint64_t expected, const char* phase,
                                 const PowderApp::BrushConfig* brush = nullptr) {
        WaterCount count;
        for (int i = 0; i < ticks; ++i) {
            app.RunFrame(app.config_.fixed_dt, brush, true);
            Require(CleanCounters(app.ReadDebugCounters()), std::string(phase) + ": water packing must report no failures");
            count = CountWater(app);
            if (expected > 0 && count.Total() != expected) {
                throw std::runtime_error(std::string(phase) + " tick " + std::to_string(i) + ": moving " +
                                         std::to_string(count.active) + " + packed " + std::to_string(count.packed) +
                                         " != " + std::to_string(expected));
            }
        }
        return count;
    }

    // Particle count is exact: moving plus packed equals the poured total every tick, through packing, an erased hole
    // and a sand stroke stirring the pool.
    static void WaterPackConservation(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        const auto pool = RandomWater(0, 0, 1000, 120, 4242U);
        WriteParticles(app, app.water_state_.particles, pool);
        const std::uint64_t total = pool.size();
        WaterCount settled;
        for (int s = 0; s < 6; ++s) {
            settled = WaterTicks(app, 60, total, "settle");
            std::cout << "water-pack-conservation settle tick " << (s + 1) * 60 << " moving/packed=" << settled.active << '/'
                      << settled.packed << " substeps=" << app.water_state_.substeps << '\n';
        }
        EraseAt(app, 500, 60, 20);
        const WaterCount after_erase = CountWater(app);
        const WaterCount opened = WaterTicks(app, 60, after_erase.Total(), "hole");
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_radius_ = 6;
        WaterCount stirred;
        for (int i = 0; i < 60; ++i) {
            app.brush_x_ = 200 + i * 10;
            app.brush_y_ = 110;
            const auto sand = app.BuildBrushConfig();
            stirred = WaterTicks(app, 1, after_erase.Total(), "stir", &sand);
        }
        const WaterCount calm = WaterTicks(app, 240, after_erase.Total(), "after stir");
        std::cout << "water-pack-conservation particles=" << total << " settled moving/packed=" << settled.active << '/'
                  << settled.packed << " erased=" << settled.Total() - after_erase.Total() << " hole moving/packed="
                  << opened.active << '/' << opened.packed << " stirred=" << stirred.active << '/' << stirred.packed
                  << " calm again=" << calm.active << '/' << calm.packed << '\n';
        Require(settled.packed > settled.Total() / 2, "a settled pool must be mostly packed");
        Require(after_erase.Total() < settled.Total(), "the erase must remove water");
        Require(opened.packed < settled.packed && stirred.packed < opened.packed, "the hole and the stroke must wake water");
        app.replay_mode_ = false;
    }

    // A calm pool stays packed: once settled, no packed cell wakes over 300 quiet ticks, and the drawn water is the same
    // packed as moving.
    static void WaterPackNoFlicker(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        const auto pool = RandomWater(0, 0, 1000, 200, 99U);
        WriteParticles(app, app.water_state_.particles, pool);
        const WaterCount settled = WaterTicks(app, 120, pool.size(), "settle");
        const double drawn = app.ReadScalarTextureSum(app.CurrentWaterAmount(), PowderApp::kGridWidth, PowderApp::kGridHeight);
        std::uint64_t fewest_packed = settled.packed;
        for (int i = 0; i < 300; ++i) fewest_packed = std::min(fewest_packed, WaterTicks(app, 1, pool.size(), "quiet").packed);
        const double drawn_late = app.ReadScalarTextureSum(app.CurrentWaterAmount(), PowderApp::kGridWidth, PowderApp::kGridHeight);
        std::cout << "water-pack-no-flicker particles=" << pool.size() << " settled moving/packed=" << settled.active << '/'
                  << settled.packed << " fewest packed over 300 ticks=" << fewest_packed << " drawn water=" << drawn << " -> "
                  << drawn_late << " (" << pool.size() / 4 << " cells)\n";
        Require(settled.packed * 10 >= settled.Total() * 8, "a calm pool must be at least 80% packed");
        Require(fewest_packed >= settled.packed, "a calm pool must not wake by itself");
        Require(std::abs(drawn_late - drawn) <= 0.002 * drawn, "packing must not change the drawn water");
        app.replay_mode_ = false;
    }

    // A pool that sloshes keeps sloshing: the left half starts 10 cells higher than the right and the water swings
    // between the walls, its bulk moving faster than packing admits (water_sleep_bulk) until the swing decays. The
    // left-minus-right level difference must follow the same envelope with packing on and off.
    static void WaterPackSlosh(PowderApp& app) {
        constexpr int windows = 4;
        std::array<std::array<double, windows>, 2> envelope{};
        for (const bool packing : {false, true}) {
            app.config_ = {};
            if (!packing) app.config_.water_sleep_ticks = 0;
            app.replay_mode_ = true;
            app.InitializeState();
            auto pool = RandomWater(0, 0, 500, 100, 31U);
            const auto right = RandomWater(500, 0, 1000, 90, 32U);
            pool.insert(pool.end(), right.begin(), right.end());
            WriteParticles(app, app.water_state_.particles, pool);
            const auto side_level = [&](int x0, int x1) {
                std::vector<float> volume(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, app.water_state_.liquid_volume);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, volume.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                double sum = 0.0;
                for (int y = 0; y < 300; ++y)
                    for (int x = x0; x < x1; ++x) sum += volume[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
                return sum / (x1 - x0) / (4.0 * app.config_.water_amount_per_particle);  // cells of water at rest density
            };
            for (int i = 0; i < 600; ++i) {
                WaterTicks(app, 1, 0, "slosh");
                if (i % 3 != 2) continue;
                const double difference = side_level(0, 100) - side_level(900, 1000);
                auto& window = envelope[packing][static_cast<std::size_t>(i / (600 / windows))];
                window = std::max(window, std::abs(difference));
            }
            const WaterCount count = CountWater(app);
            std::cout << "water-pack-slosh packing=" << packing << " moving/packed at 10 s=" << count.active << '/' << count.packed
                      << " envelope of the left-minus-right level by 2.5 s windows:";
            for (const double value : envelope[packing]) std::cout << ' ' << value;
            std::cout << '\n';
        }
        app.replay_mode_ = false;
        for (std::size_t w = 0; w < 2; ++w)
            Require(std::abs(envelope[1][w] - envelope[0][w]) < 0.15 * envelope[0][w],
                    "while the water swings fast, packing must not change the slosh");
        // Packed water holds a slow tilt that the unpacked pool levels out (the last window reads 0.2 without packing, 0.5 to
        // 1.1 with it); the swing must still have died to a tenth by 7.5 s.
        Require(envelope[1][windows - 1] < 1.5 && envelope[1][windows - 2] < 1.5,
                "a packed pool must level out as the swing decays");
    }

    // Waves still travel: a blob dropped near the left wall of a packed pool raises the water at the far wall about as
    // much as it does with packing off, because the surface band never packs and the impact wakes the bulk under it.
    static void WaterPackWave(PowderApp& app) {
        std::array<double, 2> rise{};
        for (const bool packing : {false, true}) {
            app.config_ = {};
            if (!packing) app.config_.water_sleep_ticks = 0;
            app.replay_mode_ = true;
            app.InitializeState();
            const auto pool = RandomWater(0, 0, 1000, 100, 7U);
            WriteParticles(app, app.water_state_.particles, pool);
            const WaterCount settled = WaterTicks(app, 90, pool.size(), "settle");
            const auto far_level = [&]() {
                std::vector<float> volume(static_cast<std::size_t>(PowderApp::kGridWidth * PowderApp::kGridHeight));
                glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                glBindTexture(GL_TEXTURE_2D, app.water_state_.liquid_volume);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, volume.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                double sum = 0.0;
                for (int y = 0; y < 300; ++y)
                    for (int x = 960; x < 1000; ++x) sum += volume[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
                return sum / 40.0;
            };
            const double rest_level = far_level();
            const auto blob = RandomWater(40, 200, 120, 260, 8U);
            auto particles = ReadParticles(app.water_state_.particles, app.water_state_.record_bound);
            particles.insert(particles.end(), blob.begin(), blob.end());
            WriteParticles(app, app.water_state_.particles, particles);
            double highest = rest_level;
            for (int i = 0; i < 240; ++i) {
                WaterTicks(app, 1, 0, "wave");
                highest = std::max(highest, far_level());
            }
            rise[packing] = highest - rest_level;
            std::cout << "water-pack-wave packing=" << packing << " settled moving/packed=" << settled.active << '/'
                      << settled.packed << " far-wall level " << rest_level << " rose by " << rise[packing] << '\n';
        }
        Require(rise[0] > 1.0, "the splash must raise the far wall without packing");
        Require(rise[1] > 0.7 * rise[0], "with packing the wave must still reach the far wall");
        app.replay_mode_ = false;
    }

    // Manual: pour water (radius-40 sweeps) until it would fill half the screen at rest density, let it settle, then
    // time it; GPU mean must fit 60 FPS (16.7 ms) with the calm bulk packed. POWDER_FRAME_DIR saves the settled frame.
    static void BenchmarkWaterFill(PowderApp& app) {
        app.config_ = powder_config::LoadConfigFromEnvironment();
        app.replay_mode_ = true;
        app.InitializeState();
        FrameTarget target;
        OpenFrameTarget(app, target);
        std::array<GLuint, 2> queries{};
        glGenQueries(2, queries.data());
        std::array<GLuint, 5> totals{};
        bool measuring = false;
        const auto timed_frame = [&](const PowderApp::BrushConfig* brush) {
            glQueryCounter(queries[0], GL_TIMESTAMP);
            app.RunFrame(app.config_.fixed_dt, brush, true);
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            app.Render();
            glQueryCounter(queries[1], GL_TIMESTAMP);
            std::array<GLuint64, 2> stamps{};
            glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &stamps[0]);
            glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &stamps[1]);
            const auto counters = app.ReadDebugCounters();
            for (std::size_t c = 0; c < totals.size(); ++c) totals[c] += counters[c];
            // The pour itself (2M particles crashing down) can exceed the pressure cap; the settled frames may not.
            Require(counters[0] == 0 && counters[1] == 0 && counters[3] == 0 && (!measuring || counters[2] == 0),
                    "benchmark-water-fill must report no failures: " + std::to_string(counters[0]) + "/" +
                                                 std::to_string(counters[1]) + "/" + std::to_string(counters[2]) + "/" +
                                                 std::to_string(counters[3]));
            glfwPollEvents();
            return static_cast<double>(stamps[1] - stamps[0]) / 1e6;
        };
        const std::uint64_t target_particles = static_cast<std::uint64_t>(
            0.5 * PowderApp::kGridWidth * PowderApp::kGridHeight * app.config_.water_rest_density / app.config_.water_particle_mass);
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_radius_ = 40;
        app.brush_y_ = 940;
        int ticks = 0;
        double fill_max = 0.0;
        // POWDER_WATER_FILL_POUR=1 pours instead of placing a still half-screen pool at rest density.
        const bool pour = std::getenv("POWDER_WATER_FILL_POUR") != nullptr;
        if (!pour) {
            WriteParticles(app, app.water_state_.particles,
                           RandomWater(0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight / 2, 2024U));
        }
        for (; pour && ticks < 3000 && CountWater(app).Total() < target_particles; ++ticks) {
            const int phase = ticks % 46;
            app.brush_x_ = 40 + (ticks / 46 % 2 == 0 ? phase : 45 - phase) * 20;
            const auto brush = app.BuildBrushConfig();
            fill_max = std::max(fill_max, timed_frame(&brush));
        }
        const std::uint64_t poured = CountWater(app).Total();
        std::cout << "benchmark-water-fill poured " << poured << " particles in " << ticks << " ticks; fill GPU max ms=" << fill_max
                  << '\n';
        const int settle_ticks = std::getenv("POWDER_WATER_FILL_SETTLE") ? std::atoi(std::getenv("POWDER_WATER_FILL_SETTLE")) : 1200;
        double settle_gpu = 0.0;
        for (int i = 0; i < settle_ticks; ++i) {
            settle_gpu += timed_frame(nullptr);
            if (i % 60 == 59) {
                const WaterCount count = CountWater(app);
                std::cout << "benchmark-water-fill settle tick " << i + 1 << " moving/packed=" << count.active << '/'
                          << count.packed << " GPU mean ms=" << settle_gpu / 60.0 << " substeps=" << app.water_state_.substeps;
                std::vector<float> speeds;
                for (const auto& p : ReadParticles(app.water_state_.particles, PowderApp::kMaxWaterParticles))
                    if (p[4] >= 0.5F && p[1] < 400.0F) speeds.push_back(std::hypot(p[2], p[3]));
                std::sort(speeds.begin(), speeds.end());
                const auto at = [&](double q) { return speeds.empty() ? 0.0F : speeds[static_cast<std::size_t>(q * (speeds.size() - 1))]; };
                std::cout << " bulk (y<400) speed p10/p50/p90=" << at(0.1) << '/' << at(0.5) << '/' << at(0.9) << '\n';
                settle_gpu = 0.0;
                Require(count.Total() == poured, "settling water must keep every particle");
            }
        }
        std::cout << "benchmark-water-fill counters while pouring and settling=" << totals[0] << '/' << totals[1] << '/'
                  << totals[2] << '/' << totals[3] << '\n';
        measuring = true;
        constexpr int samples = 240;
        std::vector<double> gpu;
        std::array<double, PowderApp::kReplayQueryCount> passes{};
        for (int i = 0; i < samples; ++i) {
            gpu.push_back(timed_frame(nullptr));
            for (std::size_t q = 0; q < passes.size(); ++q) {
                GLuint64 elapsed = 0;
                glGetQueryObjectui64v(app.timing_queries_[q], GL_QUERY_RESULT, &elapsed);
                passes[q] += static_cast<double>(elapsed) / 1e6 / samples;
            }
        }
        std::sort(gpu.begin(), gpu.end());
        const double mean = std::accumulate(gpu.begin(), gpu.end(), 0.0) / samples;
        const double p95 = gpu[static_cast<std::size_t>(std::ceil(samples * 0.95)) - 1];
        const WaterCount count = CountWater(app);
        std::cout << "benchmark-water-fill settled moving/packed=" << count.active << '/' << count.packed
                  << " water substeps=" << app.water_state_.substeps << " pressure iterations=" << app.water_state_.pressure_iterations
                  << " GPU mean/p95/max ms=" << mean << '/' << p95 << '/' << gpu.back() << " pass mean ms spawn/boundary/water/gas/sand/coupling/extract=";
        for (double pass : passes) std::cout << pass << '/';
        std::cout << '\n';
        if (const char* frame_dir = std::getenv("POWDER_FRAME_DIR")) {
            SaveFrame(app, target, std::filesystem::path(frame_dir) / "water-fill-settled.ppm", 0, 0, target.width, target.height);
        }
        glDeleteQueries(2, queries.data());
        app.replay_mode_ = false;
        Require(count.Total() == poured, "settled water must keep every particle");
        Require(mean <= 16.7, "settled half-screen water must fit 60 FPS");
    }

    // Manual: fill half the screen with sand (radius-40 sweeps), then add water, then smoke and fire; GPU time per phase
    // with the sand settled must fit 60 FPS (mean 16.7 ms, p95 20 ms).
    static void BenchmarkFill(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        FrameTarget target;
        OpenFrameTarget(app, target);
        std::array<GLuint, 2> queries{};
        glGenQueries(2, queries.data());
        const char* frame_dir = std::getenv("POWDER_FRAME_DIR");
        const auto timed_frame = [&](const PowderApp::BrushConfig* brush, std::vector<double>* gpu,
                                     std::array<double, PowderApp::kReplayQueryCount>* passes) {
            glQueryCounter(queries[0], GL_TIMESTAMP);
            app.RunFrame(app.config_.fixed_dt, brush, true);
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            app.Render();
            glQueryCounter(queries[1], GL_TIMESTAMP);
            std::array<GLuint64, 2> stamps{};
            glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &stamps[0]);
            glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &stamps[1]);
            if (gpu != nullptr) gpu->push_back(static_cast<double>(stamps[1] - stamps[0]) / 1e6);
            if (passes != nullptr) {
                for (std::size_t i = 0; i < passes->size(); ++i) {
                    GLuint64 elapsed = 0;
                    glGetQueryObjectui64v(app.timing_queries_[i], GL_QUERY_RESULT, &elapsed);
                    (*passes)[i] += static_cast<double>(elapsed) / 1e6;
                }
            }
            glfwPollEvents();
        };
        const auto occupancy_share = [&]() {
            const auto occupancy = ReadOccupancy(app);
            return std::accumulate(occupancy.begin(), occupancy.end(), 0.0) / static_cast<double>(occupancy.size());
        };
        // Sweep a radius-40 sand brush across the top until the drawn sand covers half the screen (or 3000 ticks).
        app.brush_mode_ = PowderApp::BrushMode::Sand;
        app.brush_radius_ = 40;
        app.brush_y_ = 940;
        int ticks = 0;
        std::vector<double> fill_gpu;
        for (; ticks < 3000; ++ticks) {
            const int phase = ticks % 46;
            app.brush_x_ = 40 + (ticks / 46 % 2 == 0 ? phase : 45 - phase) * 20;
            const auto brush = app.BuildBrushConfig();
            timed_frame(&brush, &fill_gpu, nullptr);
            if (ticks % 30 == 29 && occupancy_share() >= 0.5) break;
        }
        const auto report = [&](const char* phase, const PowderApp::BrushConfig* brush) {
            for (int i = 0; i < 240; ++i) timed_frame(brush, nullptr, nullptr);  // settle
            std::vector<double> gpu;
            std::array<double, PowderApp::kReplayQueryCount> passes{};
            constexpr int samples = 240;
            for (int i = 0; i < samples; ++i) timed_frame(brush, &gpu, &passes);
            const SandCount count = CountSand(app);
            const auto counters = app.ReadDebugCounters();
            std::sort(gpu.begin(), gpu.end());
            const double mean = std::accumulate(gpu.begin(), gpu.end(), 0.0) / samples;
            const double p95 = gpu[static_cast<std::size_t>(std::ceil(samples * 0.95)) - 1];
            std::cout << "benchmark-fill " << phase << " sand moving/packed=" << count.active << '/' << count.packed
                      << " water=" << app.ReadWaterLiveParticles() << " occupancy share=" << occupancy_share()
                      << " GPU mean/p95/max ms=" << mean << '/' << p95 << '/' << gpu.back()
                      << " pass mean ms spawn/boundary/water/gas/sand/coupling/extract=";
            for (double total : passes) std::cout << total / samples << '/';
            std::cout << " sand substeps=" << app.sand_state_.substeps << " last counters=" << counters[0] << '/' << counters[1]
                      << '/' << counters[2] << '/' << counters[3] << '\n';
            if (frame_dir != nullptr) {
                SaveFrame(app, target, std::filesystem::path(frame_dir) / (std::string("fill-") + phase + ".ppm"), 0, 0,
                          target.width, target.height);
            }
            return std::pair{mean, p95};
        };
        std::cout << "benchmark-fill filled after " << ticks << " ticks; fill GPU max ms="
                  << *std::max_element(fill_gpu.begin(), fill_gpu.end()) << '\n';
        const auto [sand_mean, sand_p95] = report("A-sand", nullptr);
        // B: 6.6% of the screen in water, poured onto the sand.
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_radius_ = 40;
        for (int i = 0; i < 120; ++i) {
            app.brush_x_ = 200 + (i % 4) * 200;
            app.brush_y_ = 900;
            const auto brush = app.BuildBrushConfig();
            timed_frame(&brush, nullptr, nullptr);
            if (app.ReadWaterLiveParticles() >= 264000U) break;
        }
        const auto [water_mean, water_p95] = report("B-water", nullptr);
        app.brush_mode_ = PowderApp::BrushMode::Fire;
        app.brush_x_ = 500;
        app.brush_y_ = 800;
        app.brush_radius_ = 18;
        const auto fire = app.BuildBrushConfig();
        report("C-fire", &fire);
        glDeleteQueries(2, queries.data());
        app.replay_mode_ = false;
        Require(sand_mean <= 16.7 && sand_p95 <= 20.0, "settled half-screen sand must fit 60 FPS");
        Require(water_mean <= 16.7 && water_p95 <= 20.0, "settled sand with water must fit 60 FPS");
    }

    // Real-scale fire through the interactive stroke path: a held flame's hot gas rises at least 100 cells (0.5 m)
    // within 0.5 s of release and the flame is out within 1 s; a fast circular stroke keeps every gas solve under
    // the residual gate.
    static void FireDynamics(PowderApp& app) {
        app.config_ = {};
        const auto run = [&](int ticks, const std::function<std::array<int, 2>(int)>& cursor) {
            std::array<GLuint, 5> totals{};
            for (int t = 1; t <= ticks; ++t) {
                const auto at = cursor(t);
                const bool paint = at[0] >= 0;
                app.painting_ = app.cursor_in_sim_ = paint;
                if (paint) {
                    app.brush_x_ = at[0];
                    app.brush_y_ = at[1];
                    app.cursor_trail_.push_back(at);
                }
                app.RunFrame(app.config_.fixed_dt, nullptr, true);
                const auto c = app.ReadDebugCounters();
                for (std::size_t i = 0; i < c.size(); ++i) totals[i] += c[i];
            }
            app.painting_ = app.cursor_in_sim_ = false;
            return totals;
        };
        const auto start = [&]() {
            app.InitializeState();
            app.replay_mode_ = false;
            app.brush_mode_ = PowderApp::BrushMode::Fire;
            app.brush_radius_ = 12;
            app.stroke_ = {};
            app.cursor_trail_.clear();
        };

        start();
        auto counters = run(30, [](int) { return std::array<int, 2>{500, 115}; });
        const double peak_reaction = FireStats(app)[0];
        counters = run(30, [](int) { return std::array<int, 2>{-1, -1}; });
        const auto risen = FireStats(app);
        counters = run(30, [](int) { return std::array<int, 2>{-1, -1}; });
        const auto out = FireStats(app);
        std::cout << "fire held 0.5 s: reaction " << peak_reaction << ", +0.5 s heat centroid y=" << risen[2] * 2.0
                  << ", +1 s reaction " << out[0] << '\n';
        Require(peak_reaction > 50.0, "held fire brush must burn");
        Require(risen[2] * 2.0 >= 215.0, "hot gas must rise at least 100 cells within 0.5 s of release");
        Require(out[0] < 0.05 * peak_reaction, "fire must burn out within 1 s after the source stops");
        Require(CleanCounters(counters), "released fire must keep clean counters");

        start();
        // One revolution of radius 120 cells in 0.5 s: about 1500 cells/s, above the brush velocity cap.
        counters = run(45, [](int t) {
            if (t > 30) return std::array<int, 2>{-1, -1};
            const double a = 2.0 * 3.14159265358979 * t / 30.0;
            return std::array<int, 2>{500 + static_cast<int>(std::lround(120.0 * std::cos(a))),
                                      350 + static_cast<int>(std::lround(120.0 * std::sin(a)))};
        });
        std::cout << "fast fire stroke counters=" << counters[0] << ' ' << counters[2] << '\n';
        Require(FireStats(app)[0] > 50.0, "fast fire stroke must burn");
        Require(CleanCounters(counters), "every gas solve of a fast fire stroke must meet the residual gate");
    }

    // Wood material census over [x0, x1) x [y0, y1): intact, burning, charred and burnt away (empty) cells.
    static std::array<int, 4> WoodCensus(PowderApp& app, int x0, int y0, int x1, int y1) {
        std::vector<GLushort> material(static_cast<std::size_t>(PowderApp::kGridWidth) * PowderApp::kGridHeight);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, material.data());
        std::array<int, 4> census{};
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const GLushort m = material[static_cast<std::size_t>(y * PowderApp::kGridWidth + x)];
                ++census[m == 3 ? 0 : (m >= 256 && m < 512) ? 1 : m >= 512 ? 2 : 3];
            }
        return census;
    }

    // A wood beam lit at its left end: the fire catches, creeps along the beam, and the burnt wood chars and crumbles
    // away. A second beam under a water pour goes out.
    static void WoodFire(PowderApp& app) {
        app.config_ = {};
        app.InitializeState();
        app.replay_mode_ = false;
        app.stroke_ = {};
        app.cursor_trail_.clear();
        PaintRock(app, 300, 200, 400, 10, 3);
        app.brush_mode_ = PowderApp::BrushMode::Fire;
        app.brush_radius_ = 10;
        std::array<GLuint, 5> totals{};
        const auto tick = [&](bool paint) {
            app.painting_ = app.cursor_in_sim_ = paint;
            app.brush_x_ = 305;
            app.brush_y_ = 222;
            if (paint) app.cursor_trail_.push_back({305, 222});
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            const auto c = app.ReadDebugCounters();
            for (std::size_t i = 0; i < c.size(); ++i) totals[i] += c[i];
        };
        for (int t = 0; t < 30; ++t) tick(true);
        int lit_at = -1, far_lit_at = -1;
        for (int t = 30; t < 60 * 20 && far_lit_at < 0; ++t) {
            tick(false);
            if (t % 15 != 0) continue;
            const auto near = WoodCensus(app, 300, 200, 340, 210), far = WoodCensus(app, 560, 200, 600, 210);
            if (lit_at < 0 && near[0] < 400) lit_at = t;
            if (far[0] < 400) far_lit_at = t;
        }
        for (int t = 0; t < 60 * 6; ++t) tick(false);
        const auto beam = WoodCensus(app, 300, 200, 700, 210);
        std::cout << "wood: near end caught at t=" << lit_at / 60.0 << " s, 260 cells along at t=" << far_lit_at / 60.0
                  << " s; beam intact/burning/charred/gone=" << beam[0] << '/' << beam[1] << '/' << beam[2] << '/' << beam[3]
                  << '\n';
        Require(lit_at >= 0 && lit_at < 60 * 2, "a fire on wood must light it within 2 s");
        Require(far_lit_at > lit_at && far_lit_at < 60 * 20, "fire must spread 260 cells along the beam within 20 s");
        Require(beam[3] > 400, "burnt wood must crumble away");
        Require(CleanCounters(totals), "burning wood must keep clean counters");

        // Once nothing is left to burn the wood pass stops and the distance field stops rebuilding.
        int cleared_at = -1;
        for (int t = 0; t < 60 * 30 && cleared_at < 0; ++t) {
            tick(false);
            if (!app.wood_present_) cleared_at = t;
        }
        const auto rest = WoodCensus(app, 300, 200, 700, 210);
        std::cout << "wood: pass stopped " << cleared_at / 60.0 << " s later; beam intact/burning/charred=" << rest[0] << '/'
                  << rest[1] << '/' << rest[2] << '\n';
        Require(cleared_at >= 0, "the wood pass must stop once the beam has burnt away");
        Require(rest[0] + rest[1] + rest[2] == 0, "the wood pass must not stop while wood remains");
        for (int t = 0; t < 30; ++t) tick(false);
        Require(!app.boundary_state_.static_dirty, "a burnt-out beam must stop rebuilding the distance field");
    }

    // Sparks and embers are particles (spark_step.comp): live records have life > 0.
    using SparkRecord = std::array<float, 8>;
    static std::vector<SparkRecord> LiveSparks(PowderApp& app) {
        auto records = ReadParticles<8>(app.spark_state_.particles, PowderApp::kSparkCapacity);
        records.erase(std::remove_if(records.begin(), records.end(), [](const SparkRecord& s) { return s[5] <= 0.0F; }), records.end());
        return records;
    }

    // Sparks: spawned from burning gas and wood into a fixed ring, advected by the gas, bounced by rock and put out by
    // water, drawn only in lit frames, and a pure function of the simulation (a rerun is bit-identical).
    static void Sparks(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        const float dt = app.config_.fixed_dt;
        FrameTarget target;
        OpenFrameTarget(app, target);
        struct Restore {
            PowderApp& app;
            ~Restore() {
                app.lighting_enabled_ = true;
                app.replay_mode_ = false;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        } restore{app};
        const auto frame = [&](bool lit) {
            app.lighting_enabled_ = lit;
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            app.Render();
            std::vector<unsigned char> pixels(static_cast<std::size_t>(target.width) * target.height * 4);
            glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
        };
        std::array<GLuint, 5> totals{};
        // A fire on a rock floor, held for `held` ticks and run for `total`.
        const auto fire = [&](int held, int total, int radius = 12, int x = 500, int y = 60) {
            app.InitializeState();
            PaintRock(app, 300, 0, 400, 20);
            for (int t = 0; t < total; ++t) {
                if (t < held) PhaseStamp(app, PowderApp::BrushMode::Fire, x, y, radius);
                PhaseTick(app, totals, 1);
            }
        };

        // Nothing burns, nothing flies: with the spark passes forced on and no fire, no record comes alive.
        app.InitializeState();
        app.heat_present_ = true;
        PhaseTick(app, totals, 90);
        Require(LiveSparks(app).empty(), "sparks must not exist before anything burns");

        // A fire held for 1 s throws sparks and embers, all inside the cave and clear of rock and sand.
        fire(60, 90);
        const auto live = LiveSparks(app);
        std::vector<GLubyte> mask(static_cast<std::size_t>(PowderApp::kGridWidth) * PowderApp::kGridHeight);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_full);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        std::size_t embers = 0, rising = 0, buried = 0;
        double highest = 0.0;
        for (const SparkRecord& s : live) {
            Require(std::isfinite(s[0]) && std::isfinite(s[1]) && std::isfinite(s[2]) && std::isfinite(s[3]), "spark state must be finite");
            Require(s[0] >= 0.0F && s[0] < PowderApp::kGridWidth && s[1] >= 0.0F && s[1] < PowderApp::kGridHeight, "sparks must stay in the domain");
            Require(s[4] >= 0.0F && s[4] < s[5] && s[5] <= 2.13F, "a spark's age must be within its life, and a life within the ring period");
            embers += static_cast<int>(s[7]) & 1;
            rising += s[3] > 0.0F;
            highest = std::max<double>(highest, s[1]);
            buried += mask[static_cast<std::size_t>(static_cast<int>(s[1])) * PowderApp::kGridWidth + static_cast<std::size_t>(static_cast<int>(s[0]))] != 0;
        }
        std::cout << "sparks: " << live.size() << " live 0.5 s after a 1 s fire (" << embers << " embers, " << rising
                  << " rising), highest y " << highest << ", in rock/sand " << buried << '\n';
        Require(live.size() >= 20 && embers >= 1 && rising >= 5, "a held fire must throw sparks and embers");
        Require(highest > 140.0, "sparks must rise with the plume");
        Require(buried == 0, "no spark may sit in rock or sand");

        // Lit frames show them, unlit frames (the pixel oracles) do not, and a frame repeats exactly.
        const auto lit = frame(true), unlit = frame(false);
        Require(frame(true) == lit && frame(false) == unlit, "a frame must repeat exactly");
        const auto saved = ReadParticles<8>(app.spark_state_.particles, PowderApp::kSparkCapacity);
        const GLfloat zero = 0.0F;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, &zero);
        const auto lit_without = frame(true), unlit_without = frame(false);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);  // Render rebound the target
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(saved.size() * sizeof(saved[0])), saved.data());
        std::size_t changed = 0;
        for (std::size_t i = 0; i < lit.size(); ++i) changed += lit[i] != lit_without[i];
        std::cout << "sparks: " << changed << " frame bytes differ with the sparks removed from a lit frame\n";
        Require(changed > 300, "sparks must be visible in lit frames");
        Require(unlit == unlit_without, "unlit frames must not draw sparks");

        // Rerun: the same ticks give the same records, byte for byte, and the same frame.
        fire(60, 90);
        const auto again = ReadParticles<8>(app.spark_state_.particles, PowderApp::kSparkCapacity);
        Require(std::memcmp(again.data(), saved.data(), saved.size() * sizeof(saved[0])) == 0, "a rerun must give identical sparks");
        Require(frame(true) == lit, "a rerun must give an identical lit frame");

        // The fire goes out after 1 s; the last spark dies within the ring period (a longest life) plus the plume's cooling.
        int out_at = -1;
        for (int t = 1; t <= 240 && out_at < 0; ++t) {
            PhaseTick(app, totals, 1);
            if (LiveSparks(app).empty()) out_at = t;
        }
        std::cout << "sparks: none left " << out_at / 60.0 << " s after the last tick above\n";
        Require(out_at > 0 && out_at <= 200, "sparks must die out after the fire does");
        PhaseTick(app, totals, 60);
        Require(LiveSparks(app).empty() && CleanCounters(totals), "no spark may appear from a dead fire, and the counters stay clean");

        // Burning wood throws embers; the unlit beam does not.
        app.InitializeState();
        PaintRock(app, 300, 200, 400, 10, 3);
        PhaseTick(app, totals, 90);
        Require(LiveSparks(app).empty(), "unlit wood must not throw embers");
        for (int t = 0; t < 30; ++t) {
            PhaseStamp(app, PowderApp::BrushMode::Fire, 305, 222, 10);
            PhaseTick(app, totals, 1);
        }
        std::size_t most = 0;
        for (int t = 0; t < 600; ++t) {
            PhaseTick(app, totals, 1);
            if (t % 20 == 0) most = std::max(most, LiveSparks(app).size());
        }
        std::cout << "sparks: most live over a burning beam " << most << '\n';
        Require(most >= 10, "a burning beam must throw sparks");

        // Dynamics on their own: a spark dropped on the rock floor bounces and settles, one dropped into water dies.
        app.InitializeState();
        PaintRock(app, 300, 0, 400, 20);
        app.RunBoundaryPass();
        app.heat_present_ = true;
        const std::vector<float> pond(40 * 20, 0.5F);
        glBindTexture(GL_TEXTURE_2D, app.CurrentWaterAmount());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 580, 20, 40, 20, GL_RED, GL_FLOAT, pond.data());
        const auto inject = [&](std::size_t slot, float x, float y, float vx, float vy) {
            const SparkRecord record{x, y, vx, vy, 0.0F, 5.0F, 0.2F, 1.0F};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(slot * sizeof(record)), sizeof(record), record.data());
        };
        inject(8000, 500.0F, 60.0F, 0.0F, -300.0F);  // slots 8000 and up are outside the first ticks' spawn range
        inject(8001, 600.0F, 60.0F, 0.0F, -200.0F);
        double lowest = 1e9, bounced = 0.0;
        for (int t = 1; t <= 40; ++t) {
            app.frame_index_ = static_cast<std::uint32_t>(t);
            app.RunSparkPass(dt);
            const auto records = ReadParticles<8>(app.spark_state_.particles, PowderApp::kSparkCapacity);
            lowest = std::min<double>(lowest, records[8000][1]);
            bounced = std::max<double>(bounced, records[8000][3]);
        }
        const auto records = ReadParticles<8>(app.spark_state_.particles, PowderApp::kSparkCapacity);
        std::cout << "sparks: floor drop lowest y " << lowest << ", best rebound " << bounced << " cells/s, rests at y "
                  << records[8000][1] << "; water drop life " << records[8001][5] << '\n';
        Require(lowest >= 19.99 && records[8000][5] > 0.0F && records[8000][1] < 30.0F, "a spark must bounce on rock and settle on it");
        Require(bounced > 20.0, "a spark hitting rock must rebound");
        Require(records[8001][5] == 0.0F, "a spark falling into water must go out");

        // A letterboxed frame draws a spark on the cell it is in: the brightest change is within two pixels of it.
        {
            glfwSetWindowSize(app.window_, 1280, 900);
            glfwPollEvents();
            int width = 0, height = 0;
            glfwGetFramebufferSize(app.window_, &width, &height);
            Require(width == 1280 && height == 900, "the letterbox check needs a 1280x900 framebuffer");
            GLuint color = 0, framebuffer = 0;
            glGenTextures(1, &color);
            glBindTexture(GL_TEXTURE_2D, color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
            const auto render = [&] {
                app.lighting_enabled_ = true;
                app.Render(true);
                std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                return pixels;
            };
            const auto empty = render();
            const SparkRecord record{300.5F, 700.5F, 0.0F, 0.0F, 1.0F, 5.0F, 0.2F, 0.0F};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 9000 * sizeof(record), sizeof(record), record.data());
            const auto shown = render();
            const PowderApp::SimRect sim = PowderApp::FitSimRect(width, height);
            int best = 0, best_x = -1, best_y = -1;
            for (int i = 0; i < width * height; ++i) {
                const int gain = shown[static_cast<std::size_t>(i) * 4] - empty[static_cast<std::size_t>(i) * 4];
                if (gain > best) best = gain, best_x = i % width, best_y = i / width;
            }
            const double want_x = sim.x + 300.5 * sim.width / PowderApp::kGridWidth, want_y = sim.y + 700.5 * sim.height / PowderApp::kGridHeight;
            std::cout << "sparks: letterboxed spark at pixel " << best_x << ',' << best_y << " (gain " << best << "), cell centre at "
                      << want_x << ',' << want_y << '\n';
            Require(best > 40 && std::abs(best_x + 0.5 - want_x) <= 2.0 && std::abs(best_y + 0.5 - want_y) <= 2.0,
                    "a spark must be drawn on its cell in a letterboxed frame");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &color);
            glfwSetWindowSize(app.window_, PowderApp::kGridWidth, PowderApp::kGridHeight);
            glfwPollEvents();
            const SparkRecord gone{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 9000 * sizeof(record), sizeof(gone), gone.data());
        }

        // Cost of the two passes in a large fire (three radius-90 brushes held), once the ring is full.
        fire(0, 0);
        GLuint query = 0;
        glGenQueries(1, &query);
        std::vector<double> times;
        for (int t = 0; t < 200; ++t) {
            for (const int x : {350, 500, 650}) PhaseStamp(app, PowderApp::BrushMode::Fire, x, 120, 90);
            PhaseTick(app, totals, 1);
            ++app.frame_index_;  // one more spark tick on the same gas, timed alone
            glBeginQuery(GL_TIME_ELAPSED, query);
            app.RunSparkPass(dt);
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 ns = 0;
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
            times.push_back(static_cast<double>(ns) / 1e6);
        }
        std::sort(times.begin(), times.end());
        std::cout << "sparks: full fire " << LiveSparks(app).size() << " live, spark passes GPU ms per tick median/max "
                  << times[times.size() / 2] << '/' << times.back() << '\n';
        Require(times[times.size() / 2] < 0.5, "the spark passes must stay well under a millisecond a tick");
        // And the lit frame with the live sparks against the same frame with none.
        const auto render_ms = [&] {
            std::vector<double> samples;
            for (int i = 0; i < 25; ++i) {
                glBeginQuery(GL_TIME_ELAPSED, query);
                app.Render();
                glEndQuery(GL_TIME_ELAPSED);
                GLuint64 ns = 0;
                glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
                samples.push_back(static_cast<double>(ns) / 1e6);
            }
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };
        app.lighting_enabled_ = true;
        const double with_sparks = render_ms();
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.spark_state_.particles);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, &zero);
        const double without_sparks = render_ms();
        std::cout << "sparks: lit frame GPU ms median with/without the live sparks " << with_sparks << '/' << without_sparks << '\n';
        glDeleteQueries(1, &query);
    }

    // Manual: a lit cave scene with every material, driven by brush stamps. With POWDER_FRAME_DIR set it saves a lit
    // 1000x1000 frame every half second (motion review) and a final 1280x900 interactive frame with lantern and HUD
    // (docs/screenshot.png).
    static void Showcase(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        const char* frame_dir = std::getenv("POWDER_FRAME_DIR");
        const std::filesystem::path frames = frame_dir != nullptr ? std::filesystem::path(frame_dir) : std::filesystem::path();
        FrameTarget target;
        if (!frames.empty()) {
            std::filesystem::create_directories(frames);
            OpenFrameTarget(app, target);
        }
        // Cave: floor, side walls, a ceiling with hanging rock, a basin on the left and a ledge with a wood beam.
        PaintRock(app, 0, 0, 1000, 40);
        PaintRock(app, 0, 0, 40, 1000);
        PaintRock(app, 960, 0, 40, 1000);
        PaintRock(app, 0, 930, 1000, 70);
        for (int i = 0; i < 9; ++i) {  // tapered stalactites
            const int x = 80 + i * 105 + (i * 29) % 40, w = 24 + (i * 13) % 20, len = 40 + (i * 37) % 70;
            for (int tier = 0; tier < 4; ++tier)
                PaintRock(app, x + tier * w / 8, 930 - len * (tier + 1) / 4, w - tier * w / 4, len * (tier + 1) / 4);
        }
        PaintRock(app, 40, 40, 60, 60);
        PaintRock(app, 480, 40, 40, 190);
        PaintRock(app, 600, 360, 80, 30);
        PaintRock(app, 610, 40, 50, 320);
        PaintRock(app, 560, 390, 300, 14, 3);  // the wood beam on its pillar
        std::array<GLuint, 5> totals{};
        int t = 0;
        const auto stamp = [&](PowderApp::BrushMode mode, int x, int y, int radius) { PhaseStamp(app, mode, x, y, radius); };
        const auto tick = [&]() {
            PhaseTick(app, totals, 1);
            ++t;
            if (!frames.empty() && t % 30 == 0) {
                char name[32];
                std::snprintf(name, sizeof(name), "showcase_%04d.ppm", t);
                SaveFrame(app, target, frames / name, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight);
            }
        };
        for (int i = 0; i < 150; ++i) {  // pour the basin full, and sand onto the cave floor on the right
            if (i < 110) stamp(PowderApp::BrushMode::Water, 180 + (i * 37) % 260, 520, 14);
            stamp(PowderApp::BrushMode::Sand, 905, 520, 12);
            tick();
        }
        for (int i = 0; i < 40; ++i) {  // an oil slick poured onto the pool
            stamp(PowderApp::BrushMode::Oil, 380, 330, 9);
            tick();
        }
        for (int i = 0; i < 240; ++i) tick();
        for (int i = 0; i < 40; ++i) {  // light the beam's far end
            stamp(PowderApp::BrushMode::Fire, 700, 415, 10);
            tick();
        }
        for (int i = 0; i < 150; ++i) tick();
        for (int i = 0; i < 25; ++i) {  // a splash of water on the flames throws up steam
            stamp(PowderApp::BrushMode::Water, 600, 560, 5);
            tick();
        }
        for (int i = 0; i < 20; ++i) tick();
        std::cout << "showcase: " << t << " ticks, water=" << app.ReadWaterLiveParticles() << " sand=" << app.ReadSandLiveParticles()
                  << " counters=" << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3] << '\n';
        Require(totals[0] == 0 && totals[3] == 0, "the showcase scene must keep clean counters");
        if (frames.empty()) {
            app.replay_mode_ = false;
            return;
        }
        // The interactive frame: letterboxed, lantern over the pool, help panel up, water selected.
        glfwSetWindowSize(app.window_, 1280, 900);
        glfwPollEvents();
        int width = 0, height = 0;
        glfwGetFramebufferSize(app.window_, &width, &height);
        Require(width == 1280 && height == 900, "the showcase needs a 1280x900 framebuffer");
        GLuint color = 0, framebuffer = 0;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        app.brush_mode_ = PowderApp::BrushMode::Water;
        app.brush_radius_ = 12;
        app.brush_x_ = 330;
        app.brush_y_ = 420;
        app.cursor_in_sim_ = true;
        app.help_visible_ = true;
        app.help_hide_time_ = -1.0;
        app.hud_motion_ = {};
        app.Render(true);
        app.DrawHud(1000.0);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream out(frames / "showcase_interactive.ppm", std::ios::binary);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        for (int row = height - 1; row >= 0; --row)
            for (int col = 0; col < width; ++col)
                out.write(reinterpret_cast<const char*>(&pixels[static_cast<std::size_t>(row * width + col) * 4]), 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &color);
        app.cursor_in_sim_ = false;
        app.hud_motion_ = {};
        app.replay_mode_ = false;
        Require(out.good(), "could not write the showcase frame");
    }

    // Manual: the kind of scene people paint by hand, for look review. Thin brushed rock strokes hanging in open space
    // (an arc, a hook and a bowl), water poured over them, an oil slick on a floor pool and in the bowl, both lit. With
    // POWDER_FRAME_DIR set it saves lit 1000x1000 frames (look_<tick>.ppm) and a letterboxed look_interactive.ppm.
    static void LookScene(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        const char* frame_dir = std::getenv("POWDER_FRAME_DIR");
        const std::filesystem::path frames = frame_dir != nullptr ? std::filesystem::path(frame_dir) : std::filesystem::path();
        FrameTarget target;
        if (!frames.empty()) {
            std::filesystem::create_directories(frames);
            OpenFrameTarget(app, target);
        }
        std::array<GLuint, 5> totals{};
        int t = 0;
        const auto stamp = [&](PowderApp::BrushMode mode, int x, int y, int radius) { PhaseStamp(app, mode, x, y, radius); };
        const auto tick = [&](int count) {
            for (int i = 0; i < count; ++i) {
                PhaseTick(app, totals, 1);
                ++t;
                if (!frames.empty() && t % 60 == 0)
                    SaveFrame(app, target, frames / ("look_" + std::to_string(t) + ".ppm"), 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight);
            }
        };
        const auto stroke = [&](const auto& point, int steps, int radius) {
            for (int i = 0; i <= steps; ++i) {
                const auto [x, y] = point(static_cast<double>(i) / steps);
                stamp(PowderApp::BrushMode::Solid, static_cast<int>(x), static_cast<int>(y), radius);
            }
        };
        constexpr double kPi = 3.14159265358979;
        stroke([](double s) { return std::pair{150.0 + 700.0 * s, 650.0 + 90.0 * std::sin(s * kPi * 1.5)}; }, 300, 5);  // wavy arc
        stroke([&](double s) { return std::pair{300.0 + 90.0 * std::cos(s * 1.6 * kPi), 450.0 + 90.0 * std::sin(s * 1.6 * kPi)}; }, 200, 4);  // hook
        stroke([&](double s) { return std::pair{600.0 + 110.0 * std::cos(kPi + s * kPi), 380.0 + 70.0 * std::sin(kPi + s * kPi)}; }, 200, 6);  // bowl
        for (int i = 0; i < 160; ++i) {
            if (i < 120) stamp(PowderApp::BrushMode::Sand, 200 + (i * 53) % 600, 200, 12);
            stamp(PowderApp::BrushMode::Water, 150 + (i * 41) % 700, 900, 10);
            if (i < 90) stamp(PowderApp::BrushMode::Water, 600, 420, 12);
            stamp(PowderApp::BrushMode::Water, 930, 850, 2);  // a held tiny brush
            stamp(PowderApp::BrushMode::Water, i % 2 == 0 ? 880 : 895, 850, 3);  // a brush revisiting two columns
            tick(1);
        }
        tick(120);
        for (int i = 0; i < 30; ++i) {
            stamp(PowderApp::BrushMode::Oil, 600, 420, 8);
            stamp(PowderApp::BrushMode::Oil, 250 + (i * 61) % 500, 300, 8);
            tick(1);
        }
        tick(120);
        for (int i = 0; i < 20; ++i) {
            stamp(PowderApp::BrushMode::Fire, 600, 400, 10);
            tick(1);
        }
        tick(160);
        std::cout << "look scene: " << t << " ticks, water=" << app.ReadWaterLiveParticles() << " sand=" << app.ReadSandLiveParticles()
                  << " counters=" << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3] << '\n';
        {
            // Face precision: divergence of the projected gas faces in open cells as stored (combustion expansion is a real
            // source), and the error rounding every face to half precision (the R16F storage they had before) would add.
            constexpr int gw = PowderApp::kGasWidth, gh = PowderApp::kGasHeight;
            std::vector<float> u(static_cast<std::size_t>((gw + 1) * gh)), v(static_cast<std::size_t>(gw * (gh + 1)));
            std::vector<GLubyte> mask(static_cast<std::size_t>(gw * gh));
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glBindTexture(GL_TEXTURE_2D, app.CurrentGasU());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, u.data());
            glBindTexture(GL_TEXTURE_2D, app.CurrentGasV());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, v.data());
            glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            const auto half = [](float x) {
                if (x == 0.0F) return x;
                int exponent = 0;
                std::frexp(x, &exponent);
                const float quantum = std::ldexp(1.0F, std::max(exponent, -13) - 11);
                return std::nearbyint(x / quantum) * quantum;
            };
            double max_stored = 0.0, max_half = 0.0, sum_stored = 0.0, sum_half = 0.0, max_speed = 0.0;
            int cells = 0;
            for (int y = 1; y < gh - 1; ++y)
                for (int x = 1; x < gw - 1; ++x) {
                    bool open = true;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) open = open && mask[static_cast<std::size_t>((y + dy) * gw + x + dx)] == 0;
                    if (!open) continue;
                    const float ul = u[static_cast<std::size_t>(y * (gw + 1) + x)], ur = u[static_cast<std::size_t>(y * (gw + 1) + x + 1)];
                    const float vb = v[static_cast<std::size_t>(y * gw + x)], vt = v[static_cast<std::size_t>((y + 1) * gw + x)];
                    const double stored = std::abs(static_cast<double>(ur) - ul + vt - vb);
                    // The divergence error half storage of these four faces would add.
                    const double halved = std::abs(static_cast<double>(half(ur)) - half(ul) + half(vt) - half(vb) - (static_cast<double>(ur) - ul + vt - vb));
                    max_stored = std::max(max_stored, stored);
                    max_half = std::max(max_half, halved);
                    sum_stored += stored * stored;
                    sum_half += halved * halved;
                    max_speed = std::max({max_speed, static_cast<double>(std::abs(ur)), static_cast<double>(std::abs(vt))});
                    ++cells;
                }
            std::cout << "gas faces: open cells " << cells << ", max |face| " << max_speed << " cells/s, divergence max/RMS stored "
                      << max_stored << '/' << std::sqrt(sum_stored / std::max(cells, 1)) << ", error of half storage " << max_half << '/'
                      << std::sqrt(sum_half / std::max(cells, 1)) << " (gate " << app.config_.gas_pressure_residual_threshold << ")\n";
        }
        if (!frames.empty()) {
            glfwSetWindowSize(app.window_, 1280, 900);
            glfwPollEvents();
            int width = 0, height = 0;
            glfwGetFramebufferSize(app.window_, &width, &height);
            GLuint color = 0, framebuffer = 0;
            glGenTextures(1, &color);
            glBindTexture(GL_TEXTURE_2D, color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
            app.Render(true);
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream out(frames / "look_interactive.ppm", std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            for (int row = height - 1; row >= 0; --row)
                for (int col = 0; col < width; ++col)
                    out.write(reinterpret_cast<const char*>(&pixels[static_cast<std::size_t>(row * width + col) * 4]), 3);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &color);
        }
        app.replay_mode_ = false;
        Require(CleanCounters(totals), "the look scene must keep clean counters");
    }

    // Live liquid records split by the oil tag (state2.z): water and oil counts and their mean rows.
    struct LiquidCensus {
        GLuint water = 0, oil = 0;
        double water_y = 0.0, oil_y = 0.0;
    };
    static LiquidCensus Liquids(PowderApp& app) {
        LiquidCensus census;
        for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound)) {
            if (p[4] < 0.5F) continue;
            if (p[10] > 0.5F) {
                ++census.oil;
                census.oil_y += p[1];
            } else {
                ++census.water;
                census.water_y += p[1];
            }
        }
        if (census.water > 0) census.water_y /= census.water;
        if (census.oil > 0) census.oil_y /= census.oil;
        return census;
    }

    // Steam total and its mass-weighted mean gas row.
    static std::array<double, 2> SteamStats(PowderApp& app) {
        constexpr int w = PowderApp::kGasWidth, h = PowderApp::kGasHeight;
        std::vector<float> steam(static_cast<std::size_t>(w) * h);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentSteam());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, steam.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        double total = 0.0, row = 0.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                total += steam[static_cast<std::size_t>(y * w + x)];
                row += steam[static_cast<std::size_t>(y * w + x)] * (y + 0.5);
            }
        return {total, total > 0.0 ? row / total : 0.0};
    }

    // Scripted single stamps and fixed ticks with the debug counters summed; walls of a tank over x in [400, 600).
    // Water closes the gas grid like dense sand: gas cells under a settled pool are marked porous (3) while the cells over
    // its surface stay open, smoke stamped into the pool does not stay there, and smoke driven down onto
    // the pool spreads over it instead of passing into it.
    static void GasWater(PowderApp& app) {
        PhaseScene scene(app);
        for (int i = 0; i < 4; ++i) {
            scene.Stamp(PowderApp::BrushMode::Water, 500, 40 + 30 * i, 35);
            scene.Tick(30);
        }
        scene.Tick(120);
        const auto read = [](GLuint texture, int width, int height) {
            std::vector<float> values(static_cast<std::size_t>(width * height));
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
            return values;
        };
        std::vector<GLubyte> mask(static_cast<std::size_t>(PowderApp::kGasWidth * PowderApp::kGasHeight));
        const auto read_mask = [&] {
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glBindTexture(GL_TEXTURE_2D, app.boundary_state_.mask_gas);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        };
        // The pool's surface: the highest full-resolution row in the tank's middle column with water in it.
        const auto liquid = read(app.water_state_.liquid_volume, PowderApp::kGridWidth, PowderApp::kGridHeight);
        int surface = 0;
        for (int y = 0; y < 400; ++y)
            if (liquid[static_cast<std::size_t>(y * PowderApp::kGridWidth + 500)] > 0.0F) surface = y;
        Require(surface > 40, "the gas-water tank needs a pool at least 40 cells deep: " + std::to_string(surface));
        read_mask();
        const auto gas_mask = [&](int x, int y) { return mask[static_cast<std::size_t>(y * PowderApp::kGasWidth + x)]; };
        int closed = 0, open_above = 0;
        for (int gx = 205; gx < 295; ++gx) {
            closed += gas_mask(gx, (surface - 16) / 2) == 3;
            open_above += gas_mask(gx, (surface + 12) / 2) == 0;
        }
        Require(closed >= 85 && open_above == 90, "gas cells under the pool must be closed and those above open: " +
                                                       std::to_string(closed) + " closed, " + std::to_string(open_above) + " open");
        // Smoke stamped into the pool is not there a tick later (the brush skips closed cells, advection clears them); smoke blown down at the pool stays above its surface.
        const auto smoke_below = [&](int full_row) {
            const auto smoke = read(app.CurrentSmokeDen(), PowderApp::kGasWidth, PowderApp::kGasHeight);
            double sum = 0.0, total = 0.0;
            for (int gy = 0; gy < PowderApp::kGasHeight; ++gy)
                for (int gx = 200; gx < 300; ++gx) {
                    const double value = smoke[static_cast<std::size_t>(gy * PowderApp::kGasWidth + gx)];
                    total += value;
                    if (2 * gy + 1 < full_row) sum += value;
                }
            return std::pair{sum, total};
        };
        scene.Stamp(PowderApp::BrushMode::Smoke, 500, surface / 2, 12);
        scene.Tick();
        Require(smoke_below(surface - 8).first == 0.0, "smoke inside the pool must be washed out");
        PowderApp::BrushConfig down = app.BuildBrushConfig();
        down.smoke_velocity = {0.0F, -150.0F};
        app.brush_y_ = surface + 30;
        for (int t = 0; t < 40; ++t) {
            app.StampBrush(down);
            scene.Tick();
        }
        const auto [under, total] = smoke_below(surface - 8);
        // Only open cells along the tank walls (where the pool meets the rock) can hold a trace.
        Require(total > 20.0 && under < 1e-4 * total, "smoke blown at the pool must stay above it: " + std::to_string(under) + " of " + std::to_string(total));
        Require(CleanCounters(scene.totals), "the gas-water scene must keep clean counters");
        std::cout << "gas-water: surface row " << surface << ", closed " << closed << "/90, smoke above " << total << '\n';
    }

    struct PhaseScene {
        PowderApp& app;
        std::array<GLuint, 5> totals{};
        explicit PhaseScene(PowderApp& a) : app(a) { PhaseSetup(app); }
        void Stamp(PowderApp::BrushMode mode, int x, int y, int radius) { PhaseStamp(app, mode, x, y, radius); }
        void Tick(int ticks = 1) { PhaseTick(app, totals, ticks); }
    };
    static void PhaseSetup(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 390, 0, 10, 400);
        PaintRock(app, 600, 0, 10, 400);
    }
    static void PhaseStamp(PowderApp& app, PowderApp::BrushMode mode, int x, int y, int radius) {
        app.brush_mode_ = mode;
        app.brush_x_ = x;
        app.brush_y_ = y;
        app.brush_radius_ = radius;
        app.StampBrush(app.BuildBrushConfig());
    }
    static void PhaseTick(PowderApp& app, std::array<GLuint, 5>& totals, int ticks) {
        for (int t = 0; t < ticks; ++t) {
            app.RunFrame(app.config_.fixed_dt, nullptr, true);
            const auto c = app.ReadDebugCounters();
            for (std::size_t i = 0; i < c.size(); ++i) totals[i] += c[i];
        }
    }

    // With POWDER_FRAME_DIR set, saves the tank region [380, 620) x [0, height) of the composite as <name>.ppm.
    static void PhaseFrame(PowderApp& app, const std::string& name, int height = 120) {
        const char* frame_dir = std::getenv("POWDER_FRAME_DIR");
        if (frame_dir == nullptr) return;
        FrameTarget target;
        OpenFrameTarget(app, target);
        SaveFrame(app, target, std::filesystem::path(frame_dir) / (name + ".ppm"), 380, 0, 240, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Oil poured into a settled pool plunges in, rises through the water and floats: its mean row ends above the water's.
    static void OilFloat(PowderApp& app) {
        PhaseScene scene(app);
        scene.Stamp(PowderApp::BrushMode::Water, 500, 60, 35);
        scene.Tick(120);
        // Pour oil into the pool from above: the stream plunges into the water.
        for (int t = 0; t < 8; ++t) {
            scene.Stamp(PowderApp::BrushMode::Oil, 500, 90, 8);
            scene.Tick();
        }
        const LiquidCensus start = Liquids(app);
        const std::uint64_t packed_start = app.ReadWaterPackedSum();
        PhaseFrame(app, "1-oil-pour-0.0s");
        scene.Tick(30);
        PhaseFrame(app, "1-oil-pour-0.5s");
        scene.Tick(60 * 4 - 30);
        PhaseFrame(app, "1-oil-pour-4.0s");
        const LiquidCensus end = Liquids(app);
        const std::uint64_t packed_end = app.ReadWaterPackedSum();
        // Oil records in the lower half of the water column, where floating oil never is.
        GLuint sunk = 0;
        for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound))
            if (p[4] >= 0.5F && p[10] > 0.5F && p[1] < end.water_y) ++sunk;
        std::cout << "oil float: oil/water records " << end.oil << '/' << end.water << ", mean rows oil " << start.oil_y << " -> "
                  << end.oil_y << ", water " << start.water_y << " -> " << end.water_y << ", oil below the water's mean row "
                  << sunk << '\n';
        Require(start.oil > 5000, "the pour must put oil in the pool");
        // Calm water packs (moving records become packed counts), so the water is counted moving plus packed.
        Require(end.oil == start.oil && end.water + packed_end == start.water + packed_start && app.ReadPhaseSinks() == std::array<GLuint, 2>{},
                "with no fire no liquid may be created or removed");
        Require(end.oil_y > end.water_y + 8.0 && sunk < end.oil / 50, "oil must float on the water after settling");
        Require(CleanCounters(scene.totals), "oil and water must keep clean counters");
    }

    // Oil in open air falls like any liquid: a blob released in the open and one released against the underside of a
    // rock ceiling both reach the floor, and none stays on the ceiling. Oil once rose through air and piled upside down
    // against ceilings because its buoyancy acted wherever oil was the local liquid.
    static void OilAir(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 300, 700, 400, 20);
        auto oil = RandomWater(450, 660, 550, 700, 7U);
        const auto open = RandomWater(100, 600, 150, 640, 8U);
        oil.insert(oil.end(), open.begin(), open.end());
        for (auto& p : oil) p[10] = 1.0F;
        WriteParticles(app, app.water_state_.particles, oil);
        app.oil_present_ = true;
        std::array<GLuint, 5> totals{};
        const auto mean_y = [&](bool open_blob) {
            double sum = 0.0;
            int n = 0;
            for (const auto& p : ReadParticles(app.water_state_.particles, oil.size()))
                if (p[4] >= 0.5F && (p[0] < 250.0F) == open_blob) {
                    sum += p[1];
                    ++n;
                }
            return n > 0 ? sum / n : 0.0;
        };
        const double open_start = mean_y(true);
        PhaseTick(app, totals, 15);  // 0.25 s: a free fall drops 0.5 g t^2 = 61 cells
        const double open_drop = open_start - mean_y(true);
        PhaseTick(app, totals, 60 * 3 - 15);
        int on_ceiling = 0;
        for (const auto& p : ReadParticles(app.water_state_.particles, oil.size()))
            // Under the slab and slow: splashes may land on top of it or still be in flight.
            if (p[4] >= 0.5F && p[1] > 400.0F && p[1] < 700.0F && std::hypot(p[2], p[3]) < 200.0F) {
                ++on_ceiling;
                std::cout << "  stuck oil at " << p[0] << ',' << p[1] << " v " << p[2] << ',' << p[3] << '\n';
            }
        std::cout << "oil in air: open blob dropped " << open_drop << " cells in 0.25 s (free fall 61), records still above row 400 after 3 s "
                  << on_ceiling << '\n';
        app.replay_mode_ = false;
        Require(open_drop > 0.9 * 61.0, "oil in open air must fall at about g");
        Require(on_ceiling == 0, "oil must not stay against a ceiling");
        Require(CleanCounters(totals), "falling oil must keep clean counters");
    }

    // Droplets touching the domain's side walls and a painted wall, blobs thrown at them and a stream poured down the
    // left edge into a pool all run off the walls: nothing stays above the pool.
    static void WaterWall(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 600, 300, 40, 700);  // up to the domain top, so no splash rests on it
        std::vector<Particle> drops;
        for (int k = 0; k < 6; ++k) {  // single-cell droplets stacked up each wall, a few cells apart
            for (const int x : {0, PowderApp::kGridWidth - 1, 599, 640}) {
                const auto d = RandomWater(x, 500 + 30 * k, x + 1, 501 + 30 * k, 11U + k);
                drops.insert(drops.end(), d.begin(), d.end());
            }
        }
        const auto film = RandomWater(0, 400, 2, 480, 5U);  // a thin film down the left edge
        drops.insert(drops.end(), film.begin(), film.end());
        // Blobs thrown at the walls: a splash against a wall must run down it.
        for (const auto& blob : {RandomWater(20, 600, 50, 620, 9U, -800.0F, 200.0F), RandomWater(970, 600, 990, 640, 10U, 900.0F, 0.0F),
                                  RandomWater(540, 600, 570, 620, 12U, 700.0F, 100.0F), RandomWater(660, 650, 690, 660, 13U, -700.0F, 0.0F)})
            drops.insert(drops.end(), blob.begin(), blob.end());
        WriteParticles(app, app.water_state_.particles, drops);
        std::array<GLuint, 5> totals{};
        const auto stuck = [&](const char* scene, float above) {
            int count = 0;
            for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound))
                // A clinging record hangs on a wall, slowly or not at all; splashes still in flight are fast.
                if (p[4] >= 0.5F && p[1] > above && std::hypot(p[2], p[3]) < 200.0F) {
                    if (count++ < 5) std::cout << "  " << scene << ": record at " << p[0] << ',' << p[1] << " v " << p[2] << ',' << p[3] << '\n';
                }
            std::cout << "water on walls (" << scene << "): " << count << " records above row " << above << '\n';
            return count;
        };
        PhaseTick(app, totals, 240);
        const int thrown = stuck("drops and splashes after 4 s", 200.0F);
        // A stream poured down the left edge into a pool, then 3 s to drain.
        app.InitializeState();
        WriteParticles(app, app.water_state_.particles, RandomWater(0, 0, PowderApp::kGridWidth, 60, 21U));
        for (int t = 0; t < 120; ++t) {
            PhaseStamp(app, PowderApp::BrushMode::Water, 12, 700, 10);
            PhaseTick(app, totals, 1);
        }
        PhaseTick(app, totals, 180);
        const int poured = stuck("stream down the wall after 3 s", 300.0F);
        // Like sand, water reaches the domain's faces: it rests on the floor's face and is no longer clamped half a cell
        // inside the side faces (a pool's flow keeps it a little off them, as it does off a painted wall). Measured on the
        // pool: scattered drops that are too sparse to be liquid land wherever they land.
        std::array<float, 3> reach{1e9F, 1e9F, -1e9F};  // min x, min y, max x
        for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound))
            if (p[4] >= 0.5F) reach = {std::min(reach[0], p[0]), std::min(reach[1], p[1]), std::max(reach[2], p[0])};
        std::cout << "water reach: min x " << reach[0] << ", min y " << reach[1] << ", max x " << reach[2] << '\n';
        Require(reach[0] < 0.5F && reach[1] < 0.25F, "water must reach the domain faces");
        app.replay_mode_ = false;
        Require(thrown == 0 && poured == 0, "water must not cling to walls");
        Require(CleanCounters(totals), "wall droplets must keep clean counters");
    }

    // Free-surface quality: randomly seeded 200 x 60 pools settle, and the smooth phi with ghost-fluid conductance leaves
    // them standing still. With the fill-fraction phi and a Dirichlet cell at the air centre (section 4.8 of the
    // algorithm reference) the mean speed after 5 s was 0.10 cells/s, the level spread 0.24 cells and the solve needed
    // 10 iterations; here 0.004, 0.05 and 3.
    static void WaterStill(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        constexpr int seeds = 3;
        double speed = 0.0, spread = 0.0, solve = 0.0, mass_error = 0.0;
        for (int s = 0; s < seeds; ++s) {
            app.InitializeState();
            PaintRock(app, 399, 0, 1, 300);
            PaintRock(app, 600, 0, 1, 300);
            const auto pool = RandomWater(400, 0, 600, 60, 1000U + 17U * static_cast<std::uint32_t>(s));
            WriteParticles(app, app.water_state_.particles, pool);
            for (int t = 0; t < 300; ++t) {
                app.RunFrame(app.config_.fixed_dt, nullptr, true);
                Require(CleanCounters(app.ReadDebugCounters()), "a settling pool must report no failures");
                if (t >= 240) solve += app.water_state_.pressure_iterations / (60.0 * seeds);
            }
            const WaterProbe probe = ProbeWater(app, 400, 599);
            speed += probe.mean_speed / seeds;
            spread += probe.level_spread / seeds;
            mass_error = std::max(mass_error, std::abs(probe.mass - static_cast<double>(pool.size())));
        }
        std::cout << "water still: mean speed " << speed << " cells/s, level spread " << spread << " cells, CG iterations " << solve << '\n';
        app.replay_mode_ = false;
        Require(mass_error == 0.0, "a settling pool must keep every particle");
        Require(speed < 0.03, "a settled pool must stand still");
        Require(spread < 0.15, "a settled pool must have a level surface");
        Require(solve < 6.0, "the solve of a settled pool must converge in a few iterations");
    }

    // The profile's full-width pour: a radius-48 brush held at y = 900, sweeping the domain at 800 cells/s, until
    // POWDER_POUR_TICKS (default 900). The brush stops at the record budget of the pool (about 49% of the screen at rest
    // density) while the churn lasts and goes on as calm water packs. Prints the pool every second; the same view of the
    // pour that VALIDATION tabulates, with the frame wall time.
    static void WaterPourFill(PowderApp& app) {
        app.config_ = powder_config::LoadConfigFromEnvironment();
        app.replay_mode_ = false;
        app.InitializeState();
        const char* ticks_env = std::getenv("POWDER_POUR_TICKS");
        const int total_ticks = ticks_env != nullptr ? std::atoi(ticks_env) : 900;
        const char* speed_env = std::getenv("POWDER_POUR_SPEED");
        const double sweep_speed = speed_env != nullptr ? std::atof(speed_env) : 800.0;
        app.render_mode_ = PowderApp::RenderMode::Composite;
        app.stroke_ = {};
        app.cursor_trail_.clear();
        app.painting_ = app.cursor_in_sim_ = false;
        const float dt = app.config_.fixed_dt;
        std::array<std::uint64_t, 5> sums{};
        std::vector<double> frame_ms;
        double solve = 0.0, substeps = 0.0;
        std::uint64_t peak_moving = 0;
        double fill = 0.0, fill_at_stop = 0.0;
        for (int frame = 0; frame < total_ticks; ++frame) {
            const double span = 900.0, phase = std::fmod(frame * static_cast<double>(dt) * sweep_speed, 2.0 * span);
            const int x = 50 + static_cast<int>(phase < span ? phase : 2.0 * span - phase);
            app.brush_mode_ = PowderApp::BrushMode::Water;
            app.brush_radius_ = 48;
            app.cursor_trail_.push_back({x, 900});
            app.brush_x_ = x;
            app.brush_y_ = 900;
            app.cursor_in_sim_ = app.painting_ = true;
            const auto begin = std::chrono::steady_clock::now();
            app.RunFrame(dt, nullptr, true);
            glFinish();
            frame_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            solve += app.water_state_.pressure_iterations;
            substeps += app.water_state_.substeps;
            const auto counters = app.ReadDebugCounters();
            for (std::size_t i = 0; i < counters.size(); ++i) sums[i] += counters[i];
            if (frame % 60 != 59) continue;
            const WaterCount count = CountWater(app);
            peak_moving = std::max(peak_moving, count.active);
            fill = 100.0 * static_cast<double>(count.Total()) / 4.0 / (PowderApp::kGridWidth * PowderApp::kGridHeight);
            if (frame == 359) fill_at_stop = fill;
            std::cout << "pour t=" << frame + 1 << " moving " << count.active << " packed " << count.packed << " fill " << fill
                      << "% frame ms " << frame_ms.back() << " CG " << app.water_state_.pressure_iterations << " substeps "
                      << app.water_state_.substeps << '\n';
        }
        std::sort(frame_ms.begin(), frame_ms.end());
        std::cout << "pour counters " << sums[0] << '/' << sums[1] << '/' << sums[2] << '/' << sums[3] << ", mean CG "
                  << solve / total_ticks << ", mean substeps " << substeps / total_ticks << ", frame ms p50/p95 "
                  << frame_ms[frame_ms.size() / 2] << '/' << frame_ms[frame_ms.size() * 95 / 100] << '\n';
        app.painting_ = app.cursor_in_sim_ = false;
        app.cursor_trail_.clear();
        Require(sums[0] == 0 && sums[2] == 0 && sums[3] == 0, "the pour must keep clean counters");
        Require(peak_moving <= static_cast<std::uint64_t>(PowderApp::kMaxWaterParticles), "the pool must never overflow");
        if (total_ticks >= 480)
            Require(fill_at_stop > 47.0 && fill > 48.0, "a full-width pour must fill about half the screen before the brush stops");
    }

    // A pool poured into a 300-cell tank splashes and sloshes for a few seconds, then calms: level surface, slow bulk,
    // and calm water packs.
    static void WaterCalm(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintRock(app, 349, 0, 1, 400);
        PaintRock(app, 650, 0, 1, 400);
        std::array<GLuint, 5> totals{};
        for (int t = 0; t < 60; ++t) {
            PhaseStamp(app, PowderApp::BrushMode::Water, 500, 250, 12);
            PhaseTick(app, totals, 1);
        }
        std::array<double, 3> surface{};
        double p50 = 0.0, packed_share = 0.0, early_p50 = 0.0;
        for (int second = 1; second <= 10; ++second) {
            PhaseTick(app, totals, 60);
            surface = WaterSurface(app, 355, 644);
            std::vector<float> speeds;
            for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound))
                if (p[4] >= 0.5F) speeds.push_back(std::hypot(p[2], p[3]));
            std::sort(speeds.begin(), speeds.end());
            p50 = speeds.empty() ? 0.0 : speeds[speeds.size() / 2];
            if (second == 2) early_p50 = p50;
            const WaterCount count = CountWater(app);
            packed_share = static_cast<double>(count.packed) / static_cast<double>(std::max<std::uint64_t>(1, count.Total()));
            std::cout << "water calm t=" << second << " s: level " << surface[0] << " std " << surface[1] << " max-min " << surface[2]
                      << ", moving p50 speed " << p50 << ", packed " << packed_share << '\n';
        }
        app.replay_mode_ = false;
        Require(early_p50 > 20.0, "a poured pool must still be moving 2 s after the pour");
        Require(surface[1] < 1.0 && surface[2] <= 4.0, "a calm pool must have a level surface after 10 s");
        Require(p50 < 1.0, "a poured pool must calm within 10 s");
        Require(packed_share > 0.6, "calm water must pack");
        Require(CleanCounters(totals), "the calming pool must keep clean counters");
    }

    // The profile's user-scene bowl (wood arc of radius-5 stamps) has one-cell pinholes in its wall whose four neighbours
    // are wood; a radius-35 oil stamp makes one of them (765,569) liquid through the particles at its open diagonal.
    // Its density correction was a right-hand side for a cell without a pressure equation (diagonal 0), so CG never
    // converged, diverged and overflowed the projected faces (here 1100167/0/633187 failure counts over 60 ticks).
    static void WaterSealedCell(PowderApp& app) {
        app.config_ = {};
        app.InitializeState();
        std::vector<Particle> water, sand;
        for (int y = 90; y < 190; ++y)
            for (int x = 20; x < 980; ++x) water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
        for (int y = 10; y < 90; ++y)
            for (int x = 20; x < 980; ++x) sand.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0});
        WriteParticles(app, app.water_state_.particles, water);
        WriteParticles(app, app.sand_state_.particles, SandRecords(sand));
        for (const auto [cx, cy, r] : {std::array<int, 3>{300, 560, 90}, std::array<int, 3>{700, 640, 100}})
            for (int a = 0; a <= 36; ++a) {
                const double t = 3.14159265358979 * (1.0 + a / 36.0);
                PhaseStamp(app, PowderApp::BrushMode::Wood, cx + static_cast<int>(r * std::cos(t)),
                           cy + static_cast<int>(r * std::sin(t)), 5);
            }
        std::array<GLuint, 5> totals{};
        PhaseStamp(app, PowderApp::BrushMode::Oil, 300, 520, 35);
        PhaseStamp(app, PowderApp::BrushMode::Oil, 700, 600, 35);
        PhaseTick(app, totals, 60);
        std::cout << "sealed cell counters " << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3] << '\n';
        Require(CleanCounters(totals), "oil against a pinholed wood bowl must keep clean counters");
    }

    // A small water brush held in the air pours an even stream. Spawning every record at the tick's start emitted one
    // slab per tick, v dt apart (2 cells at the brush, 18 a metre lower): the stream fell as a string of beads. Each
    // record's time since birth tau follows from its height and velocity (y = y0 + v tau - g tau^2 / 2); the mean
    // resultant length R of the birth phases 2 pi tau / dt is large when records are born with the tick and about
    // 1 / sqrt(N) when they are born at random times within it (water_particle_spawn.comp).
    static void WaterStream(PowderApp& app) {
        PhaseScene scene(app);
        for (int t = 0; t < 40; ++t) {
            scene.Stamp(PowderApp::BrushMode::Water, 500, 800, 2);
            scene.Tick();
        }
        const double g = app.config_.water_gravity, dt = app.config_.fixed_dt;
        double re = 0.0, im = 0.0, n = 0.0;
        float front = 1e9F;
        for (const auto& p : ReadParticles(app.water_state_.particles, app.water_state_.record_bound)) {
            if (p[4] < 0.5F || p[1] >= 790.0F) continue;  // records that have left the brush
            const double v = p[3], tau = (v + std::sqrt(std::max(0.0, v * v - 2.0 * g * (p[1] - 800.5)))) / g;
            re += std::cos(6.283185307179586 * tau / dt);
            im += std::sin(6.283185307179586 * tau / dt);
            n += 1.0;
            front = std::min(front, p[1]);
        }
        const double r = std::hypot(re, im) / std::max(n, 1.0);
        std::cout << "water stream: " << n << " records below the brush, front row " << front << ", birth-phase R " << r
                  << " (random phases about " << 1.0 / std::sqrt(std::max(n, 1.0)) << ")" << '\n';
        Require(n > 1000.0 && front < 600.0F, "the held brush must pour a stream");
        Require(r < 0.06, "a held brush must emit continuously, not one slab per tick");
        Require(CleanCounters(scene.totals), "the stream must keep clean counters");
    }

    // A rock frame around [x, x + w) x [y, y + h), walls `wall` cells thick.
    static void PaintFrame(PowderApp& app, int x, int y, int w, int h, int wall) {
        PaintRock(app, x - wall, y - wall, wall, h + 2 * wall);
        PaintRock(app, x + w, y - wall, wall, h + 2 * wall);
        PaintRock(app, x, y - wall, w, wall);
        PaintRock(app, x, y + h, w, wall);
    }

    // Liquid sealed in painted rock with no air cell is a singular Neumann system: its pressure is fixed only up to a
    // constant, and an overfilled pocket's density correction does not sum to zero, a right-hand side with no
    // solution, on which CG stalled. A pinhole (one cell walled in on all four sides), pockets of 2 to 5 cells and an
    // overfilled oil-and-water tank, all sealed, must keep clean counters and every record.
    static void WaterSealedPocket(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        std::vector<Particle> liquid;
        std::uint32_t state = 7U;
        const auto fill = [&](int x0, int y0, int x1, int y1, int per_cell, float oil) {
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    for (int i = 0; i < per_cell; ++i) {
                        state = state * 1664525U + 1013904223U;
                        const float fx = static_cast<float>(state >> 8) / 16777216.0F;
                        state = state * 1664525U + 1013904223U;
                        const float fy = static_cast<float>(state >> 8) / 16777216.0F;
                        liquid.push_back({x + fx, y + fy, 0, 0, 1, 1, 0, 0, 0, 0, oil, 0});
                    }
        };
        const std::array<std::array<int, 2>, 6> pockets{{{1, 1}, {2, 1}, {3, 1}, {2, 2}, {5, 1}, {1, 2}}};
        for (std::size_t k = 0; k < pockets.size(); ++k) {
            const int x = 100 + 40 * static_cast<int>(k), y = 500;
            PaintFrame(app, x, y, pockets[k][0], pockets[k][1], 4);
            fill(x, y, x + pockets[k][0], y + pockets[k][1], 6, 0.0F);
        }
        // The tank: water at rest density plus an oil record per cell, 25% over rest everywhere.
        PaintFrame(app, 400, 600, 200, 80, 6);
        fill(400, 600, 600, 680, 4, 0.0F);
        fill(400, 600, 600, 680, 1, 1.0F);
        WriteParticles(app, app.water_state_.particles, liquid);
        std::array<GLuint, 5> totals{};
        PhaseTick(app, totals, 120);
        const LiquidCensus census = Liquids(app);
        std::cout << "sealed pockets: counters " << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3]
                  << ", records " << census.water + census.oil << " of " << liquid.size() << ", oil mean row " << census.oil_y
                  << '\n';
        app.replay_mode_ = false;
        Require(census.water + census.oil + app.ReadWaterPackedSum() == liquid.size(), "sealed pockets must keep every record");
        Require(CleanCounters(totals), "liquid sealed in rock must keep clean counters");
    }

    // Gas sealed in a painted rock box (no open top, no porous sand) is singular the same way: a fire burning in it asks
    // the closed box to expand, which no pressure can do.
    static void GasSealedPocket(PowderApp& app) {
        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        PaintFrame(app, 400, 300, 120, 80, 8);
        // A gas cell is closed when its footprint plus a one-cell halo holds rock: these leave a single gas cell (351, 151)
        // and a two-by-two pocket (361-362, 151-152).
        PaintFrame(app, 701, 301, 4, 4, 8);
        PaintFrame(app, 721, 301, 6, 6, 8);
        std::array<GLuint, 5> totals{};
        double burning = 0.0;
        for (int t = 0; t < 90; ++t) {
            if (t < 60) {
                PhaseStamp(app, PowderApp::BrushMode::Fire, 460, 330, 20);
                PhaseStamp(app, PowderApp::BrushMode::Fire, 702, 302, 3);
                PhaseStamp(app, PowderApp::BrushMode::Fire, 724, 304, 3);
            }
            PhaseTick(app, totals, 1);
            if (t == 59) burning = FireStats(app)[0];
        }
        std::cout << "sealed gas: counters " << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3]
                  << ", gas CG iterations " << app.gas_state_.pressure_iterations << ", reaction while fed " << burning << '\n';
        app.replay_mode_ = false;
        Require(burning > 10.0, "the sealed fires must burn");
        Require(CleanCounters(totals), "fire sealed in rock must keep clean counters");
    }

    // A slick on a pool, lit from above, keeps burning after the brush stops and is consumed; every burned record is
    // counted, and the water under it only loses what the flame evaporated.
    static void OilBurn(PowderApp& app) {
        std::array<double, 2> late_reaction{};
        for (const bool oil : {false, true}) {
            PhaseScene scene(app);
            scene.Stamp(PowderApp::BrushMode::Water, 500, 40, 30);
            if (oil) scene.Stamp(PowderApp::BrushMode::Oil, 500, 110, 14);
            scene.Tick(90);
            const LiquidCensus before = Liquids(app);
            const std::uint64_t packed_before = app.ReadWaterPackedSum();  // calm water packs; packed cells are plain water
            // The level of the settled liquid, twice its mean row; the flame sits on it.
            const int surface = static_cast<int>(2.0 * (before.water_y * before.water + before.oil_y * before.oil) / (before.water + before.oil));
            for (int t = 0; t < 30; ++t) {
                // Lit across the slick: how far a flame spreads along it by itself is chaotic (one spot burned 21-98%
                // of the oil as the liquid details changed).
                scene.Stamp(PowderApp::BrushMode::Fire, 430 + 70 * (t % 3), surface + 8, 10);
                scene.Tick();
            }
            if (oil) PhaseFrame(app, "2-oil-burn-0.0s", 200);
            scene.Tick(60);  // a second after the brush: an unfed fire is out by now (fire-dynamics)
            if (oil) PhaseFrame(app, "2-oil-burn-1.0s", 200);
            late_reaction[oil ? 1 : 0] = FireStats(app)[0];
            scene.Tick(60 * 2);
            if (oil) PhaseFrame(app, "2-oil-burn-3.0s", 200);
            scene.Tick(60 * 3);
            if (oil) PhaseFrame(app, "2-oil-burn-6.0s", 200);
            const LiquidCensus after = Liquids(app);
            const auto sinks = app.ReadPhaseSinks();
            std::cout << "oil burn (" << (oil ? "slick" : "water only") << "): oil " << before.oil << " (mean rows oil " << before.oil_y << ", water " << before.water_y << ")" << " -> " << after.oil
                      << " burned=" << sinks[1] << ", water " << before.water << " -> " << after.water
                      << " evaporated=" << sinks[0] << ", reaction 1 s after the brush=" << late_reaction[oil ? 1 : 0] << '\n';
            Require(after.oil + sinks[1] == before.oil && after.water + app.ReadWaterPackedSum() + sinks[0] == before.water + packed_before,
                    "burning and evaporation must be the only, counted, liquid sinks");
            if (oil) Require(sinks[1] > before.oil / 2, "a lit slick must burn most of its oil within 6 s");
            Require(CleanCounters(scene.totals), "burning oil must keep clean counters");
        }
        // Three brush spots: 7.5 is three times the 2.5 a single spot was held to.
        Require(late_reaction[0] < 7.5 && late_reaction[1] > 10.0 && late_reaction[1] > 5.0 * late_reaction[0],
                "burning oil must keep the flame going after the brush stops; a fire over plain water goes out");
    }

    // The gas projection leaves the divergence its solve was asked for, up to the residual gate: per fluid cell,
    // div(projected faces) minus the burning source, recomputed in double from the solve's input and output faces.
    // With the right-hand side stored as half floats (the faces are R32F) its rounding alone reached 0.5 /s, twice the
    // 0.25 gate, in the combined benchmark's fire and smoke (RMS 0.025, against 0.0023 now).
    static void GasRhsPrecision(PowderApp& app) {
        static PowderApp* target = nullptr;
        static PFNGLDISPATCHCOMPUTEPROC native = nullptr;
        static std::vector<double> stats;  // max |e|, sum e^2, cells, max |b16 - b|, sum (b16-b)^2, max divergence change since the ask
        target = &app;
        native = glad_glDispatchCompute;
        struct Restore { ~Restore() { glad_glDispatchCompute = native; } } restore;
        // The solve is asked about the field at its first dispatch (smoke_divergence) and applies its pressure to the field
        // at the last (smoke_project), which the brush stamps and coupling of the tick between have changed. Its error is
        // the divergence it was asked about minus the divergence the correction removed; the change in between is the
        // coupling lag the next solve removes.
        static std::vector<float> asked_u, asked_v, asked_reaction;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            GLint program = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            const bool asks = static_cast<GLuint>(program) == target->smoke_divergence_program_;
            if (!asks && static_cast<GLuint>(program) != target->smoke_project_program_) return native(x, y, z);
            constexpr int w = PowderApp::kGasWidth, h = PowderApp::kGasHeight;
            const auto read = [](GLuint tex, std::size_t n, GLenum f = GL_RED) {
                std::vector<float> v(n);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glBindTexture(GL_TEXTURE_2D, tex);
                glGetTexImage(GL_TEXTURE_2D, 0, f, GL_FLOAT, v.data());
                return v;
            };
            if (asks) {
                asked_u = read(target->CurrentGasU(), (w + 1) * h);
                asked_v = read(target->CurrentGasV(), w * (h + 1));
                asked_reaction = read(target->CurrentReactionRate(), w * h * 2, GL_RG);
                return native(x, y, z);
            }
            const auto u0 = read(target->CurrentGasU(), (w + 1) * h), v0 = read(target->CurrentGasV(), w * (h + 1));
            const auto b = read(target->gas_state_.divergence, w * h);
            std::vector<GLubyte> kind(w * h);
            glBindTexture(GL_TEXTURE_2D, target->gas_state_.level_kind[0]);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, kind.data());
            native(x, y, z);
            const auto u1 = read(target->NextGasU(), (w + 1) * h), v1 = read(target->NextGasV(), w * (h + 1));
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) {
                const std::size_t c = static_cast<std::size_t>(j * w + i);
                if (kind[c] != 0) continue;
                const auto divergence = [&](const std::vector<float>& u, const std::vector<float>& v) {
                    return (double(u[j * (w + 1) + i + 1]) - u[j * (w + 1) + i]) + (double(v[(j + 1) * w + i]) - v[j * w + i]);
                };
                const double source = target->config_.gas_expansion * std::max(0.0F, asked_reaction[c * 2]);
                const double exact = divergence(asked_u, asked_v) - source;
                const double e = exact - (divergence(u0, v0) - divergence(u1, v1));
                stats[0] = std::max(stats[0], std::abs(e));
                stats[1] += e * e;
                stats[2] += 1;
                stats[3] = std::max(stats[3], std::abs(double(b[c]) - exact));
                stats[4] += (double(b[c]) - exact) * (double(b[c]) - exact);
                stats[5] = std::max(stats[5], std::abs(divergence(u0, v0) - divergence(asked_u, asked_v)));
            }
        };
        // The second pass drops sand through the flame: dense sand faces conduct at kSandLeak in the operator, so the
        // projection must correct them by the same weight.
        for (const bool sand : {false, true}) {
            stats.assign(6, 0.0);
            app.config_ = {};
            app.replay_mode_ = true;
            app.InitializeState();
            std::array<GLuint, 5> totals{};
            double iterations = 0;
            for (int t = 0; t < 240; ++t) {
                if (sand && t < 60) PhaseStamp(app, PowderApp::BrushMode::Sand, 500, 220, 16);
                app.brush_mode_ = t % 2 == 0 ? PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke;
                app.brush_x_ = 500;
                app.brush_y_ = 120;
                app.brush_radius_ = 18;
                const auto source = app.BuildBrushConfig();
                app.RunFrame(app.config_.fixed_dt, &source, true);
                const auto c = app.ReadDebugCounters();
                for (std::size_t i = 0; i < c.size(); ++i) totals[i] += c[i];
                iterations += app.gas_state_.pressure_iterations;
            }
            std::cout << "gas projection" << (sand ? " with sand" : "") << ": divergence error max " << stats[0]
                      << " RMS " << std::sqrt(stats[1] / stats[2]) << ", rhs rounding max " << stats[3] << " RMS "
                      << std::sqrt(stats[4] / stats[2]) << ", divergence change between ask and correction max " << stats[5]
                      << ", gas CG iterations mean " << iterations / 240.0
                      << ", counters " << totals[0] << '/' << totals[2] << '\n';
            app.replay_mode_ = false;
            Require(stats[2] > 0.0 && stats[0] <= app.config_.gas_pressure_residual_threshold,
                    "the gas projection must leave the divergence its solve was asked for");
            Require(CleanCounters(totals), "the gas precision scene must keep clean counters");
        }
    }

    // The gas pressure solve spans a tick pair: the first tick begins it and runs about half of the CG iterations, the
    // second finishes it. (1) Splitting changes nothing: from identical states, a projection split after three iterations
    // leaves bit-identical pressure and faces to the same projection in one go. (2) Each half of a pair carries about half
    // of the V-cycles. (3) The solve keeps the mask it was built on: rock painted between the halves changes the live gas
    // mask, not the solve's, and the residual gate and counters stay clean.
    static void GasSplitSolve(PowderApp& app) {
        constexpr int w = PowderApp::kGasWidth, h = PowderApp::kGasHeight;
        static PowderApp* target = nullptr;
        static PFNGLDISPATCHCOMPUTEPROC native = nullptr;
        static std::array<int, 2> relaxations{};  // by frame_index_ parity during the dispatch: [1] first tick, [0] second
        target = &app;
        native = glad_glDispatchCompute;
        struct Restore { ~Restore() { glad_glDispatchCompute = native; } } restore;
        glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
            GLint program = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            if (static_cast<GLuint>(program) == target->gas_pressure_relax_program_) ++relaxations[target->frame_index_ % 2U];
            native(x, y, z);
        };
        const auto read = [](GLuint tex, std::size_t n) {
            std::vector<float> v(n);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glBindTexture(GL_TEXTURE_2D, tex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, v.data());
            return v;
        };
        const auto tick = [&](int t, PowderApp::BrushMode mode = PowderApp::BrushMode::Fire, int y = 120, int radius = 18) {
            app.brush_mode_ = mode == PowderApp::BrushMode::Fire ? (t % 2 == 0 ? PowderApp::BrushMode::Fire : PowderApp::BrushMode::Smoke) : mode;
            app.brush_x_ = 500;
            app.brush_y_ = y;
            app.brush_radius_ = radius;
            const auto source = app.BuildBrushConfig();
            app.RunFrame(app.config_.fixed_dt, &source, true);
            return app.ReadDebugCounters();
        };

        std::array<std::array<std::vector<float>, 3>, 2> result;
        int iterations = 0;
        for (const int variant : {0, 1}) {
            app.config_ = {};
            app.replay_mode_ = true;
            app.InitializeState();
            for (int t = 0; t < 40; ++t) tick(t);
            Require(!app.gas_state_.solve_pending, "forty ticks must end a tick pair");
            if (variant == 0) {
                app.RunGasProjection();
            } else {
                app.RunGasProjectionStage(true, false, 3);
                Require(app.gas_state_.solve_pending && app.gas_state_.solve_iterations == 3, "the first half must stop at its budget");
                app.RunGasProjectionStage(false, true, -1);
            }
            iterations = app.gas_state_.solve_iterations;
            result[variant] = {read(app.CurrentGasU(), (w + 1) * h), read(app.CurrentGasV(), w * (h + 1)), read(app.gas_state_.pressure_a, w * h)};
        }
        Require(iterations > 3, "the fire scene must need more than the split's three iterations");
        for (std::size_t i = 0; i < 3; ++i)
            Require(result[0][i] == result[1][i], "a solve split across two calls must equal the one-call solve bit for bit");

        app.config_ = {};
        app.replay_mode_ = true;
        app.InitializeState();
        std::array<GLuint, 5> totals{};
        std::array<int, 2> vcycles{};
        int changed = 0;
        for (int t = 0; t < 140; ++t) {
            relaxations = {};
            // Rock painted into the plume on a second tick moves the live mask between that pair's halves.
            const bool paint = t == 101;
            const auto counters = paint ? tick(t, PowderApp::BrushMode::Solid, 150, 25) : tick(t);
            for (std::size_t i = 0; i < totals.size(); ++i) totals[i] += counters[i];
            const bool first_half = app.frame_index_ % 2U == 1U;
            Require(app.gas_state_.solve_pending == first_half, "a solve must be pending exactly between the two ticks of a pair");
            if (t >= 40) {
                vcycles[first_half ? 0 : 1] += relaxations[first_half ? 1 : 0];
            }
            if (paint) {
                // The tick just run was the second of its pair: its solve ran on the first tick's mask.
                const auto bytes = [](GLuint tex) {
                    std::vector<GLubyte> v(static_cast<std::size_t>(w) * h);
                    glMemoryBarrier(GL_ALL_BARRIER_BITS);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, v.data());
                    return v;
                };
                const auto live = bytes(app.boundary_state_.mask_gas), solved = bytes(app.gas_state_.mask_solve);
                for (std::size_t i = 0; i < live.size(); ++i) changed += live[i] != solved[i] ? 1 : 0;
            }
        }
        Require(changed > 100, "the rock painted between a pair's halves must change the live mask and not the solve's");
        std::cout << "gas split solve: " << iterations << " iterations in the bit-identity check; relaxation dispatches first/second tick "
                  << vcycles[0] << '/' << vcycles[1] << ", mask cells changed between the halves " << changed << ", counters " << totals[0] << '/' << totals[1] << '/' << totals[2] << '/' << totals[3] << '\n';
        Require(CleanCounters(totals), "the split solve must keep clean counters, rock painted between its halves included");
        const double share = static_cast<double>(vcycles[0]) / std::max(1, vcycles[0] + vcycles[1]);
        Require(share > 0.4 && share < 0.6, "each half of a pair must carry about half of the V-cycles");
        app.replay_mode_ = false;
    }

    // Water poured over a burning slick flashes to steam where it meets the lit oil and quenches the flame there, so a
    // slick doused all along goes out; before, the water sank through the oil and the slick burned on (flame half a
    // second after the dousing about that of the slick left alone). Every evaporated and burned record is counted:
    // moving plus packed water plus the evaporation sink equals the water there was plus what the brush spawned.
    static void OilQuench(PowderApp& app) {
        std::array<double, 2> flame{}, burned{}, flashed{};
        for (const bool douse : {false, true}) {
            PhaseScene scene(app);
            scene.Stamp(PowderApp::BrushMode::Water, 500, 40, 30);
            scene.Stamp(PowderApp::BrushMode::Oil, 500, 110, 14);
            scene.Tick(90);
            const LiquidCensus settled = Liquids(app);
            const int surface = static_cast<int>(2.0 * (settled.water_y * settled.water + settled.oil_y * settled.oil) / (settled.water + settled.oil));
            for (int t = 0; t < 30; ++t) {
                scene.Stamp(PowderApp::BrushMode::Fire, 430 + 70 * (t % 3), surface + 8, 10);
                scene.Tick();
            }
            const LiquidCensus before = Liquids(app);
            const std::uint64_t packed_before = app.ReadWaterPackedSum();
            const auto sinks_before = app.ReadPhaseSinks();
            const auto cursor = [&] {
                GLuint value = 0;
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.water_state_.spawn_cursor);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(value), &value);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                return value;
            };
            std::uint64_t spawned = 0;
            for (int t = 0; t < 30; ++t) {
                if (douse) {
                    const GLuint start = cursor();
                    scene.Stamp(PowderApp::BrushMode::Water, 420 + 40 * (t % 5), surface + 30, 12);
                    spawned += cursor() - start;
                }
                scene.Tick();
            }
            scene.Tick(30);
            flame[douse ? 1 : 0] = FireStats(app)[0];  // half a second after the last water
            scene.Tick(60);
            const LiquidCensus after = Liquids(app);
            const auto sinks = app.ReadPhaseSinks();
            burned[douse ? 1 : 0] = sinks[1] - sinks_before[1];
            flashed[douse ? 1 : 0] = sinks[0] - sinks_before[0];
            std::cout << "oil quench (" << (douse ? "doused" : "left burning") << "): water spawned " << spawned << ", evaporated "
                      << flashed[douse ? 1 : 0] << ", oil burned " << burned[douse ? 1 : 0] << " of " << before.oil
                      << ", flame 0.5 s after the dousing " << flame[douse ? 1 : 0] << '\n';
            Require(after.oil + sinks[1] == before.oil + sinks_before[1], "burned oil must be counted");
            Require(after.water + app.ReadWaterPackedSum() + sinks[0] == before.water + packed_before + sinks_before[0] + spawned,
                    "flashed water must be counted as evaporated");
            Require(CleanCounters(scene.totals), "dousing burning oil must keep clean counters");
        }
        Require(flame[0] > 10.0, "the undoused slick must still burn");
        Require(flame[1] < 0.3 * flame[0], "a slick doused all along must go out");
        Require(flashed[1] > 0.2 * (flashed[0] + 1.0) + 100.0, "water meeting burning oil must flash to steam");
    }

    // Water poured onto a fire that is still fed boils off into steam that rises; the fire is still quenched
    // (gas-quenching). Where the stream lands decides how much water flashes late and holds the steam's mean row down,
    // so a 2-cell shift of the stream moves the rise from 1 to 40 rows; the rise is the median over five streams.
    static void SteamPlume(PowderApp& app) {
        std::vector<double> rises;
        for (const int dx : {0, -4, -2, 2, 4}) {
            PhaseScene scene(app);
            std::array<double, 2> early{}, late{};
            double steam_peak = 0.0;
            for (int t = 0; t < 120; ++t) {
                scene.Stamp(PowderApp::BrushMode::Fire, 500, 30, 14);
                if (t >= 20 && t < 80) scene.Stamp(PowderApp::BrushMode::Water, 500 + dx, 170, 4);
                scene.Tick();
                const auto steam = SteamStats(app);
                steam_peak = std::max(steam_peak, steam[0]);
                if (t == 82) early = steam;
                if (t == 97) late = steam;
                if (dx == 0 && (t == 50 || t == 79)) PhaseFrame(app, "3-dousing-steam-" + std::to_string(t), 300);
            }
            const auto sinks = app.ReadPhaseSinks();
            std::cout << "steam: stream x " << 500 + dx << " evaporated=" << sinks[0] << " peak steam=" << steam_peak
                      << " mean gas row " << early[1] << " (t=82) -> " << late[1] << " (t=97), totals " << early[0] << " -> "
                      << late[0] << '\n';
            Require(sinks[0] > 100 && steam_peak > 20.0, "water hitting a fed fire must make steam");
            Require(early[0] > 5.0 && early[1] > 15.0, "the steam must leave the fire");
            Require(CleanCounters(scene.totals), "steam must keep clean counters");
            rises.push_back(late[1] - early[1]);
        }
        std::sort(rises.begin(), rises.end());
        std::cout << "steam: median rise " << rises[rises.size() / 2] << " rows\n";
        Require(rises[rises.size() / 2] > 5.0, "the steam must rise from the fire");
    }

    // Sand poured onto a fed fire smothers it.
    static void SandSmother(PowderApp& app) {
        std::array<double, 2> reaction{};
        for (const bool sand : {false, true}) {
            PhaseScene scene(app);
            for (int t = 0; t < 150; ++t) {
                scene.Stamp(PowderApp::BrushMode::Fire, 500, 16, 12);
                if (sand && t < 40) scene.Stamp(PowderApp::BrushMode::Sand, 500, 120, 20);
                scene.Tick();
                if (t >= 120) reaction[sand ? 1 : 0] += FireStats(app)[0] / 30.0;
            }
            Require(CleanCounters(scene.totals), "smothering must keep clean counters");
        }
        std::cout << "sand smother: fed fire reaction " << reaction[0] << " open, " << reaction[1] << " under sand\n";
        Require(reaction[0] > 10.0 && reaction[1] < 0.2 * reaction[0], "sand covering a fed fire must smother it");
    }

    // Gas-grid reaction and temperature totals, temperature-weighted centroid row, smoke total, centroid row and peak.
    static std::array<double, 6> FireStats(PowderApp& app) {
        constexpr int w = PowderApp::kGasWidth, h = PowderApp::kGasHeight;
        std::vector<float> temperature(static_cast<std::size_t>(w) * h), reaction(static_cast<std::size_t>(w) * h * 2),
            smoke(static_cast<std::size_t>(w) * h);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentTemperature());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, temperature.data());
        glBindTexture(GL_TEXTURE_2D, app.CurrentReactionRate());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, reaction.data());
        glBindTexture(GL_TEXTURE_2D, app.CurrentSmokeDen());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, smoke.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        double r = 0, heat = 0, heat_y = 0, s = 0, s_y = 0, s_max = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * w + x);
                r += reaction[i * 2 + 1];
                heat += temperature[i];
                heat_y += temperature[i] * (y + 0.5);
                s += smoke[i];
                s_y += smoke[i] * (y + 0.5);
                s_max = std::max<double>(s_max, smoke[i]);
            }
        return {r, heat, heat > 0 ? heat_y / heat : 0.0, s, s > 0 ? s_y / s : 0.0, s_max};
    }

    static int Run(const std::string& selected) {
        const std::array<std::pair<const char*, void (*)(PowderApp&)>, 80> tests{{
            {"fire-dynamics", FireDynamics}, {"wood-fire", WoodFire}, {"sparks", Sparks},
            {"water-still", WaterStill}, {"water-pour-fill", WaterPourFill}, {"water-pack-slosh", WaterPackSlosh}, {"water-ghost-fluid", WaterGhostFluid},
            {"oil-float", OilFloat}, {"oil-air", OilAir}, {"water-wall", WaterWall}, {"water-calm", WaterCalm}, {"water-sealed-cell", WaterSealedCell}, {"water-sealed-pocket", WaterSealedPocket}, {"water-stream", WaterStream}, {"gas-sealed-pocket", GasSealedPocket}, {"oil-burn", OilBurn}, {"oil-quench", OilQuench}, {"gas-rhs-precision", GasRhsPrecision}, {"gas-split-solve", GasSplitSolve}, {"steam", SteamPlume}, {"sand-smother", SandSmother}, {"gas-water", GasWater},
            {"brush-input", BrushInput}, {"hud", HudRender}, {"uniform-cache", UniformCache},
            {"upload-alignment", UploadAlignment}, {"reset", Reset}, {"erase", Erase}, {"water-force", WaterForce},
            {"water-force-range", WaterForceRange},
            {"water-surface", WaterSurface}, {"water-mass", WaterMass}, {"water-pool-full", WaterPoolFull}, {"water-density", WaterDensity}, {"water-free-slip", WaterFreeSlip}, {"water-free-fall", WaterFreeFall}, {"gas-projection", GasProjection},
            {"readback-invalid", ValidateReadbacks}, {"render-modes", RenderModes}, {"lighting", Lighting}, {"water-pressure", WaterPressure},
            {"water-multigrid", WaterMultigrid},
            {"water-solve-schedule", WaterSolveSchedule},
            {"replay-metrics", ValidateReplayMetrics}, {"boundary", BoundaryLifecycle},
            {"sand-substeps", SandSubsteps}, {"sand-water", SandWater}, {"sand-repose", SandRepose}, {"sand-silo", SandSilo}, {"sand-wet", SandWet}, {"sand-wet-mixed", SandWetMixed}, {"sand-pool-full", SandPoolFull}, {"sand-domain-edge", SandDomainEdge}, {"sand-sleep-conservation", SandSleepConservation}, {"sand-sleep-bias", SandSleepBias}, {"sand-sleep-no-flicker", SandSleepNoFlicker}, {"sand-sleep-water", SandSleepWater}, {"sand-sleep-submerged", SandSleepSubmerged}, {"benchmark-fill", BenchmarkFill}, {"water-pack-conservation", WaterPackConservation}, {"water-pack-no-flicker", WaterPackNoFlicker}, {"water-pack-wave", WaterPackWave}, {"benchmark-water-fill", BenchmarkWaterFill}, {"gas-diffusion-substeps", GasDiffusionSubsteps}, {"benchmark-gas-activity", GasActivity}, {"coupling-lifecycle", CouplingLifecycle},
            {"benchmark-pressure", BenchmarkPressure}, {"benchmark-coupling", BenchmarkCoupling}, {"benchmark-gas", BenchmarkGas},
            {"benchmark-water-scenes", WaterScenes}, {"benchmark-feel", FeelScenes}, {"showcase", Showcase}, {"look-scene", LookScene},
            {"benchmark-gas-pressure", [](PowderApp& app) { BenchmarkGasPressure(app, false); }},
            {"benchmark-gas-slow", [](PowderApp& app) { BenchmarkGasPressure(app, true); }},
            {"gas-quenching", [](PowderApp& app) {
                GasQuenching(app, false);
                GasQuenching(app, true);
            }},
            {"benchmark", [](PowderApp& app) { Benchmark(app, 1); }},
            {"benchmark-realtime", [](PowderApp& app) { Benchmark(app, 2); }},
            {"benchmark-interactive", BenchmarkInteractive}, {"benchmark-profile", BenchmarkProfile}}};
        Require(selected == "all" || std::any_of(tests.begin(), tests.end(), [&](const auto& test) {
                    return selected == test.first;
                }), "unknown app lifecycle test: " + selected);
        Require(glfwInit() == GLFW_TRUE, "GLFW initialization failed");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        PowderApp app;
        int failures = 0;
        try {
            app.window_ = glfwCreateWindow(32, 32, "Powder lifecycle regressions", nullptr, nullptr);
            Require(app.window_ != nullptr, "could not create hidden OpenGL 4.3 context");
            glfwMakeContextCurrent(app.window_);
            Require(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0,
                    "GLAD failed to load OpenGL");
            app.gl_ready_ = true;
            // This harness creates its window directly instead of calling Initialize.
            glfwSetWindowUserPointer(app.window_, &app);
            glfwSetKeyCallback(app.window_, PowderApp::KeyCallback);
            glfwSetCursorPosCallback(app.window_, PowderApp::CursorPosCallback);
            glfwSetMouseButtonCallback(app.window_, PowderApp::MouseButtonCallback);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            app.CreateTextures();
            app.CreateBuffers();
            app.CreatePrograms();
            for (const auto& [name, test] : tests) {
                if (selected == "all" && (std::string(name).starts_with("benchmark") || std::string(name) == "showcase")) continue;
                if (selected != "all" && selected != name) {
                    continue;
                }
                try {
                    test(app);
                    CheckGl(name);
                    std::cout << "PASS " << name << '\n';
                } catch (const std::exception& error) {
                    ++failures;
                    std::cerr << "FAIL " << name << ": " << error.what() << '\n';
                }
                glfwPollEvents();
            }
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL setup: " << error.what() << '\n';
        }
        app.Shutdown();
        return failures == 0 ? 0 : 1;
    }
};

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "Usage: powder_app_lifecycle_tests [all|water-still|water-pour-fill|water-pack-slosh|water-ghost-fluid|brush-input|hud|uniform-cache|upload-alignment|reset|erase|water-force|water-force-range|water-surface|water-mass|water-pool-full|water-density|water-free-slip|water-free-fall|gas-projection|gas-quenching|readback-invalid|render-modes|lighting|water-pressure|water-multigrid|water-solve-schedule|replay-metrics|boundary|sand-substeps|sand-water|sand-repose|sand-silo|sand-wet|sand-wet-mixed|sand-pool-full|sand-domain-edge|sand-sleep-conservation|sand-sleep-bias|sand-sleep-no-flicker|sand-sleep-water|sand-sleep-submerged|benchmark-fill|water-pack-conservation|water-pack-no-flicker|water-pack-wave|benchmark-water-fill|gas-diffusion-substeps|coupling-lifecycle|fire-dynamics|wood-fire|sparks|oil-float|oil-air|water-wall|water-calm|water-sealed-cell|water-sealed-pocket|gas-sealed-pocket|water-stream|oil-burn|oil-quench|gas-rhs-precision|gas-split-solve|steam|sand-smother|gas-water|benchmark-pressure|benchmark-coupling|benchmark-gas|benchmark-water-scenes|benchmark-feel|showcase|look-scene|benchmark-gas-pressure|benchmark-gas-slow|benchmark-gas-activity|benchmark|benchmark-realtime|benchmark-interactive|benchmark-profile]");
        return AppLifecycleTests::Run(argc == 2 ? argv[1] : "all");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

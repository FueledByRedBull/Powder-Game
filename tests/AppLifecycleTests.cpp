#include "PowderApp.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
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
using SandParticle = std::array<float, 16>;

template <std::size_t Components>
void WriteParticles(GLuint buffer, const std::vector<std::array<float, Components>>& particles) {
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
}

std::vector<SandParticle> SandRecords(const std::vector<Particle>& common, float density = 2.0F) {
    std::vector<SandParticle> result(common.size());
    for (std::size_t i = 0; i < common.size(); ++i) {
        std::copy(common[i].begin(), common[i].end(), result[i].begin());
        result[i][10] = result[i][5] / density;
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
                        exact = exact && mask[y * PowderApp::kGridWidth + x] == (occupied(x, y, dynamic) ? 1 : 0);
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
                    bool expected = false;
                    for (int sy = std::max(0, y * 2 - 1); sy < std::min(PowderApp::kGridHeight, y * 2 + 3); ++sy)
                        for (int sx = std::max(0, x * 2 - 1); sx < std::min(PowderApp::kGridWidth, x * 2 + 3); ++sx)
                            expected = expected || occupied(sx, sy, true);
                    exact = exact && mask[y * PowderApp::kGasWidth + x] == (expected ? 1 : 0);
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
        WriteScalar(app.sand_state_.occupancy, 0.2F, 510, 510);
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
        for (GLuint texture : {app.gas_state_.mac_u_a, app.gas_state_.pressure_coarse_a}) {
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
        WriteParticles(app.sand_state_.particles, SandRecords(particle));
        WriteParticles(app.water_state_.particles, particle);
        app.brush_mode_ = PowderApp::BrushMode::Erase;
        const auto config = app.BuildBrushConfig();
        app.RunSpawnPass(&config);
        Require(ReadInteger(app.CurrentMaterial(), 500, 500) == 0U, "scripted erase must clear painted solids");
        Require(ReadParticles<16>(app.sand_state_.particles, 1)[0][4] == 0.0F, "scripted erase must kill sand particles");
        Require(ReadParticles(app.water_state_.particles, 1)[0][4] == 0.0F, "scripted erase must kill water particles");
        Require(ReadScalar(app.CurrentWaterAmount(), 500, 500) == 0.0F, "scripted erase must clear derived water");
        Require(ReadScalar(app.CurrentSmokeDen(), 250, 250) == 0.0F &&
                    ReadScalar(app.CurrentFuel(), 250, 250) == 0.0F,
                "scripted erase must clear gas scalars");
    }

    static void WaterForce(PowderApp& app) {
        const auto original_config = app.config_;
        app.config_.water_flip_blend = 1.0F;
        app.config_.water_velocity_damping = 1.0F;
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
            WriteParticles(app.water_state_.particles, particles);
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
            dispatches = 0;
            app.RunWaterPass(app.config_.fixed_dt);
            const float actual = forced_v;
            const float expected = gravity * (app.config_.fixed_dt / steps);
            const float half_ulp = std::ldexp(1.0F, std::ilogb(std::abs(expected)) - 10);
            Require(dispatches > 0 && app.water_state_.substeps == steps && std::isfinite(actual) &&
                        std::abs(actual - expected) <= half_ulp &&
                        app.ReadDebugCounters() == std::array<GLuint, 4>{},
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
            WriteParticles(app.water_state_.particles, particles);
            app.config_.water_gravity = impulse / (app.config_.fixed_dt / 2.0F);
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
            std::cout << " counters=" << counters[0] << ',' << counters[1] << ',' << counters[2] << ',' << counters[3] << '\n';
            if (overflow && counters[0] == 0U)
                failures << " [out-of-range additive force silently saturated without a diagnostic]";
            if (!overflow && (projections == 0 || counters != std::array<GLuint, 4>{}))
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
        WriteParticles(app.water_state_.particles, particles);
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
        WriteParticles(app.water_state_.particles, particles);
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

    static void BenchmarkPressure(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth, height = PowderApp::kGridHeight;
        constexpr std::size_t cells = static_cast<std::size_t>(width) * height;
        std::ifstream snapshot("build/goal-baseline/water-pressure-failing-system.bin", std::ios::binary);
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
                    ++diagonal;
                    const auto j = static_cast<std::size_t>(ny * width + nx);
                    if (phi[j] < app.config_.water_liquid_phi_threshold) neighbors += pressure[j];
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
            Require(liquid_cells > 0 && failures == 0 && counters == std::array<GLuint, 4>{},
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
            valid = valid && fluid > 0 && failures == 0 && counters == std::array<GLuint, 4>{} &&
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
                glBindImageTexture(0, app.gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
                glBindImageTexture(2, app.boundary_state_.mask_gas, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8UI);
                glBindImageTexture(3, app.gas_state_.divergence, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
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

    static void WaterPressure(PowderApp& app) {
        constexpr int width = PowderApp::kGridWidth;
        constexpr int height = PowderApp::kGridHeight;
        constexpr int liquid_width = 64;
        constexpr int liquid_depth = 32;
        constexpr int liquid_cells = 64 * liquid_depth;
        const auto original_config = app.config_;
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
        static PFNGLDISPATCHCOMPUTEPROC native_pressure_dispatch = nullptr;
        static int pressure_dispatches = 0;
        static GLuint audited_pressure_program = 0, iteration_groups = 0;
        const auto counted_projection = [&]() {
            native_pressure_dispatch = glad_glDispatchCompute;
            audited_pressure_program = app.pressure_cg_program_;
            struct Restore {
                ~Restore() { glad_glDispatchCompute = native_pressure_dispatch; }
            } restore;
            pressure_dispatches = 0;
            iteration_groups = 0;
            glad_glDispatchCompute = [](GLuint x, GLuint y, GLuint z) {
                ++pressure_dispatches;
                GLint program = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                if (static_cast<GLuint>(program) == audited_pressure_program) {
                    GLint phase = 0;
                    glGetUniformiv(program, glGetUniformLocation(program, "phase"), &phase);
                    if (phase == 1) iteration_groups = std::max(iteration_groups, x);
                }
                native_pressure_dispatch(x, y, z);
            };
            app.RunWaterProjection();
            return pressure_dispatches;
        };
        for (int cap : checkpoints) {
            const GLuint zero = 0;
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            app.config_.water_pressure_iterations = cap;
            const std::vector<float> zero_pressure(width * height, 0.0F);
            upload(app.CurrentWaterPressure(), GL_RED, GL_FLOAT, zero_pressure.data());
            const int dispatches = counted_projection();
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
                      << " dispatches=" << dispatches
                      << " iteration-groups=" << iteration_groups
                      << ": RMS=" << residual.rms << " max=" << residual.maximum << " mean=" << residual.mean
                      << " count=" << residual.above_threshold << '/' << liquid_cells
                      << " max|p|=" << residual.pressure_maximum << " breakdown=" << solver[5] << '\n';
            Require(counters[0] == 0 && counters[2] == static_cast<GLuint>(residual.above_threshold),
                    "production residual counter must match the independent pool residual at every cap");
            Require(dispatches <= 6 + 6 * std::min(cap, static_cast<int>((solver[6] + 7) / 8 * 8)),
                    "host must stop dispatching within one batch of GPU convergence");
            Require(iteration_groups == static_cast<GLuint>((liquid_cells + 255) / 256),
                    "pressure iteration dispatch must cover the liquid rectangle, not the whole domain");
            if (cap == 1) Require(residual.above_threshold > 0, "one CG iteration must report insufficient pool convergence");
            if (cap == default_iterations) {
                default_result = residual;
            }
        }
        Require(counted_projection() == 4, "an unchanged converged system must reuse pressure without iteration dispatches");
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
        Require(counted_projection() > 4, "changed RHS and mask must trigger a new pressure solve");
        Require(iteration_groups == 4, "pressure iteration bounds must shrink with the current liquid mask");
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
            counted_projection();
            const GLuint expected_groups = full_extent ? static_cast<GLuint>((width * height + 255) / 256) : 1U;
            Require(iteration_groups == expected_groups, "bounds must include both disconnected liquid regions and domain extrema");
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
        Require(counted_projection() == 4, "an empty solve must initialize and validate without iteration dispatches");
        app.config_ = original_config;
        CheckGl("production water pressure pool");
        Require(default_result.above_threshold == 0,
                "default water pressure cap " + std::to_string(default_iterations) + " leaves " +
                    std::to_string(default_result.above_threshold) + " pool cells above the existing residual threshold; max=" +
                    std::to_string(default_result.maximum));
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
            Require(relaxation_dispatches % 16 == 0, "gas solve must execute complete multigrid cycles");
            return relaxation_dispatches / 16;
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
        expect(app.ReadDebugCounters() == std::array<GLuint, 4>{}, "zero-RHS gas must retain clean diagnostics");
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
        expect(app.ReadDebugCounters() == std::array<GLuint, 4>{}, "RMS-limited solve must retain clean diagnostics");
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
                if (cycles[i] <= 4) expect(executed_cycles == cycles[i], scene + ": insufficient budget must execute every cycle");
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
                expect(counters[0] == 0 && counters[2] == nonconverged,
                       context + ": final residual counter must match independent stored-field operator");
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
            expect(errors[1][0].rms < errors[0][0].rms && errors[2][0].rms < errors[1][0].rms,
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
        WriteParticles(app.water_state_.particles, std::vector<Particle>{{500.5F, 920.5F, 0.0F, 0.0F, 1.0F, 1.0F}});
        reset_metrics("boundary");
        app.UpdateReplayMetrics(boundary_scene, 0);
        expect(!app.ReplayMetricsOk(), "water that never reaches an obstacle must fail the boundary scene");
        WriteInteger(app.boundary_state_.mask_static, 1, 500, 500);
        for (const float y : {501.5F, 500.5F, 499.5F}) {
            WriteParticles(app.water_state_.particles, std::vector<Particle>{{500.5F, y, 0.0F, 0.0F, 1.0F, 1.0F}});
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
            if (buffer == app.sand_state_.particles) WriteParticles(buffer, SandRecords(particles));
            else WriteParticles(buffer, particles);
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
        WriteParticles(app.sand_state_.particles, SandRecords(particles));
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
        const std::array<GLuint, 4> prior_tick_counters{0, 7, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(prior_tick_counters), prior_tick_counters.data());
        app.RunFrame(app.config_.fixed_dt, nullptr, false);
        expect(app.ReadDebugCounters()[1] == 7, "later catchup ticks must preserve earlier debug failures");
        app.RunFrame(app.config_.fixed_dt, nullptr, true);
        expect(app.ReadDebugCounters() == std::array<GLuint, 4>{}, "first tick must start a fresh counter interval");

        int rejected = 0;
        for (int component : {0, 4, 5}) {
            auto invalid = particles;
            invalid[0][component] = std::nanf("");
            WriteParticles(app.water_state_.particles, invalid);
            try { app.ReadWaterLiveParticles(); } catch (const std::runtime_error&) { ++rejected; }
        }
        for (int component : {10, 12, 15}) {
            auto invalid = SandRecords(particles);
            invalid[0][component] = std::nanf("");
            WriteParticles(app.sand_state_.particles, invalid);
            try { app.ReadSandLiveParticles(); } catch (const std::runtime_error&) { ++rejected; }
        }
        expect(rejected == 6, "particle readback must reject invalid position, active flag, mass, and sand material state");
        std::cout << "particle metric invalid records rejected=" << rejected << "/6\n";
        Require(failures.str().empty(), "replay validation is not measuring actual behavior:" + failures.str());
    }

    static void RenderModes(PowderApp& app) {
        app.InitializeState();
        app.ResetReplayMetrics();
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
        check_mode(PowderApp::RenderMode::Composite, {0.09F, 0.11F, 0.14F});
        std::vector<GLuint> material(PowderApp::kGridWidth * PowderApp::kGridHeight, 2U);
        glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
        check_mode(PowderApp::RenderMode::Composite, {0.70F, 0.74F, 0.79F});
        // Sand spans empty and solid cells, including the solid-dependent tint.
        // Wide constant bands make the filtered occupancy analytically known away from their edges.
        std::vector<float> occupancy(material.size());
        constexpr std::array<float, 4> levels{0.0F, 0.03125F, 0.125F, 1.0F};
        for (int y = 0; y < PowderApp::kGridHeight; ++y) for (int x = 0; x < PowderApp::kGridWidth; ++x) {
            const auto index = static_cast<std::size_t>(y * PowderApp::kGridWidth + x);
            const bool solid = (x / 125 + y / 125) % 2 != 0;
            material[index] = solid ? 2U : 0U;
            occupancy[index] = levels[static_cast<std::size_t>(x / 250)];
        }
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, app.CurrentMaterial());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, material.data());
        glBindTexture(GL_TEXTURE_2D, app.sand_state_.occupancy);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PowderApp::kGridWidth, PowderApp::kGridHeight,
                        GL_RED, GL_FLOAT, occupancy.data());
        app.Render();
        std::vector<unsigned char> mixed_pixels(static_cast<std::size_t>(width * height * 4));
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, mixed_pixels.data());
        std::array<int, 4> checked{};
        std::uint64_t mixed_hash = 14695981039346656037ULL;
        for (unsigned char value : mixed_pixels) mixed_hash = (mixed_hash ^ value) * 1099511628211ULL;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const float grid_x = (x + 0.5F) * PowderApp::kGridWidth / width;
            const int sx = static_cast<int>(grid_x);
            const int sy = static_cast<int>((y + 0.5F) * PowderApp::kGridHeight / height);
            // Filter taps crossing an occupancy band are covered by the hash, not this constant-band oracle.
            if (std::min(std::fmod(grid_x, 250.0F), 250.0F - std::fmod(grid_x, 250.0F)) < 2.0F) continue;
            const bool solid = (sx / 125 + sy / 125) % 2 != 0;
            const float sand = std::min(1.0F, levels[static_cast<std::size_t>(sx / 250)] * 1.02F);
            std::array<float, 3> expected = solid ? std::array<float, 3>{0.70F, 0.74F, 0.79F} :
                                                  std::array<float, 3>{0.09F, 0.11F, 0.14F};
            if (sand > 0.06F) {
                const float blend = std::max(solid ? 0.65F : 0.0F, sand * 0.68F);
                constexpr std::array<float, 3> relaxed{0.95F, 0.78F, 0.34F}, compressed{0.63F, 0.41F, 0.16F};
                for (int channel = 0; channel < 3; ++channel)
                    expected[channel] = relaxed[channel] + (compressed[channel] - relaxed[channel]) * blend;
            }
            ++checked[(sand > 0.06F ? 2U : 0U) + (solid ? 1U : 0U)];
            for (int channel = 0; channel < 4; ++channel) {
                const int expected_byte = channel == 3 ? 255 : static_cast<int>(std::lround(expected[channel] * 255.0F));
                Require(std::abs(static_cast<int>(mixed_pixels[static_cast<std::size_t>((y * width + x) * 4 + channel)]) - expected_byte) <= 1,
                        "mixed empty/solid/sand composite pixel mismatch");
            }
        }
        Require(std::all_of(checked.begin(), checked.end(), [](int count) { return count > 0; }),
                "mixed composite must exercise empty, solid, sand over empty, and sand over solid");
        std::cout << "mixed composite RGBA hash=" << mixed_hash << " checked="
                  << checked[0] << '/' << checked[1] << '/' << checked[2] << '/' << checked[3] << '\n';
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
        const std::array<GLuint, 4> counters{8, 16, 4, 4};
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, app.debug_state_.counters);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        check_mode(PowderApp::RenderMode::DebugCounters, {0.25F, 0.5F, 0.25F});
        app.render_mode_ = PowderApp::RenderMode::Composite;
        app.InitializeState();
        std::cout << "render extraction corners/constants and all ten render modes passed\n";
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
                        ++diagonal;
                        const auto j = static_cast<std::size_t>(ny * width + nx);
                        if (liquid == 0 || phi[j] < app.config_.water_liquid_phi_threshold) sum += pressure[j];
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
                const int cycles = gas_relaxations / 16;
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
                std::vector<Particle> water, sand;
                water.reserve(100000);
                sand.reserve(50000);
                for (int y = 200; y < 600; ++y)
                    for (int x = 600; x < 850; ++x)
                        water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                for (int y = 200; y < 450; ++y)
                    for (int x = 100; x < 300; ++x)
                        sand.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0});
                WriteParticles(app.water_state_.particles, water);
                WriteParticles(app.sand_state_.particles, SandRecords(sand, app.config_.sand_reference_density));
            }
            const auto initial_water = app.ReadParticleMetrics(app.water_state_.particles, PowderApp::kMaxWaterParticles,
                                                               PowderApp::kWaterParticleComponents);
            const auto initial_sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                              PowderApp::kSandParticleComponents);
            Require(initial_water.count == (combined ? 100000U : 0U) && initial_sand.count == (combined ? 50000U : 0U),
                    "benchmark initial particle load does not match its declared workload");
            std::vector<double> gpu_ms, wall_ms;
            std::array<double, PowderApp::kReplayQueryCount> last_tick_pass_ms{};
            double last_water_iterations = 0.0;
            GLuint max_water_iterations = 0;
            std::array<std::uint64_t, 4> counters{};
            double largest_mass_drift = 0.0;
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
                for (int tick = 0; tick < ticks_per_render; ++tick)
                    app.RunFrame(app.config_.fixed_dt, combined ? &source : nullptr, tick == 0);
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
                    const auto water = app.ReadParticleMetrics(app.water_state_.particles, PowderApp::kMaxWaterParticles,
                                                               PowderApp::kWaterParticleComponents);
                    const auto sand = app.ReadParticleMetrics(app.sand_state_.particles, PowderApp::kMaxSandParticles,
                                                              PowderApp::kSandParticleComponents);
                    largest_mass_drift = std::max({largest_mass_drift,
                        std::abs(water.mass - initial_water.mass) / std::max(1.0, initial_water.mass),
                        std::abs(sand.mass - initial_sand.mass) / std::max(1.0, initial_sand.mass)});
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
                      << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '\n';
            std::cout << "benchmark " << name << " last tick mean ms spawn/boundary/water/gas/sand/coupling/extract=";
            for (double total : last_tick_pass_ms) std::cout << total / samples << '/';
            std::cout << " last water solve iterations mean/max=" << last_water_iterations / samples << '/' << max_water_iterations << '\n';
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
                largest_mass_drift > 0.005 || counters != std::array<std::uint64_t, 4>{}) {
                failures << " [" << name << " full-frame acceptance failed]";
            }
        }
        app.replay_mode_ = false;
        if (audit_pressure) std::cout << "pressure audit total water/gas failing cells=" << pressure_failures[0] << '/' << pressure_failures[1] << '\n';
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
        app.RunSandPass(1.0F / 60.0F);
        Require(app.sand_state_.substeps == 18 && dispatches == 3 * 18 + 3,
                "default sand step must dispatch 18 complete material substeps");
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
        app.config_.sand_max_substeps = 17;
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
            app.config_.sand_shear_modulus = scale;
            app.config_.sand_lame_lambda = scale;
            app.config_.sand_reference_density = scale;
            app.config_.sand_friction_alpha = 0.0F;
            dispatches = 0;
            app.RunSandPass(1.0F / 60.0F);
            Require(app.sand_state_.substeps == 12 && dispatches == 39,
                    "representable extreme material coefficients must remain accepted");
        }
        app.config_ = {};
        app.config_.sand_shear_modulus = 1.0e-30F;
        dispatches = 0;
        app.RunSandPass(1.0F / 60.0F);
        Require(app.sand_state_.substeps == 15 && dispatches == 48,
                "large finite yield ratio must remain accepted");
        int force_failures = 0;
        for (float gravity : {-1.0e8F, 1.0e8F, -6.0e7F, 6.0e7F}) {
            app.config_ = {};
            app.InitializeState();
            app.config_.sand_gravity = gravity;
            WriteParticles(app.sand_state_.particles, SandRecords({Particle{500.5F, 500.5F, 0, 0, 1, 1}}));
            dispatches = 0;
            bool rejected = false;
            try { app.RunSandPass(1.0F / 60.0F); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("sand_gravity") != std::string::npos;
            }
            const bool unsafe = std::abs(gravity) == 1.0e8F;
            const auto particle = ReadParticles<16>(app.sand_state_.particles, 1)[0];
            const auto counters = app.ReadDebugCounters();
            std::cout << "sand gravity=" << gravity << " rejected=" << rejected << " dispatches=" << dispatches
                      << " alive/mass=" << particle[4] << '/' << particle[5] << " position=" << particle[0] << '/' << particle[1]
                      << " velocity=" << particle[2] << '/' << particle[3] << " debug="
                      << counters[0] << '/' << counters[1] << '/' << counters[2] << '/' << counters[3] << '\n';
            if (unsafe && !rejected) {
                // Record the driver outcome: out-of-range R16F stores may saturate rather than become infinity.
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
                std::cout << "  unchecked R16F sand grid peak=" << peak << " nonfinite=" << nonfinite << '\n';
            }
            const bool valid = unsafe ? rejected && dispatches == 0 && particle[0] == 500.5F && particle[1] == 500.5F :
                !rejected && dispatches == 57 && std::all_of(particle.begin(), particle.end(), [](float x) { return std::isfinite(x); });
            if (!valid || particle[4] != 1.0F || particle[5] != 1.0F || counters != std::array<GLuint, 4>{}) {
                ++force_failures;
                std::cerr << "FAIL sand force preflight for gravity=" << gravity << '\n';
            }
        }
        int additive_failures = 0;
        for (float sign : {-1.0F, 1.0F}) {
            constexpr float dt = 0.0005F;
            app.config_ = {};
            app.InitializeState();
            app.config_.sand_damping = 1.0F;
            app.config_.sand_drag = 0.0F;
            app.config_.sand_gravity = -sign * (65400.0F / dt);
            const float initial_velocity = sign * 280.0F;
            const float impulse = -app.config_.sand_gravity * dt;
            const float expected = initial_velocity + impulse;
            WriteParticles(app.sand_state_.particles,
                           SandRecords({Particle{500.5F, 500.5F, 0, initial_velocity, 1, 1}}));
            dispatches = 0;
            bool rejected = false;
            try { app.RunSandPass(dt); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("sand_gravity") != std::string::npos;
            }
            const auto counters = app.ReadDebugCounters();
            const auto particle = ReadParticles<16>(app.sand_state_.particles, 1)[0];
            float stored = 0.0F;
            if (!rejected) {
                Require(app.sand_state_.substeps == 1 && dispatches == 6,
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
            if (!reported || particle[4] != 1.0F || particle[5] != 1.0F) ++additive_failures;
        }
        app.config_ = {};
        std::cout << "sand material substeps=18 dispatches=57; budget/extreme guards, material failures="
                  << material_failures << ", force failures=" << force_failures << '\n';
        Require(material_failures == 0, "sand derived material coefficients must reject before GPU dispatch");
        Require(force_failures == 0, "sand unrepresentable gravity must reject before GPU dispatch");
        Require(additive_failures == 0, "finite sand force overflow must reject or report before R16F storage");
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
                app.config_.gas_buoyancy_temperature = app.config_.gas_buoyancy_smoke = 0.0F;
                app.config_.gas_velocity_dissipation = app.config_.gas_scalar_dissipation = 1.0F;
                app.config_.gas_combustion_rate = app.config_.gas_temperature_diffusion = 0.0F;
                app.config_.gas_reaction_decay = app.config_.gas_temperature_cooling = 0.0F;
                WriteParticles(app.water_state_.particles,
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

    static void GasDiffusionSubsteps(PowderApp& app) {
        app.config_ = {};
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
        double final_reaction = 0.0;
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
            final_reaction = reaction;
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
                const auto particles = app.ReadParticleMetrics(app.water_state_.particles, PowderApp::kMaxWaterParticles,
                                                                PowderApp::kWaterParticleComponents);
                maximum_wet_reaction = std::max(maximum_wet_reaction, wet_reaction);
                maximum_coupling_reduction = std::max(maximum_coupling_reduction, pre_sum - reaction);
                Require(particles.count == expected_particles &&
                        std::abs(particles.mass - expected_particles * app.config_.water_particle_mass) <= 0.001,
                        "gas quench source must conserve the independently counted particle mass");
                std::cout << "gas replay frame=" << frame << " reaction before/wet/predicted/after=" << pre_sum << '/'
                          << wet_reaction << '/' << predicted_sum << '/' << reaction
                          << " reaction bounds=" << min_x << ',' << min_y << ':' << max_x << ',' << max_y
                          << " center=" << center_x / std::max(reaction, 1.0e-12) << ',' << center_y / std::max(reaction, 1.0e-12)
                          << " water mass=" << particles.mass << " max coupling rounding=" << maximum_formula_error << '\n';
            }
            Require(app.ReadDebugCounters() == std::array<GLuint, 4>{}, "gas quenching must keep clean diagnostics");
        }
        Require(ignited, "gas replay must actually ignite before quenching");
        Require(omit_water ? maximum_wet_reaction == 0.0 && maximum_coupling_reduction == 0.0 :
                            maximum_wet_reaction >= 2.5 && maximum_coupling_reduction >= 2.5,
                "extinction must include measured water/reaction overlap and actual coupling suppression");
        Require(maximum_formula_error <= 1.0 / 1024.0, "actual coupling must follow the existing formula within one R16F ULP");
        Require(omit_water ? final_reaction >= 2.5 : final_reaction < 2.5,
                "water must extinguish the ignited scene below 2.5 while the no-water control keeps burning; final=" +
                    std::to_string(final_reaction));
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
                    std::vector<Particle> water, sand;
                    water.reserve(100000); sand.reserve(50000);
                    for (int y = 200; y < 600; ++y) for (int x = 600; x < 850; ++x)
                        water.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                    for (int y = 200; y < 450; ++y) for (int x = 100; x < 300; ++x)
                        sand.push_back({x + 0.5F, y + 0.5F, 0, 0, 1, 1});
                    WriteParticles(app.water_state_.particles, water);
                    WriteParticles(app.sand_state_.particles, SandRecords(sand, app.config_.sand_reference_density));
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
                const auto water = app.ReadParticleMetrics(app.water_state_.particles, PowderApp::kMaxWaterParticles,
                                                           PowderApp::kWaterParticleComponents);
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
                std::array<GLuint, 4> counters{};
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
            ~RestoreRadius() {
                app.brush_radius_ = radius;
                app.replay_mode_ = replay;
                app.brush_mode_ = brush;
                app.render_mode_ = render;
                glfwSetWindowShouldClose(app.window_, closing);
            }
        } restore{app, original_radius, app.replay_mode_, app.brush_mode_, app.render_mode_, glfwWindowShouldClose(app.window_)};
        app.replay_mode_ = false;
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
        app.replay_mode_ = true;
        event(GLFW_KEY_RIGHT_BRACKET, GLFW_PRESS);
        event(GLFW_KEY_LEFT_BRACKET, GLFW_REPEAT);
        event(GLFW_KEY_1, GLFW_PRESS);
        event(GLFW_KEY_F2, GLFW_PRESS);
        event(GLFW_KEY_ESCAPE, GLFW_PRESS);
        Require(app.brush_radius_ == 8 && app.brush_mode_ == Brush::Fire && app.render_mode_ == View::Composite &&
                    glfwWindowShouldClose(app.window_) == GLFW_FALSE,
                "live key events must not change scripted replay input state");
        std::cout << "brush callback radius/bounds, 60/144 polls, all6brushes/10views, tap/order/release/Escape/replay pass; native mouse and held input remain manual\n";
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

    static int Run(const std::string& selected) {
        const std::array<std::pair<const char*, void (*)(PowderApp&)>, 26> tests{{
            {"brush-input", BrushInput}, {"uniform-cache", UniformCache},
            {"upload-alignment", UploadAlignment}, {"reset", Reset}, {"erase", Erase}, {"water-force", WaterForce},
            {"water-force-range", WaterForceRange},
            {"water-surface", WaterSurface}, {"water-mass", WaterMass}, {"gas-projection", GasProjection},
            {"readback-invalid", ValidateReadbacks}, {"render-modes", RenderModes}, {"water-pressure", WaterPressure},
            {"replay-metrics", ValidateReplayMetrics}, {"boundary", BoundaryLifecycle},
            {"sand-substeps", SandSubsteps}, {"gas-diffusion-substeps", GasDiffusionSubsteps}, {"coupling-lifecycle", CouplingLifecycle},
            {"benchmark-pressure", BenchmarkPressure}, {"benchmark-coupling", BenchmarkCoupling}, {"benchmark-gas", BenchmarkGas},
            {"benchmark-gas-pressure", [](PowderApp& app) { BenchmarkGasPressure(app, false); }},
            {"benchmark-gas-slow", [](PowderApp& app) { BenchmarkGasPressure(app, true); }},
            {"gas-quenching", [](PowderApp& app) {
                GasQuenching(app, false);
                GasQuenching(app, true);
            }},
            {"benchmark", [](PowderApp& app) { Benchmark(app, 1); }},
            {"benchmark-realtime", [](PowderApp& app) { Benchmark(app, 2); }}}};
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
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            app.CreateTextures();
            app.CreateBuffers();
            app.CreatePrograms();
            for (const auto& [name, test] : tests) {
                if (selected == "all" && std::string(name).starts_with("benchmark")) continue;
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
        Require(argc <= 2, "Usage: powder_app_lifecycle_tests [all|brush-input|uniform-cache|upload-alignment|reset|erase|water-force|water-force-range|water-surface|water-mass|gas-projection|gas-quenching|readback-invalid|render-modes|water-pressure|replay-metrics|boundary|sand-substeps|gas-diffusion-substeps|coupling-lifecycle|benchmark-pressure|benchmark-coupling|benchmark-gas|benchmark-gas-pressure|benchmark-gas-slow|benchmark|benchmark-realtime]");
        return AppLifecycleTests::Run(argc == 2 ? argv[1] : "all");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include "GLUtil.hpp"
#include "Config.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Particle = std::array<float, 16>;
constexpr int kSize = 16;
constexpr int kCells = kSize * kSize;
constexpr int kTransferSize = kSize + 2;
constexpr int kTransferCells = kTransferSize * kTransferSize;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckGl() {
    std::ostringstream errors;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) errors << " 0x" << std::hex << error;
    Require(errors.str().empty(), "OpenGL errors:" + errors.str());
}

void Barrier() {
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    CheckGl();
}

void Image(GLuint binding, GLuint texture, GLenum access, GLenum format) {
    glBindImageTexture(binding, texture, 0, GL_FALSE, 0, access, format);
}

struct SandFixture {
    std::vector<GLuint> textures;
    std::vector<GLuint> buffers;
    std::vector<GLuint> programs;
    GLuint sdf, water, water_velocity, velocity;
    GLuint particles, mass, momentum_x, momentum_y, counters;
    GLuint p2g, grid, step;
    int particle_count = 1;
    float shear_modulus = 20.0F, lame_lambda = 30.0F, friction_alpha = 0.2F;
    float gravity = 0.0F, wall_friction = 0.0F;

    GLuint Texture(GLenum format, GLenum external, float value = 0.0F, int width = kSize, int height = kSize) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::vector<float> values(width * height * (external == GL_RG ? 2 : 1), value);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, external, GL_FLOAT, values.data());
        return texture;
    }

    GLuint Buffer(std::size_t words) {
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        buffers.push_back(buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        const std::vector<GLuint> zeros(words);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(words * sizeof(GLuint)), zeros.data(), GL_DYNAMIC_READ);
        return buffer;
    }

    GLuint Program(const char* filename) {
        const GLuint program = glutil::CreateComputeProgramFromFile((std::filesystem::path(POWDER_SHADER_DIR) / filename).string());
        programs.push_back(program);
        return program;
    }

    SandFixture() {
        sdf = Texture(GL_R16F, GL_RED, 16.0F);
        water = Texture(GL_R16F, GL_RED);
        water_velocity = Texture(GL_RG16F, GL_RG);
        velocity = Texture(GL_RG16F, GL_RG, 0.0F, kTransferSize, kTransferSize);
        particles = Buffer(16);
        mass = Buffer(kTransferCells);
        momentum_x = Buffer(kTransferCells * 2);
        momentum_y = Buffer(kTransferCells * 2);
        counters = Buffer(4);
        p2g = Program("sand_particle_p2g.comp");
        grid = Program("sand_grid_update.comp");
        step = Program("sand_particle_step.comp");
        CheckGl();
    }

    ~SandFixture() {
        glUseProgram(0);
        for (GLuint program : programs) glDeleteProgram(program);
        glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }

    void Upload(Particle particle) {
        if (particle[10] == 0.0F) particle[10] = particle[5] / 2.0F;
        if (particle[12] == 0.0F && particle[13] == 0.0F && particle[14] == 0.0F && particle[15] == 0.0F) {
            particle[12] = particle[15] = 1.0F;
        }
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(Particle), particle.data());
    }

    Particle Read() const {
        Particle particle{};
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(Particle), particle.data());
        for (float value : particle) Require(std::isfinite(value), "sand fixture produced a nonfinite particle");
        Require(particle[4] > 0.5F, "sand fixture lost its particle");
        std::array<GLuint, 4> debug{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, counters);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(debug), debug.data());
        Require(std::all_of(debug.begin(), debug.end(), [](GLuint value) { return value == 0U; }),
                "sand fixture reported a debug failure");
        return particle;
    }

    void Scatter(float dt = 0.0F) {
        const GLuint zero = 0;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        for (GLuint buffer : {mass, momentum_x, momentum_y}) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        }
        glUseProgram(p2g);
        glUniform2i(glGetUniformLocation(p2g, "gridSize"), kSize, kSize);
        glUniform1i(glGetUniformLocation(p2g, "maxParticles"), particle_count);
        glUniform1f(glGetUniformLocation(p2g, "dt"), dt);
        glUniform1f(glGetUniformLocation(p2g, "shearModulus"), shear_modulus);
        glUniform1f(glGetUniformLocation(p2g, "lameLambda"), lame_lambda);
        glUniform1f(glGetUniformLocation(p2g, "massScale"), 4096.0F);
        glUniform1f(glGetUniformLocation(p2g, "velocityScale"), 4096.0F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, counters);
        glDispatchCompute((particle_count + 255) / 256, 1, 1);
        Barrier();
    }

    void Grid(float dt, float damping) {
        glUseProgram(grid);
        glUniform2i(glGetUniformLocation(grid, "gridSize"), kSize, kSize);
        glUniform1f(glGetUniformLocation(grid, "dt"), dt);
        glUniform1f(glGetUniformLocation(grid, "gravity"), gravity);
        glUniform1f(glGetUniformLocation(grid, "damping"), damping);
        glUniform1f(glGetUniformLocation(grid, "drag"), 0.0F);
        glUniform1f(glGetUniformLocation(grid, "wallFriction"), wall_friction);
        glUniform1f(glGetUniformLocation(grid, "massScale"), 4096.0F);
        glUniform1f(glGetUniformLocation(grid, "velocityScale"), 4096.0F);
        Image(0, sdf, GL_READ_ONLY, GL_R16F);
        Image(1, water, GL_READ_ONLY, GL_R16F);
        Image(2, water_velocity, GL_READ_ONLY, GL_RG16F);
        Image(3, velocity, GL_WRITE_ONLY, GL_RG16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, mass);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, momentum_x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, momentum_y);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, counters);
        glDispatchCompute((kTransferSize + 15) / 16, (kTransferSize + 15) / 16, 1);
        Barrier();
    }

    void Step(float dt, float cap, float damping = 1.0F, float restitution = 0.0F, int width = kSize) {
        glUseProgram(step);
        glUniform2i(glGetUniformLocation(step, "gridSize"), width, kSize);
        glUniform1f(glGetUniformLocation(step, "dt"), dt);
        glUniform1f(glGetUniformLocation(step, "damping"), damping);
        glUniform1f(glGetUniformLocation(step, "restitution"), restitution);
        glUniform1f(glGetUniformLocation(step, "maxVelocity"), cap);
        glUniform1i(glGetUniformLocation(step, "maxParticles"), particle_count);
        glUniform1f(glGetUniformLocation(step, "shearModulus"), shear_modulus);
        glUniform1f(glGetUniformLocation(step, "lameLambda"), lame_lambda);
        glUniform1f(glGetUniformLocation(step, "frictionAlpha"), friction_alpha);
        Image(0, sdf, GL_READ_ONLY, GL_R16F);
        Image(1, velocity, GL_READ_ONLY, GL_RG16F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particles);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, counters);
        glDispatchCompute((particle_count + 255) / 256, 1, 1);
        Barrier();
    }
};

void Near(std::ostringstream& failures, float actual, float expected, float tolerance, const std::string& label) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        failures << " [" << label << ": " << actual << ", expected " << expected << ']';
    }
}

void Transfer() {
    std::ostringstream failures;
    for (bool affine : {false, true}) {
        SandFixture fixture;
        const Particle before{8.75F, 8.75F, 1.0F, -1.0F, 1.0F, 16.0F,
                              affine ? 0.25F : 0.0F, affine ? -0.5F : 0.0F,
                              affine ? 0.5F : 0.0F, affine ? 0.25F : 0.0F, 1.0F, 0.0F};
        fixture.Upload(before);
        fixture.Scatter();
        fixture.Grid(1.0F / 60.0F, 1.0F);
        fixture.Step(1.0F / 60.0F, 1000.0F);
        const auto after = fixture.Read();
        const std::string label = affine ? "affine support" : "constant support";
        std::cout << label << ": velocity=" << after[2] << ',' << after[3]
                  << " C=" << after[6] << ',' << after[7] << ',' << after[8] << ',' << after[9] << '\n';
        // The reciprocal mass normalization and RG16F grid storage can introduce one half-float ULP.
        for (int component : {2, 3, 6, 7, 8, 9}) Near(failures, after[component], before[component], 0.001F, label);
    }
    SandFixture quadratic;
    quadratic.Upload({8.75F, 8.75F, 0.0F, 0.0F, 1.0F, 1.0F});
    std::array<float, kTransferCells * 2> field{};
    for (int y = 0; y < kTransferSize; ++y) {
        for (int x = 0; x < kTransferSize; ++x) {
            const float dx = x - 0.5F - 8.75F;
            const float dy = y - 0.5F - 8.75F;
            field[(y * kTransferSize + x) * 2] = dx * dx;
            field[(y * kTransferSize + x) * 2 + 1] = dy * dy;
        }
    }
    glBindTexture(GL_TEXTURE_2D, quadratic.velocity);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTransferSize, kTransferSize, GL_RG, GL_FLOAT, field.data());
    quadratic.Step(1.0F / 60.0F, 1000.0F);
    const auto result = quadratic.Read();
    // Quadratic weights at this offset have second moment 1/4 and third moment 3/64.
    for (int component : {2, 3}) Near(failures, result[component], 0.25F, 0.00001F, "quadratic velocity moment");
    for (int component : {6, 9}) Near(failures, result[component], 0.1875F, 0.00001F, "quadratic affine moment");
    for (int component : {7, 8}) Near(failures, result[component], 0.0F, 0.00001F, "quadratic cross moment");
    std::cout << "quadratic field: velocity=" << result[2] << ',' << result[3]
              << " C=" << result[6] << ',' << result[7] << ',' << result[8] << ',' << result[9] << '\n';
    Require(failures.str().empty(), "sand transfer mismatch:" + failures.str());
}

void Integration() {
    std::ostringstream failures;
    SandFixture capped;
    capped.Upload({8.75F, 8.75F, 100.0F, 0.0F, 1.0F, 16.0F});
    capped.Scatter();
    capped.Grid(1.0F / 60.0F, 1.0F);
    capped.Step(1.0F / 60.0F, 1.0F);
    const auto cap_result = capped.Read();
    std::cout << "cap: velocity=" << cap_result[2] << " displacement=" << cap_result[0] - 8.75F << '\n';
    Near(failures, cap_result[2], 1.0F, 0.00001F, "capped velocity");
    Near(failures, cap_result[0] - 8.75F, 1.0F / 60.0F, 0.00001F, "capped displacement");
    for (float dt : {1.0F / 120.0F, 1.0F / 60.0F, 1.0F / 30.0F}) {
        SandFixture fixture;
        fixture.Upload({8.75F, 8.75F, 8.0F, 0.0F, 1.0F, 16.0F});
        fixture.Scatter();
        fixture.Grid(dt, 0.5F);
        fixture.Step(dt, 1000.0F, 0.5F);
        const auto result = fixture.Read();
        const float expected = 8.0F * std::pow(0.5F, dt * 60.0F);
        std::cout << "damping dt=" << dt << ": velocity=" << result[2]
                  << " displacement=" << result[0] - 8.75F << " expected velocity=" << expected << '\n';
        Near(failures, result[2], expected, 0.002F, "single time-scaled damping");
        Near(failures, result[0] - 8.75F, result[2] * dt, 0.00001F, "damped displacement");
    }
    Require(failures.str().empty(), "sand integration mismatch:" + failures.str());
}

void Support() {
    std::ostringstream failures;
    const std::array<std::array<float, 2>, 13> positions{{
        {8.5F, 8.5F}, {8.5F, 0.5F}, {8.5F, 15.5F}, {0.5F, 8.5F}, {15.5F, 8.5F},
        {0.5F, 0.5F}, {15.5F, 0.5F}, {0.5F, 15.5F}, {15.5F, 15.5F},
        {8.75F, 0.75F}, {15.25F, 8.75F}, {0.75F, 0.75F}, {15.25F, 15.25F}}};
    for (const auto& position : positions) {
        SandFixture fixture;
        fixture.Upload({position[0], position[1], 1.0F, 2.0F, 1.0F, 1.0F});
        fixture.Scatter();
        std::array<GLuint, kTransferCells> grid_mass{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.mass);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(grid_mass), grid_mass.data());
        double total_mass = 0.0;
        for (GLuint value : grid_mass) total_mass += value / 4096.0;
        if ((position[0] == 8.5F && position[1] == 8.5F) ||
            (position[0] == 0.5F && position[1] == 0.5F)) {
            const GLuint occupancy = fixture.Texture(GL_R16F, GL_RED);
            const GLuint raster = fixture.Program("sand_rasterize.comp");
            glUseProgram(raster);
            glUniform2i(glGetUniformLocation(raster, "gridSize"), kSize, kSize);
            glUniform1f(glGetUniformLocation(raster, "massScale"), 4096.0F);
            Image(0, occupancy, GL_WRITE_ONLY, GL_R16F);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, fixture.mass);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, fixture.counters);
            glDispatchCompute(1, 1, 1);
            Barrier();
            std::array<float, kCells> rendered{};
            glBindTexture(GL_TEXTURE_2D, occupancy);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, rendered.data());
            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    const float expected = std::min(grid_mass[(y + 1) * kTransferSize + x + 1] / 4096.0F, 1.0F);
                    Near(failures, rendered[y * kSize + x], expected, 0.001F, "physical raster coordinates");
                }
            }
        }
        fixture.Grid(0.0F, 1.0F);
        fixture.Step(0.0F, 1000.0F);
        const auto result = fixture.Read();
        const std::string label = "support at " + std::to_string(position[0]) + ',' + std::to_string(position[1]);
        std::cout << label << ": mass=" << total_mass << " velocity=" << result[2] << ',' << result[3]
                  << " C=" << result[6] << ',' << result[7] << ',' << result[8] << ',' << result[9] << '\n';
        // Nine independently rounded fixed-point deposits can differ by at most 9/(2*4096).
        Near(failures, static_cast<float>(total_mass), 1.0F, 0.0011F, label + " unit mass");
        Near(failures, result[2], 1.0F, 0.002F, label + " constant velocity x");
        Near(failures, result[3], 2.0F, 0.002F, label + " constant velocity y");
        for (int component : {6, 7, 8, 9}) Near(failures, result[component], 0.0F, 0.002F, label + " zero affine");

        const Particle affine{position[0], position[1], 1.0F, 2.0F, 1.0F, 16.0F, 0.25F, -0.5F, 0.5F, 0.25F};
        fixture.Upload(affine);
        fixture.Scatter();
        fixture.Grid(0.0F, 1.0F);
        fixture.Step(0.0F, 1000.0F);
        const auto affine_result = fixture.Read();
        for (int component : {2, 3, 6, 7, 8, 9}) {
            Near(failures, affine_result[component], affine[component], 0.003F, label + " affine recovery");
        }
    }
    for (const auto& particle : std::array<Particle, 4>{{
             {8.5F, 0.5F, 1.0F, 0.0F, 1.0F, 16.0F},
             {8.5F, 15.5F, 1.0F, 0.0F, 1.0F, 16.0F},
             {0.5F, 8.5F, 0.0F, 1.0F, 1.0F, 16.0F},
             {15.5F, 8.5F, 0.0F, 1.0F, 1.0F, 16.0F}}}) {
        SandFixture fixture;
        fixture.Upload(particle);
        fixture.Scatter();
        fixture.Grid(1.0F / 60.0F, 1.0F);
        fixture.Step(1.0F / 60.0F, 1000.0F);
        const auto result = fixture.Read();
        for (int axis = 0; axis < 2; ++axis) {
            Near(failures, result[axis], particle[axis] + particle[axis + 2] / 60.0F, 0.00002F,
                 "wall tangent displacement");
            Near(failures, result[axis + 2], particle[axis + 2], 0.002F, "wall tangent velocity");
        }
        for (int component : {6, 7, 8, 9}) Near(failures, result[component], 0.0F, 0.002F, "wall tangent zero affine");
    }
    Require(failures.str().empty(), "sand domain support mismatch:" + failures.str());
}

void Material() {
    constexpr float mu = 20.0F;
    constexpr float lambda = 30.0F;
    constexpr float alpha = 0.2F;
    constexpr float force_dt = 0.01F;
    using MaterialParticle = std::array<float, 16>;
    std::ostringstream failures;
    const auto deformation = [](float a, float b, float left, float right) {
        const float c = std::cos(left), s = std::sin(left);
        const float d = std::cos(right), t = std::sin(right);
        return std::array<float, 4>{c * a * d + s * b * t, s * a * d - c * b * t,
                                    c * a * t - s * b * d, s * a * t + c * b * d};
    };
    const auto upload = [](SandFixture& fixture, const std::array<float, 4>& f) {
        const MaterialParticle particle{8.5F, 8.5F, 0.0F, 0.0F, 1.0F, 1.0F,
                                        0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, f[0], f[1], f[2], f[3]};
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.particles);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(particle), particle.data(), GL_DYNAMIC_READ);
    };
    struct StrainCase { const char* name; float a, b, left, right; };
    const std::array<StrainCase, 7> stress_cases{{
        {"identity", 1.0F, 1.0F, 0.0F, 0.0F}, {"rigid rotation", 1.0F, 1.0F, 0.7F, 0.0F},
        {"isotropic compression", 0.8F, 0.8F, 0.0F, 0.0F},
        {"unequal compression", std::exp(-0.05F), std::exp(-0.15F), 0.0F, 0.0F},
        {"rotated unequal compression", std::exp(-0.05F), std::exp(-0.15F), 0.6F, 0.25F},
        {"nearly equal compression", 0.800001F, 0.8F, 0.4F, 0.2F},
        {"near singular compression", 0.001F, 0.8F, 0.4F, 0.2F}}};
    for (const auto& test : stress_cases) {
        SandFixture fixture;
        upload(fixture, deformation(test.a, test.b, test.left, test.right));
        fixture.Scatter(force_dt);
        fixture.Read();
        std::array<GLint, kTransferCells * 2> px{}, py{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.momentum_x);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(px), px.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.momentum_y);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(py), py.data());
        const float trace = std::log(test.a) + std::log(test.b);
        const float t0 = 2.0F * mu * std::log(test.a) + lambda * trace;
        const float t1 = 2.0F * mu * std::log(test.b) + lambda * trace;
        const float c = std::cos(test.left), s = std::sin(test.left);
        const float xx = c * c * t0 + s * s * t1;
        const float yy = s * s * t0 + c * c * t1;
        const float xy = c * s * (t0 - t1);
        const std::array<float, 3> w{0.125F, 0.75F, 0.125F};
        const std::array<float, 3> dw{-0.5F, 0.0F, 0.5F};
        float resultant_x = 0.0F, resultant_y = 0.0F, torque = 0.0F, outward = 0.0F;
        for (int y = 0; y < kTransferSize; ++y) {
            for (int x = 0; x < kTransferSize; ++x) {
                const int i = x - 8, j = y - 8;
                float expected_x = 0.0F, expected_y = 0.0F;
                if (i >= 0 && i < 3 && j >= 0 && j < 3) {
                    const float gx = dw[i] * w[j], gy = w[i] * dw[j];
                    expected_x = -force_dt * (xx * gx + xy * gy);
                    expected_y = -force_dt * (xy * gx + yy * gy);
                }
                const int offset = (y * kTransferSize + x) * 2;
                const float actual_x = static_cast<float>((px[offset] + 4294967296.0 * px[offset + 1]) / 4096.0);
                const float actual_y = static_cast<float>((py[offset] + 4294967296.0 * py[offset + 1]) / 4096.0);
                Near(failures, actual_x, expected_x, 0.0005F, std::string(test.name) + " stress impulse x");
                Near(failures, actual_y, expected_y, 0.0005F, std::string(test.name) + " stress impulse y");
                resultant_x += actual_x;
                resultant_y += actual_y;
                torque += (x - 9.0F) * actual_y - (y - 9.0F) * actual_x;
                outward += (x - 9.0F) * actual_x + (y - 9.0F) * actual_y;
            }
        }
        Near(failures, resultant_x, 0.0F, 0.0011F, "internal impulse resultant x");
        Near(failures, resultant_y, 0.0F, 0.0011F, "internal impulse resultant y");
        Near(failures, torque, 0.0F, 0.002F, "internal impulse torque");
        if (test.a < 0.9F && test.b < 0.9F && outward <= 0.0F) failures << " [compression has no restoring impulse]";
        std::cout << "stress " << test.name << ": outward=" << outward << " net=" << resultant_x << ',' << resultant_y
                  << " torque=" << torque << '\n';
    }
    const std::array<StrainCase, 8> projection_cases{{
        {"identity", 1.0F, 1.0F, 0.0F, 0.0F}, {"rigid rotation", 1.0F, 1.0F, 0.7F, 0.0F},
        {"isotropic compression", 0.8F, 0.8F, 0.3F, 0.1F},
        {"below yield", std::exp(-0.1F), std::exp(-0.15F), 0.5F, 0.2F},
        {"tension release", std::exp(0.1F), std::exp(0.2F), 0.4F, 0.2F},
        {"yielding shear", std::exp(0.1F), std::exp(-0.5F), 0.0F, 0.0F},
        {"rotated yielding shear", std::exp(0.1F), std::exp(-0.5F), 0.6F, 0.25F},
        {"near singular shear", 0.001F, 0.8F, 0.4F, 0.2F}}};
    for (const auto& test : projection_cases) {
        SandFixture fixture;
        upload(fixture, deformation(test.a, test.b, test.left, test.right));
        fixture.Step(0.0F, 1000.0F);
        fixture.Read();
        MaterialParticle result{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(result), result.data());
        float a = std::log(test.a), b = std::log(test.b);
        const float trace = a + b;
        const float radius = std::abs(a - b) / std::sqrt(2.0F);
        const float limit = -(lambda + mu) / mu * alpha * trace;
        if (trace > 0.0F) {
            a = b = 0.0F;
        } else if (radius > limit && radius > 0.0F) {
            const float scale = limit / radius;
            a = 0.5F * trace + (a - 0.5F * trace) * scale;
            b = 0.5F * trace + (b - 0.5F * trace) * scale;
        }
        const auto expected = deformation(std::exp(a), std::exp(b), test.left, test.right);
        for (int i = 0; i < 4; ++i) Near(failures, result[12 + i], expected[i], 0.00002F, std::string(test.name) + " plastic projection");
        const float determinant = result[12] * result[15] - result[13] * result[14];
        Near(failures, determinant, trace > 0.0F ? 1.0F : test.a * test.b, 0.00002F, "plastic volume preservation");
        std::cout << "projection " << test.name << ": determinant=" << determinant << '\n';
    }
    SandFixture deforming;
    for (const auto& rotation : std::array<std::array<float, 2>, 2>{{{1.0F, 0.0F}, {0.0F, 1.0F}}}) {
        for (const auto& stretch : std::array<std::array<float, 2>, 2>{{{1.125F, 1.0F}, {0.875F, 0.875F}}}) {
            SandFixture frictionless;
            frictionless.friction_alpha = 0.0F;
            const std::array<float, 4> orientation{rotation[0], rotation[1], -rotation[1], rotation[0]};
            std::array<float, 4> initial{};
            for (int i = 0; i < 4; ++i) initial[i] = stretch[0] * orientation[i];
            upload(frictionless, initial);
            frictionless.Step(0.0F, 1000.0F);
            frictionless.Read();
            MaterialParticle result{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, frictionless.particles);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(result), result.data());
            for (int i = 0; i < 4; ++i)
                Near(failures, result[12 + i], stretch[1] * orientation[i], 0.00002F,
                     stretch[0] > 1.0F ? "frictionless isotropic tension release" : "frictionless isotropic compression");
            std::cout << "frictionless isotropic projection: rotation=" << rotation[0] << ',' << rotation[1]
                      << " stretch=" << stretch[0] << " determinant="
                      << result[12] * result[15] - result[13] * result[14] << '\n';
        }
    }
    upload(deforming, {0.8F, 0.0F, 0.0F, 0.8F});
    std::array<float, kTransferCells * 2> field{};
    for (int y = 0; y < kTransferSize; ++y) {
        for (int x = 0; x < kTransferSize; ++x) {
            const float dx = x - 9.0F, dy = y - 9.0F;
            field[(y * kTransferSize + x) * 2] = -0.25F * dx + 0.125F * dy;
            field[(y * kTransferSize + x) * 2 + 1] = 0.125F * dx - 0.5F * dy;
        }
    }
    glBindTexture(GL_TEXTURE_2D, deforming.velocity);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTransferSize, kTransferSize, GL_RG, GL_FLOAT, field.data());
    deforming.Step(1.0F / 64.0F, 1000.0F);
    deforming.Read();
    MaterialParticle deformed{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, deforming.particles);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(deformed), deformed.data());
    const std::array<float, 4> expected_f{0.796875F, 0.0015625F, 0.0015625F, 0.79375F};
    for (int i = 0; i < 4; ++i) Near(failures, deformed[12 + i], expected_f[i], 0.00002F, "velocity-gradient deformation");

    for (float normal_velocity : {-4.0F, 4.0F}) {
        SandFixture contact;
        contact.wall_friction = 0.5F;
        contact.Upload({8.5F, 0.5F, 3.0F, normal_velocity, 1.0F, 1.0F});
        contact.Scatter();
        contact.Grid(1.0F / 60.0F, 1.0F);
        std::array<float, kTransferCells * 2> velocity{};
        glBindTexture(GL_TEXTURE_2D, contact.velocity);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, velocity.data());
        const int floor = (kTransferSize + 9) * 2;
        Near(failures, velocity[floor], normal_velocity < 0.0F ? 1.0F : 3.0F, 0.002F, "Coulomb wall tangent");
        Near(failures, velocity[floor + 1], std::max(normal_velocity, 0.0F), 0.002F, "separating wall normal");
        contact.Read();
    }
    for (float angle : {0.0F, 0.7F, 1.2F}) {
        for (float tangent_speed : {0.9F, 1.1F}) {
            SandFixture contact;
            contact.wall_friction = 0.5F;
            const float nx = std::cos(angle), ny = std::sin(angle);
            const float vx = -ny * tangent_speed - 2.0F * nx;
            const float vy = nx * tangent_speed - 2.0F * ny;
            contact.Upload({8.5F, 8.5F, vx, vy, 1.0F, 1.0F});
            std::array<float, kCells> plane{};
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x) plane[y * kSize + x] = (x - 8.0F) * nx + (y - 8.0F) * ny - 0.25F;
            glBindTexture(GL_TEXTURE_2D, contact.sdf);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSize, kSize, GL_RED, GL_FLOAT, plane.data());
            contact.Scatter();
            std::array<GLuint, kTransferCells> mass{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, contact.mass);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(mass), mass.data());
            Near(failures, mass[9 * kTransferSize + 9] / 4096.0F, 0.5625F, 0.00001F, "solid support mass retained");
            contact.Grid(1.0F / 60.0F, 1.0F);
            std::array<float, kTransferCells * 2> velocity{};
            glBindTexture(GL_TEXTURE_2D, contact.velocity);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, velocity.data());
            const int node = (9 * kTransferSize + 9) * 2;
            const float expected_tangent = std::max(tangent_speed - 1.0F, 0.0F);
            Near(failures, velocity[node], -ny * expected_tangent, 0.002F, "rotated friction threshold x");
            Near(failures, velocity[node + 1], nx * expected_tangent, 0.002F, "rotated friction threshold y");
            contact.Read();
        }
    }
    SandFixture dense;
    dense.particle_count = 262144;
    const Particle dense_particle{8.5F, 8.5F, 72.0F, -72.0F, 1.0F, 1.0F,
                                 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    const std::vector<Particle> dense_particles(dense.particle_count, dense_particle);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dense.particles);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(dense_particles.size() * sizeof(Particle)),
                 dense_particles.data(), GL_DYNAMIC_READ);
    dense.Scatter();
    const int center = 9 * kTransferSize + 9;
    GLuint center_mass = 0;
    std::array<GLint, 2> px{}, py{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dense.mass);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, center * sizeof(GLuint), sizeof(center_mass), &center_mass);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dense.momentum_x);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, center * sizeof(px), sizeof(px), px.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dense.momentum_y);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, center * sizeof(py), sizeof(py), py.data());
    const double vx = (px[0] + 4294967296.0 * px[1]) / center_mass;
    const double vy = (py[0] + 4294967296.0 * py[1]) / center_mass;
    Near(failures, static_cast<float>(vx), 72.0F, 0.00001F, "dense positive momentum carry");
    Near(failures, static_cast<float>(vy), -72.0F, 0.00001F, "dense negative momentum carry");
    std::cout << "dense sand momentum: velocity=" << vx << ',' << vy << " mass=" << center_mass / 4096.0 << '\n';
    dense.Read();
    Require(failures.str().empty(), "sand material mismatch:" + failures.str());
}

void Pile() {
    using MaterialParticle = std::array<float, 16>;
    const powder_config::SimulationConfig config;
    const auto make = [](float x, float y, float stretch = 1.0F) {
        return MaterialParticle{x, y, 0.0F, 0.0F, 1.0F, 0.5F, 0.0F, 0.0F,
                                0.0F, 0.0F, 0.25F, 0.0F, stretch, 0.0F, 0.0F, stretch};
    };
    const auto upload = [&](SandFixture& fixture, const std::vector<MaterialParticle>& particles) {
        fixture.particle_count = static_cast<int>(particles.size());
        fixture.shear_modulus = config.sand_shear_modulus;
        fixture.lame_lambda = config.sand_lame_lambda;
        fixture.friction_alpha = config.sand_friction_alpha;
        fixture.wall_friction = config.sand_friction_coefficient;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.particles);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(particles.size() * sizeof(MaterialParticle)),
                     particles.data(), GL_DYNAMIC_READ);
    };
    const auto read = [](const SandFixture& fixture) {
        std::vector<MaterialParticle> particles(fixture.particle_count);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, fixture.particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                          static_cast<GLsizeiptr>(particles.size() * sizeof(MaterialParticle)), particles.data());
        for (const auto& particle : particles) {
            Require(std::all_of(particle.begin(), particle.end(), [](float x) { return std::isfinite(x); }),
                    "material patch has nonfinite particle state");
            Require(particle[4] > 0.5F && particle[5] == 0.5F, "material patch lost particle mass");
            Require(particle[12] * particle[15] - particle[13] * particle[14] > 0.0F,
                    "material patch inverted its deformation");
        }
        fixture.Read();
        return particles;
    };
    const auto evolve = [](SandFixture& fixture, int steps, float dt, float damping) {
        for (int step = 0; step < steps; ++step) {
            fixture.Scatter(dt);
            fixture.Grid(dt, damping);
            fixture.Step(dt, 100.0F);
        }
    };
    std::vector<MaterialParticle> rest;
    std::vector<MaterialParticle> compressed;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            rest.push_back(make(7.25F + 0.5F * x, 7.25F + 0.5F * y));
            compressed.push_back(make(7.4F + 0.4F * x, 7.4F + 0.4F * y, 0.8F));
        }
    }
    std::ostringstream failures;
    SandFixture resting;
    upload(resting, rest);
    evolve(resting, 50, 0.001F, 1.0F);
    const auto resting_result = read(resting);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        for (int component : {0, 1, 2, 3, 12, 13, 14, 15}) {
            Near(failures, resting_result[i][component], rest[i][component], 0.00001F, "stress-free patch rest");
        }
    }
    SandFixture releasing;
    upload(releasing, compressed);
    evolve(releasing, 100, 0.00025F, 1.0F);
    const auto released = read(releasing);
    float initial_radius = 0.0F, final_radius = 0.0F, center_x = 0.0F, center_y = 0.0F, determinant_sum = 0.0F;
    for (std::size_t i = 0; i < compressed.size(); ++i) {
        initial_radius += std::hypot(compressed[i][0] - 8.0F, compressed[i][1] - 8.0F);
        final_radius += std::hypot(released[i][0] - 8.0F, released[i][1] - 8.0F);
        center_x += released[i][0];
        center_y += released[i][1];
        determinant_sum += released[i][12] * released[i][15] - released[i][13] * released[i][14];
    }
    std::cout << "compression release: radius ratio=" << final_radius / initial_radius
              << " mean determinant=" << determinant_sum / released.size() << '\n';
    Require(final_radius > initial_radius * 1.1F, "compressed patch failed to restore volume");
    Require(determinant_sum / released.size() > 0.7F, "compressed patch retained excessive elastic compression");
    Near(failures, center_x / released.size(), 8.0F, 0.01F, "release center x");
    Near(failures, center_y / released.size(), 8.0F, 0.01F, "release center y");

    std::vector<MaterialParticle> column;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 8; ++x) column.push_back(make(6.25F + 0.5F * x, 0.75F + 0.5F * y));
    }
    std::array<float, 4> heights{};
    const std::array<float, 4> friction{config.sand_friction_alpha, config.sand_friction_alpha, 0.2F, 0.6F};
    for (int refinement = 0; refinement < 4; ++refinement) {
        const float dt = refinement == 0 ? 0.001F : 0.0005F;
        SandFixture pile;
        upload(pile, column);
        pile.friction_alpha = friction[refinement];
        pile.gravity = config.sand_gravity;
        evolve(pile, refinement == 0 ? 2000 : 4000, dt, config.sand_damping);
        const auto settled = read(pile);
        std::vector<float> y_values;
        float speed_squared = 0.0F, minimum_det = 1.0F, minimum_x = kSize, maximum_x = 0.0F;
        for (const auto& p : settled) {
            y_values.push_back(p[1]);
            speed_squared += p[2] * p[2] + p[3] * p[3];
            minimum_det = std::min(minimum_det, p[12] * p[15] - p[13] * p[14]);
            minimum_x = std::min(minimum_x, p[0]);
            maximum_x = std::max(maximum_x, p[0]);
            Require(p[0] >= 0.499F && p[0] <= 15.501F && p[1] >= 0.499F && p[1] <= 15.501F,
                    "pile escaped the closed domain");
        }
        std::sort(y_values.begin(), y_values.end());
        heights[refinement] = y_values[y_values.size() * 9 / 10] - 0.5F;
        const float rms_speed = std::sqrt(speed_squared / settled.size());
        pile.Scatter();
        std::array<GLuint, kTransferCells> mass{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pile.mass);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(mass), mass.data());
        const float peak_density = *std::max_element(mass.begin(), mass.end()) / 4096.0F;
        double total_mass = 0.0;
        for (GLuint value : mass) total_mass += value / 4096.0;
        std::cout << "pile dt=" << dt << " alpha=" << pile.friction_alpha << ": height90=" << heights[refinement] << " rms speed=" << rms_speed
                  << " min determinant=" << minimum_det << " peak density=" << peak_density
                  << " mass=" << total_mass << " max height=" << y_values.back() - 0.5F
                  << " width=" << maximum_x - minimum_x << '\n';
        if (refinement < 2 && heights[refinement] <= 2.5F) failures << " [pile height90 below 2.5 at dt=" << dt << ']';
        if (rms_speed >= 3.0F) failures << " [pile failed to settle at dt=" << dt << ']';
        if (minimum_det <= 0.4F || peak_density >= 6.0F) failures << " [pile retained excessive compression at dt=" << dt << ']';
        Near(failures, static_cast<float>(total_mass), 64.0F, 0.15F, "source-free pile grid mass");
    }
    Near(failures, heights[0], heights[1], 0.5F, "pile timestep refinement height");
    if (heights[3] <= heights[2] + 0.2F) failures << " [increased friction did not support a steeper pile]";
    Require(failures.str().empty(), "sand pile mismatch:" + failures.str());
}

void Collisions() {
    struct Case {
        const char* name;
        std::array<float, 2> position;
        std::array<float, 2> velocity;
        int mask;
        std::array<float, 2> expected_position;
        std::array<float, 2> expected_velocity;
        float restitution = 0.0F;
        int width = kSize;
    };
    const std::array<Case, 18> cases{{
        {"free motion", {6.5F, 6.5F}, {3.0F, -2.0F}, 0, {6.6F, 6.5F - 2.0F / 30.0F}, {3.0F, -2.0F}},
        {"right wall crossing", {6.5F, 6.5F}, {120.0F, 0.0F}, 1, {8.0F, 6.5F}, {0.0F, 0.0F}},
        {"left wall crossing", {10.5F, 6.5F}, {-120.0F, 0.0F}, 1, {9.0F, 6.5F}, {0.0F, 0.0F}},
        {"up wall crossing", {6.5F, 6.5F}, {0.0F, 120.0F}, 2, {6.5F, 8.0F}, {0.0F, 0.0F}},
        {"down wall crossing", {6.5F, 10.5F}, {0.0F, -120.0F}, 2, {6.5F, 9.0F}, {0.0F, 0.0F}},
        {"impact sliding", {6.5F, 6.5F}, {120.0F, 30.0F}, 1, {8.0F, 7.5F}, {0.0F, 30.0F}},
        {"normal restitution", {6.5F, 6.5F}, {120.0F, 30.0F}, 1, {8.0F, 7.5F}, {-30.0F, 30.0F}, 0.25F},
        {"rotated impact sliding", {6.5F, 6.5F}, {30.0F, 120.0F}, 2, {7.5F, 8.0F}, {30.0F, 0.0F}},
        {"parallel sliding", {7.75F, 6.5F}, {0.0F, 30.0F}, 1, {7.75F, 7.5F}, {0.0F, 30.0F}},
        {"second wall during slide", {6.5F, 6.5F}, {120.0F, 60.0F}, 3, {8.0F, 8.0F}, {0.0F, 0.0F}},
        {"diagonal cell corner", {6.5F, 6.5F}, {90.0F, 90.0F}, 4, {8.0F, 8.0F}, {0.0F, 0.0F}},
        {"domain right high speed", {6.5F, 6.5F}, {60000.0F, 0.0F}, 0, {15.5F, 6.5F}, {0.0F, 0.0F}},
        {"domain left high speed", {6.5F, 6.5F}, {-60000.0F, 0.0F}, 0, {0.5F, 6.5F}, {0.0F, 0.0F}},
        {"domain top corner", {6.5F, 6.5F}, {60000.0F, 60000.0F}, 0, {15.5F, 15.5F}, {0.0F, 0.0F}},
        {"domain bottom", {6.5F, 6.5F}, {0.0F, -60000.0F}, 0, {6.5F, 0.5F}, {0.0F, 0.0F}},
        {"starting inside", {8.5F, 6.5F}, {1.0F, 2.0F}, 1, {8.5F, 6.5F}, {0.0F, 0.0F}},
        {"long right sweep", {6.5F, 6.5F}, {60000.0F, 0.0F}, 5, {900.0F, 6.5F}, {0.0F, 0.0F}, 0.0F, 1000},
        {"long left sweep", {993.5F, 6.5F}, {-60000.0F, 0.0F}, 6, {101.0F, 6.5F}, {0.0F, 0.0F}, 0.0F, 1000},
    }};
    std::ostringstream failures;
    int checked = 0;
    for (bool water_particle : {false, true}) {
        for (const auto& test : cases) {
            const std::string label = std::string(water_particle ? "water " : "sand ") + test.name;
            try {
                SandFixture fixture;
                if (test.width != kSize) {
                    fixture.sdf = fixture.Texture(GL_R16F, GL_RED, 0.5F, test.width, kSize);
                    fixture.velocity = fixture.Texture(GL_RG16F, GL_RG, 0.0F, test.width + 2, kTransferSize);
                    fixture.water = fixture.Texture(GL_R16F, GL_RED, 0.0F, test.width, kSize);
                    fixture.water_velocity = fixture.Texture(GL_RG16F, GL_RG, 0.0F, test.width, kSize);
                }
                std::vector<float> boundary(test.width * kSize);
                std::vector<float> field((test.width + 2) * kTransferSize * 2);
                for (std::size_t i = 0; i < field.size(); i += 2) {
                    field[i] = test.velocity[0];
                    field[i + 1] = test.velocity[1];
                }
                for (int y = 0; y < kSize; ++y) {
                    for (int x = 0; x < test.width; ++x) {
                        const bool solid = (test.mask == 1 && x == 8) || (test.mask == 2 && y == 8) ||
                                           (test.mask == 3 && (x == 8 || y == 8)) ||
                                           (test.mask == 4 && x == 8 && y == 8) ||
                                           (test.mask == 5 && x == 900) || (test.mask == 6 && x == 100);
                        boundary[y * test.width + x] = solid ? -0.5F : 0.5F;
                    }
                }
                glBindTexture(GL_TEXTURE_2D, fixture.sdf);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, test.width, kSize, GL_RED, GL_FLOAT, boundary.data());
                fixture.Upload({test.position[0], test.position[1], test.velocity[0], test.velocity[1], 1.0F, 1.0F});
                if (water_particle) {
                    std::vector<GLubyte> mask(boundary.size());
                    std::transform(boundary.begin(), boundary.end(), mask.begin(), [](float value) {
                        return static_cast<GLubyte>(value <= 0.0F);
                    });
                    GLuint solid = 0;
                    glGenTextures(1, &solid);
                    fixture.textures.push_back(solid);
                    glBindTexture(GL_TEXTURE_2D, solid);
                    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, test.width, kSize);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, test.width, kSize, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                    const GLuint u = fixture.Texture(GL_R16F, GL_RED, test.velocity[0], test.width + 1, kSize);
                    const GLuint v = fixture.Texture(GL_R16F, GL_RED, test.velocity[1], test.width, kSize + 1);
                    const GLuint program = fixture.Program("water_particle_step.comp");
                    glUseProgram(program);
                    glUniform2i(glGetUniformLocation(program, "gridSize"), test.width, kSize);
                    glUniform1f(glGetUniformLocation(program, "dt"), 1.0F / 30.0F);
                    glUniform1f(glGetUniformLocation(program, "velocityDamping"), 1.0F);
                    glUniform1f(glGetUniformLocation(program, "maxVelocity"), 1000000.0F);
                    glUniform1f(glGetUniformLocation(program, "flipBlend"), 1.0F);
                    glUniform1i(glGetUniformLocation(program, "maxParticles"), 1);
                    Image(0, solid, GL_READ_ONLY, GL_R8UI);
                    Image(1, u, GL_READ_ONLY, GL_R16F);
                    Image(2, v, GL_READ_ONLY, GL_R16F);
                    Image(3, u, GL_READ_ONLY, GL_R16F);
                    Image(4, v, GL_READ_ONLY, GL_R16F);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, fixture.particles);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, fixture.counters);
                    glDispatchCompute(1, 1, 1);
                    Barrier();
                } else {
                    glBindTexture(GL_TEXTURE_2D, fixture.velocity);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, test.width + 2, kTransferSize, GL_RG, GL_FLOAT, field.data());
                    fixture.Step(1.0F / 30.0F, 1000000.0F, 1.0F, test.restitution, test.width);
                }
                const auto result = fixture.Read();
                std::cout << label << ": position=" << result[0] << ',' << result[1]
                          << " velocity=" << result[2] << ',' << result[3] << " live=" << result[4] << '\n';
                for (int axis = 0; axis < 2; ++axis) {
                    Near(failures, result[axis], test.expected_position[axis], 0.002F, label + " position");
                    const float expected_velocity = water_particle && test.restitution > 0.0F && axis == 0
                                                        ? 0.0F : test.expected_velocity[axis];
                    Near(failures, result[axis + 2], expected_velocity, 0.002F, label + " velocity");
                }
                if (test.mask != 0 && std::string(test.name) != "starting inside") {
                    const int x = std::clamp(static_cast<int>(std::floor(result[0])), 0, test.width - 1);
                    const int y = std::clamp(static_cast<int>(std::floor(result[1])), 0, kSize - 1);
                    Require(boundary[y * test.width + x] > 0.0F, "exterior particle finished inside a solid cell");
                }
                ++checked;
            } catch (const std::exception& error) {
                failures << " [" << label << ": " << error.what() << ']';
            }
        }
    }
    std::cout << "collision cases checked=" << checked << '/' << cases.size() * 2 << '\n';
    Require(failures.str().empty(), "particle collision mismatch:" + failures.str());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    const std::array<std::pair<const char*, void (*)()>, 6> tests{{
        {"transfer", Transfer}, {"integration", Integration}, {"support", Support}, {"material", Material},
        {"pile", Pile}, {"collisions", Collisions}}};
    if (argc > 2 || (selected != "all" && std::none_of(tests.begin(), tests.end(), [&](const auto& test) {
                        return selected == test.first;
                    }))) {
        std::cerr << "Usage: powder_sand_tests [all|transfer|integration|support|material|pile|collisions]\n";
        return 2;
    }
    GLFWwindow* window = nullptr;
    int failures = 0;
    try {
        Require(glfwInit() == GLFW_TRUE, "GLFW initialization failed");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(32, 32, "Sand GPU regressions", nullptr, nullptr);
        Require(window != nullptr, "could not create hidden OpenGL 4.3 context");
        glfwMakeContextCurrent(window);
        Require(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0 && GLAD_GL_VERSION_4_3 != 0,
                "OpenGL 4.3 is required");
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const auto& [name, test] : tests) {
            if (selected != "all" && selected != name) continue;
            try {
                CheckGl();
                test();
                CheckGl();
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
    if (window != nullptr) glfwDestroyWindow(window);
    glfwTerminate();
    return failures == 0 ? 0 : 1;
}

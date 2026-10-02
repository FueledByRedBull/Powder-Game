#include "GLUtil.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const std::filesystem::path kShaderRoot{POWDER_SHADER_DIR};

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void CheckGl(const std::string& operation) {
    std::ostringstream errors;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        errors << " 0x" << std::hex << error;
    }
    Require(errors.str().empty(), operation + ": OpenGL errors" + errors.str());
}

struct Resources {
    std::vector<GLuint> textures;
    std::vector<GLuint> buffers;
    std::vector<GLuint> programs;

    ~Resources() {
        glUseProgram(0);
        for (GLuint program : programs) {
            glDeleteProgram(program);
        }
        glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }

    GLuint Program(const char* filename) {
        const GLuint program = glutil::CreateComputeProgramFromFile((kShaderRoot / filename).string());
        programs.push_back(program);
        CheckGl(std::string("compile ") + filename);
        return program;
    }

    GLuint Texture(int width, int height, GLenum internal_format, GLenum format, GLenum type,
                   const void* data = nullptr) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, internal_format, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (data != nullptr) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
        }
        CheckGl("create test texture");
        return texture;
    }

    GLuint Buffer(GLuint binding, GLsizeiptr bytes, const void* data) {
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        buffers.push_back(buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, data, GL_DYNAMIC_READ);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer);
        CheckGl("create test buffer");
        return buffer;
    }
};

void BindImage(GLuint binding, GLuint texture, GLenum access, GLenum format) {
    glBindImageTexture(binding, texture, 0, GL_FALSE, 0, access, format);
}

void Dispatch(int width, int height) {
    glDispatchCompute(static_cast<GLuint>((width + 15) / 16), static_cast<GLuint>((height + 15) / 16), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    CheckGl("dispatch test shader");
}

std::vector<float> ReadScalar(GLuint texture, int width, int height) {
    std::vector<float> values(static_cast<std::size_t>(width * height));
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, values.data());
    CheckGl("read scalar texture");
    return values;
}

std::vector<float> ReadVector(GLuint texture, int width, int height) {
    std::vector<float> values(static_cast<std::size_t>(width * height * 2));
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, values.data());
    CheckGl("read vector texture");
    return values;
}

void HalfStorage() {
    constexpr int size = 4;
    const std::array<const char*, 4> shaders{{"water_mac_build.comp", "water_divergence.comp",
                                           "water_project.comp", "sand_grid_update.comp"}};
    std::ostringstream failures;
    int checks = 0;
    for (int stage = 0; stage < 4; ++stage) {
        for (float value : {65504.0F, -65504.0F, 65536.0F, -65536.0F,
                            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            if (stage == 0 && !std::isfinite(value)) continue; // Integer input cannot represent NaN/Inf.
            Resources resources;
            const std::array<unsigned char, size * size> mask{};
            const std::array<float, size * size> zero{};
            const std::array<float, size * size * 2> zero_vector{};
            const std::array<GLuint, 4> no_errors{};
            const GLuint counters = resources.Buffer(7, sizeof(no_errors), no_errors.data());
            const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            const GLuint phi = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, zero.data());
            const GLuint program = resources.Program(shaders[stage]);
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
            GLuint result = 0, second_result = 0;
            int width = size, height = size, index = size + 1;
            GLuint invalid_count = 1;
            bool vector_result = false;
            if (stage == 0) {
                const std::vector<GLuint> weights(size * (size + 1), 4096U);
                const std::vector<std::array<GLint, 2>> momenta(weights.size(),
                    {static_cast<GLint>(value * 4096.0F), 0});
                resources.Buffer(0, static_cast<GLsizeiptr>(weights.size() * sizeof(GLuint)), weights.data());
                resources.Buffer(1, static_cast<GLsizeiptr>(weights.size() * sizeof(GLuint)), weights.data());
                resources.Buffer(2, static_cast<GLsizeiptr>(momenta.size() * sizeof(momenta[0])), momenta.data());
                resources.Buffer(3, static_cast<GLsizeiptr>(momenta.size() * sizeof(momenta[0])), momenta.data());
                glUniform1f(glGetUniformLocation(program, "weightScale"), 4096.0F);
                glUniform1f(glGetUniformLocation(program, "velocityScale"), 4096.0F);
                result = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
                second_result = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
                BindImage(0, result, GL_WRITE_ONLY, GL_R16F);
                BindImage(1, second_result, GL_WRITE_ONLY, GL_R16F);
                BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
                width = size + 1;
                index = width + 1;
                invalid_count = 2 * size * (size - 1);
                Dispatch(size + 1, size + 1);
            } else if (stage == 1) {
                std::array<float, size * (size + 1)> u{}, v{};
                u[size + 1 + 1] = -value * 0.5F;
                u[size + 1 + 2] = value * 0.5F;
                std::array<float, size * size> liquid{};
                liquid.fill(1.0F);
                liquid[index] = 0.0F;
                const GLuint u_source = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u.data());
                const GLuint v_source = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v.data());
                const GLuint liquid_phi = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, liquid.data());
                result = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
                glUniform1f(glGetUniformLocation(program, "liquidPhiThreshold"), 0.42F);
                glUniform1f(glGetUniformLocation(program, "gravityDelta"), 0.0F);
                BindImage(0, u_source, GL_READ_ONLY, GL_R16F);
                BindImage(1, v_source, GL_READ_ONLY, GL_R16F);
                BindImage(2, result, GL_WRITE_ONLY, GL_R16F);
                BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
                BindImage(4, liquid_phi, GL_READ_ONLY, GL_R16F);
                Dispatch(size, size);
            } else if (stage == 2) {
                std::array<float, size * (size + 1)> u{}, v{};
                v.fill(32.0F);
                std::array<float, size * size> pressure{};
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x) pressure[y * size + x] = -value * x;
                const GLuint u_source = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u.data());
                const GLuint v_source = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v.data());
                const GLuint pressure_source = resources.Texture(size, size, GL_R32F, GL_RED, GL_FLOAT, pressure.data());
                result = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
                second_result = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
                glUniform1f(glGetUniformLocation(program, "liquidPhiThreshold"), 0.42F);
                glUniform1f(glGetUniformLocation(program, "gravityDelta"), value - 32.0F);
                BindImage(0, u_source, GL_READ_ONLY, GL_R16F);
                BindImage(1, v_source, GL_READ_ONLY, GL_R16F);
                BindImage(2, pressure_source, GL_READ_ONLY, GL_R32F);
                BindImage(3, result, GL_WRITE_ONLY, GL_R16F);
                BindImage(4, second_result, GL_WRITE_ONLY, GL_R16F);
                BindImage(5, boundary, GL_READ_ONLY, GL_R8UI);
                BindImage(6, phi, GL_READ_ONLY, GL_R16F);
                width = size + 1;
                index = width + 1;
                invalid_count = 2 * size * (size - 1);
                Dispatch(size + 1, size + 1);
            } else {
                std::vector<GLuint> mass((size + 2) * (size + 2), 0U);
                mass[2 * (size + 2) + 2] = 4096U;
                const float raw = std::isfinite(value) ? value : 0.0F;
                const std::vector<std::array<GLint, 2>> momentum(mass.size(), {static_cast<GLint>(raw * 4096.0F), 0});
                resources.Buffer(0, static_cast<GLsizeiptr>(mass.size() * sizeof(GLuint)), mass.data());
                resources.Buffer(1, static_cast<GLsizeiptr>(momentum.size() * sizeof(momentum[0])), momentum.data());
                resources.Buffer(2, static_cast<GLsizeiptr>(momentum.size() * sizeof(momentum[0])), momentum.data());
                std::array<float, size * size> distance{};
                distance.fill(1.0F);
                const GLuint sdf = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, distance.data());
                const GLuint water = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, zero.data());
                const GLuint water_velocity = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, zero_vector.data());
                result = resources.Texture(size + 2, size + 2, GL_RG16F, GL_RG, GL_FLOAT);
                glUniform1f(glGetUniformLocation(program, "dt"), std::isfinite(value) ? 0.0F : 1.0F);
                glUniform1f(glGetUniformLocation(program, "gravity"), std::isfinite(value) ? 0.0F : -value);
                glUniform1f(glGetUniformLocation(program, "damping"), 1.0F);
                glUniform1f(glGetUniformLocation(program, "drag"), 0.0F);
                glUniform1f(glGetUniformLocation(program, "wallFriction"), 0.0F);
                glUniform1f(glGetUniformLocation(program, "massScale"), 4096.0F);
                glUniform1f(glGetUniformLocation(program, "velocityScale"), 4096.0F);
                BindImage(0, sdf, GL_READ_ONLY, GL_R16F);
                BindImage(1, water, GL_READ_ONLY, GL_R16F);
                BindImage(2, water_velocity, GL_READ_ONLY, GL_RG16F);
                BindImage(3, result, GL_WRITE_ONLY, GL_RG16F);
                width = height = size + 2;
                index = 2 * width + 2;
                vector_result = true;
                invalid_count = 1;
                Dispatch(width, height);
            }
            const bool invalid = !std::isfinite(value) || std::abs(value) > 65504.0F;
            const float expected = invalid ? 0.0F : value;
            const auto actual = vector_result ? ReadVector(result, width, height) : ReadScalar(result, width, height);
            bool values_ok = actual[index * (vector_result ? 2 : 1)] == expected;
            if (vector_result) values_ok = values_ok && actual[index * 2 + 1] == expected;
            if (second_result != 0) values_ok = values_ok && ReadScalar(second_result, size, size + 1)[size + 1] == expected;
            std::array<GLuint, 4> errors{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, counters);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(errors), errors.data());
            const std::array<GLuint, 4> expected_errors{{invalid ? invalid_count : 0U, 0U, 0U, 0U}};
            std::cout << "half storage " << shaders[stage] << " value=" << value << " stored="
                      << actual[index * (vector_result ? 2 : 1)] << " invalid=" << errors[0] << '\n';
            if (!values_ok || errors != expected_errors)
                failures << " [" << shaders[stage] << " value=" << value << " did not preserve valid output or diagnose/zero invalid output]";
            ++checks;
        }
    }
    Require(checks == 22 && failures.str().empty(), failures.str());
}

void WaterTransfer() {
    constexpr int width = 4;
    constexpr int height = 4;
    struct TransferCase {
        float weight_scale;
        float velocity_scale;
        GLuint weight;
        GLint momentum_x;
        GLint momentum_y;
        float velocity_x;
        float velocity_y;
    };
    // One unit weight at velocity (8,8); two unit weights at (-3,5) or (6,-8).
    const std::array<TransferCase, 3> cases{{
        {4096.0F, 4096.0F, 4096U, 32768, 32768, 8.0F, 8.0F},
        {1024.0F, 4096.0F, 2048U, -24576, 40960, -3.0F, 5.0F},
        {4096.0F, 1024.0F, 8192U, 12288, -16384, 6.0F, -8.0F},
    }};
    for (const auto& test : cases) {
        Resources resources;
        const std::array<unsigned char, width * height> empty{};
        const GLuint boundary = resources.Texture(width, height, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
        const GLuint u = resources.Texture(width + 1, height, GL_R16F, GL_RED, GL_FLOAT);
        const GLuint v = resources.Texture(width, height + 1, GL_R16F, GL_RED, GL_FLOAT);
        const std::vector<GLuint> weights(20, test.weight);
        const std::vector<std::array<GLint, 2>> u_momenta(20, {test.momentum_x, 0});
        const std::vector<std::array<GLint, 2>> v_momenta(20, {test.momentum_y, 0});
        resources.Buffer(0, static_cast<GLsizeiptr>(weights.size() * sizeof(GLuint)), weights.data());
        resources.Buffer(1, static_cast<GLsizeiptr>(weights.size() * sizeof(GLuint)), weights.data());
        resources.Buffer(2, static_cast<GLsizeiptr>(u_momenta.size() * sizeof(u_momenta[0])), u_momenta.data());
        resources.Buffer(3, static_cast<GLsizeiptr>(v_momenta.size() * sizeof(v_momenta[0])), v_momenta.data());
        const std::array<GLuint, 4> no_errors{};
        const GLuint debug = resources.Buffer(7, sizeof(no_errors), no_errors.data());

        GLuint program = resources.Program("water_mac_build.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), width, height);
        glUniform1f(glGetUniformLocation(program, "velocityScale"), test.velocity_scale);
        glUniform1f(glGetUniformLocation(program, "weightScale"), test.weight_scale);
        glUniform1f(glGetUniformLocation(program, "dt"), 1.0F / 60.0F);
        glUniform1f(glGetUniformLocation(program, "gravity"), 0.0F);
        BindImage(0, u, GL_WRITE_ONLY, GL_R16F);
        BindImage(1, v, GL_WRITE_ONLY, GL_R16F);
        BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug);
        Dispatch(width + 1, height + 1);

        const std::string context = " [weight scale " + std::to_string(test.weight_scale) +
                                    ", momentum scale " + std::to_string(test.velocity_scale) + "]";
        const auto check_velocity = [&](float actual, float expected, const std::string& location) {
            // These integer velocities and power-of-two scales are exactly representable in R16F.
            Require(actual == expected, location + context + ": expected " +
                                            std::to_string(expected) + ", got " + std::to_string(actual));
        };
        const auto u_values = ReadScalar(u, width + 1, height);
        const auto v_values = ReadScalar(v, width, height + 1);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x <= width; ++x) {
                const float expected = x == 0 || x == width ? 0.0F : test.velocity_x;
                check_velocity(u_values[static_cast<std::size_t>(y * (width + 1) + x)], expected,
                               "U face (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
        for (int y = 0; y <= height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float expected = y == 0 || y == height ? 0.0F : test.velocity_y;
                check_velocity(v_values[static_cast<std::size_t>(y * width + x)], expected,
                               "V face (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
        std::array<GLuint, 4> counters{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        Require(counters == no_errors, "finite MAC transfer must not report invalid output");
    }
}

void WaterP2G() {
    constexpr int size = 8;
    const std::array<unsigned char, size * size> empty{};
    const std::array<GLuint, size * (size + 1)> zero_faces{};
    const std::array<GLint, size * (size + 1) * 2> zero_face_momentum{};
    const std::array<GLuint, size * size> zero_cells{};
    const std::array<GLuint, 4> no_errors{};
    std::ostringstream failures;
    for (int scenario = 0; scenario < 4; ++scenario) {
        Resources resources;
        const int count = scenario < 2 ? (scenario == 0 ? 1 : 2) : 262144;
        std::vector<std::array<float, 12>> particles(static_cast<std::size_t>(count));
        double expected_mass = 0.0;
        double momentum_x = 0.0;
        double momentum_y = 0.0;
        for (int i = 0; i < count; ++i) {
            const float mass = scenario == 1 && i == 1 ? 3.0F : 1.0F;
            const float vx = scenario < 2 ? (i == 0 ? 12.0F : -4.0F)
                                           : (scenario == 3 && i % 2 ? -72.0F : 72.0F);
            const float vy = scenario < 2 ? -4.0F : -72.0F;
            particles[static_cast<std::size_t>(i)] = {4.5F, 4.5F, vx, vy, 1.0F, mass};
            expected_mass += mass;
            momentum_x += mass * vx;
            momentum_y += mass * vy;
        }
        const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
        resources.Buffer(0, static_cast<GLsizeiptr>(particles.size() * sizeof(particles[0])), particles.data());
        const GLuint u_weights = resources.Buffer(1, sizeof(zero_faces), zero_faces.data());
        const GLuint v_weights = resources.Buffer(2, sizeof(zero_faces), zero_faces.data());
        const GLuint u_momentum = resources.Buffer(3, sizeof(zero_face_momentum), zero_face_momentum.data());
        const GLuint v_momentum = resources.Buffer(4, sizeof(zero_face_momentum), zero_face_momentum.data());
        const GLuint counts = resources.Buffer(5, sizeof(zero_cells), zero_cells.data());
        const GLuint debug = resources.Buffer(7, sizeof(no_errors), no_errors.data());
        GLuint program = resources.Program("water_particle_p2g.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
        glUniform1i(glGetUniformLocation(program, "maxParticles"), count);
        glUniform1f(glGetUniformLocation(program, "weightScale"), 4096.0F);
        glUniform1f(glGetUniformLocation(program, "velocityScale"), 4096.0F);
        BindImage(0, boundary, GL_READ_ONLY, GL_R8UI);
        glDispatchCompute(static_cast<GLuint>((count + 255) / 256), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        const GLuint u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
        const GLuint v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
        program = resources.Program("water_mac_build.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
        glUniform1f(glGetUniformLocation(program, "weightScale"), 4096.0F);
        glUniform1f(glGetUniformLocation(program, "velocityScale"), 4096.0F);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, u_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, v_weights);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, u_momentum);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, v_momentum);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debug);
        BindImage(0, u, GL_WRITE_ONLY, GL_R16F);
        BindImage(1, v, GL_WRITE_ONLY, GL_R16F);
        BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(size + 1, size + 1);
        const auto u_values = ReadScalar(u, size + 1, size);
        const auto v_values = ReadScalar(v, size, size + 1);

        std::array<GLuint, size * size> actual_counts{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, counts);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(actual_counts), actual_counts.data());
        double actual_mass = 0.0;
        for (GLuint count_value : actual_counts) actual_mass += count_value / 4096.0;
        const auto exact_face = [&](GLuint buffer, std::size_t index) {
            std::array<GLint, 2> words{};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(index * sizeof(words)), sizeof(words), words.data());
            return static_cast<std::int64_t>(words[0]) + static_cast<std::int64_t>(words[1]) * (std::int64_t{1} << 32);
        };
        std::array<GLuint, 4> counters{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        const float expected_x = static_cast<float>(momentum_x / expected_mass);
        const float expected_y = static_cast<float>(momentum_y / expected_mass);
        const auto matches = [](float actual, float expected) {
            // The final image is R16F; allow one representable half-float step.
            const float ulp = expected == 0.0F ? 0.0001F :
                std::max(0.0001F, std::ldexp(1.0F, std::ilogb(std::abs(expected)) - 10));
            return std::isfinite(actual) && std::abs(actual - expected) <= ulp;
        };
        std::cout << "  water P2G case " << scenario << " mass=" << actual_mass << '/' << expected_mass
                  << " face velocity=" << u_values[4 * (size + 1) + 4] << ',' << v_values[4 * size + 4] << '\n';
        if (actual_mass != expected_mass || counters != no_errors ||
            exact_face(u_momentum, 4 * (size + 1) + 4) != static_cast<std::int64_t>(momentum_x * 1536.0) ||
            exact_face(v_momentum, 4 * size + 4) != static_cast<std::int64_t>(momentum_y * 1536.0) ||
            !matches(u_values[4 * (size + 1) + 4], expected_x) || !matches(v_values[4 * size + 4], expected_y)) {
            failures << " [case " << scenario << " mass or momentum transfer failed]";
        }
        CheckGl("water mass/dense transfer");
    }
    Require(failures.str().empty(), failures.str());
}

void WaterAffineTransfer() {
    constexpr int width = 16;
    constexpr int height = 16;
    constexpr float py = 7.3F;
    for (int scenario = 0; scenario < 4; ++scenario) {
        const float px = scenario == 3 ? 0.5F : 8.2F;
        Resources resources;
        const std::vector<unsigned char> empty(width * height, 0);
        const GLuint boundary = resources.Texture(width, height, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
        std::vector<float> u((width + 1) * height);
        std::vector<float> v(width * (height + 1));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x <= width; ++x) {
                u[y * (width + 1) + x] = scenario != 1 ? 0.125F * x * x
                    : 1.0F + 0.5F * x + 0.25F * (y + 0.5F);
            }
        }
        for (int y = 0; y <= height; ++y) {
            for (int x = 0; x < width; ++x) {
                v[y * width + x] = scenario != 1 ? -0.125F * y * y
                    : -2.0F - 0.25F * (x + 0.5F) + 0.5F * y;
            }
        }
        if (scenario == 3) {
            std::fill(u.begin(), u.end(), 0.0F);
            std::fill(v.begin(), v.end(), 1.0F);
        }
        const GLuint current_u = resources.Texture(width + 1, height, GL_R16F, GL_RED, GL_FLOAT, u.data());
        const GLuint current_v = resources.Texture(width, height + 1, GL_R16F, GL_RED, GL_FLOAT, v.data());
        for (float& value : u) value -= 2.0F;
        for (float& value : v) value += 3.0F;
        const GLuint previous_u = resources.Texture(width + 1, height, GL_R16F, GL_RED, GL_FLOAT, u.data());
        const GLuint previous_v = resources.Texture(width, height + 1, GL_R16F, GL_RED, GL_FLOAT, v.data());
        std::array<float, 12> particle{px, py, 7.0F, 8.0F, 1.0F, 1.0F};
        const GLuint particles = resources.Buffer(0, sizeof(particle), particle.data());
        const std::array<GLuint, 4> zero_counters{};
        const GLuint counters = resources.Buffer(1, sizeof(zero_counters), zero_counters.data());
        const GLuint program = resources.Program("water_particle_step.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), width, height);
        glUniform1i(glGetUniformLocation(program, "maxParticles"), 1);
        glUniform1f(glGetUniformLocation(program, "dt"), scenario == 1 ? 0.1F : 0.0F);
        glUniform1f(glGetUniformLocation(program, "velocityDamping"), 1.0F);
        glUniform1f(glGetUniformLocation(program, "maxVelocity"), 1000.0F);
        glUniform1f(glGetUniformLocation(program, "flipBlend"), scenario == 2 ? 1.0F : 0.0F);
        BindImage(0, boundary, GL_READ_ONLY, GL_R8UI);
        BindImage(1, previous_u, GL_READ_ONLY, GL_R16F);
        BindImage(2, previous_v, GL_READ_ONLY, GL_R16F);
        BindImage(3, current_u, GL_READ_ONLY, GL_R16F);
        BindImage(4, current_v, GL_READ_ONLY, GL_R16F);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle), particle.data());
        std::array<GLuint, 4> debug{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, counters);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(debug), debug.data());
        CheckGl("water affine transfer readback");
        Require(particle[4] == 1.0F && debug == zero_counters, "affine transfer must retain the particle without invalid counters");
        const auto near = [&](float actual, float expected, const char* quantity) {
            Require(std::isfinite(actual) && std::abs(actual - expected) < 0.0001F,
                    "water affine case " + std::to_string(scenario) + " " + quantity + ": expected " +
                        std::to_string(expected) + ", got " + std::to_string(actual));
        };
        if (scenario == 0) {
            // The complete quadratic B-spline stencil has variance h^2/4.
            near(particle[2], 0.125F * (px * px + 0.25F), "quadratic U gather");
            near(particle[3], -0.125F * (py * py + 0.25F), "quadratic V gather");
        } else if (scenario == 1) {
            const float u0 = 1.0F + 0.5F * px + 0.25F * py;
            const float v0 = -2.0F - 0.25F * px + 0.5F * py;
            const float mx = px + 0.05F * u0;
            const float my = py + 0.05F * v0;
            near(particle[2], u0, "linear U gather");
            near(particle[3], v0, "linear V gather");
            near(particle[6], 0.5F, "du/dx");
            near(particle[7], -0.25F, "dv/dx");
            near(particle[8], 0.25F, "du/dy");
            near(particle[9], 0.5F, "dv/dy");
            near(particle[0], px + 0.1F * (1.0F + 0.5F * mx + 0.25F * my), "RK2 x");
            near(particle[1], py + 0.1F * (-2.0F - 0.25F * mx + 0.5F * my), "RK2 y");
        } else if (scenario == 2) {
            near(particle[2], 9.0F, "FLIP U increment");
            near(particle[3], 5.0F, "FLIP V increment");
        } else {
            near(particle[2], 0.0F, "wall-normal velocity");
            near(particle[3], 1.0F, "wall-tangential velocity");
            for (int component = 6; component < 10; ++component)
                near(particle[component], 0.0F, "constant-field affine component");
        }
    }
}


void BoundaryMask() {
    Resources resources;
    constexpr int size = 9;
    std::array<unsigned char, size * size> occupancy{};
    occupancy[40] = 1;
    const GLuint source = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, occupancy.data());
    GLuint seeds = resources.Texture(size, size, GL_RG32I, GL_RG_INTEGER, GL_INT);
    GLuint scratch = resources.Texture(size, size, GL_RG32I, GL_RG_INTEGER, GL_INT);
    const GLuint sdf = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
    const GLuint result = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE);

    GLuint program = resources.Program("boundary_seed.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    BindImage(0, source, GL_READ_ONLY, GL_R8UI);
    BindImage(1, seeds, GL_WRITE_ONLY, GL_RG32I);
    Dispatch(size, size);

    program = resources.Program("boundary_jumpflood.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    for (int step : {8, 4, 2, 1}) {
        glUniform1i(glGetUniformLocation(program, "stepSize"), step);
        BindImage(0, seeds, GL_READ_ONLY, GL_RG32I);
        BindImage(1, scratch, GL_WRITE_ONLY, GL_RG32I);
        Dispatch(size, size);
        std::swap(seeds, scratch);
    }

    program = resources.Program("boundary_distance.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1f(glGetUniformLocation(program, "maxDistance"), 16.0F);
    BindImage(0, source, GL_READ_ONLY, GL_R8UI);
    BindImage(1, seeds, GL_READ_ONLY, GL_RG32I);
    BindImage(2, sdf, GL_WRITE_ONLY, GL_R16F);
    Dispatch(size, size);

    program = resources.Program("boundary_mask.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1f(glGetUniformLocation(program, "solidThreshold"), 0.0F);
    BindImage(0, sdf, GL_READ_ONLY, GL_R16F);
    BindImage(1, result, GL_WRITE_ONLY, GL_R8UI);
    Dispatch(size, size);

    std::array<unsigned char, size * size> actual{};
    glBindTexture(GL_TEXTURE_2D, result);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, actual.data());
    CheckGl("read reconstructed boundary mask");
    const auto blocked = std::count_if(actual.begin(), actual.end(), [](unsigned char value) { return value != 0; });
    Require(actual == occupancy, "one isolated solid must remain exactly one blocked cell; got " +
                                     std::to_string(blocked) + " blocked cells");
}

void WaterIntegration() {
    Resources resources;
    constexpr int size = 48;
    constexpr float px = 16.5F, py = 16.5F;
    std::vector<GLubyte> fluid(size * size, 0);
    const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, fluid.data());
    const GLuint u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
    const GLuint v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
    const GLuint particles = resources.Buffer(0, sizeof(float) * 12, nullptr);
    const std::array<GLuint, 4> zeros{};
    const GLuint debug = resources.Buffer(1, sizeof(zeros), zeros.data());
    const GLuint program = resources.Program("water_particle_step.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1i(glGetUniformLocation(program, "maxParticles"), 1);
    glUniform1f(glGetUniformLocation(program, "maxVelocity"), 72.0F);
    BindImage(0, boundary, GL_READ_ONLY, GL_R8UI);
    BindImage(1, u, GL_READ_ONLY, GL_R16F);
    BindImage(2, v, GL_READ_ONLY, GL_R16F);
    BindImage(3, u, GL_READ_ONLY, GL_R16F);
    BindImage(4, v, GL_READ_ONLY, GL_R16F);
    struct Timer {
        GLuint query = 0;
        Timer() { glGenQueries(1, &query); }
        ~Timer() { glDeleteQueries(1, &query); }
    } timer;
    std::ostringstream failures;
    int checks = 0;
    const auto near = [&](float actual, double expected, const std::string& label) {
        ++checks;
        if (!std::isfinite(actual) || std::abs(actual - expected) > 0.0002)
            failures << " [" << label << ": expected " << expected << ", got " << actual << ']';
    };
    const auto cap = [](double x, double y) {
        const double scale = std::min(1.0, 72.0 / std::max(1e-30, std::hypot(x, y)));
        return std::array<double, 2>{x * scale, y * scale};
    };
    const auto run = [&](float ux, float vy, bool turn, float initial_x, float initial_y,
                         float damping, float dt, int steps, float flip) {
        std::vector<float> values_u((size + 1) * size, ux), values_v(size * (size + 1));
        for (int y = 0; y <= size; ++y) for (int x = 0; x < size; ++x)
            values_v[y * size + x] = turn ? 120.0F * (x + 0.5F - px) : vy;
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, u);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size + 1, size, GL_RED, GL_FLOAT, values_u.data());
        glBindTexture(GL_TEXTURE_2D, v);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size + 1, GL_RED, GL_FLOAT, values_v.data());
        std::array<float, 12> particle{px, py, initial_x, initial_y, 1.0F, 1.0F};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle), particle.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros.data());
        glUniform1f(glGetUniformLocation(program, "dt"), dt);
        glUniform1f(glGetUniformLocation(program, "velocityDamping"), damping);
        glUniform1f(glGetUniformLocation(program, "flipBlend"), flip);
        glBeginQuery(GL_TIME_ELAPSED, timer.query);
        for (int step = 0; step < steps; ++step) {
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        glEndQuery(GL_TIME_ELAPSED);
        GLuint64 elapsed = 0;
        glGetQueryObjectui64v(timer.query, GL_QUERY_RESULT, &elapsed);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle), particle.data());
        std::array<GLuint, 4> counters{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        Require(counters == zeros && particle[4] == 1.0F && particle[5] == 1.0F,
                "water integration must preserve live mass without invalid counters");
        std::cout << "water integration dt=" << dt << " steps=" << steps << " GPU us=" << elapsed / 1000.0 << '\n';
        return particle;
    };
    constexpr float dt = 1.0F / 120.0F;
    for (const auto& flow : std::array<std::array<float, 2>, 7>{{
             {144, 0}, {-144, 0}, {0, 144}, {0, -144}, {144, 144}, {-144, 144}, {-36, 12}}}) {
        const auto actual = run(flow[0], flow[1], false, flow[0], flow[1], 1.0F, dt, 1, 0.0F);
        const auto expected = cap(flow[0], flow[1]);
        near(actual[0], px + dt * expected[0], "capped x displacement");
        near(actual[1], py + dt * expected[1], "capped y displacement");
        near(actual[2], expected[0], "capped stored x velocity");
        near(actual[3], expected[1], "capped stored y velocity");
    }
    for (float sign : {-1.0F, 1.0F}) {
        const auto actual = run(sign * 144.0F, 0.0F, true, sign * 144.0F, 0.0F, 1.0F, dt, 1, 0.0F);
        const auto expected = cap(sign * 144.0, sign * 36.0);
        near(actual[0], px + dt * expected[0], "capped midpoint x displacement");
        near(actual[1], py + dt * expected[1], "capped midpoint y displacement");
    }
    for (int rate : {60, 120, 240}) {
        const auto actual = run(0, 0, false, 12.0F, -5.0F, 0.992F, 1.0F / rate, rate / 2, 1.0F);
        const double factor = std::pow(static_cast<double>(0.992F), 60.0);
        near(actual[2], 12.0 * factor, "equal-duration x damping at " + std::to_string(rate));
        near(actual[3], -5.0 * factor, "equal-duration y damping at " + std::to_string(rate));
        near(actual[0], px, "zero-grid transport x");
        near(actual[1], py, "zero-grid transport y");
    }
    // A tangential edge impulse samples exactly at its last face row/column, as at the mirrored first edge.
    for (int axis = 0; axis < 2; ++axis) for (bool upper : {false, true}) {
        std::vector<float> values_u((size + 1) * size, 0.0F), values_v(size * (size + 1), 0.0F);
        const int edge = upper ? size - 1 : 0;
        if (axis == 0) {
            for (int x = 0; x <= size; ++x) values_u[edge * (size + 1) + x] = 1.0F;
        } else {
            for (int y = 0; y <= size; ++y) values_v[y * size + edge] = 1.0F;
        }
        const float start_x = axis == 0 ? px : edge + 0.5F;
        const float start_y = axis == 0 ? edge + 0.5F : py;
        std::array<float, 12> particle{start_x, start_y, axis == 0 ? 1.0F : 0.0F,
                                      axis == 1 ? 1.0F : 0.0F, 1.0F, 1.0F};
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, u);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size + 1, size, GL_RED, GL_FLOAT, values_u.data());
        glBindTexture(GL_TEXTURE_2D, v);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size + 1, GL_RED, GL_FLOAT, values_v.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle), particle.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros.data());
        glUniform1f(glGetUniformLocation(program, "dt"), 0.5F);
        glUniform1f(glGetUniformLocation(program, "velocityDamping"), 1.0F);
        glUniform1f(glGetUniformLocation(program, "flipBlend"), 1.0F);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle), particle.data());
        std::array<GLuint, 4> counters{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        Require(counters == zeros && particle[4] == 1.0F && particle[5] == 1.0F,
                "water edge impulse must preserve live mass and finite state");
        const std::string edge_name = std::string(axis == 0 ? "U row " : "V column ") + (upper ? "last" : "first");
        near(particle[0], start_x + (axis == 0 ? 0.5 : 0.0), edge_name + " RK2 x");
        near(particle[1], start_y + (axis == 1 ? 0.5 : 0.0), edge_name + " RK2 y");
        std::cout << "water edge impulse " << edge_name << " displacement="
                  << particle[axis] - (axis == 0 ? start_x : start_y) << '\n';
    }
    CheckGl("water integration");
    std::cout << "water integration checks=" << checks << '\n';
    Require(failures.str().empty(), "water transport/timestep mismatch:" + failures.str());
}

void WaterWallPressure() {
    Resources resources;
    const std::array<GLuint, 4> no_storage_errors{};
    const GLuint storage_debug = resources.Buffer(7, sizeof(no_storage_errors), no_storage_errors.data());
    constexpr int size = 4;
    constexpr int liquid_index = size + 1;
    constexpr float phi_threshold = 0.42F;
    std::array<unsigned char, size * size> walls;
    walls.fill(1);
    walls[liquid_index] = 0;
    walls[liquid_index + 1] = 0;
    std::array<float, size * size> phi;
    phi.fill(1.0F);
    phi[liquid_index] = 0.0F;
    const std::array<float, size * size> zeros{};
    const GLuint cg_program = resources.Program("pressure_cg.comp");
    const GLuint cg_reduce = resources.Program("pressure_cg_reduce.comp");
    const GLuint cg_vectors = resources.Buffer(0, sizeof(float) * 4 * size * size, nullptr);
    const GLuint cg_partials = resources.Buffer(1, sizeof(float) * 8, nullptr);
    const GLuint cg_scalars = resources.Buffer(2, sizeof(float) * 4 + sizeof(GLuint) * 8, nullptr);
    const GLuint cg_debug = resources.Buffer(3, sizeof(GLuint) * 4, nullptr);
    const auto solve = [&](GLuint rhs, GLuint boundary, GLuint phi, GLuint pressure) {
        const GLuint zero = 0;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, cg_debug);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cg_vectors);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, cg_partials);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, cg_scalars);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cg_debug);
        BindImage(0, rhs, GL_READ_ONLY, GL_R16F);
        BindImage(1, pressure, GL_READ_WRITE, GL_R32F);
        BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
        BindImage(3, phi, GL_READ_ONLY, GL_R16F);
        glUseProgram(cg_program);
        glUniform2i(glGetUniformLocation(cg_program, "gridSize"), size, size);
        glUniform2i(glGetUniformLocation(cg_program, "solveOrigin"), 0, 0);
        glUniform2i(glGetUniformLocation(cg_program, "solveSize"), size, size);
        glUniform1f(glGetUniformLocation(cg_program, "liquidPhiThreshold"), phi_threshold);
        glUseProgram(cg_reduce);
        glUniform2i(glGetUniformLocation(cg_reduce, "gridSize"), size, size);
        glUniform2i(glGetUniformLocation(cg_reduce, "solveOrigin"), 0, 0);
        glUniform2i(glGetUniformLocation(cg_reduce, "solveSize"), size, size);
        glUniform1i(glGetUniformLocation(cg_reduce, "cellCount"), size * size);
        glUniform1i(glGetUniformLocation(cg_reduce, "partialCount"), 1);
        glUniform1f(glGetUniformLocation(cg_reduce, "residualThreshold"), 0.002F);
        const auto dispatch = [&](GLuint program, int phase) {
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "phase"), phase);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        };
        dispatch(cg_program, 0);
        dispatch(cg_reduce, 0);
        dispatch(cg_reduce, 1);
        dispatch(cg_program, 5);
        dispatch(cg_reduce, 7);
        for (int iteration = 0; iteration < 32; ++iteration) {
            dispatch(cg_program, 1);
            dispatch(cg_reduce, 3);
            dispatch(cg_program, 2);
            dispatch(cg_program, 4);
            dispatch(cg_reduce, 5);
            dispatch(cg_program, 3);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        std::array<GLuint, 4> counters{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, cg_debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        return counters;
    };
    std::array<float, (size + 1) * size> u_values{};
    const std::array<float, size * (size + 1)> v_values{};
    // Liquid (1,1) has three solid neighbors and air at (2,1), with unit outward flux.
    u_values[(size + 1) + 2] = 1.0F;
    const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, walls.data());
    const GLuint liquid_phi = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, phi.data());
    const GLuint u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u_values.data());
    const GLuint v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v_values.data());
    const GLuint projected_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
    const GLuint projected_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
    const GLuint divergence = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
    GLuint pressure = resources.Texture(size, size, GL_R32F, GL_RED, GL_FLOAT, zeros.data());

    const GLuint divergence_program = resources.Program("water_divergence.comp");
    const auto compute_divergence = [&](GLuint u_faces, GLuint v_faces) {
        glUseProgram(divergence_program);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, storage_debug);
        glUniform2i(glGetUniformLocation(divergence_program, "gridSize"), size, size);
        glUniform1f(glGetUniformLocation(divergence_program, "liquidPhiThreshold"), phi_threshold);
        BindImage(0, u_faces, GL_READ_ONLY, GL_R16F);
        BindImage(1, v_faces, GL_READ_ONLY, GL_R16F);
        BindImage(2, divergence, GL_WRITE_ONLY, GL_R16F);
        BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
        BindImage(4, liquid_phi, GL_READ_ONLY, GL_R16F);
        Dispatch(size, size);
    };
    compute_divergence(u, v);
    const float initial_divergence = ReadScalar(divergence, size, size)[liquid_index];
    Require(std::isfinite(initial_divergence) && std::abs(initial_divergence - 1.0F) < 0.001F,
            "wall-pressure fixture must start with unit divergence; got " + std::to_string(initial_divergence));

    Require(solve(divergence, boundary, liquid_phi, pressure) == std::array<GLuint, 4>{},
            "free-surface pressure solve must finish without breakdown");

    GLuint program = resources.Program("water_project.comp");
    glUseProgram(program);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, storage_debug);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1f(glGetUniformLocation(program, "liquidPhiThreshold"), phi_threshold);
    BindImage(0, u, GL_READ_ONLY, GL_R16F);
    BindImage(1, v, GL_READ_ONLY, GL_R16F);
    BindImage(2, pressure, GL_READ_ONLY, GL_R32F);
    BindImage(3, projected_u, GL_WRITE_ONLY, GL_R16F);
    BindImage(4, projected_v, GL_WRITE_ONLY, GL_R16F);
    BindImage(5, boundary, GL_READ_ONLY, GL_R8UI);
    BindImage(6, liquid_phi, GL_READ_ONLY, GL_R16F);
    Dispatch(size + 1, size + 1);

    const auto actual_u = ReadScalar(projected_u, size + 1, size);
    const auto actual_v = ReadScalar(projected_v, size, size + 1);
    for (float wall_flux : {actual_u[(size + 1) + 1], actual_v[size + 1], actual_v[2 * size + 1]}) {
        Require(std::isfinite(wall_flux) && std::abs(wall_flux) < 0.001F,
                "projection must leave zero flux through the three solid faces; got " + std::to_string(wall_flux));
    }
    compute_divergence(projected_u, projected_v);
    const float final_divergence = ReadScalar(divergence, size, size)[liquid_index];
    Require(std::isfinite(final_divergence) && std::abs(final_divergence) < 0.001F,
            "one liquid cell next to three walls must project to zero divergence; got " +
                std::to_string(final_divergence));
    std::array<GLuint, 4> storage_errors{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, storage_debug);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(storage_errors), storage_errors.data());
    Require(storage_errors == no_storage_errors, "finite wall projection must not report storage-range errors");

    // This closed chamber has compatible, zero-sum divergence and no air pressure anchor.
    walls.fill(1);
    phi.fill(1.0F);
    std::array<float, size * size> chamber_rhs{};
    for (int y = 1; y <= 2; ++y) {
        for (int x = 1; x <= 2; ++x) {
            const auto cell = static_cast<std::size_t>(y * size + x);
            walls[cell] = 0;
            phi[cell] = 0.0F;
            chamber_rhs[cell] = (x + y) % 2 == 0 ? 1.0F : -1.0F;
        }
    }
    const GLuint chamber_boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, walls.data());
    const GLuint chamber_phi = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, phi.data());
    const GLuint chamber_divergence = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, chamber_rhs.data());
    pressure = resources.Texture(size, size, GL_R32F, GL_RED, GL_FLOAT, zeros.data());
    Require(solve(chamber_divergence, chamber_boundary, chamber_phi, pressure) == std::array<GLuint, 4>{},
            "compatible closed chamber must converge without a pressure anchor");
    const auto chamber_pressure = ReadScalar(pressure, size, size);
    float maximum_residual = 0.0F;
    for (int y = 1; y <= 2; ++y) {
        for (int x = 1; x <= 2; ++x) {
            const auto cell = static_cast<std::size_t>(y * size + x);
            // Each chamber cell has exactly two fluid neighbors; the other two faces are solid.
            const float laplacian = chamber_pressure[static_cast<std::size_t>(y * size + (3 - x))] +
                                    chamber_pressure[static_cast<std::size_t>((3 - y) * size + x)] -
                                    2.0F * chamber_pressure[cell];
            const float residual = std::abs(chamber_rhs[cell] - laplacian);
            Require(std::isfinite(residual), "closed chamber pressure residual must be finite");
            maximum_residual = std::max(maximum_residual, residual);
        }
    }
    std::array<GLuint, 4> counters{};
    const GLuint debug = resources.Buffer(0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
    program = resources.Program("pressure_residual.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1i(glGetUniformLocation(program, "boundaryScale"), 1);
    glUniform1f(glGetUniformLocation(program, "residualThreshold"), 0.002F);
    glUniform1i(glGetUniformLocation(program, "useLiquidMask"), 1);
    glUniform1f(glGetUniformLocation(program, "liquidPhiThreshold"), phi_threshold);
    BindImage(0, chamber_divergence, GL_READ_ONLY, GL_R16F);
    glUniform1i(glGetUniformLocation(program, "pressureTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pressure);
    BindImage(2, chamber_boundary, GL_READ_ONLY, GL_R8UI);
    BindImage(3, chamber_phi, GL_READ_ONLY, GL_R16F);
    Dispatch(size, size);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
    CheckGl("read closed chamber residual counters");
    std::cout << "  closed chamber residual=" << maximum_residual << ", nonconverged=" << counters[2] << '\n';
    Require(maximum_residual < 0.002F && counters == std::array<GLuint, 4>{},
            "closed liquid chamber must damp checkerboard pressure: residual " + std::to_string(maximum_residual) +
                ", nonconverged cells " + std::to_string(counters[2]));

    glBindTexture(GL_TEXTURE_2D, chamber_divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, zeros.data());
    Require(solve(chamber_divergence, chamber_boundary, chamber_phi, pressure) == std::array<GLuint, 4>{},
            "zero residual must stop without dividing by zero");
    const auto zero_rhs_pressure = ReadScalar(pressure, size, size);
    float zero_rhs_maximum = 0.0F;
    for (float value : zero_rhs_pressure) zero_rhs_maximum = std::max(zero_rhs_maximum, std::abs(value));
    std::cout << "  warm zero RHS pressure maximum=" << zero_rhs_maximum << '\n';
    for (int y = 1; y <= 2; ++y) for (int x = 1; x <= 2; ++x) {
        const float residual = zero_rhs_pressure[y * size + (3 - x)] +
                               zero_rhs_pressure[(3 - y) * size + x] - 2.0F * zero_rhs_pressure[y * size + x];
        Require(std::isfinite(residual) && std::abs(residual) < 0.002F,
                "warm pressure must solve the changed zero RHS at the original residual threshold");
    }
    glBindTexture(GL_TEXTURE_2D, pressure);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, zeros.data());
    Require(solve(chamber_divergence, chamber_boundary, chamber_phi, pressure) == std::array<GLuint, 4>{},
            "cold zero RHS must stop without dividing by zero");
    for (float value : ReadScalar(pressure, size, size)) Require(value == 0.0F, "cold zero RHS must leave exact zero pressure");
    chamber_rhs.fill(1.0F);
    glBindTexture(GL_TEXTURE_2D, chamber_divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, chamber_rhs.data());
    const auto breakdown = solve(chamber_divergence, chamber_boundary, chamber_phi, pressure);
    Require(breakdown[0] == 0 && breakdown[2] > 0, "incompatible closed RHS must report breakdown without NaNs");
    for (float value : ReadScalar(pressure, size, size)) Require(std::isfinite(value), "breakdown must keep pressure finite");

    walls.fill(1);
    walls[liquid_index] = 0;
    glBindTexture(GL_TEXTURE_2D, chamber_boundary);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED_INTEGER, GL_UNSIGNED_BYTE, walls.data());
    glBindTexture(GL_TEXTURE_2D, chamber_divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, zeros.data());
    Require(solve(chamber_divergence, chamber_boundary, chamber_phi, pressure) == std::array<GLuint, 4>{},
            "isolated zero-diagonal liquid with zero RHS must stop safely");
    glBindTexture(GL_TEXTURE_2D, chamber_divergence);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, chamber_rhs.data());
    const auto isolated = solve(chamber_divergence, chamber_boundary, chamber_phi, pressure);
    Require(isolated[0] == 0 && isolated[2] > 0, "isolated nonzero RHS must report nonconvergence without NaNs");
    for (float value : ReadScalar(pressure, size, size)) Require(value == 0.0F, "zero-diagonal solve must leave zero pressure");

    phi.fill(1.0F);
    glBindTexture(GL_TEXTURE_2D, chamber_phi);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, phi.data());
    Require(solve(chamber_divergence, chamber_boundary, chamber_phi, pressure) == std::array<GLuint, 4>{},
            "all-air domain must not attempt a pressure solve");
    std::array<GLuint, 8> solver{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, cg_scalars);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(solver), solver.data());
    Require(solver[4] == 0 && solver[5] == 0 && solver[6] == 0, "all-air domain must perform zero PCG iterations");
}

void GasPressure() {
    int failures = 0;
    const auto run = [&](const char* name, const auto& check) {
        try {
            check();
            std::cout << "  PASS gas " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "  FAIL gas " << name << ": " << error.what() << '\n';
        }
    };
    const auto expect = [](float actual, float expected, const std::string& label) {
        // Allows R16F coarse-RHS rounding; the tested operator errors are much larger.
        Require(std::isfinite(actual) && std::abs(actual - expected) < 0.004F,
                label + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    };
    const auto relax_once = [](int width, int height, int scale, int mask_width, int mask_height,
                               const std::vector<unsigned char>& mask, const std::vector<float>& rhs,
                               const std::vector<float>& pressure) {
        Resources resources;
        const GLuint boundary = resources.Texture(mask_width, mask_height, GL_R8UI, GL_RED_INTEGER,
                                                  GL_UNSIGNED_BYTE, mask.data());
        const GLuint rhs_texture = resources.Texture(width, height, GL_R16F, GL_RED, GL_FLOAT, rhs.data());
        const GLuint source = resources.Texture(width, height, GL_R32F, GL_RED, GL_FLOAT, pressure.data());
        const GLuint output = resources.Texture(width, height, GL_R32F, GL_RED, GL_FLOAT);
        const GLuint program = resources.Program("gas_pressure_relax.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), width, height);
        glUniform1i(glGetUniformLocation(program, "boundaryScale"), scale);
        BindImage(0, rhs_texture, GL_READ_ONLY, GL_R16F);
        BindImage(1, source, GL_READ_ONLY, GL_R32F);
        BindImage(2, output, GL_WRITE_ONLY, GL_R32F);
        BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(width, height);
        return ReadScalar(output, width, height);
    };
    const auto restrict_rhs = [](int width, int height, int scale, const std::vector<unsigned char>& mask,
                                 const std::vector<float>& rhs, const std::vector<float>& pressure) {
        Resources resources;
        const int coarse_width = (width + 1) / 2;
        const int coarse_height = (height + 1) / 2;
        const GLuint boundary = resources.Texture(width * scale, height * scale, GL_R8UI, GL_RED_INTEGER,
                                                  GL_UNSIGNED_BYTE, mask.data());
        const GLuint rhs_texture = resources.Texture(width, height, GL_R16F, GL_RED, GL_FLOAT, rhs.data());
        const GLuint source = resources.Texture(width, height, GL_R32F, GL_RED, GL_FLOAT, pressure.data());
        const GLuint output = resources.Texture(coarse_width, coarse_height, GL_R16F, GL_RED, GL_FLOAT);
        const GLuint program = resources.Program("gas_pressure_restrict.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "fineSize"), width, height);
        glUniform2i(glGetUniformLocation(program, "coarseSize"), coarse_width, coarse_height);
        glUniform1i(glGetUniformLocation(program, "boundaryScale"), scale);
        BindImage(0, rhs_texture, GL_READ_ONLY, GL_R16F);
        BindImage(1, source, GL_READ_ONLY, GL_R32F);
        BindImage(2, output, GL_WRITE_ONLY, GL_R16F);
        BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(coarse_width, coarse_height);
        return ReadScalar(output, coarse_width, coarse_height);
    };

    run("solid face diagonal", [&] {
        std::vector<unsigned char> mask(16, 1);
        mask[5] = mask[6] = 0;
        std::vector<float> rhs(16, 0.0F);
        rhs[5] = 1.0F;
        rhs[6] = -1.0F;
        const auto output = relax_once(4, 4, 1, 4, 4, mask, rhs, std::vector<float>(16, 0.0F));
        // Each fluid cell has one open face: omega=2/3 relaxes toward pressure -rhs.
        expect(output[5], -2.0F / 3.0F, "left pressure");
        expect(output[6], 2.0F / 3.0F, "right pressure");
        expect(output[0], 0.0F, "solid pressure");
    });
    run("outside face diagonal", [&] {
        const auto output = relax_once(2, 1, 1, 2, 1, {0, 0}, {1.0F, -1.0F}, {0.0F, 0.0F});
        expect(output[0], -2.0F / 3.0F, "left domain-edge pressure");
        expect(output[1], 2.0F / 3.0F, "right domain-edge pressure");
    });
    for (const auto& [scale, expected] : {std::pair{2, -2.0F / 3.0F}, {4, -8.0F / 3.0F}}) {
        run(scale == 2 ? "spacing h=2" : "spacing h=4", [&] {
            std::vector<float> rhs(9, 0.0F);
            rhs[4] = 1.0F;
            const auto output = relax_once(3, 3, scale, 3 * scale, 3 * scale,
                                           std::vector<unsigned char>(9 * scale * scale, 0), rhs,
                                           std::vector<float>(9, 0.0F));
            // Four open faces, zero neighbors: relaxed pressure is -(2/3)*h*h/4.
            expect(output[4], expected, "interior coarse pressure");
        });
    }
    run("partial boundary footprints", [&] {
        std::vector<unsigned char> mask(25, 0);
        mask[0] = 1;
        const auto output = relax_once(3, 3, 2, 5, 5, mask, std::vector<float>(9, 0.0F),
                                       std::vector<float>(9, 1.0F));
        for (std::size_t cell : {2U, 5U, 6U, 7U, 8U}) {
            expect(output[cell], 0.0F, "partial footprint pressure " + std::to_string(cell));
        }
        expect(output[0], 0.0F, "solid-containing footprint pressure");
        expect(output[4], 1.0F, "constant pressure in complete fluid footprint");
    });
    run("restriction wall operator", [&] {
        std::vector<unsigned char> mask(16, 0);
        mask[5] = 1;
        const auto output = restrict_rhs(4, 4, 1, mask, std::vector<float>(16, 0.0F),
                                         std::vector<float>(16, 2.0F));
        // Constant pressure has zero normal derivative at every solid and outside face.
        for (float value : output) {
            expect(value, 0.0F, "restricted constant-pressure residual");
        }
    });
    run("restriction physical rhs", [&] {
        const auto output = restrict_rhs(4, 4, 1, std::vector<unsigned char>(16, 0),
                                         std::vector<float>(16, 1.0F), std::vector<float>(16, 0.0F));
        for (float value : output) {
            expect(value, 1.0F, "restricted physical residual");
        }
    });
    run("restriction spacing h=2", [&] {
        std::vector<float> pressure(36);
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 6; ++x) {
                pressure[static_cast<std::size_t>(y * 6 + x)] = static_cast<float>(x * x + y * y);
            }
        }
        const auto output = restrict_rhs(6, 6, 2, std::vector<unsigned char>(144, 0),
                                         std::vector<float>(36, 2.0F), pressure);
        // The center footprint is entirely interior: Lap(x*x+y*y)=4, so physical residual is 2-4/4=1.
        expect(output[4], 1.0F, "restricted coarse-spacing residual");
    });
    run("restriction blocked coarse cell", [&] {
        std::vector<unsigned char> mask(16, 0);
        mask[5] = 1;
        const auto output = restrict_rhs(4, 4, 1, mask, std::vector<float>(16, 1.0F),
                                         std::vector<float>(16, 0.0F));
        expect(output[0], 0.0F, "coarse cell with a solid child");
        for (std::size_t cell = 1; cell < output.size(); ++cell) {
            expect(output[cell], 1.0F, "complete fluid coarse cell");
        }
    });
    run("restriction odd extent", [&] {
        const auto output = restrict_rhs(3, 3, 1, std::vector<unsigned char>(9, 0),
                                         std::vector<float>(9, 1.0F), std::vector<float>(9, 0.0F));
        expect(output[0], 1.0F, "complete coarse footprint");
        for (std::size_t cell = 1; cell < output.size(); ++cell) {
            expect(output[cell], 0.0F, "partial coarse footprint");
        }
    });
    run("prolongation blocked donor", [&] {
        Resources resources;
        const std::array<float, 4> coarse_values{100.0F, 1.0F, 1.0F, 1.0F};
        std::array<float, 16> fine_values{};
        for (std::size_t cell : {0U, 1U, 4U, 5U}) {
            fine_values[cell] = 0.25F;
        }
        std::array<unsigned char, 16> mask{};
        mask[0] = 1;
        const GLuint boundary = resources.Texture(4, 4, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        const GLuint coarse = resources.Texture(2, 2, GL_R32F, GL_RED, GL_FLOAT, coarse_values.data());
        const GLuint fine = resources.Texture(4, 4, GL_R32F, GL_RED, GL_FLOAT, fine_values.data());
        const GLuint program = resources.Program("gas_pressure_prolongate.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "fineSize"), 4, 4);
        glUniform2i(glGetUniformLocation(program, "coarseSize"), 2, 2);
        glUniform1i(glGetUniformLocation(program, "boundaryScale"), 1);
        BindImage(0, coarse, GL_READ_ONLY, GL_R32F);
        BindImage(1, fine, GL_READ_WRITE, GL_R32F);
        BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(4, 4);
        const auto output = ReadScalar(fine, 4, 4);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const float expected = x == 0 && y == 0 ? 0.0F : (x < 2 && y < 2 ? 0.25F : 1.0F);
                expect(output[static_cast<std::size_t>(y * 4 + x)], expected,
                       "prolongated pressure (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
    });
    run("projection can exceed the trace component bound", [&] {
        Resources resources;
        constexpr int size = 3;
        const std::array<unsigned char, 9> mask{};
        const std::array<float, 12> u{0, 1, -1, 0, 0, 1, 1, 0, 0, -1, 1, 0};
        const std::array<float, 12> v{0, 0, 0, -1, 1, -1, -1, 1, -1, 0, 0, 0};
        // Exact zero-mean solution of Lap(p) = divergence for this closed chamber.
        const std::array<float, 9> pressure{0, 0.5F, 0.5F, -0.5F, 0, 0.5F, -0.5F, -0.5F, 0};
        const std::vector<float> expected_rhs{0, -1, 0, 1, 0, -1, 0, 1, 0};
        const std::vector<float> expected_u{0, 0.5F, -1, 0, 0, 0.5F, 0.5F, 0, 0, -1, 0.5F, 0};
        const std::vector<float> expected_v{0, 0, 0, -0.5F, 1.5F, -1, -1, 1.5F, -0.5F, 0, 0, 0};
        const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        const GLuint source_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u.data());
        const GLuint source_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v.data());
        const GLuint p = resources.Texture(size, size, GL_R32F, GL_RED, GL_FLOAT, pressure.data());
        const GLuint output_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
        const GLuint output_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
        const GLuint center = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
        const GLuint divergence = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
        std::array<GLuint, 4> counters{};
        const GLuint debug = resources.Buffer(7, sizeof(counters), counters.data());
        const GLuint divergence_program = resources.Program("smoke_divergence.comp");
        const auto measure_divergence = [&](GLuint input_u, GLuint input_v) {
            glUseProgram(divergence_program);
            glUniform2i(glGetUniformLocation(divergence_program, "gasSize"), size, size);
            BindImage(0, input_u, GL_READ_ONLY, GL_R16F);
            BindImage(1, input_v, GL_READ_ONLY, GL_R16F);
            BindImage(2, divergence, GL_WRITE_ONLY, GL_R16F);
            BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);
            return ReadScalar(divergence, size, size);
        };
        Require(measure_divergence(source_u, source_v) == expected_rhs, "closed chamber input divergence");
        GLuint program = resources.Program("smoke_project.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
        BindImage(0, source_u, GL_READ_ONLY, GL_R16F);
        BindImage(1, source_v, GL_READ_ONLY, GL_R16F);
        BindImage(2, p, GL_READ_ONLY, GL_R32F);
        BindImage(3, output_u, GL_WRITE_ONLY, GL_R16F);
        BindImage(4, output_v, GL_WRITE_ONLY, GL_R16F);
        BindImage(5, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(size + 1, size + 1);
        Require(ReadScalar(output_u, size + 1, size) == expected_u && ReadScalar(output_v, size, size + 1) == expected_v,
                "projection must retain the exact divergence-free MAC result without component clipping");
        Require(measure_divergence(output_u, output_v) == std::vector<float>(9, 0.0F), "projected divergence must be zero");
        program = resources.Program("gas_face_to_center.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
        BindImage(0, output_u, GL_READ_ONLY, GL_R16F);
        BindImage(1, output_v, GL_READ_ONLY, GL_R16F);
        BindImage(2, center, GL_WRITE_ONLY, GL_RG16F);
        BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
        Dispatch(size, size);
        const auto velocity = ReadVector(center, size, size);
        Require(velocity[8] == 0.5F && velocity[9] == 1.5F && std::hypot(velocity[8], velocity[9]) > std::sqrt(2.0F),
                "unit-bounded input faces must demonstrate projected center speed above sqrt(2)");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        Require(counters == std::array<GLuint, 4>{}, "exact projection must leave debug counters zero");
        CheckGl("closed chamber projection bound proof");
    });
    Require(failures == 0, std::to_string(failures) + " gas pressure fixtures failed");
}

void SmokeDissipation() {
    constexpr int size = 4;
    constexpr float initial = 0.8F;
    constexpr float initial_reaction = 0.625F;
    constexpr float density_dissipation = 0.5F;
    constexpr float scalar_dissipation = 0.75F;
    const std::array<const char*, 4> field_names{"smoke", "temperature", "fuel", "oxidizer"};
    for (int substeps : {1, 2, 3}) {
        Resources resources;
        std::array<float, size * size> constant;
        constant.fill(initial);
        const std::array<float, size * size * 2> zero_vectors{};
        std::array<float, size * size * 2> reaction_values{};
        for (std::size_t cell = 0; cell < reaction_values.size() / 2; ++cell) {
            reaction_values[cell * 2 + 1] = initial_reaction;
        }
        const std::array<unsigned char, size * size> empty{};
        const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
        const GLuint velocity = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, zero_vectors.data());
        std::array<GLuint, 4> source{};
        std::array<GLuint, 4> forward{};
        std::array<GLuint, 4> corrected{};
        for (std::size_t field = 0; field < source.size(); ++field) {
            source[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, constant.data());
            forward[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
            corrected[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
        }
        GLuint reaction_source = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reaction_values.data());
        const GLuint reaction_forward = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
        GLuint reaction_corrected = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
        const std::array<GLuint, 4> counters{};
        resources.Buffer(7, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
        const GLuint advect = resources.Program("smoke_advect.comp");
        const GLuint correct = resources.Program("smoke_correct.comp");
        const float dt = (1.0F / 60.0F) / static_cast<float>(substeps);
        const std::string context = " with " + std::to_string(substeps) + " substeps";

        for (int step = 0; step < substeps; ++step) {
            glUseProgram(advect);
            glUniform2i(glGetUniformLocation(advect, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(advect, "dt"), dt);
            glUniform1f(glGetUniformLocation(advect, "maxVelocity"), 20.0F);
            BindImage(0, velocity, GL_READ_ONLY, GL_RG16F);
            for (GLuint field = 0; field < source.size(); ++field) {
                BindImage(1 + field, source[field], GL_READ_ONLY, GL_R16F);
                BindImage(6 + field, forward[field], GL_WRITE_ONLY, GL_R16F);
            }
            BindImage(5, reaction_source, GL_READ_ONLY, GL_RG16F);
            BindImage(10, reaction_forward, GL_WRITE_ONLY, GL_RG16F);
            BindImage(11, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);
            for (std::size_t field = 0; field < source.size(); ++field) {
                Require(ReadScalar(forward[field], size, size) == ReadScalar(source[field], size, size),
                        std::string("pure forward transport changed ") + field_names[field] + context);
            }
            Require(ReadVector(reaction_forward, size, size) == ReadVector(reaction_source, size, size),
                    "pure forward transport changed reaction" + context);

            glUseProgram(correct);
            glUniform2i(glGetUniformLocation(correct, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(correct, "dt"), dt);
            glUniform1f(glGetUniformLocation(correct, "maxVelocity"), 20.0F);
            glUniform1f(glGetUniformLocation(correct, "densityDissipation"), std::pow(density_dissipation, dt * 60.0F));
            glUniform1f(glGetUniformLocation(correct, "scalarDissipation"), std::pow(scalar_dissipation, dt * 60.0F));
            BindImage(0, velocity, GL_READ_ONLY, GL_RG16F);
            BindImage(1, velocity, GL_READ_ONLY, GL_RG16F);
            for (GLuint field = 0; field < source.size(); ++field) {
                BindImage(2 + field, source[field], GL_READ_ONLY, GL_R16F);
                BindImage(7 + field, forward[field], GL_READ_ONLY, GL_R16F);
                BindImage(12 + field, corrected[field], GL_WRITE_ONLY, GL_R16F);
            }
            BindImage(6, reaction_source, GL_READ_ONLY, GL_RG16F);
            BindImage(11, reaction_forward, GL_READ_ONLY, GL_RG16F);
            BindImage(16, reaction_corrected, GL_WRITE_ONLY, GL_RG16F);
            BindImage(17, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);
            std::swap(source, corrected);
            std::swap(reaction_source, reaction_corrected);
        }

        // R16F spacing here is at most 1/2048; allow upload plus three rounded correction stores.
        constexpr float tolerance = 0.002F;
        // All runs cover 1/60 second, so their expected decay is independent of the substep count.
        for (std::size_t field = 0; field < source.size(); ++field) {
            const float expected = initial * (field == 0 ? density_dissipation : scalar_dissipation);
            for (float value : ReadScalar(source[field], size, size)) {
                Require(std::isfinite(value) && std::abs(value - expected) < tolerance,
                        std::string("corrected ") + field_names[field] + context + ": expected " +
                            std::to_string(expected) + ", got " + std::to_string(value));
            }
        }
        const auto reaction = ReadVector(reaction_source, size, size);
        for (std::size_t cell = 0; cell < reaction.size() / 2; ++cell) {
            Require(reaction[cell * 2] == 0.0F, "reserved reaction.x must remain zero" + context);
            const float value = reaction[cell * 2 + 1];
            const float expected = initial_reaction * scalar_dissipation;
            Require(std::isfinite(value) && std::abs(value - expected) < tolerance,
                    "corrected reaction.y" + context + ": expected " +
                        std::to_string(expected) + ", got " + std::to_string(value));
        }
    }
}

void GasVelocityDissipation() {
    constexpr int size = 12;
    constexpr int sample = size / 2;
    constexpr int u_index = sample * (size + 1) + sample;
    constexpr int v_index = sample * size + sample;
    constexpr float initial_u = 0.8F;
    constexpr float initial_v = -0.6F;
    int failures = 0;
    for (int scenario = 0; scenario < 4; ++scenario) {
        const float retention = std::array<float, 4>{0.0F, 0.5F, 1.0F, 0.0F}[scenario];
        const float buoyancy = scenario == 3 ? 6.0F : 0.0F;
        for (int substeps : {1, 2, 3}) {
            if (scenario == 3 && substeps != 1) {
                continue;
            }
            const std::string label = "retention=" + std::to_string(retention) +
                ", substeps=" + std::to_string(substeps) + ", buoyancy=" + std::to_string(buoyancy);
            try {
                Resources resources;
                std::array<unsigned char, size * size> mask{};
                mask[size + 1] = 1;
                const auto blocked = [&](int x, int y) {
                    return x < 0 || y < 0 || x >= size || y >= size || mask[y * size + x] != 0;
                };
                std::array<float, (size + 1) * size> u_values{};
                std::array<float, size * (size + 1)> v_values{};
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x <= size; ++x) {
                        u_values[y * (size + 1) + x] = blocked(x - 1, y) || blocked(x, y) ? 0.0F : initial_u;
                    }
                }
                for (int y = 0; y <= size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        v_values[y * size + x] = blocked(x, y - 1) || blocked(x, y) ? 0.0F : initial_v;
                    }
                }
                const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
                GLuint source_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u_values.data());
                GLuint source_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v_values.data());
                const GLuint forward_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
                const GLuint forward_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
                GLuint corrected_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
                GLuint corrected_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
                const GLuint source_center = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
                const GLuint forward_center = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
                const std::array<float, size * size> zeros{};
                std::array<float, size * size> ones;
                ones.fill(1.0F);
                const GLuint smoke = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, ones.data());
                const GLuint temperature = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, zeros.data());
                std::array<GLuint, 4> counters{};
                const GLuint debug = resources.Buffer(7, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
                const GLuint advect = resources.Program("gas_velocity_advect.comp");
                const GLuint correct = resources.Program("gas_velocity_correct.comp");
                const GLuint center = resources.Program("gas_face_to_center.comp");
                const auto reconstruct = [&](GLuint u, GLuint v, GLuint destination) {
                    glUseProgram(center);
                    glUniform2i(glGetUniformLocation(center, "gasSize"), size, size);
                    BindImage(0, u, GL_READ_ONLY, GL_R16F);
                    BindImage(1, v, GL_READ_ONLY, GL_R16F);
                    BindImage(2, destination, GL_WRITE_ONLY, GL_RG16F);
                    BindImage(3, boundary, GL_READ_ONLY, GL_R8UI);
                    Dispatch(size, size);
                };
                const float dt = (1.0F / 60.0F) / static_cast<float>(substeps);
                const float step_retention = std::pow(retention, dt * 60.0F);
                bool pure_transport = true;
                for (int step = 0; step < substeps; ++step) {
                    reconstruct(source_u, source_v, source_center);
                    glUseProgram(advect);
                    glUniform2i(glGetUniformLocation(advect, "gasSize"), size, size);
                    glUniform1f(glGetUniformLocation(advect, "dt"), dt);
                    glUniform1f(glGetUniformLocation(advect, "maxVelocity"), 20.0F);
                    BindImage(0, source_u, GL_READ_ONLY, GL_R16F);
                    BindImage(1, source_v, GL_READ_ONLY, GL_R16F);
                    BindImage(2, source_center, GL_READ_ONLY, GL_RG16F);
                    BindImage(3, forward_u, GL_WRITE_ONLY, GL_R16F);
                    BindImage(4, forward_v, GL_WRITE_ONLY, GL_R16F);
                    BindImage(5, boundary, GL_READ_ONLY, GL_R8UI);
                    Dispatch(size + 1, size + 1);
                    pure_transport = pure_transport &&
                        ReadScalar(forward_u, size + 1, size)[u_index] == ReadScalar(source_u, size + 1, size)[u_index] &&
                        ReadScalar(forward_v, size, size + 1)[v_index] == ReadScalar(source_v, size, size + 1)[v_index];
                    reconstruct(forward_u, forward_v, forward_center);
                    glUseProgram(correct);
                    glUniform2i(glGetUniformLocation(correct, "gasSize"), size, size);
                    glUniform1f(glGetUniformLocation(correct, "dt"), dt);
                    glUniform1f(glGetUniformLocation(correct, "velocityDissipation"), step_retention);
                    glUniform1f(glGetUniformLocation(correct, "buoyancyTemperature"), 0.0F);
                    glUniform1f(glGetUniformLocation(correct, "buoyancySmoke"), buoyancy);
                    glUniform1f(glGetUniformLocation(correct, "maxVelocity"), 20.0F);
                    BindImage(0, source_u, GL_READ_ONLY, GL_R16F);
                    BindImage(1, source_v, GL_READ_ONLY, GL_R16F);
                    BindImage(2, source_center, GL_READ_ONLY, GL_RG16F);
                    BindImage(3, forward_u, GL_READ_ONLY, GL_R16F);
                    BindImage(4, forward_v, GL_READ_ONLY, GL_R16F);
                    BindImage(5, forward_center, GL_READ_ONLY, GL_RG16F);
                    BindImage(6, smoke, GL_READ_ONLY, GL_R16F);
                    BindImage(7, temperature, GL_READ_ONLY, GL_R16F);
                    BindImage(8, corrected_u, GL_WRITE_ONLY, GL_R16F);
                    BindImage(9, corrected_v, GL_WRITE_ONLY, GL_R16F);
                    BindImage(10, boundary, GL_READ_ONLY, GL_R8UI);
                    Dispatch(size + 1, size + 1);
                    std::swap(source_u, corrected_u);
                    std::swap(source_v, corrected_v);
                }
                const auto actual_u = ReadScalar(source_u, size + 1, size);
                const auto actual_v = ReadScalar(source_v, size, size + 1);
                const float expected_u = initial_u * retention;
                const float expected_v = initial_v * retention - buoyancy / 60.0F;
                std::cout << "  " << label << " -> u=" << actual_u[u_index] << ", v=" << actual_v[v_index] << '\n';
                // Constant interior transport is exact; allow initial upload plus three rounded R16F decay stores.
                constexpr float tolerance = 0.002F;
                Require(std::abs(actual_u[u_index] - expected_u) < tolerance &&
                            std::abs(actual_v[v_index] - expected_v) < tolerance,
                        "expected corrected velocity (" + std::to_string(expected_u) + "," + std::to_string(expected_v) + ")");
                Require(pure_transport, "forward velocity transport must not apply decay");
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x <= size; ++x) {
                        const float value = actual_u[y * (size + 1) + x];
                        Require(std::isfinite(value) && (!(blocked(x - 1, y) || blocked(x, y)) || value == 0.0F),
                                "U must remain finite with zero wall and domain faces");
                    }
                }
                for (int y = 0; y <= size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const float value = actual_v[y * size + x];
                        Require(std::isfinite(value) && (!(blocked(x, y - 1) || blocked(x, y)) || value == 0.0F),
                                "V must remain finite with zero wall and domain faces");
                    }
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
                CheckGl("read gas velocity counters");
                Require(counters == std::array<GLuint, 4>{}, "finite velocity fixture must leave debug counters zero");
                std::cout << "  PASS gas velocity " << label << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "  FAIL gas velocity " << label << ": " << error.what() << '\n';
            }
        }
    }
    Require(failures == 0, std::to_string(failures) + " gas velocity dissipation fixtures failed");
}

void GasWallTransport() {
    constexpr int size = 8;
    constexpr float initial = 0.75F;
    int failures = 0;
    constexpr std::array<std::pair<int, int>, 8> directions{{{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                                          {1, 1}, {-1, -1}, {1, -1}, {-1, 1}}};
    for (int scenario = 0; scenario < 23; ++scenario) {
        const bool bounded_trace = scenario >= 15;
        const auto [trace_x, trace_y] = bounded_trace ? directions[scenario - 15] : std::pair{0, 0};
        const bool diagonal_trace = trace_x != 0 && trace_y != 0;
        const bool endpoint = scenario >= 9; // Endpoint controls and bounded interior characteristics.
        const int endpoint_mode = bounded_trace ? 2 : (scenario - 9) / 2; // Zero dt, constant, then impulse.
        const int edge = bounded_trace ? 4 : scenario % 2 == 0 ? size - 1 : 0;
        const float dt = endpoint && endpoint_mode == 0 ? 0.0F : 1.0F / 30.0F;
        const bool corner = scenario == 8;
        const bool horizontal = (scenario & 1) != 0;
        const bool reverse = (scenario & 2) != 0;
        const bool wall = (scenario & 4) == 0;
        const int target_axis = reverse ? 1 : 5;
        const int target_x = endpoint ? edge : corner ? 4 : (horizontal ? 4 : target_axis);
        const int target_y = endpoint ? edge : corner ? 4 : (horizontal ? target_axis : 4);
        const int target_cell = target_y * size + target_x;
        const std::string label = bounded_trace ? "bounded trace (" + std::to_string(trace_x) + "," + std::to_string(trace_y) + ")" : endpoint ? std::string(edge == 0 ? "lower" : "upper") +
            " endpoint " + (endpoint_mode == 0 ? "zero dt" : endpoint_mode == 1 ? "constant" : "impulse") : corner ? "diagonal sealed cells" :
            std::string(horizontal ? "horizontal" : "vertical") + (reverse ? " reverse" : " forward") +
            (wall ? " wall" : " open control");
        try {
            Resources resources;
            std::array<unsigned char, size * size> mask{};
            std::array<float, size * size> scalar_values{};
            std::array<float, size * size * 2> reaction_values{}, velocity_values{};
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int axis = horizontal ? y : x;
                    const int cell = y * size + x;
                    mask[cell] = corner ? !((x == 3 && y == 3) || (x == 4 && y == 4)) : wall && axis == 3;
                    const bool source = corner ? x == 3 && y == 3 : (reverse ? axis > 3 : axis < 3);
                    scalar_values[cell] = source ? initial : 0.0F;
                    reaction_values[cell * 2 + 1] = scalar_values[cell];
                    if (!mask[cell]) {
                        velocity_values[cell * 2] = corner ? 7.5F : (horizontal ? 0.0F : (reverse ? -90.0F : 90.0F));
                        velocity_values[cell * 2 + 1] = corner ? 7.5F : (horizontal ? (reverse ? -90.0F : 90.0F) : 0.0F);
                    }
                }
            }
            if (endpoint) {
                mask.fill(0);
                scalar_values.fill(endpoint_mode == 1 ? initial : 0.0F);
                scalar_values[target_cell] = initial;
                reaction_values.fill(0.0F);
                velocity_values.fill(0.0F);
                for (int cell = 0; cell < size * size; ++cell)
                    reaction_values[cell * 2 + 1] = scalar_values[cell];
                if (bounded_trace) {
                    // Projection may exceed the configured component bound; traces must still move only 0.5 cell.
                    for (int cell = 0; cell < size * size; ++cell) {
                        velocity_values[cell * 2] = 60.0F * static_cast<float>(trace_x);
                        velocity_values[cell * 2 + 1] = 60.0F * static_cast<float>(trace_y);
                    }
                } else {
                    // A corner impulse traces half a cell inward; staggered markers trace tangentially.
                    velocity_values[target_cell * 2] = velocity_values[target_cell * 2 + 1] = edge == 0 ? -15.0F : 15.0F;
                    for (int i : {3, 4}) {
                        velocity_values[(edge * size + i) * 2] = 15.0F;
                        velocity_values[(i * size + edge) * 2 + 1] = 15.0F;
                    }
                }
            }
            const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
            const GLuint velocity = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, velocity_values.data());
            auto reverse_velocity_values = velocity_values;
            if (bounded_trace) {
                for (float& value : reverse_velocity_values) value *= -1.0F;
            } else if (endpoint) {
                reverse_velocity_values[target_cell * 2] *= -1.0F;
                reverse_velocity_values[target_cell * 2 + 1] *= -1.0F;
            }
            const GLuint reverse_velocity = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reverse_velocity_values.data());
            const GLuint source = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, scalar_values.data());
            const GLuint reaction_source = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reaction_values.data());
            std::array<GLuint, 4> forward{}, corrected{};
            for (std::size_t field = 0; field < forward.size(); ++field) {
                forward[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
                corrected[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
            }
            const GLuint reaction_forward = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
            const GLuint reaction_corrected = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
            std::array<GLuint, 4> counters{};
            const GLuint debug = resources.Buffer(7, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
            GLuint program = resources.Program("smoke_advect.comp");
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(program, "dt"), dt);
            glUniform1f(glGetUniformLocation(program, "maxVelocity"), bounded_trace ? 15.0F : 200.0F);
            BindImage(0, velocity, GL_READ_ONLY, GL_RG16F);
            for (GLuint field = 0; field < forward.size(); ++field) {
                BindImage(1 + field, source, GL_READ_ONLY, GL_R16F);
                BindImage(6 + field, forward[field], GL_WRITE_ONLY, GL_R16F);
            }
            BindImage(5, reaction_source, GL_READ_ONLY, GL_RG16F);
            BindImage(10, reaction_forward, GL_WRITE_ONLY, GL_RG16F);
            BindImage(11, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);
            std::array<float, 5> endpoint_forward{};
            if (endpoint) {
                for (std::size_t field = 0; field < forward.size(); ++field)
                    endpoint_forward[field] = ReadScalar(forward[field], size, size)[target_cell];
                endpoint_forward[4] = ReadVector(reaction_forward, size, size)[target_cell * 2 + 1];
                if (endpoint_mode == 2) {
                    // Independent correction input: reverse interpolation averages known dyadic donors.
                    std::array<float, size * size> prescribed{};
                    prescribed[target_cell] = bounded_trace && !diagonal_trace ? 0.0625F : 0.25F;
                    const int inward = edge == 0 ? 1 : size - 2;
                    prescribed[bounded_trace ? (target_y - trace_y) * size + target_x - trace_x : inward * size + inward] = 1.0F;
                    for (GLuint texture : forward) {
                        glBindTexture(GL_TEXTURE_2D, texture);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_FLOAT, prescribed.data());
                    }
                    std::array<float, size * size * 2> prescribed_reaction{};
                    for (int cell = 0; cell < size * size; ++cell) prescribed_reaction[cell * 2 + 1] = prescribed[cell];
                    glBindTexture(GL_TEXTURE_2D, reaction_forward);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RG, GL_FLOAT, prescribed_reaction.data());
                }
            }
            program = resources.Program("smoke_correct.comp");
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(program, "dt"), dt);
            glUniform1f(glGetUniformLocation(program, "maxVelocity"), bounded_trace ? 15.0F : 200.0F);
            glUniform1f(glGetUniformLocation(program, "densityDissipation"), 1.0F);
            glUniform1f(glGetUniformLocation(program, "scalarDissipation"), 1.0F);
            BindImage(0, velocity, GL_READ_ONLY, GL_RG16F);
            BindImage(1, reverse_velocity, GL_READ_ONLY, GL_RG16F);
            for (GLuint field = 0; field < forward.size(); ++field) {
                BindImage(2 + field, source, GL_READ_ONLY, GL_R16F);
                BindImage(7 + field, forward[field], GL_READ_ONLY, GL_R16F);
                BindImage(12 + field, corrected[field], GL_WRITE_ONLY, GL_R16F);
            }
            BindImage(6, reaction_source, GL_READ_ONLY, GL_RG16F);
            BindImage(11, reaction_forward, GL_READ_ONLY, GL_RG16F);
            BindImage(16, reaction_corrected, GL_WRITE_ONLY, GL_RG16F);
            BindImage(17, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);

            // Advect a tangential velocity marker using the same prescribed transport field.
            const auto blocked = [&](int x, int y) {
                return x < 0 || y < 0 || x >= size || y >= size || mask[y * size + x] != 0;
            };
            std::array<float, (size + 1) * size> u_values{};
            std::array<float, size * (size + 1)> v_values{};
            for (int y = 0; y < size; ++y) {
                for (int x = 1; x < size; ++x) {
                    if (horizontal && !blocked(x - 1, y) && !blocked(x, y))
                        u_values[y * (size + 1) + x] = scalar_values[y * size + x];
                }
            }
            for (int y = 1; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    if (!horizontal && !blocked(x, y - 1) && !blocked(x, y))
                        v_values[y * size + x] = scalar_values[y * size + x];
                }
            }
            const int endpoint_u = edge * (size + 1) + 4;
            const int endpoint_v = 4 * size + edge;
            if (endpoint) {
                u_values.fill(endpoint_mode == 1 ? initial : 0.0F);
                v_values.fill(endpoint_mode == 1 ? initial : 0.0F);
                u_values[endpoint_u] = v_values[endpoint_v] = initial;
            }
            const GLuint source_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT, u_values.data());
            const GLuint source_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT, v_values.data());
            const GLuint forward_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
            const GLuint forward_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
            const GLuint corrected_u = resources.Texture(size + 1, size, GL_R16F, GL_RED, GL_FLOAT);
            const GLuint corrected_v = resources.Texture(size, size + 1, GL_R16F, GL_RED, GL_FLOAT);
            program = resources.Program("gas_velocity_advect.comp");
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(program, "dt"), dt);
            glUniform1f(glGetUniformLocation(program, "maxVelocity"), bounded_trace ? 15.0F : 200.0F);
            BindImage(0, source_u, GL_READ_ONLY, GL_R16F);
            BindImage(1, source_v, GL_READ_ONLY, GL_R16F);
            BindImage(2, velocity, GL_READ_ONLY, GL_RG16F);
            BindImage(3, forward_u, GL_WRITE_ONLY, GL_R16F);
            BindImage(4, forward_v, GL_WRITE_ONLY, GL_R16F);
            BindImage(5, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size + 1, size + 1);
            std::array<float, 2> endpoint_face_forward{};
            if (endpoint) {
                endpoint_face_forward = {ReadScalar(forward_u, size + 1, size)[endpoint_u],
                                         ReadScalar(forward_v, size, size + 1)[endpoint_v]};
                if (endpoint_mode == 2) {
                    // The one-axis correction is 0.171875; the diagonal correction is 0.46875.
                    u_values.fill(0.0F);
                    v_values.fill(0.0F);
                    u_values[endpoint_u] = v_values[endpoint_v] = diagonal_trace ? 0.25F : 0.0625F;
                    u_values[endpoint_u + (bounded_trace ? -trace_y * (size + 1) - trace_x : 1)] = 1.0F;
                    v_values[endpoint_v + (bounded_trace ? -trace_y * size - trace_x : size)] = 1.0F;
                    glBindTexture(GL_TEXTURE_2D, forward_u);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size + 1, size, GL_RED, GL_FLOAT, u_values.data());
                    glBindTexture(GL_TEXTURE_2D, forward_v);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size + 1, GL_RED, GL_FLOAT, v_values.data());
                }
            }
            program = resources.Program("gas_velocity_correct.comp");
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
            glUniform1f(glGetUniformLocation(program, "dt"), dt);
            glUniform1f(glGetUniformLocation(program, "velocityDissipation"), 1.0F);
            glUniform1f(glGetUniformLocation(program, "buoyancyTemperature"), 0.0F);
            glUniform1f(glGetUniformLocation(program, "buoyancySmoke"), 0.0F);
            glUniform1f(glGetUniformLocation(program, "maxVelocity"), bounded_trace ? 15.0F : 200.0F);
            BindImage(0, source_u, GL_READ_ONLY, GL_R16F);
            BindImage(1, source_v, GL_READ_ONLY, GL_R16F);
            BindImage(2, velocity, GL_READ_ONLY, GL_RG16F);
            BindImage(3, forward_u, GL_READ_ONLY, GL_R16F);
            BindImage(4, forward_v, GL_READ_ONLY, GL_R16F);
            BindImage(5, bounded_trace ? reverse_velocity : velocity, GL_READ_ONLY, GL_RG16F);
            BindImage(6, source, GL_READ_ONLY, GL_R16F);
            BindImage(7, source, GL_READ_ONLY, GL_R16F);
            BindImage(8, corrected_u, GL_WRITE_ONLY, GL_R16F);
            BindImage(9, corrected_v, GL_WRITE_ONLY, GL_R16F);
            BindImage(10, boundary, GL_READ_ONLY, GL_R8UI);
            Dispatch(size + 1, size + 1);

            std::ostringstream errors;
            if (endpoint) {
                // These half-cell, dyadic inputs have exactly representable R16F results.
                const auto exact = [&](float value, float expected_value, const std::string& field) {
                    if (!std::isfinite(value) || std::abs(value - expected_value) > 1.0e-6F)
                        errors << " [" << field << '=' << value << ", expected " << expected_value << ']';
                };
                const float scalar_forward_expected = bounded_trace && !diagonal_trace ? 0.375F : 0.1875F;
                const float scalar_correct_expected = bounded_trace && !diagonal_trace ? 0.171875F : 0.46875F;
                for (float value : endpoint_forward) exact(value, endpoint_mode == 2 ? scalar_forward_expected : initial, "scalar forward");
                for (GLuint texture : corrected)
                    exact(ReadScalar(texture, size, size)[target_cell], endpoint_mode == 2 ? scalar_correct_expected : initial, "scalar correction");
                exact(ReadVector(reaction_corrected, size, size)[target_cell * 2 + 1],
                      endpoint_mode == 2 ? scalar_correct_expected : initial, "reaction correction");
                for (float value : endpoint_face_forward) exact(value, endpoint_mode == 2 ? (diagonal_trace ? 0.1875F : 0.375F) : initial, "face forward");
                const float face_correct_expected = diagonal_trace ? 0.46875F : 0.171875F;
                exact(ReadScalar(corrected_u, size + 1, size)[endpoint_u], endpoint_mode == 2 ? face_correct_expected : initial, "U correction");
                exact(ReadScalar(corrected_v, size, size + 1)[endpoint_v], endpoint_mode == 2 ? face_correct_expected : initial, "V correction");
            }
            const float expected = wall ? 0.0F : initial;
            const auto check = [&](float value, const std::string& field) {
                if (!std::isfinite(value) || std::abs(value - expected) > 0.002F)
                    errors << " [" << field << '=' << value << ", expected " << expected << ']';
            };
            for (std::size_t field = 0; !endpoint && field < forward.size(); ++field) {
                check(ReadScalar(forward[field], size, size)[target_cell], "scalar forward " + std::to_string(field));
                check(ReadScalar(corrected[field], size, size)[target_cell], "scalar corrected " + std::to_string(field));
            }
            if (!endpoint) {
                check(ReadVector(reaction_forward, size, size)[target_cell * 2 + 1], "reaction forward");
                check(ReadVector(reaction_corrected, size, size)[target_cell * 2 + 1], "reaction corrected");
            }
            if (!corner && !endpoint) {
                const int marker_width = horizontal ? size + 1 : size;
                const int marker_height = horizontal ? size : size + 1;
                const int marker_index = target_y * marker_width + target_x;
                check(ReadScalar(horizontal ? forward_u : forward_v, marker_width, marker_height)[marker_index], "velocity forward");
                check(ReadScalar(horizontal ? corrected_u : corrected_v, marker_width, marker_height)[marker_index], "velocity corrected");
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
            CheckGl("read wall transport counters");
            Require(counters == std::array<GLuint, 4>{}, "wall transport must leave debug counters zero");
            Require(errors.str().empty(), errors.str());
            std::cout << "  PASS gas wall transport " << label << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "  FAIL gas wall transport " << label << ": " << error.what() << '\n';
        }
    }
    Require(failures == 0, std::to_string(failures) + " gas wall transport fixtures failed");
}

void CouplingTime() {
    constexpr int size = 2;
    constexpr float initial_heat = 0.8F;
    constexpr float initial_reaction = 0.6F;
    constexpr float initial_fuel = 0.7F;
    constexpr float initial_oxidizer = 0.9F;
    int failures = 0;
    for (const auto [water, cooling] : std::array<std::pair<float, float>, 4>{{{0.0F, 2.4F}, {0.1F, 2.4F}, {1.0F, 2.4F}, {0.5F, 0.0F}}}) {
        for (int steps : {1, 2, 4}) {
            const std::string label = "water=" + std::to_string(water) + ", cooling=" + std::to_string(cooling) +
                                      ", steps=" + std::to_string(steps);
            try {
                Resources resources;
                std::array<float, size * size * 4> water_values;
                water_values.fill(water);
                const GLuint water_texture = resources.Texture(size * 2, size * 2, GL_R16F, GL_RED, GL_FLOAT, water_values.data());
                const auto scalar = [&](float value) {
                    std::array<float, size * size> values;
                    values.fill(value);
                    return resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, values.data());
                };
                const GLuint heat = scalar(initial_heat);
                const GLuint fuel = scalar(initial_fuel);
                const GLuint oxidizer = scalar(initial_oxidizer);
                std::array<float, size * size * 2> reaction_values{};
                for (int cell = 0; cell < size * size; ++cell) reaction_values[cell * 2 + 1] = initial_reaction;
                const GLuint reaction = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reaction_values.data());
                const std::array<unsigned char, size * size> empty{};
                const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
                const GLuint fire_program = resources.Program("coupling_fire.comp");
                const GLuint heat_program = resources.Program("coupling_heat.comp");
                const float dt = (1.0F / 60.0F) / static_cast<float>(steps);
                for (int step = 0; step < steps; ++step) {
                    glUseProgram(fire_program);
                    glUniform2i(glGetUniformLocation(fire_program, "gridSize"), size * 2, size * 2);
                    glUniform2i(glGetUniformLocation(fire_program, "heatSize"), size, size);
                    glUniform1f(glGetUniformLocation(fire_program, "dt"), dt);
                    glUniform1f(glGetUniformLocation(fire_program, "waterCooling"), cooling);
                    BindImage(0, boundary, GL_READ_ONLY, GL_R8UI);
                    BindImage(1, water_texture, GL_READ_ONLY, GL_R16F);
                    BindImage(2, heat, GL_READ_ONLY, GL_R16F);
                    BindImage(3, reaction, GL_READ_WRITE, GL_RG16F);
                    BindImage(4, oxidizer, GL_READ_WRITE, GL_R16F);
                    BindImage(5, fuel, GL_READ_WRITE, GL_R16F);
                    Dispatch(size, size);
                    glUseProgram(heat_program);
                    glUniform2i(glGetUniformLocation(heat_program, "gridSize"), size * 2, size * 2);
                    glUniform2i(glGetUniformLocation(heat_program, "heatSize"), size, size);
                    glUniform1f(glGetUniformLocation(heat_program, "dt"), dt);
                    glUniform1f(glGetUniformLocation(heat_program, "waterCooling"), cooling * 0.7F);
                    BindImage(0, heat, GL_READ_WRITE, GL_R16F);
                    BindImage(1, water_texture, GL_READ_ONLY, GL_R16F);
                    Dispatch(size, size);
                }
                const float suppression = std::clamp(water * cooling, 0.0F, 1.0F);
                const std::array<float, 4> expected{
                    initial_heat * (1.0F - std::clamp(water * cooling * 0.7F, 0.0F, 1.0F)),
                    initial_reaction * std::max(1.0F - suppression * 2.2F, 0.0F),
                    initial_fuel * (1.0F - suppression * 0.85F),
                    initial_oxidizer * std::max(1.0F - suppression * 1.2F, 0.0F)};
                const auto reaction_result = ReadVector(reaction, size, size);
                const std::array<std::vector<float>, 4> actual{
                    ReadScalar(heat, size, size), {}, ReadScalar(fuel, size, size), ReadScalar(oxidizer, size, size)};
                std::cout << "  coupling " << label << " -> heat=" << actual[0][0] << ", reaction=" << reaction_result[1]
                          << ", fuel=" << actual[2][0] << ", oxidizer=" << actual[3][0] << '\n';
                // At most four half-float stores plus initial upload; all expected values are below one.
                for (std::size_t field = 0; field < actual.size(); ++field) {
                    for (int cell = 0; cell < size * size; ++cell) {
                        const float value = field == 1 ? reaction_result[cell * 2 + 1] : actual[field][cell];
                        Require(std::isfinite(value) && std::abs(value - expected[field]) < 0.002F,
                                "field " + std::to_string(field) + " expected " + std::to_string(expected[field]));
                    }
                }
                for (int cell = 0; cell < size * size; ++cell)
                    Require(reaction_result[cell * 2] == 0.0F, "coupling must preserve reserved reaction.x");
                std::cout << "  PASS coupling " << label << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "  FAIL coupling " << label << ": " << error.what() << '\n';
            }
        }
    }
    Require(failures == 0, std::to_string(failures) + " coupling time fixtures failed");
}

void GasBrushBlocking() {
    constexpr int size = 4;
    constexpr int full_size = size * 2;
    constexpr int target = 1 + size;
    struct BrushCase {
        const char* name;
        float water;
        bool block_water;
        bool adjacent_wall;
        bool clear;
        bool wet_outside_brush;
        float expected;
    };
    const std::array<BrushCase, 7> cases{{
        {"dry fire", 0.0F, true, false, false, false, 0.375F},
        {"trace water fire", 0.01F, true, false, false, false, 0.375F},
        {"wet fire", 0.02F, true, false, false, false, 0.125F},
        {"smoke over water", 1.0F, false, false, false, false, 0.375F},
        {"gas mask wall", 0.0F, true, true, false, false, 0.125F},
        {"clear wet blocked cell", 1.0F, true, true, true, false, 0.0F},
        {"water outside brush", 1.0F, true, false, false, true, 0.375F},
    }};
    int failures = 0;
    for (const auto& test : cases) {
        try {
            Resources resources;
            std::array<unsigned char, full_size * full_size> full_mask{};
            std::array<float, full_size * full_size> water{};
            water[(test.wet_outside_brush ? 3 : 2) * full_size + (test.wet_outside_brush ? 3 : 2)] = test.water;
            if (test.adjacent_wall) {
                // Outside the target's 2x2 footprint, inside the real gas-mask downsample halo.
                full_mask[2 * full_size + 4] = 1;
            }
            const GLuint water_texture = resources.Texture(full_size, full_size, GL_R16F, GL_RED, GL_FLOAT, water.data());
            const GLuint boundary_full = resources.Texture(full_size, full_size, GL_R8UI, GL_RED_INTEGER,
                                                           GL_UNSIGNED_BYTE, full_mask.data());
            const GLuint boundary_gas = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
            const GLuint downsample = resources.Program("boundary_downsample.comp");
            glUseProgram(downsample);
            glUniform2i(glGetUniformLocation(downsample, "srcSize"), full_size, full_size);
            glUniform2i(glGetUniformLocation(downsample, "dstSize"), size, size);
            glUniform1i(glGetUniformLocation(downsample, "scale"), 2);
            BindImage(0, boundary_full, GL_READ_ONLY, GL_R8UI);
            BindImage(1, boundary_gas, GL_WRITE_ONLY, GL_R8UI);
            Dispatch(size, size);
            const auto scalar = [&](int width, int height) {
                const std::vector<float> values(static_cast<std::size_t>(width * height), 0.125F);
                return resources.Texture(width, height, GL_R16F, GL_RED, GL_FLOAT, values.data());
            };
            const GLuint u = scalar(size + 1, size), v = scalar(size, size + 1);
            const std::array<GLuint, 4> fields{scalar(size, size), scalar(size, size), scalar(size, size), scalar(size, size)};
            std::array<float, size * size * 2> center_values;
            center_values.fill(0.125F);
            std::array<float, size * size * 2> reaction_values{};
            for (int cell = 0; cell < size * size; ++cell) reaction_values[cell * 2 + 1] = 0.125F;
            const GLuint center = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, center_values.data());
            const GLuint reaction = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reaction_values.data());
            const GLuint program = resources.Program("spawn_smoke.comp");
            glUseProgram(program);
            glUniform2i(glGetUniformLocation(program, "gasSize"), size, size);
            glUniform2i(glGetUniformLocation(program, "gridSize"), full_size, full_size);
            glUniform2i(glGetUniformLocation(program, "dispatchOrigin"), 0, 0);
            glUniform2i(glGetUniformLocation(program, "brushCenter"), 2, 2);
            glUniform1i(glGetUniformLocation(program, "brushRadius"), 0);
            glUniform2f(glGetUniformLocation(program, "gasVelocity"), 0.25F, 0.25F);
            for (const char* uniform : {"smokeDensity", "temperatureAdd", "fuelAdd", "oxidizerAdd", "reactionAdd"})
                glUniform1f(glGetUniformLocation(program, uniform), 0.25F);
            glUniform1i(glGetUniformLocation(program, "blockedBySolid"), 1);
            glUniform1i(glGetUniformLocation(program, "blockedByWater"), test.block_water ? 1 : 0);
            glUniform1i(glGetUniformLocation(program, "clearMode"), test.clear ? 1 : 0);
            BindImage(0, u, GL_READ_WRITE, GL_R16F);
            BindImage(1, v, GL_READ_WRITE, GL_R16F);
            BindImage(2, center, GL_READ_WRITE, GL_RG16F);
            for (GLuint field = 0; field < fields.size(); ++field) BindImage(3 + field, fields[field], GL_READ_WRITE, GL_R16F);
            BindImage(7, reaction, GL_READ_WRITE, GL_RG16F);
            BindImage(9, water_texture, GL_READ_ONLY, GL_R16F);
            BindImage(10, boundary_gas, GL_READ_ONLY, GL_R8UI);
            Dispatch(size, size);
            const auto check = [&](const std::vector<float>& values, int changed, const std::string& field) {
                for (std::size_t i = 0; i < values.size(); ++i) {
                    const float expected = static_cast<int>(i) == changed ? test.expected : 0.125F;
                    Require(values[i] == expected, field + " index=" + std::to_string(i) + " expected=" +
                                std::to_string(expected) + " got=" + std::to_string(values[i]));
                }
            };
            for (GLuint field : fields) check(ReadScalar(field, size, size), target, "scalar");
            check(ReadScalar(u, size + 1, size), 1 + size + 1, "U");
            check(ReadScalar(v, size, size + 1), target, "V");
            const auto centers = ReadVector(center, size, size);
            const auto reactions = ReadVector(reaction, size, size);
            for (int cell = 0; cell < size * size; ++cell) {
                const float expected = cell == target ? test.expected : 0.125F;
                Require(centers[cell * 2] == expected && centers[cell * 2 + 1] == expected,
                        "center velocity mismatch");
                Require(reactions[cell * 2] == 0.0F && reactions[cell * 2 + 1] == expected,
                        "reaction mismatch");
            }
            std::cout << "  PASS gas brush " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "  FAIL gas brush " << test.name << ": " << error.what() << '\n';
        }
    }
    Require(failures == 0, std::to_string(failures) + " gas brush fixtures failed");
}

void ParticleCapacity() {
    constexpr int size = 5;
    constexpr GLuint capacity = 4;
    int failures = 0;
    const auto material_cases = [&]<std::size_t Components>() {
        using Particle = std::array<float, Components>;
        constexpr bool sand = Components == 16;
        for (const GLuint initial_cursor : {0U, capacity - 1U, capacity, std::numeric_limits<GLuint>::max() - 1U}) {
            const std::string label = std::string(sand ? "sand" : "water") + " cursor=" + std::to_string(initial_cursor);
            try {
                Resources resources;
                const GLuint existing_count = std::min(initial_cursor, capacity);
                std::array<Particle, capacity + 2> original{};
                for (std::size_t slot = 0; slot < original.size(); ++slot) {
                    for (std::size_t field = 0; field < original[slot].size(); ++field) {
                        original[slot][field] = 1000.0F + static_cast<float>(slot * 100 + field);
                    }
                    if (slot < capacity) {
                        original[slot][4] = slot < existing_count ? 1.0F : 0.0F;
                    }
                }
                auto actual = original;
                GLuint cursor = initial_cursor;
                std::array<GLuint, 4> counters{};
                const GLuint particles = resources.Buffer(0, static_cast<GLsizeiptr>(sizeof(original)), original.data());
                const GLuint cursor_buffer = resources.Buffer(1, sizeof(cursor), &cursor);
                const GLuint debug = resources.Buffer(2, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
                GLuint program = 0;
                if (sand) {
                    const std::array<GLushort, size * size> material{};
                    const std::array<float, size * size> occupancy{};
                    const GLuint material_texture = resources.Texture(size, size, GL_R16UI, GL_RED_INTEGER,
                                                                      GL_UNSIGNED_SHORT, material.data());
                    const GLuint occupancy_texture = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, occupancy.data());
                    program = resources.Program("sand_particle_spawn.comp");
                    glUseProgram(program);
                    glUniform1i(glGetUniformLocation(program, "blockedBySolid"), 1);
                    glUniform1f(glGetUniformLocation(program, "occupancyLimit"), 1.0F);
                    glUniform1f(glGetUniformLocation(program, "referenceDensity"), 2.0F);
                    BindImage(0, material_texture, GL_READ_ONLY, GL_R16UI);
                    BindImage(1, occupancy_texture, GL_READ_ONLY, GL_R16F);
                } else {
                    const std::array<unsigned char, size * size> empty{};
                    const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
                    program = resources.Program("water_particle_spawn.comp");
                    glUseProgram(program);
                    BindImage(0, boundary, GL_READ_ONLY, GL_R8UI);
                }
                glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
                glUniform2i(glGetUniformLocation(program, "dispatchOrigin"), 0, 0);
                glUniform2i(glGetUniformLocation(program, "brushCenter"), 2, 2);
                glUniform1i(glGetUniformLocation(program, "brushRadius"), 1);
                glUniform2f(glGetUniformLocation(program, "spawnVelocity"), 2.0F, -3.0F);
                glUniform1i(glGetUniformLocation(program, "spawnCountPerCell"), 5);
                glUniform1f(glGetUniformLocation(program, "particleMass"), 1.0F);
                glUniform1i(glGetUniformLocation(program, "maxParticles"), static_cast<GLint>(capacity));
                Dispatch(size, size);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(actual)), actual.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, cursor_buffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(cursor), &cursor);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
                CheckGl("read particle capacity fixture");

                const bool preserved = std::equal(original.begin(), original.begin() + existing_count, actual.begin());
                const bool guards_preserved = std::equal(original.begin() + capacity, original.end(), actual.begin() + capacity);
                const auto active_count = std::count_if(actual.begin(), actual.begin() + capacity,
                                                        [](const Particle& particle) { return particle[4] > 0.5F; });
                std::cout << "  " << label << " -> cursor=" << cursor << ", active=" << active_count
                          << ", existing preserved=" << preserved << ", guards preserved=" << guards_preserved
                          << ", blocked=" << counters[1] << '\n';
                const GLuint expected_cursor = initial_cursor < capacity ? capacity : initial_cursor;
                Require(cursor == expected_cursor, "full capacity must stop cursor advancement and cannot wrap");
                Require(preserved, "spawn overwrote an existing particle");
                Require(guards_preserved, "spawn wrote beyond particle capacity");
                Require(active_count == capacity, "spawn must fill exactly the remaining available slots");
                for (GLuint slot = existing_count; slot < capacity; ++slot) {
                    const auto& particle = actual[slot];
                    Require(std::all_of(particle.begin(), particle.end(), [](float value) { return std::isfinite(value); }),
                            "spawned particle fields must be finite");
                    Require(particle[0] >= 0.0F && particle[0] < size && particle[1] >= 0.0F && particle[1] < size &&
                                particle[2] == 2.0F && particle[3] == -3.0F && particle[4] == 1.0F && particle[5] == 1.0F,
                            "spawned particle must have the requested state");
                    if constexpr (sand) {
                        Require(particle[10] == 0.5F && particle[12] == 1.0F && particle[13] == 0.0F &&
                                    particle[14] == 0.0F && particle[15] == 1.0F,
                                "sand spawn must initialize rest area and identity elastic deformation");
                    }
                }
                // A radius-one brush reaches five cells. Each requests more than the entire capacity,
                // so each invocation eventually records exactly one blocked allocation, regardless of scheduling.
                Require(counters == std::array<GLuint, 4>{0U, 5U, 0U, 0U}, "expected exactly five blocked spawn invocations");
                std::cout << "  PASS particle capacity " << label << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "  FAIL particle capacity " << label << ": " << error.what() << '\n';
            }
        }
    };
    material_cases.template operator()<12>();
    material_cases.template operator()<16>();
    Require(failures == 0, std::to_string(failures) + " particle capacity fixtures failed");
}

void PressureInvalid() {
    Resources resources;
    constexpr int size = 4;
    const std::array<float, size * size> zeros{};
    const std::array<unsigned char, size * size> empty{};
    const GLuint rhs = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, zeros.data());
    const GLuint pressure = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, zeros.data());
    const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, empty.data());
    std::array<GLuint, 4> counters{};
    const GLuint debug = resources.Buffer(0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
    const GLuint program = resources.Program("pressure_residual.comp");
    glUseProgram(program);
    glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
    glUniform1i(glGetUniformLocation(program, "boundaryScale"), 1);
    glUniform1f(glGetUniformLocation(program, "residualThreshold"), 0.1F);
    glUniform1i(glGetUniformLocation(program, "useLiquidMask"), 0);
    glUniform1f(glGetUniformLocation(program, "liquidPhiThreshold"), 0.42F);
    BindImage(0, rhs, GL_READ_ONLY, GL_R16F);
    glUniform1i(glGetUniformLocation(program, "pressureTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pressure);
    BindImage(2, boundary, GL_READ_ONLY, GL_R8UI);
    BindImage(3, rhs, GL_READ_ONLY, GL_R16F);
    Dispatch(size, size);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
    CheckGl("read finite pressure counters");
    Require(counters == std::array<GLuint, 4>{}, "finite zero pressure must not raise any debug counter");

    const float invalid = std::numeric_limits<float>::quiet_NaN();
    glBindTexture(GL_TEXTURE_2D, pressure);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RED, GL_FLOAT, &invalid);
    CheckGl("inject non-finite pressure");
    Require(std::isnan(ReadScalar(pressure, size, size)[5]), "NaN fixture did not survive texture upload");
    Dispatch(size, size);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(counters)), counters.data());
    CheckGl("read invalid pressure counters");
    Require(counters[0] > 0U, "NaN in a fluid pressure cell must increment nanInfDetected; got 0");

    const GLuint pressure32 = resources.Texture(size, size, GL_R32F, GL_RED, GL_FLOAT, zeros.data());
    glBindTexture(GL_TEXTURE_2D, pressure32);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RED, GL_FLOAT, &invalid);
    counters.fill(0U);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
    Dispatch(size, size);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
    Require(counters[0] > 0U && counters[2] > 0U, "R32F pressure must use the same nonfinite residual validation");

    // Two workgroups, mostly padded lanes, with solid and air cells excluded from the RMS denominator.
    constexpr int stats_width = 17, stats_height = 3;
    std::array<float, stats_width * stats_height> stats_rhs{}, stats_pressure{}, phi{};
    std::array<unsigned char, stats_width * stats_height> stats_mask{};
    stats_rhs.fill(0.25F);
    stats_mask[0] = 1;
    phi[1] = 1.0F;
    const GLuint stats_rhs_tex = resources.Texture(stats_width, stats_height, GL_R16F, GL_RED, GL_FLOAT, stats_rhs.data());
    const GLuint stats_pressure_tex = resources.Texture(stats_width, stats_height, GL_R32F, GL_RED, GL_FLOAT, stats_pressure.data());
    const GLuint stats_mask_tex = resources.Texture(stats_width, stats_height, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, stats_mask.data());
    const GLuint stats_phi_tex = resources.Texture(stats_width, stats_height, GL_R16F, GL_RED, GL_FLOAT, phi.data());
    std::array<std::array<float, 4>, 2> partials{};
    const GLuint stats_buffer = resources.Buffer(1, sizeof(partials), partials.data());
    glUniform2i(glGetUniformLocation(program, "gridSize"), stats_width, stats_height);
    glUniform1i(glGetUniformLocation(program, "collectStats"), 1);
    glUniform1i(glGetUniformLocation(program, "useLiquidMask"), 1);
    BindImage(0, stats_rhs_tex, GL_READ_ONLY, GL_R16F);
    BindImage(2, stats_mask_tex, GL_READ_ONLY, GL_R8UI);
    BindImage(3, stats_phi_tex, GL_READ_ONLY, GL_R16F);
    glBindTexture(GL_TEXTURE_2D, stats_pressure_tex);
    const auto read_stats = [&]() {
        Dispatch(stats_width, stats_height);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, stats_buffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(partials), partials.data());
    };
    read_stats();
    Require(partials[0] == std::array<float, 4>{46.0F * 0.0625F, 0.25F, 46.0F, 0.0F} &&
            partials[1] == std::array<float, 4>{3.0F * 0.0625F, 0.25F, 3.0F, 0.0F},
            "pressure statistics must reduce exact residuals and active counts across padded, masked groups");
    glBindTexture(GL_TEXTURE_2D, stats_pressure_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 16, 1, 1, 1, GL_RED, GL_FLOAT, &invalid);
    read_stats();
    Require(partials[1][3] == 1.0F, "pressure statistics must preserve nonfinite input detection");
}

PFNGLCREATESHADERPROC native_create_shader = nullptr;
std::vector<GLuint> created_shaders;

GLuint APIENTRY TrackShader(GLenum type) {
    const GLuint shader = native_create_shader(type);
    created_shaders.push_back(shader);
    return shader;
}

void CompileFailureCleanup() {
    created_shaders.clear();
    native_create_shader = glad_glCreateShader;
    glad_glCreateShader = TrackShader;
    bool rejected = false;
    try {
        // A compute shader is deliberately invalid as a fragment shader.
        const GLuint program = glutil::CreateProgramFromFiles(kShaderRoot / "fullscreen.vert",
                                                               kShaderRoot / "water_particle_p2g.comp");
        glDeleteProgram(program);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    glad_glCreateShader = native_create_shader;
    Require(rejected && created_shaders.size() == 2U, "fragment compilation failure must reach both shader objects");
    for (GLuint shader : created_shaders) {
        Require(glIsShader(shader) == GL_FALSE, "failed render program leaked a compiled shader");
    }
    CheckGl("fragment compilation cleanup");
}

void FireReaction() {
    constexpr int size = 5;
    constexpr int center = 2 * size + 2;
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool passed, const std::string& message) {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL fire: " << message << '\n'; }
    };
    for (int scenario = 0; scenario < 6; ++scenario) {
        Resources resources;
        std::array<std::array<float, size * size>, 4> values{}; // Fuel, oxidizer, temperature, smoke.
        std::array<float, size * size * 2> reactions{};
        std::array<unsigned char, size * size> mask{};
        float rate = 0.0F;
        float diffusion = 0.0F;
        float consumption = 0.75F;
        if (scenario < 3) {
            values[0][center] = scenario == 0 ? 0.125F : 0.5F;
            values[1][center] = scenario == 1 ? 0.125F : 0.5F;
            values[2][center] = 0.25F;
            reactions[2 * center + 1] = 0.75F;
            rate = scenario == 2 ? 16.0F : 256.0F;
            if (scenario == 1) consumption = 2.0F;
        } else {
            diffusion = 64.0F; // dt*D=1: a convex average, at the monotonicity endpoint.
            if (scenario == 3) {
                mask.fill(1U);
                mask[center] = mask[center + size + 1] = 0U;
                values[2][center + size + 1] = 1.0F;
            } else if (scenario == 4) {
                mask.fill(1U);
                mask[center] = 0U;
                values[2][center] = 0.5F;
            } else {
                values[2][center] = 1.0F;
            }
        }
        std::array<GLuint, 4> source{};
        std::array<GLuint, 4> output{};
        for (std::size_t field = 0; field < source.size(); ++field) {
            source[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT, values[field].data());
            output[field] = resources.Texture(size, size, GL_R16F, GL_RED, GL_FLOAT);
        }
        const GLuint reaction = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT, reactions.data());
        const GLuint reaction_out = resources.Texture(size, size, GL_RG16F, GL_RG, GL_FLOAT);
        const GLuint boundary = resources.Texture(size, size, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, mask.data());
        std::array<GLuint, 4> counters{};
        const GLuint debug = resources.Buffer(7, sizeof(counters), counters.data());
        const GLuint program = resources.Program("fire_rd.comp");
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "gridSize"), size, size);
        glUniform1f(glGetUniformLocation(program, "dt"), 1.0F / 64.0F);
        glUniform1f(glGetUniformLocation(program, "combustionRate"), rate);
        glUniform1f(glGetUniformLocation(program, "oxidizerConsumption"), consumption);
        glUniform1f(glGetUniformLocation(program, "reactionDecay"), 0.0F);
        glUniform1f(glGetUniformLocation(program, "temperatureDiffusion"), diffusion);
        glUniform1f(glGetUniformLocation(program, "temperatureCooling"), 0.0F);
        glUniform1f(glGetUniformLocation(program, "heatRelease"), 2.0F);
        glUniform1f(glGetUniformLocation(program, "smokeYield"), 0.5F);
        glUniform1f(glGetUniformLocation(program, "smokeDissipation"), 0.0F);
        for (GLuint field = 0; field < source.size(); ++field) {
            BindImage(field, source[field], GL_READ_ONLY, GL_R16F);
            BindImage(field == 3 ? 10 : 6 + field, output[field], GL_WRITE_ONLY, GL_R16F);
        }
        BindImage(4, boundary, GL_READ_ONLY, GL_R8UI);
        BindImage(5, reaction, GL_READ_ONLY, GL_RG16F);
        BindImage(9, reaction_out, GL_WRITE_ONLY, GL_RG16F);
        Dispatch(size, size);
        std::array<std::vector<float>, 4> result;
        for (std::size_t field = 0; field < result.size(); ++field) result[field] = ReadScalar(output[field], size, size);
        const auto reaction_values = ReadVector(reaction_out, size, size);
        const std::string context = "scenario " + std::to_string(scenario);
        for (const auto& field : result) {
            check(std::all_of(field.begin(), field.end(), [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; }),
                  context + " scalar range/finite");
        }
        const auto near = [&](float actual, float expected, const char* field) {
            // One R16F store; all reaction expectations are exactly representable, diffusion weights need rounding.
            check(std::isfinite(actual) && std::abs(actual - expected) <= 0.00025F,
                  context + " " + field + " expected " + std::to_string(expected) + ", got " + std::to_string(actual));
        };
        if (scenario < 3) {
            const std::array<std::array<float, 5>, 3> expected{{
                {0.0F, 0.40625F, 0.5F, 0.0625F, 0.875F},
                {0.4375F, 0.0F, 0.375F, 0.03125F, 0.8125F},
                {0.4375F, 0.453125F, 0.375F, 0.03125F, 0.8125F}}};
            const std::array<const char*, 4> names{"fuel", "oxidizer", "temperature", "smoke"};
            for (std::size_t field = 0; field < result.size(); ++field) near(result[field][center], expected[scenario][field], names[field]);
            near(reaction_values[2 * center + 1], expected[scenario][4], "reaction");
        } else if (scenario == 3) {
            near(result[2][center], 0.0F, "sealed diagonal heat");
            near(result[2][center + size + 1], 1.0F, "isolated hot cell");
        } else if (scenario == 4) {
            near(result[2][center], 0.5F, "constant isolated heat");
        } else {
            near(result[2][center], 0.0F, "diffused impulse center");
            near(result[2][center + 1], 0.2F, "cardinal diffusion");
            near(result[2][center + size + 1], 0.05F, "reachable diagonal diffusion");
            float total = 0.0F;
            for (float value : result[2]) total += value;
            near(total, 1.0F, "conserved heat");
        }
        for (std::size_t cell = 0; cell < mask.size(); ++cell) {
            if (mask[cell] != 0U) {
                for (const auto& field : result) check(field[cell] == 0.0F, context + " solid scalar must be zero");
                check(reaction_values[2 * cell + 1] == 0.0F, context + " solid reaction must be zero");
            }
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, debug);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counters), counters.data());
        CheckGl("read fire counters");
        check(counters == std::array<GLuint, 4>{}, context + " unexpected debug counter");
    }
    std::cout << "fire reaction/diffusion checks=" << checks << " failures=" << failures << '\n';
    Require(failures == 0, "fire reaction/diffusion regressions failed");
}

void CompileShaders() {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(kShaderRoot)) {
        if (entry.is_regular_file() && entry.path().extension() == ".comp") {
            paths.push_back(entry.path());
        }
    }
    Require(!paths.empty(), "no compute shaders found in " + kShaderRoot.string());
    std::sort(paths.begin(), paths.end());
    int failures = 0;
    for (const auto& path : paths) {
        try {
            Resources resources;
            resources.Program(path.filename().string().c_str());
            std::cout << "  compiled " << path.filename().string() << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "  FAIL " << path.filename().string() << ": " << error.what() << '\n';
        }
        glfwPollEvents();
    }
    try {
        Resources resources;
        resources.programs.push_back(glutil::CreateProgramFromFiles((kShaderRoot / "fullscreen.vert").string(),
                                                                   (kShaderRoot / "composite.frag").string()));
        CheckGl("compile render shaders");
        std::cout << "  compiled fullscreen.vert + composite.frag\n";
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "  FAIL render shaders: " << error.what() << '\n';
    }
    Require(failures == 0, std::to_string(failures) + " shader programs failed compilation/linking");
    CompileFailureCleanup();
}

void PrintContextInfo() {
    for (const auto& [name, parameter] : std::array<std::pair<const char*, GLenum>, 4>{
             {{"Vendor", GL_VENDOR}, {"Renderer", GL_RENDERER}, {"OpenGL", GL_VERSION},
              {"GLSL", GL_SHADING_LANGUAGE_VERSION}}}) {
        const auto* value = glGetString(parameter);
        Require(value != nullptr, std::string("could not query ") + name);
        std::cout << name << ": " << reinterpret_cast<const char*>(value) << '\n';
    }
    for (const auto& [name, parameter] : std::array<std::pair<const char*, GLenum>, 7>{
             {{"GL_MAX_IMAGE_UNITS", GL_MAX_IMAGE_UNITS},
              {"GL_MAX_COMPUTE_IMAGE_UNIFORMS", GL_MAX_COMPUTE_IMAGE_UNIFORMS},
              {"GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES", GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES},
              {"GL_MAX_TEXTURE_IMAGE_UNITS", GL_MAX_TEXTURE_IMAGE_UNITS},
              {"GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS", GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS},
              {"GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS", GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS},
              {"GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS", GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS}}}) {
        GLint value = 0;
        glGetIntegerv(parameter, &value);
        std::cout << name << ": " << value << '\n';
    }
    CheckGl("query context capabilities");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    const std::array<std::pair<const char*, void (*)()>, 17> tests{{
        {"compile", CompileShaders}, {"water-transfer", WaterTransfer}, {"water-p2g", WaterP2G},
        {"half-storage", HalfStorage},
        {"water-affine", WaterAffineTransfer}, {"boundary-mask", BoundaryMask}, {"water-wall-pressure", WaterWallPressure},
        {"water-integration", WaterIntegration},
        {"gas-pressure", GasPressure}, {"smoke-dissipation", SmokeDissipation},
        {"gas-velocity-dissipation", GasVelocityDissipation},
        {"gas-wall-transport", GasWallTransport}, {"coupling-time", CouplingTime},
        {"gas-brush-blocking", GasBrushBlocking}, {"fire-reaction", FireReaction},
        {"particle-capacity", ParticleCapacity}, {"pressure-invalid", PressureInvalid}}};
    if (argc > 2 || (selected != "all" && std::none_of(tests.begin(), tests.end(), [&](const auto& test) {
                         return selected == test.first;
                     }))) {
        std::cerr << "Usage: powder_gpu_tests "
                     "[all|compile|half-storage|water-transfer|water-p2g|water-affine|water-integration|boundary-mask|water-wall-pressure|gas-pressure|smoke-dissipation|gas-velocity-dissipation|gas-wall-transport|coupling-time|gas-brush-blocking|fire-reaction|particle-capacity|pressure-invalid]\n";
        return 2;
    }

    glfwSetErrorCallback([](int code, const char* description) {
        std::cerr << "GLFW error " << code << ": " << description << '\n';
    });
    GLFWwindow* window = nullptr;
    int failures = 0;
    try {
        Require(glfwInit() == GLFW_TRUE, "GLFW initialization failed");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(32, 32, "Powder GPU regressions", nullptr, nullptr);
        Require(window != nullptr, "could not create a hidden OpenGL 4.3 context");
        glfwMakeContextCurrent(window);
        Require(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0,
                "GLAD could not load OpenGL");
        Require(GLAD_GL_VERSION_4_3 != 0, "OpenGL 4.3 is required");
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        PrintContextInfo();
        std::cout << "Shaders: " << kShaderRoot.string() << '\n';
        for (const auto& [name, test] : tests) {
            if (selected != "all" && selected != name) {
                continue;
            }
            try {
                CheckGl(std::string("before ") + name);
                test();
                CheckGl(std::string("after ") + name);
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
    if (window != nullptr) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();
    return failures == 0 ? 0 : 1;
}

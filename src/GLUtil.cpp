#include "GLUtil.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

GLuint CompileShader(GLenum type, const std::string& source, const std::string& path) {
    const GLuint shader = glCreateShader(type);
    const char* source_ptr = source.c_str();
    glShaderSource(shader, 1, &source_ptr, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::vector<char> log(static_cast<std::size_t>(log_length) + 1U, '\0');
    if (log_length > 0) {
        glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    }

    std::ostringstream error;
    error << "Shader compilation failed: " << path << "\n" << log.data();
    glDeleteShader(shader);
    throw std::runtime_error(error.str());
}

GLuint LinkProgram(const std::vector<GLuint>& shaders) {
    const GLuint program = glCreateProgram();
    for (const GLuint shader : shaders) {
        glAttachShader(program, shader);
    }

    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    for (const GLuint shader : shaders) {
        glDetachShader(program, shader);
        glDeleteShader(shader);
    }

    if (linked == GL_TRUE) {
        return program;
    }

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::vector<char> log(static_cast<std::size_t>(log_length) + 1U, '\0');
    if (log_length > 0) {
        glGetProgramInfoLog(program, log_length, nullptr, log.data());
    }

    std::ostringstream error;
    error << "Program link failed:\n" << log.data();
    glDeleteProgram(program);
    throw std::runtime_error(error.str());
}

}  // namespace

namespace glutil {

std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLuint CreateProgramFromFiles(const std::string& vertex_path, const std::string& fragment_path) {
    const std::string vertex_source = ReadTextFile(vertex_path);
    const std::string fragment_source = ReadTextFile(fragment_path);

    const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex_source, vertex_path);
    const GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment_source, fragment_path);

    return LinkProgram({vertex_shader, fragment_shader});
}

GLuint CreateComputeProgramFromFile(const std::string& compute_path) {
    const std::string compute_source = ReadTextFile(compute_path);
    const GLuint compute_shader = CompileShader(GL_COMPUTE_SHADER, compute_source, compute_path);
    return LinkProgram({compute_shader});
}

}  // namespace glutil

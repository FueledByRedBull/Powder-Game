#include "GLUtil.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace glutil {
// Defined in the EmbeddedShaders.cpp that cmake/EmbedShaders.cmake generates; empty for an unknown name.
std::string_view FindEmbeddedShader(std::string_view name);
}  // namespace glutil

namespace {

struct ShaderText {
    std::string label;
    std::string source;
};

std::string PathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

GLuint CompileShader(GLenum type, const ShaderText& text) {
    const GLuint shader = glCreateShader(type);
    const char* source_ptr = text.source.c_str();
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
    error << "Shader compilation failed: " << text.label << "\n" << log.data();
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

GLuint BuildProgram(const ShaderText& vertex, const ShaderText& fragment) {
    const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex);
    GLuint fragment_shader = 0;
    try {
        fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment);
    } catch (...) {
        glDeleteShader(vertex_shader);
        throw;
    }

    return LinkProgram({vertex_shader, fragment_shader});
}

GLuint BuildComputeProgram(const ShaderText& compute) {
    return LinkProgram({CompileShader(GL_COMPUTE_SHADER, compute)});
}

ShaderText LoadShader(const std::string& name) {
#ifdef _WIN32
    const wchar_t* directory = _wgetenv(L"POWDER_SHADER_DIR");  // wide, so Unicode directories survive
#else
    const char* directory = std::getenv("POWDER_SHADER_DIR");
#endif
    if (directory != nullptr) {
        if (*directory == 0) {
            throw std::runtime_error("POWDER_SHADER_DIR is set but empty");
        }
        const std::filesystem::path path = std::filesystem::path(directory) / name;
        return {PathText(path), glutil::ReadTextFile(path)};
    }
    const std::string_view source = glutil::FindEmbeddedShader(name);
    if (source.empty()) {
        throw std::runtime_error("Shader not embedded in the executable: " + name);
    }
    return {"embedded " + name, std::string(source)};
}

}  // namespace

namespace glutil {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + PathText(path));
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLuint CreateProgramFromFiles(const std::filesystem::path& vertex_path, const std::filesystem::path& fragment_path) {
    return BuildProgram({PathText(vertex_path), ReadTextFile(vertex_path)},
                        {PathText(fragment_path), ReadTextFile(fragment_path)});
}

GLuint CreateComputeProgramFromFile(const std::filesystem::path& compute_path) {
    return BuildComputeProgram({PathText(compute_path), ReadTextFile(compute_path)});
}

GLuint CreateProgram(const std::string& vertex_name, const std::string& fragment_name) {
    return BuildProgram(LoadShader(vertex_name), LoadShader(fragment_name));
}

GLuint CreateComputeProgram(const std::string& compute_name) {
    return BuildComputeProgram(LoadShader(compute_name));
}

}  // namespace glutil

#pragma once

#include <glad/glad.h>

#include <filesystem>
#include <string>

namespace glutil {

std::string ReadTextFile(const std::filesystem::path& path);
GLuint CreateProgramFromFiles(const std::filesystem::path& vertex_path, const std::filesystem::path& fragment_path);
GLuint CreateComputeProgramFromFile(const std::filesystem::path& compute_path);

// Programs from the shaders the app ships, by file name: the copies embedded at build time, or the files in
// POWDER_SHADER_DIR when that variable is set. A shader missing from the selected source is an error.
GLuint CreateProgram(const std::string& vertex_name, const std::string& fragment_name);
GLuint CreateComputeProgram(const std::string& compute_name);

}  // namespace glutil

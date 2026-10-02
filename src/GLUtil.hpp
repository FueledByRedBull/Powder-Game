#pragma once

#include <glad/glad.h>

#include <filesystem>
#include <string>

namespace glutil {

std::string ReadTextFile(const std::filesystem::path& path);
GLuint CreateProgramFromFiles(const std::filesystem::path& vertex_path, const std::filesystem::path& fragment_path);
GLuint CreateComputeProgramFromFile(const std::filesystem::path& compute_path);

}  // namespace glutil

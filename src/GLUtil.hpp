#pragma once

#include <glad/glad.h>

#include <string>

namespace glutil {

std::string ReadTextFile(const std::string& path);
GLuint CreateProgramFromFiles(const std::string& vertex_path, const std::string& fragment_path);
GLuint CreateComputeProgramFromFile(const std::string& compute_path);

}  // namespace glutil

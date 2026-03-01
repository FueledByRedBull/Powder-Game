cmake_minimum_required(VERSION 3.24)
project(PowderGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG QUIET)
find_package(glad CONFIG QUIET)

include(FetchContent)

if(NOT TARGET glfw AND NOT TARGET glfw3 AND NOT TARGET glfw::glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    FetchContent_MakeAvailable(glfw)
endif()

if(NOT TARGET glad AND NOT TARGET glad::glad)
    set(GLAD_PROFILE "core" CACHE STRING "" FORCE)
    set(GLAD_API "gl=4.3" CACHE STRING "" FORCE)
    set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
    set(GLAD_SPEC "gl" CACHE STRING "" FORCE)
    set(GLAD_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glad_src
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG v0.1.36
    )
    FetchContent_MakeAvailable(glad_src)
endif()

set(GLFW_TARGET "")
if(TARGET glfw)
    set(GLFW_TARGET glfw)
elseif(TARGET glfw3)
    set(GLFW_TARGET glfw3)
elseif(TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
endif()

if(GLFW_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not determine GLFW target name.")
endif()

set(GLAD_TARGET "")
if(TARGET glad::glad)
    set(GLAD_TARGET glad::glad)
elseif(TARGET glad)
    set(GLAD_TARGET glad)
endif()

if(GLAD_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not determine GLAD target name.")
endif()

add_executable(powder_game
    src/main.cpp
    src/PowderApp.cpp
    src/GLUtil.cpp
)

target_include_directories(powder_game PRIVATE src)
target_compile_definitions(powder_game PRIVATE POWDER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
target_link_libraries(powder_game PRIVATE OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})

if(MINGW)
    # Keep MinGW runtime linked statically to avoid extra DLL dependencies.
    target_link_options(powder_game PRIVATE -static-libgcc -static-libstdc++)
endif()

add_custom_command(
    TARGET powder_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:powder_game>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:powder_game>/shaders"
)

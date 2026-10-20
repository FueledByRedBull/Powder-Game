cmake_minimum_required(VERSION 3.24)
project(PowderGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT, /MTd), so the executable needs no VC++ redistributable; fetched GLFW and GLAD follow it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)

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
    set(GLAD_REPRODUCIBLE ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glad_src
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG v0.1.36
    )
    # GLAD 0.1.36 declares CMake 3.0; CMake 4 requires at least 3.5 policies.
    if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()
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

# Every shader is compiled into the executable; editing one regenerates the source (adding one needs a reconfigure).
file(GLOB powder_shader_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert")
set(powder_embedded_shaders "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedShaders.cpp")
add_custom_command(
    OUTPUT "${powder_embedded_shaders}"
    COMMAND ${CMAKE_COMMAND}
        "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "-DOUTPUT=${powder_embedded_shaders}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    DEPENDS ${powder_shader_files} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    COMMENT "Embedding shaders"
    VERBATIM
)

add_library(powder_gl STATIC src/GLUtil.cpp "${powder_embedded_shaders}")
target_include_directories(powder_gl PUBLIC src)
target_link_libraries(powder_gl PUBLIC OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})

add_executable(powder_game
    src/main.cpp
    src/PowderApp.cpp
)
target_link_libraries(powder_game PRIVATE powder_gl)

if(MINGW)
    # Link libgcc, libstdc++ and winpthread statically so the executable needs no MinGW DLLs.
    target_link_options(powder_game PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(powder_config_tests tests/ConfigTests.cpp)
    target_include_directories(powder_config_tests PRIVATE src)
    add_test(NAME config-parsing COMMAND powder_config_tests)
    set_tests_properties(config-parsing PROPERTIES TIMEOUT 30)

    add_executable(powder_gpu_tests tests/GpuTests.cpp)
    target_compile_definitions(powder_gpu_tests PRIVATE POWDER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    target_link_libraries(powder_gpu_tests PRIVATE powder_gl)
    foreach(test_case compile water-transfer water-p2g water-affine water-integration water-sand water-wall-density water-sparse-face water-density-term particle-capacity boundary-mask water-wall-pressure gas-pressure smoke-dissipation gas-velocity-dissipation gas-wall-transport coupling-time gas-brush-blocking fire-reaction pressure-invalid half-storage)
        add_test(NAME gpu-${test_case} COMMAND powder_gpu_tests ${test_case})
        set_tests_properties(gpu-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(powder_sand_tests tests/SandTests.cpp)
    target_compile_definitions(powder_sand_tests PRIVATE POWDER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    target_link_libraries(powder_sand_tests PRIVATE powder_gl)
    foreach(test_case transfer integration support collisions material pile coupling thin-wall occupancy repair impact rheology wall-rounding)
        add_test(NAME sand-${test_case} COMMAND powder_sand_tests ${test_case})
        set_tests_properties(sand-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()

    set_tests_properties(sand-pile PROPERTIES TIMEOUT 60)

    add_executable(powder_app_lifecycle_tests tests/AppLifecycleTests.cpp src/PowderApp.cpp)
    target_link_libraries(powder_app_lifecycle_tests PRIVATE powder_gl)
    foreach(test_case brush-input hud uniform-cache upload-alignment reset erase water-force water-surface water-mass water-pool-full gas-projection gas-quenching readback-invalid render-modes lighting water-pressure water-multigrid replay-metrics boundary sand-substeps sand-water sand-repose sand-silo sand-wet sand-domain-edge sand-sleep-conservation sand-sleep-bias sand-sleep-no-flicker sand-sleep-water sand-sleep-submerged water-pack-conservation water-pack-no-flicker water-pack-wave gas-diffusion-substeps coupling-lifecycle fire-dynamics wood-fire oil-float oil-air water-wall water-calm water-sealed-cell water-sealed-pocket gas-sealed-pocket water-stream oil-burn oil-quench gas-rhs-precision steam sand-smother gas-water water-force-range water-density water-free-slip water-free-fall)
        add_test(NAME app-${test_case} COMMAND powder_app_lifecycle_tests ${test_case})
        set_tests_properties(app-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()
    set_tests_properties(app-sand-silo app-sand-wet app-sand-sleep-conservation app-sand-sleep-bias app-sand-sleep-no-flicker app-sand-sleep-water app-sand-sleep-submerged
        app-water-pack-conservation app-water-pack-no-flicker app-water-pack-wave
        PROPERTIES TIMEOUT 180)
    add_test(NAME app-shader-package COMMAND ${CMAKE_COMMAND}
        "-DEXECUTABLE=$<TARGET_FILE:powder_game>"
        "-DSHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/shader-package-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShaderPackageTest.cmake")
    set_tests_properties(app-shader-package PROPERTIES TIMEOUT 120)
    # Painted-wall containment: no water particle may end inside a wall or past the barrier.
    foreach(scene boundary leakage)
        add_test(NAME app-replay-${scene} COMMAND ${CMAKE_COMMAND} -E env POWDER_REPLAY=${scene} $<TARGET_FILE:powder_game>)
        set_tests_properties(app-replay-${scene} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

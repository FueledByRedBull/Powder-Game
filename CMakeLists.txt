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

add_executable(powder_game
    src/main.cpp
    src/PowderApp.cpp
    src/GLUtil.cpp
)

target_include_directories(powder_game PRIVATE src)
target_link_libraries(powder_game PRIVATE OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})

if(MINGW)
    # Keep MinGW runtime linked statically to avoid extra DLL dependencies.
    target_link_options(powder_game PRIVATE -static-libgcc -static-libstdc++)
endif()

if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
endif()
set(powder_runtime_libraries "$<TARGET_RUNTIME_DLLS:powder_game>;${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")

add_custom_target(powder_shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:powder_game>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:powder_game>/shaders"
    COMMAND ${CMAKE_COMMAND}
        "-DRUNTIME_DLLS=${powder_runtime_libraries}"
        "-DDESTINATION=$<TARGET_FILE_DIR:powder_game>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeLibraries.cmake"
    DEPENDS ${GLFW_TARGET} ${GLAD_TARGET}
    COMMENT "Copying runtime shaders and libraries"
    VERBATIM
)
add_dependencies(powder_game powder_shaders)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:powder_game>/runtime-dlls.txt"
    CONTENT "$<JOIN:${powder_runtime_libraries},\n>\n")

include(CTest)
if(BUILD_TESTING)
    add_executable(powder_config_tests tests/ConfigTests.cpp)
    target_include_directories(powder_config_tests PRIVATE src)
    add_test(NAME config-parsing COMMAND powder_config_tests)
    set_tests_properties(config-parsing PROPERTIES TIMEOUT 30)

    add_executable(powder_gpu_tests tests/GpuTests.cpp src/GLUtil.cpp)
    target_include_directories(powder_gpu_tests PRIVATE src)
    target_compile_definitions(powder_gpu_tests PRIVATE POWDER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    target_link_libraries(powder_gpu_tests PRIVATE OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})
    add_dependencies(powder_gpu_tests powder_shaders)
    foreach(test_case compile water-transfer water-p2g water-affine water-integration particle-capacity boundary-mask water-wall-pressure gas-pressure smoke-dissipation gas-velocity-dissipation gas-wall-transport coupling-time gas-brush-blocking fire-reaction pressure-invalid half-storage)
        add_test(NAME gpu-${test_case} COMMAND powder_gpu_tests ${test_case})
        set_tests_properties(gpu-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(powder_sand_tests tests/SandTests.cpp src/GLUtil.cpp)
    target_include_directories(powder_sand_tests PRIVATE src)
    target_compile_definitions(powder_sand_tests PRIVATE POWDER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    target_link_libraries(powder_sand_tests PRIVATE OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})
    add_dependencies(powder_sand_tests powder_shaders)
    foreach(test_case transfer integration support collisions material pile)
        add_test(NAME sand-${test_case} COMMAND powder_sand_tests ${test_case})
        set_tests_properties(sand-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()

    set_tests_properties(sand-pile PROPERTIES TIMEOUT 60)

    add_executable(powder_app_lifecycle_tests tests/AppLifecycleTests.cpp src/PowderApp.cpp src/GLUtil.cpp)
    target_include_directories(powder_app_lifecycle_tests PRIVATE src)
    target_link_libraries(powder_app_lifecycle_tests PRIVATE OpenGL::GL ${GLFW_TARGET} ${GLAD_TARGET})
    add_dependencies(powder_app_lifecycle_tests powder_shaders)
    foreach(test_case brush-input uniform-cache upload-alignment reset erase water-force water-surface water-mass gas-projection gas-quenching readback-invalid render-modes water-pressure replay-metrics boundary sand-substeps gas-diffusion-substeps coupling-lifecycle water-force-range)
        add_test(NAME app-${test_case} COMMAND powder_app_lifecycle_tests ${test_case})
        set_tests_properties(app-${test_case} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME app-shader-package COMMAND ${CMAKE_COMMAND}
        "-DEXECUTABLE=$<TARGET_FILE:powder_game>"
        "-DSHADER_SOURCE=$<TARGET_FILE_DIR:powder_game>/shaders"
        "-DRUNTIME_DLL_MANIFEST=$<TARGET_FILE_DIR:powder_game>/runtime-dlls.txt"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/shader-package-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShaderPackageTest.cmake")
    set_tests_properties(app-shader-package PROPERTIES TIMEOUT 40)
endif()

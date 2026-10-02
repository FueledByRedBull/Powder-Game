# A relocated executable must load its own shaders even while the source tree exists.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(TEST_DIRECTORY "${TEST_DIRECTORY}/run-${run_id}")
if(EXISTS "${TEST_DIRECTORY}")
    message(FATAL_ERROR "Package test requires a fresh directory: ${TEST_DIRECTORY}")
endif()
file(STRINGS "${RUNTIME_DLL_MANIFEST}" RUNTIME_DLLS)
foreach(package_name IN ITEMS "package" "πακέτο")
    set(package_directory "${TEST_DIRECTORY}/${package_name}")
    file(MAKE_DIRECTORY "${package_directory}/unrelated-working-directory")
    file(COPY "${EXECUTABLE}" DESTINATION "${package_directory}")
    set(DESTINATION "${package_directory}")
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CopyRuntimeLibraries.cmake")
    file(COPY "${SHADER_SOURCE}" DESTINATION "${package_directory}")
    get_filename_component(executable_name "${EXECUTABLE}" NAME)
    set(fragment "${package_directory}/shaders/composite.frag")
    file(WRITE "${fragment}" "#version 430 core\n#error package_shader_probe\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env POWDER_REPLAY=water
            "${package_directory}/${executable_name}"
        WORKING_DIRECTORY "${package_directory}/unrelated-working-directory"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8
        TIMEOUT 15)
    configure_file("${SHADER_SOURCE}/composite.frag" "${fragment}" COPYONLY)
    file(WRITE "${package_directory}/shader-selection.log" "exit=${result}\n${output}\n${error}")
    string(FIND "${error}" "Shader compilation failed: ${fragment}" selected_package_shader)
    string(FIND "${error}" "package_shader_probe" compiled_probe)
    if(NOT result EQUAL 1 OR selected_package_shader EQUAL -1 OR compiled_probe EQUAL -1)
        message(FATAL_ERROR "Executable did not reject its own invalid packaged shader: exit=${result}\n${error}")
    endif()
    message(STATUS "Relocated ${package_name} selected its packaged shaders from an unrelated working directory")
endforeach()

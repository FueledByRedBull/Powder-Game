# The executable copied alone into a fresh folder runs on its embedded shaders; POWDER_SHADER_DIR replaces them
# with that directory's files, and a file missing there is an error naming it.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(TEST_DIRECTORY "${TEST_DIRECTORY}/run-${run_id}")
if(EXISTS "${TEST_DIRECTORY}")
    message(FATAL_ERROR "Package test requires a fresh directory: ${TEST_DIRECTORY}")
endif()
get_filename_component(executable_name "${EXECUTABLE}" NAME)

function(run_replay package_directory log_name)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env POWDER_REPLAY=water POWDER_REPLAY_REPEAT=1 ${ARGN}
            "${package_directory}/${executable_name}"
        WORKING_DIRECTORY "${package_directory}/unrelated-working-directory"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8
        TIMEOUT 50)
    file(WRITE "${package_directory}/${log_name}.log" "exit=${result}\n${output}\n${error}")
    set(result "${result}" PARENT_SCOPE)
    set(error "${error}" PARENT_SCOPE)
endfunction()

foreach(package_name IN ITEMS "package" "πακέτο")
    set(package_directory "${TEST_DIRECTORY}/${package_name}")
    file(MAKE_DIRECTORY "${package_directory}/unrelated-working-directory")
    file(COPY "${EXECUTABLE}" DESTINATION "${package_directory}")
    file(GLOB package_files RELATIVE "${package_directory}" "${package_directory}/*")
    if(NOT package_files STREQUAL "${executable_name};unrelated-working-directory")
        message(FATAL_ERROR "Package folder holds more than the executable: ${package_files}")
    endif()

    run_replay("${package_directory}" embedded)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Executable alone in ${package_name} failed on its embedded shaders: exit=${result}\n${error}")
    endif()

    set(override "${package_directory}/override shaders")
    file(COPY "${SHADER_SOURCE}/" DESTINATION "${override}")
    set(fragment "${override}/composite.frag")
    file(WRITE "${fragment}" "#version 430 core\n#error package_shader_probe\n")
    run_replay("${package_directory}" override "POWDER_SHADER_DIR=${override}")
    string(FIND "${error}" "Shader compilation failed: ${fragment}" selected_override)
    string(FIND "${error}" "package_shader_probe" compiled_probe)
    if(NOT result EQUAL 1 OR selected_override EQUAL -1 OR compiled_probe EQUAL -1)
        message(FATAL_ERROR "POWDER_SHADER_DIR did not select ${fragment}: exit=${result}\n${error}")
    endif()

    file(REMOVE "${fragment}")
    run_replay("${package_directory}" missing "POWDER_SHADER_DIR=${override}")
    string(FIND "${error}" "Failed to open file: ${fragment}" reported_missing)
    if(NOT result EQUAL 1 OR reported_missing EQUAL -1)
        message(FATAL_ERROR "A missing override shader did not fail by name: exit=${result}\n${error}")
    endif()
    message(STATUS "${package_name}: embedded shaders run; POWDER_SHADER_DIR selects its files and rejects a missing one")
endforeach()

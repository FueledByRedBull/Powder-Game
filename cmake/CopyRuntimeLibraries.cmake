cmake_minimum_required(VERSION 3.24)

file(MAKE_DIRECTORY "${DESTINATION}")
list(FILTER RUNTIME_DLLS EXCLUDE REGEX "^$")
foreach(library IN LISTS RUNTIME_DLLS)
    get_filename_component(filename "${library}" NAME)
    file(COPY_FILE "${library}" "${DESTINATION}/${filename}" ONLY_IF_DIFFERENT)
endforeach()

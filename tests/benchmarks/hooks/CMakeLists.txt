cmake_minimum_required(VERSION 3.15)

project(unicorn_hook_benchmark C)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

get_filename_component(_default_source_dir
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(UNICORN_SOURCE_DIR "${_default_source_dir}" CACHE PATH
    "Path to the Unicorn source tree")
set(UNICORN_BUILD_DIR "${UNICORN_SOURCE_DIR}/build" CACHE PATH
    "Path to an existing CMake-produced Unicorn build")

set(_unicorn_hints
    "${UNICORN_BUILD_DIR}"
    "${UNICORN_BUILD_DIR}/Release"
    "${UNICORN_BUILD_DIR}/RelWithDebInfo"
    "${UNICORN_BUILD_DIR}/Debug")

find_path(UNICORN_INCLUDE_DIR unicorn/unicorn.h
    HINTS "${UNICORN_SOURCE_DIR}/include")

if(WIN32)
    if(MSVC)
        find_library(UNICORN_LIBRARY NAMES unicorn-import unicorn
            HINTS ${_unicorn_hints})
    else()
        find_library(UNICORN_LIBRARY NAMES unicorn
            HINTS ${_unicorn_hints})
    endif()
    find_file(UNICORN_RUNTIME NAMES unicorn.dll
        HINTS ${_unicorn_hints})
    if(NOT UNICORN_LIBRARY OR NOT UNICORN_RUNTIME)
        message(FATAL_ERROR
            "UNICORN_BUILD_DIR must point to a shared Unicorn build")
    endif()
    add_library(unicorn_built SHARED IMPORTED)
    set_target_properties(unicorn_built PROPERTIES
        IMPORTED_IMPLIB "${UNICORN_LIBRARY}"
        IMPORTED_LOCATION "${UNICORN_RUNTIME}")
else()
    find_library(UNICORN_LIBRARY NAMES unicorn HINTS ${_unicorn_hints})
    if(NOT UNICORN_LIBRARY)
        message(FATAL_ERROR
            "No Unicorn library found below UNICORN_BUILD_DIR")
    endif()
    add_library(unicorn_built UNKNOWN IMPORTED)
    set_target_properties(unicorn_built PROPERTIES
        IMPORTED_LOCATION "${UNICORN_LIBRARY}")
endif()

if(NOT UNICORN_INCLUDE_DIR)
    message(FATAL_ERROR "No Unicorn headers found below UNICORN_SOURCE_DIR")
endif()

add_executable(hook_bench benchmark.c)
target_compile_features(hook_bench PRIVATE c_std_99)
target_include_directories(hook_bench SYSTEM PRIVATE "${UNICORN_INCLUDE_DIR}")
target_link_libraries(hook_bench PRIVATE unicorn_built)

set_target_properties(hook_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_configuration}" _configuration_upper)
    set_target_properties(hook_bench PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_configuration_upper}"
        "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(MSVC)
    target_compile_options(hook_bench PRIVATE /W4)
else()
    target_compile_options(hook_bench PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    add_custom_command(TARGET hook_bench POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${UNICORN_RUNTIME}" "$<TARGET_FILE_DIR:hook_bench>")
endif()

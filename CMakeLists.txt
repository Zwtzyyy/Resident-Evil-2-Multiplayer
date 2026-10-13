cmake_minimum_required(VERSION 3.20)
project(re2r_mp LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC (Visual Studio 2022).")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../dist/plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../dist/plugins")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/lib")
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/../dist/plugins")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/../dist/plugins")
endforeach()

add_subdirectory(src)
add_subdirectory(src/plugins)

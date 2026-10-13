cmake_minimum_required(VERSION 3.20)
project(re2r_mp_plugin CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC (Visual Studio 2022) for a drop-in REFramework plugin.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(re2r_mp SHARED plugin_main.cpp)
target_compile_definitions(re2r_mp PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
# so `#include "reframework/API.h"` resolves against re2r_mp/
target_include_directories(re2r_mp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(re2r_mp PRIVATE ws2_32)
set_target_properties(re2r_mp PROPERTIES PREFIX "" OUTPUT_NAME "re2r_mp")

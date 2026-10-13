cmake_minimum_required(VERSION 3.20)
project(re2r_mp_helpers C)

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC (Visual Studio 2022) for a drop-in REFramework plugin.")
endif()

set(CMAKE_C_STANDARD 11)

add_library(re2r_mp_pads SHARED re2r_mp_pads/pads.c)
target_compile_definitions(re2r_mp_pads PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(re2r_mp_pads PRIVATE winmm advapi32)
set_target_properties(re2r_mp_pads PROPERTIES PREFIX "" OUTPUT_NAME "re2r_mp_pads")

add_library(re2r_mp_sfx SHARED re2r_mp_sfx/sfx.c)
target_compile_definitions(re2r_mp_sfx PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(re2r_mp_sfx PRIVATE winmm)
set_target_properties(re2r_mp_sfx PROPERTIES PREFIX "" OUTPUT_NAME "re2r_mp_sfx")

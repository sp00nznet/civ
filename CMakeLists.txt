cmake_minimum_required(VERSION 3.20)
project(civ-recomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)

# MSVC-specific settings
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# MinGW/GCC settings
if(MINGW OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(tools)

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

# ─── Analysis tools ───
add_subdirectory(tools)

# ─── SDL2 (from vcpkg or system) ───
find_package(SDL2 CONFIG REQUIRED)

# ─── Include paths ───
include_directories(${CMAKE_SOURCE_DIR}/include)

# The recompiled code lives in RecompiledFuncs/ and needs access to
# our headers. It also needs the generated master header.
include_directories(${CMAKE_SOURCE_DIR}/RecompiledFuncs)

# ─── HAL + DOS compatibility library ───
add_library(civ_hal STATIC
    src/hal/video.c
    src/hal/input.c
    src/hal/timer.c
    src/recomp/cpu.c
    src/recomp/dos_compat.c
    src/recomp/startup.c
)
target_include_directories(civ_hal PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs
)

# ─── SDL2 platform library ───
add_library(civ_platform STATIC
    src/platform/sdl_platform.c
)
target_include_directories(civ_platform PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(civ_platform PUBLIC SDL2::SDL2 SDL2::SDL2main)

# ─── Recompiled game code ───
# Collect all generated .c files from the recompiler output
file(GLOB RECOMP_SOURCES
    "${CMAKE_SOURCE_DIR}/RecompiledFuncs/civ_recomp_*.c"
    "${CMAKE_SOURCE_DIR}/RecompiledFuncs/civ_stubs.c"
    "${CMAKE_SOURCE_DIR}/RecompiledFuncs/civ_aliases.c"
    "${CMAKE_SOURCE_DIR}/RecompiledFuncs/civ_impl.c"
)

if(RECOMP_SOURCES)
    add_library(civ_recomp STATIC ${RECOMP_SOURCES})
    target_include_directories(civ_recomp PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/RecompiledFuncs
    )
    # Recompiled code calls DOS/BIOS handlers
    target_link_libraries(civ_recomp PUBLIC civ_hal)

    # Disable some warnings for auto-generated code
    if(MSVC)
        target_compile_options(civ_recomp PRIVATE
            /W2                   # Lower warning level for generated code
            /wd4101 /wd4102       # Unreferenced locals, unreferenced labels
            /wd4244 /wd4267       # Conversion warnings
            /wd4146               # Unary minus on unsigned
        )
    else()
        target_compile_options(civ_recomp PRIVATE
            -Wno-unused-variable
            -Wno-unused-label
            -Wno-unused-but-set-variable
        )
    endif()

    # ─── Main executable ───
    add_executable(civ src/main.c)
    target_include_directories(civ PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/RecompiledFuncs
    )
    target_link_libraries(civ PRIVATE
        civ_recomp
        civ_hal
        civ_platform
    )

    # On Windows, use WinMain entry for SDL2
    if(WIN32)
        target_link_libraries(civ PRIVATE SDL2::SDL2main)
    endif()

    message(STATUS "Found ${CMAKE_SOURCE_DIR}/RecompiledFuncs/ - building main executable")
else()
    message(STATUS "No recompiled sources found - run tools/recomp/recomp.py first")
    message(STATUS "  py -3 tools/recomp/recomp.py <path-to-CIV.EXE> RecompiledFuncs")
endif()

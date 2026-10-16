cmake_minimum_required(VERSION 3.16)
project(EternalDarkness C)

# Eternal Darkness: Sanity's Requiem, statically recompiled. The game code comes
# from the user's own disc (tools/generate.py writes generated/); this tree
# holds only the host and the tools.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ED_ENABLE_AURORA "Build with the Aurora graphics/input/audio backend" ON)
set(ED_GXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/.deps/GXRuntime" CACHE PATH "GXRuntime checkout")
set(ED_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "tools/generate.py output")

if(NOT EXISTS "${ED_GENERATED_DIR}/game_data.h")
    message(FATAL_ERROR
        "Recompiled game code not found at ${ED_GENERATED_DIR}.\n"
        "Generate it from your disc image first:\n"
        "  python3 tools/generate.py /path/to/your/disc.iso")
endif()
file(STRINGS "${ED_GENERATED_DIR}/game_id.txt" ED_GAME_ID LIMIT_COUNT 1)

set(GXRUNTIME_ENABLE_AURORA ${ED_ENABLE_AURORA} CACHE BOOL "" FORCE)
# The launcher and in-game menu are RmlUi documents (res/ui).
set(AURORA_ENABLE_RMLUI ${ED_ENABLE_AURORA} CACHE BOOL "" FORCE)
set(GXRUNTIME_AURORA_DIR "${ED_GXRUNTIME_DIR}/graphics/aurora" CACHE PATH "" FORCE)
add_subdirectory("${ED_GXRUNTIME_DIR}" "${CMAKE_BINARY_DIR}/GXRuntime")

file(GLOB ED_CHUNKS CONFIGURE_DEPENDS "${ED_GENERATED_DIR}/chunks/*.c")
list(LENGTH ED_CHUNKS ED_CHUNK_COUNT)
message(STATUS "Eternal Darkness ${ED_GAME_ID}: ${ED_CHUNK_COUNT} recompiled chunks")

# Host code that never sees the disc (one build serves every disc), and the
# host code built with it (game_data.h, generated.h, ED_GAME_ID).
set(ED_HOST_SOURCES
    runtime/host/mmio.c
    runtime/host/dtk.c
    runtime/host/interrupt.c
    runtime/host/settings.c
    runtime/host/compat.c
)
set(ED_DISC_SOURCES
    runtime/host/audio.c
    runtime/host/hle.c
    runtime/host/sdk_map.c
    runtime/host/quickstate.c
    runtime/host/main.c
    "${ED_GENERATED_DIR}/generated.c"
    ${ED_CHUNKS}
)
if(ED_ENABLE_AURORA)
    # Launcher and in-game menu (RmlUi documents in res/ui).
    list(APPEND ED_HOST_SOURCES runtime/host/frontend.cpp runtime/host/controls.cpp
         runtime/host/buttons.cpp runtime/host/fonts.cpp runtime/host/texpack.cpp)
endif()

# ED_SPLIT_HOST (the Mac .app): the host is a shared library, libedhost, and
# the executable holds only what is built from the player's disc, which the
# app compiles and links against libedhost on first launch (tools/mac).
option(ED_SPLIT_HOST "Build the host as libedhost and link the game against it" OFF)
if(ED_SPLIT_HOST)
    add_library(edhost SHARED ${ED_HOST_SOURCES})
    set(ED_HOST_TARGET edhost)
    add_executable(EternalDarkness ${ED_DISC_SOURCES})
    target_link_libraries(EternalDarkness PRIVATE edhost)
else()
    add_executable(EternalDarkness ${ED_HOST_SOURCES} ${ED_DISC_SOURCES})
    set(ED_HOST_TARGET EternalDarkness)
endif()
# The generated chunks are huge switch-heavy units: no warnings, and keep the
# optimiser from spending minutes on each.
set_source_files_properties(${ED_CHUNKS} "${ED_GENERATED_DIR}/generated.c"
    PROPERTIES COMPILE_OPTIONS "-w")
# Chunk code addresses the one guest CPU (main.c dolrecomp_cpu) by name rather
# than through a pointer the register allocator spills (generated.h), and
# CodeGenPrepare may fold the CPU-state field addresses into each access even
# though every field has thousands of users in a chunk (its default stops at
# 100 and leaves them hoisted and spilled).
set_property(SOURCE ${ED_CHUNKS} APPEND PROPERTY COMPILE_DEFINITIONS
    DOLRECOMP_STATIC_CPU=dolrecomp_cpu)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${ED_CHUNKS} APPEND PROPERTY COMPILE_OPTIONS
        -mllvm -cgp-max-address-users-to-scan=100000)
endif()
target_include_directories(EternalDarkness PRIVATE
    "${CMAKE_SOURCE_DIR}/runtime"
    "${ED_GENERATED_DIR}")
target_compile_definitions(EternalDarkness PRIVATE ED_GAME_ID="${ED_GAME_ID}")
# Split: the runtime lives in libedhost; the game takes only its headers (a
# second copy of the runtime in the executable would have state of its own).
if(ED_SPLIT_HOST)
    set(ED_RT "$<COMPILE_ONLY:GXRuntime::runtime>")
    set(ED_AURORA_RT "$<COMPILE_ONLY:GXRuntime::aurora>")
else()
    set(ED_RT GXRuntime::runtime)
    set(ED_AURORA_RT GXRuntime::aurora)
endif()
target_link_libraries(EternalDarkness PRIVATE ${ED_RT})

include(CheckLibraryExists)
check_library_exists(m sqrt "" ED_HAVE_LIBM)
if(ED_HAVE_LIBM)
    target_link_libraries(EternalDarkness PRIVATE m)
endif()

if(ED_ENABLE_AURORA)
    enable_language(CXX)
    target_compile_definitions(EternalDarkness PRIVATE ED_AURORA=1 AURORA_ENABLE_RMLUI=1)
    target_compile_features(EternalDarkness PRIVATE cxx_std_20)
    target_link_libraries(EternalDarkness PRIVATE ${ED_AURORA_RT})
    set_target_properties(EternalDarkness PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(ED_SPLIT_HOST)
    target_include_directories(edhost PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
    target_compile_definitions(edhost PRIVATE ED_AURORA=1 AURORA_ENABLE_RMLUI=1)
    target_compile_features(edhost PRIVATE cxx_std_20)
    set_target_properties(edhost PROPERTIES LINKER_LANGUAGE CXX)
    # Everything the game's code calls must be in the library, not only what
    # the host itself uses: the runtime archives go in whole. What the host
    # calls in the game's half resolves when the game loads it.
    target_link_libraries(edhost PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,GXRuntime::runtime>"
        GXRuntime::aurora rmlui rmlui_backends PNG::PNG)
    target_link_options(edhost PRIVATE
        "LINKER:-undefined,dynamic_lookup" "LINKER:-no_fixup_chains")
    if(ED_HAVE_LIBM)
        target_link_libraries(edhost PRIVATE m)
    endif()
elseif(ED_ENABLE_AURORA)
    target_link_libraries(EternalDarkness PRIVATE rmlui rmlui_backends PNG::PNG)
endif()

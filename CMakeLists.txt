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

add_executable(EternalDarkness
    runtime/host/mmio.c
    runtime/host/audio.c
    runtime/host/dtk.c
    runtime/host/interrupt.c
    runtime/host/hle.c
    runtime/host/sdk_map.c
    runtime/host/quickstate.c
    runtime/host/settings.c
    runtime/host/main.c
    runtime/host/compat.c
    "${ED_GENERATED_DIR}/generated.c"
    ${ED_CHUNKS}
)
# The generated chunks are huge switch-heavy units: no warnings, and keep the
# optimiser from spending minutes on each.
set_source_files_properties(${ED_CHUNKS} "${ED_GENERATED_DIR}/generated.c"
    PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(EternalDarkness PRIVATE
    "${CMAKE_SOURCE_DIR}/runtime"
    "${ED_GENERATED_DIR}")
target_compile_definitions(EternalDarkness PRIVATE ED_GAME_ID="${ED_GAME_ID}")
target_link_libraries(EternalDarkness PRIVATE GXRuntime::runtime)

include(CheckLibraryExists)
check_library_exists(m sqrt "" ED_HAVE_LIBM)
if(ED_HAVE_LIBM)
    target_link_libraries(EternalDarkness PRIVATE m)
endif()

if(ED_ENABLE_AURORA)
    enable_language(CXX)
    target_link_libraries(EternalDarkness PRIVATE GXRuntime::aurora)
    target_compile_features(EternalDarkness PRIVATE cxx_std_20)
    target_compile_definitions(EternalDarkness PRIVATE ED_AURORA=1)
    set_target_properties(EternalDarkness PROPERTIES LINKER_LANGUAGE CXX)
    # Launcher and in-game menu (RmlUi documents in res/ui).
    target_sources(EternalDarkness PRIVATE runtime/host/frontend.cpp runtime/host/controls.cpp
                   runtime/host/buttons.cpp)
    target_compile_definitions(EternalDarkness PRIVATE AURORA_ENABLE_RMLUI=1)
    target_link_libraries(EternalDarkness PRIVATE rmlui rmlui_backends PNG::PNG)
endif()

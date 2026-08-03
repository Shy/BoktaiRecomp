cmake_minimum_required(VERSION 3.20)
project(BoktaiRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────────────
# BoktaiRecomp — static recompilation of Boktai: The Sun Is in Your Hand.
#
# Per-variant scaffold (EmeraldRecomp pattern) so the JP release can drop
# in as a second variants/<name>/ directory later.
#
#   Target         Game                 ROM sha1   Debug port
#   BoktaiRecomp   Boktai 1 (USA)       71643262   19893
#
# Boktai's cartridge carries an EEPROM save, a Seiko S-3511A RTC, AND a
# solar sensor sharing the cartridge GPIO port. Save + RTC are handled by
# the engine already; the solar sensor is the piece under development.
# ─────────────────────────────────────────────────────────────────────

# Prefer the submodule; fall back to a sibling checkout so a developer working on
# the engine alongside this repo does not have to point the cache at it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp/CMakeLists.txt")
    set(GBARECOMP_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp")
else()
    set(GBARECOMP_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../gbarecomp")
endif()
set(GBARECOMP_ROOT "${GBARECOMP_ROOT_DEFAULT}"
    CACHE PATH "Path to the gbarecomp platform-core checkout")

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not point to a gbarecomp checkout:\n"
        "  ${GBARECOMP_ROOT}\n"
        "Point it at the engine checkout (default ../gbarecomp) or pass\n"
        "-DGBARECOMP_ROOT=<path> at configure time.")
endif()

# ─────────────────────────────────────────────────────────────────────
# recomp-ui — the Dear ImGui pre-boot launcher AND the in-game overlay menu.
#
# Turning this on is what gives players a GUI instead of undocumented
# keystrokes: ROM/BIOS pickers, window scale, fullscreen, volume, the
# linear-filter toggle, and — because recomp-ui already ships a GBA profile
# with a rebind page — full controller/keyboard remapping plus a hotkeys
# panel. Both write the same config.ini / keybinds.ini that host_window.cpp
# reads, so launcher-set and hand-edited bindings cannot disagree.
#
# GBARECOMP_RUNTIME_UI_ROOT MUST be set before add_subdirectory(gbarecomp):
# the engine reads it at configure time to decide whether to compile the
# in-game overlay (GBARECOMP_RUNTIME_UI) at all.
# ─────────────────────────────────────────────────────────────────────
option(GBAGAME_RECOMP_UI "Build the recomp-ui launcher + in-game menu" ON)

if(GBAGAME_RECOMP_UI)
    # Prefer a submodule inside the game, fall back to a sibling checkout.
    foreach(_candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
            "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui")
        if(EXISTS "${_candidate}/recomp_ui.cmake" AND NOT RECOMP_UI_FOUND_ROOT)
            set(RECOMP_UI_FOUND_ROOT "${_candidate}")
        endif()
    endforeach()
    if(NOT RECOMP_UI_FOUND_ROOT)
        message(FATAL_ERROR
            "GBAGAME_RECOMP_UI=ON but no recomp-ui checkout found at\n"
            "  ${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui\n"
            "  ${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui\n"
            "Clone it (https://github.com/mstan/recomp-ui) or configure with "
            "-DGBAGAME_RECOMP_UI=OFF for the plain SDL2 path.")
    endif()
    set(GBARECOMP_RUNTIME_UI_ROOT "${RECOMP_UI_FOUND_ROOT}" CACHE PATH
        "Path to the recomp-ui checkout (launcher + in-game menu)")
    message(STATUS "BoktaiRecomp: recomp-ui at ${GBARECOMP_RUNTIME_UI_ROOT}")

    # Include here, in the top-level scope, and BEFORE add_subdirectory. The
    # helpers resolve their console-asset manifests from variables set at
    # include time, and CMake variables do not propagate out of a subdirectory
    # — so relying on gbarecomp's own include would leave that manifest empty
    # in this scope and reject CONSOLE gba as unknown.
    set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}")
    include("${GBARECOMP_RUNTIME_UI_ROOT}/recomp_ui.cmake")
endif()

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

# ─────────────────────────────────────────────────────────────────────
# Real-world sunlight (src/solar_weather.*): postal code -> coordinates ->
# live irradiance -> sensor brightness. libcurl is OPTIONAL — without it the
# file still compiles, --solar-zip reports itself unavailable, and the
# number-row debug keys remain the light source. macOS and most Linux
# distributions ship libcurl; on Windows it comes from vcpkg.
# ─────────────────────────────────────────────────────────────────────
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "BoktaiRecomp: libcurl found — weather-driven solar sensor enabled")
else()
    message(WARNING
        "BoktaiRecomp: libcurl NOT found. --solar-zip will be unavailable; "
        "the solar sensor falls back to the number-row debug keys.")
endif()

# ─────────────────────────────────────────────────────────────────────
# The builder (tools/builder/) — what a player actually downloads.
#
# A recompiled binary is translated cartridge code and cannot be distributed,
# so this repository ships the machinery that PRODUCES one instead: a ROM-free
# app that compiles the game on the player's own machine from their own dump,
# then execs it on every launch after that. See docs/BUILDER.md.
#
# It carries no generated sources, so it configures and links with no ROM
# present — which is what makes an automated release possible at all.
# GBAGAME_BUILDER_ONLY skips the game target entirely for exactly that case.
# ─────────────────────────────────────────────────────────────────────
option(GBAGAME_BUILDER "Build the first-run builder app" ON)
option(GBAGAME_BUILDER_ONLY
       "Build ONLY the builder — skip the game target (no ROM needed)" OFF)

add_executable(verify_rom_hash tools/verify_rom_hash/main.cpp)
target_include_directories(verify_rom_hash PRIVATE "${GBARECOMP_ROOT}/src/gba")
target_link_libraries(verify_rom_hash PRIVATE gbarecomp_gba)

# add_gba_variant(<target> <name> <port> <win_title> <game_name> <sha1> <crc32>)
function(add_gba_variant TARGET NAME PORT WIN_TITLE GAME_NAME ROM_SHA1 ROM_CRC32)
    set(VARIANT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/variants/${NAME}")
    if(EXISTS "${VARIANT_DIR}/generated/recompiled.cpp")
        message(FATAL_ERROR
            "${NAME}: stale monolithic generated/recompiled.cpp found. "
            "Regenerate with the current gba_recompile before building.")
    endif()
    file(GLOB VARIANT_GENERATED CONFIGURE_DEPENDS
        "${VARIANT_DIR}/generated/*.c"
        "${VARIANT_DIR}/generated/*.cpp")

    if(NOT VARIANT_GENERATED)
        message(FATAL_ERROR
            "${NAME}: variants/${NAME}/generated/ is empty. Run the "
            "recompiler first — see README.md 'Recompile'.")
    endif()

    add_executable(${TARGET} src/main.cpp src/solar_weather.cpp src/game_ui.cpp
                   ${VARIANT_GENERATED})

    if(CURL_FOUND)
        target_link_libraries(${TARGET} PRIVATE CURL::libcurl)
        target_compile_definitions(${TARGET} PRIVATE BOKTAI_HAVE_CURL=1)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET} PRIVATE Threads::Threads)

    # Keep generated guest translation units at a bounded optimization and
    # debug-info cost. gba_recompile emits stable recompiled_NNN.cpp shards.
    set(VARIANT_RECOMPILED_SOURCES ${VARIANT_GENERATED})
    list(FILTER VARIANT_RECOMPILED_SOURCES INCLUDE REGEX
        "/recompiled_[0-9]+\\.cpp$")
    if(VARIANT_RECOMPILED_SOURCES)
        set_source_files_properties(${VARIANT_RECOMPILED_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-O1;-g0")
    endif()

    target_include_directories(${TARGET} PRIVATE
        "${GBARECOMP_ROOT}/src/armv4t"
        "${GBARECOMP_ROOT}/src/gba"
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/debug"
        src)

    # The four engine archives are mutually recursive. GNU ld needs an
    # explicit --start-group; Apple's ld64 does not support that flag, so
    # repeat the list instead (equivalent effect, portable).
    if(APPLE)
        target_link_libraries(${TARGET} PRIVATE
            gbarecomp_runtime gbarecomp_debug gbarecomp_gba gbarecomp_armv4t
            gbarecomp_runtime gbarecomp_debug gbarecomp_gba gbarecomp_armv4t)
    else()
        target_link_libraries(${TARGET} PRIVATE
            "-Wl,--start-group"
            gbarecomp_runtime
            gbarecomp_debug
            gbarecomp_gba
            gbarecomp_armv4t
            "-Wl,--end-group")
    endif()

    target_compile_definitions(${TARGET} PRIVATE
        GBARECOMP_DEFAULT_DEBUG_PORT=${PORT}
        GBARECOMP_WINDOW_TITLE="${WIN_TITLE}"
        GBARECOMP_DEFAULT_GAME_CONFIG="variants/${NAME}/game.toml"
        GBARECOMP_BUILTIN_NAME="${GAME_NAME}"
        GBARECOMP_BUILTIN_SHA1="${ROM_SHA1}"
        GBARECOMP_BUILTIN_CRC32=${ROM_CRC32}
        GBARECOMP_BUILTIN_REGION="USA")

    if(GBAGAME_RECOMP_UI)
        # Defines RECOMP_LAUNCHER (un-gating launcher_seam.h in main.cpp),
        # adds the recomp-ui include dirs, and stages the gba console assets.
        set(VARIANT_BOXART "${VARIANT_DIR}/launcher/boxart.tga")
        if(EXISTS "${VARIANT_BOXART}")
            recomp_target_launcher_ui(${TARGET} CONSOLE gba
                BOXART "${VARIANT_BOXART}"
                BOXART_NAME "boxart_${NAME}.tga")
            target_compile_definitions(${TARGET} PRIVATE
                GBARECOMP_BOXART="assets/img/boxart_${NAME}.tga")
        else()
            recomp_target_launcher_ui(${TARGET} CONSOLE gba)
        endif()
    endif()

    # Large stack reserve — Win64 SEH unwind tables scale with the
    # recompiled function count. macOS/Linux set thread stacks at runtime.
    if(MSVC)
        target_link_options(${TARGET} PRIVATE /STACK:268435456)
    elseif(MINGW OR WIN32)
        target_link_options(${TARGET} PRIVATE -Wl,--stack,268435456)
    endif()
endfunction()

# Boktai 1 — USA, sha1 71643262… (verified clean dump)
if(NOT GBAGAME_BUILDER_ONLY)
    add_gba_variant(BoktaiRecomp boktai1_usa 19893 "BoktaiRecomp"
        "Boktai - The Sun Is in Your Hand (USA)"
        "7164326283df46a3941ec7b6ceca889cbc40e660" 0xE715AC45)
endif()

if(GBAGAME_BUILDER)
    if(NOT GBAGAME_RECOMP_UI)
        message(FATAL_ERROR
            "GBAGAME_BUILDER needs GBAGAME_RECOMP_UI=ON: the builder's window, "
            "its file pickers and its SHA-1 all come from recomp-ui.")
    endif()

    # SDL2 is the host's to provide (recomp_target_launcher_ui contributes
    # OpenGL but deliberately not SDL, whose provenance varies). Config mode
    # covers Homebrew and Debian; the find_path/find_library pair covers MSYS2
    # and anything hand-installed.
    find_package(SDL2 QUIET)
    if(TARGET SDL2::SDL2)
        set(BUILDER_SDL2_LIBS SDL2::SDL2)
        set(BUILDER_SDL2_INCLUDES "")
    else()
        find_path(BUILDER_SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
        find_library(BUILDER_SDL2_LIBRARY NAMES SDL2 SDL2-2.0)
        if(NOT BUILDER_SDL2_INCLUDE_DIR OR NOT BUILDER_SDL2_LIBRARY)
            message(FATAL_ERROR
                "GBAGAME_BUILDER=ON but SDL2 was not found. Install it "
                "(brew install sdl2 / apt install libsdl2-dev / pacman -S "
                "mingw-w64-x86_64-SDL2) or configure with "
                "-DGBAGAME_BUILDER=OFF.")
        endif()
        set(BUILDER_SDL2_LIBS "${BUILDER_SDL2_LIBRARY}")
        set(BUILDER_SDL2_INCLUDES "${BUILDER_SDL2_INCLUDE_DIR}")
    endif()

    add_executable(BoktaiBuilder
        tools/builder/builder_main.cpp
        tools/builder/builder_core.cpp)
    target_include_directories(BoktaiBuilder PRIVATE
        tools/builder ${BUILDER_SDL2_INCLUDES})
    target_link_libraries(BoktaiBuilder PRIVATE ${BUILDER_SDL2_LIBS})
    find_package(Threads REQUIRED)
    target_link_libraries(BoktaiBuilder PRIVATE Threads::Threads)

    # Brings recomp-ui's sources, the vendored Dear ImGui, tinyfiledialogs (the
    # native file pickers), sha1.c (the asset fingerprints) and OpenGL. CONSOLE
    # gba also stages the launcher assets next to the exe, which the builder's
    # own window does not use — it draws with ImGui's built-in font so a missing
    # assets/ directory can never block a player from building — but the GAME it
    # produces does.
    recomp_target_launcher_ui(BoktaiBuilder CONSOLE gba)

    # tools/builder/*.cpp include "sha1.h" and "third_party/tinyfiledialogs.h"
    # by those names; recomp_target_launcher_ui already puts src/ and
    # src/common/ on the include path, so nothing further is needed here.
    # The builder's two pure pieces, tested without a window, a ROM or a
    # compiler. Links sha1.c directly rather than going through
    # recomp_target_launcher_ui: a test does not need the launcher, ImGui or
    # staged assets.
    add_executable(test_builder_core
        tools/builder/test_builder_core.cpp
        tools/builder/builder_core.cpp
        "${GBARECOMP_RUNTIME_UI_ROOT}/src/common/sha1.c")
    target_include_directories(test_builder_core PRIVATE
        tools/builder
        "${GBARECOMP_RUNTIME_UI_ROOT}/src"
        "${GBARECOMP_RUNTIME_UI_ROOT}/src/common")
    target_link_libraries(test_builder_core PRIVATE Threads::Threads)
    enable_testing()
    add_test(NAME builder_core COMMAND test_builder_core)

    message(STATUS "BoktaiRecomp: builder app enabled (target BoktaiBuilder)")
endif()

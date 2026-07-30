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
add_gba_variant(BoktaiRecomp boktai1_usa 19893 "BoktaiRecomp"
    "Boktai - The Sun Is in Your Hand (USA)"
    "7164326283df46a3941ec7b6ceca889cbc40e660" 0xE715AC45)

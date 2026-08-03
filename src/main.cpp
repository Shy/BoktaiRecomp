// main.cpp — BoktaiRecomp entry point.
//
// The build picks the variant via compile-defs set in CMakeLists.txt
// (add_gba_variant):
//
//   GBARECOMP_BUILTIN_NAME         e.g. "Boktai - The Sun Is in Your Hand (USA)"
//   GBARECOMP_BUILTIN_SHA1         expected ROM sha1 (hash gate)
//   GBARECOMP_BUILTIN_CRC32        CRC32 of the same pinned dump
//   GBARECOMP_DEFAULT_GAME_CONFIG  variants/<name>/game.toml
//   GBARECOMP_DEFAULT_DEBUG_PORT / GBARECOMP_WINDOW_TITLE  (read by runtime)
//
// Every gbarecomp game binary takes BOTH a BIOS and a ROM at launch (see
// ../gbarecomp/PRINCIPLES.md "BIOS is sacred"). The CLI accepts:
//
//   BoktaiRecomp [--bios <path>] [--rom <path>] [game.toml]
//
// All are optional on the command line; missing values come from game.toml.
// Hashes are verified before any guest code runs.
//
// Unlike the Gen3 Pokémon targets, Boktai needs no g_runtime_ram_dispatch_hook:
// those games copy position-independent FLASH routines to moving stack slots,
// whereas Boktai saves to EEPROM (EEPROM_V at 0x584508), which the engine's
// save model services on the bus without RAM-resident guest code.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game_ui.h"
#include "runtime.h"
#include "solar_weather.h"

#ifndef GBARECOMP_BUILTIN_NAME
#define GBARECOMP_BUILTIN_NAME "GBA cartridge"
#endif
#ifndef GBARECOMP_BUILTIN_SHA1
#define GBARECOMP_BUILTIN_SHA1 ""
#endif
#ifndef GBARECOMP_WINDOW_TITLE
#define GBARECOMP_WINDOW_TITLE "gbarecomp"
#endif
#ifndef GBARECOMP_BUILTIN_CRC32
#define GBARECOMP_BUILTIN_CRC32 0
#endif
#ifndef GBARECOMP_BUILTIN_REGION
#define GBARECOMP_BUILTIN_REGION ""
#endif
#ifndef GBARECOMP_BOXART
#define GBARECOMP_BOXART ""
#endif

// The pre-boot launcher. launcher_seam.h is header-only and inert unless
// recomp_target_launcher_ui() defined RECOMP_LAUNCHER, so this include is
// unconditional and the plain SDL2 build is unaffected.
#include "launcher_seam.h"

namespace {

void print_usage() {
    std::printf(
        "%s [--bios <path>] [--rom <path>] [game.toml]\n"
        "\n"
        "Both BIOS and ROM are required (either via flags or via the\n"
        "[bios] / [rom] sections of game.toml). The runtime refuses\n"
        "to start unless both hash-verify.\n"
        "\n"
        "Solar sensor (the Gun del Sol charges from real sunlight):\n"
        "  --solar-zip <code>      poll this postal code's live irradiance\n"
        "  --solar-country <cc>    country for the code (default us)\n"
        "  --solar-full-sun <wm2>  W/m^2 that fills the gauge (default 850;\n"
        "                          lower it at high latitude or in winter)\n"
        "  --solar-diffuse-weight <f>  how much diffuse skylight counts vs\n"
        "                          direct sun (default 0.10). The gauge tracks\n"
        "                          sun, not sky brightness, so dense overcast\n"
        "                          reads near zero; 1.0 counts bright cloud as\n"
        "                          sunshine the way plain GHI used to\n"
        "  --solar-poll <seconds>  interval between polls (default 600)\n"
        "  --solar-verbose         log every reading\n"
        "Each has a BOKTAI_SOLAR_* environment equivalent (ZIP, COUNTRY,\n"
        "FULL_SUN, POLL, VERBOSE). Without a postal code no network request\n"
        "is ever made. To set light by hand instead, bind SolarBrighter /\n"
        "SolarDimmer / SolarLive in the launcher's Hotkeys panel -- they are\n"
        "unbound by default -- or use the in-game menu (Esc).\n"
        "\n"
        "Presentation:\n"
        "  --touch-ui              near-full-screen panels and larger hit\n"
        "                         targets, for a handheld touchscreen such as a\n"
        "                         Steam Deck (BOKTAI_TOUCH_UI=1); --no-touch-ui\n"
        "                         forces the desktop presentation back on\n"
        "A sharp scaler (integer prescale + linear finish) is offered as a\n"
        "toggle in the launcher's Video panel; it is off by default.\n"
        "\n"
        "Default game config: " GBARECOMP_DEFAULT_GAME_CONFIG " (relative to CWD)\n",
        GBARECOMP_WINDOW_TITLE);
}

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

// Pulls the --solar-* flags out of argv so the runtime's own parser never sees
// them, and returns the resulting config. CLI wins over environment.
// Also takes --touch-ui / --no-touch-ui, which are game-owned like the solar
// flags: the engine has no CLI switch for RunOptions::ui_touch_friendly, and an
// unrecognised flag reaching the runtime's parser is an error.
boktai::SolarWeatherConfig take_solar_args(std::vector<std::string>& args,
                                           bool* touch_ui) {
    boktai::SolarWeatherConfig cfg;
    if (const char* v = std::getenv("BOKTAI_TOUCH_UI")) {
        *touch_ui = v[0] && v[0] != '0';
    }
    cfg.zipcode = env_or("BOKTAI_SOLAR_ZIP", "");
    cfg.country = env_or("BOKTAI_SOLAR_COUNTRY", "us");
    if (const char* v = std::getenv("BOKTAI_SOLAR_FULL_SUN")) {
        if (v[0]) cfg.full_sun_irradiance = std::strtod(v, nullptr);
    }
    if (const char* v = std::getenv("BOKTAI_SOLAR_DIFFUSE_WEIGHT")) {
        if (v[0]) cfg.diffuse_weight = std::strtod(v, nullptr);
    }
    if (const char* v = std::getenv("BOKTAI_SOLAR_POLL")) {
        if (v[0]) cfg.poll_seconds = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    }
    if (const char* v = std::getenv("BOKTAI_SOLAR_VERBOSE")) {
        cfg.verbose = v[0] && v[0] != '0';
    }

    std::vector<std::string> kept;
    kept.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        // A flag needing a value consumes the next argv entry; a missing value
        // is reported rather than silently swallowing the following flag.
        auto value = [&](const char* what) -> const char* {
            if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
                std::printf("%s: %s needs a value; ignoring\n",
                            GBARECOMP_WINDOW_TITLE, what);
                return nullptr;
            }
            return args[++i].c_str();
        };
        if (a == "--solar-zip") {
            if (const char* v = value("--solar-zip")) cfg.zipcode = v;
        } else if (a == "--solar-country") {
            if (const char* v = value("--solar-country")) cfg.country = v;
        } else if (a == "--solar-full-sun") {
            if (const char* v = value("--solar-full-sun"))
                cfg.full_sun_irradiance = std::strtod(v, nullptr);
        } else if (a == "--solar-poll") {
            if (const char* v = value("--solar-poll"))
                cfg.poll_seconds = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--solar-diffuse-weight") {
            if (const char* v = value("--solar-diffuse-weight"))
                cfg.diffuse_weight = std::strtod(v, nullptr);
        } else if (a == "--solar-verbose") {
            cfg.verbose = true;
        } else if (a == "--touch-ui") {
            *touch_ui = true;
        } else if (a == "--no-touch-ui") {
            *touch_ui = false;
        } else {
            kept.push_back(a);
        }
    }
    args.swap(kept);
    if (cfg.poll_seconds < 60u) cfg.poll_seconds = 60u;   // be a good citizen
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    // Strip the --solar-* flags before the runtime's parser sees them.
    std::vector<std::string> args(argv, argv + argc);
    bool touch_ui = false;
    const boktai::SolarWeatherConfig solar_cfg = take_solar_args(args, &touch_ui);

    // config.ini lives next to the executable, the same file the launcher seam
    // and host_window read, so [Solar] persists alongside [Launcher]/[KeyMap].
    {
        std::string dir = ".";
        if (!args.empty() && !args[0].empty()) {
            const std::size_t cut = args[0].find_last_of("/\\");
            if (cut != std::string::npos) dir = args[0].substr(0, cut);
        }
        boktai::solar_config_set_dir(dir);
    }

    // Built-in defaults so a standalone BoktaiRecomp binary ships without a
    // sibling game.toml. The asset picker still validates against these
    // values; CLI / TOML can override.
    gbarecomp::RunOptions opts;
    opts.builtin_game_name = GBARECOMP_BUILTIN_NAME;
    opts.builtin_rom_sha1  = (sizeof(GBARECOMP_BUILTIN_SHA1) > 1)
                                 ? GBARECOMP_BUILTIN_SHA1
                                 : nullptr;
    opts.builtin_rom_crc32 = GBARECOMP_BUILTIN_CRC32;
    opts.launcher_region   = (sizeof(GBARECOMP_BUILTIN_REGION) > 1)
                                 ? GBARECOMP_BUILTIN_REGION
                                 : nullptr;
    opts.launcher_boxart = (sizeof(GBARECOMP_BOXART) > 1)
                               ? GBARECOMP_BOXART
                               : nullptr;
    opts.launcher_game_config = GBARECOMP_DEFAULT_GAME_CONFIG;

    // Boktai's cartridge always has the photodiode, so declare it rather than
    // making the player set GBARECOMP_SOLAR. Light itself is game policy.
    opts.has_solar_sensor = true;
    opts.solar_provider   = &boktai::solar_weather_brightness;

    // ─────────────────────────────────────────────────────────────────
    // Sharp scaler — integer prescale, then a linear finish.
    //
    // Worth exposing because the GBA's 240x160 does not scale evenly to a
    // 16:10 handheld: on a Steam Deck's 1280x800, 800/160 is exactly 5 but
    // 1280/240 is 5.33, so a pure integer scale must letterbox or crop. The
    // sharp scaler prescales by the integer factor and only interpolates the
    // fractional remainder, which keeps pixel edges crisp instead of blurring
    // the whole image the way a plain linear stretch does. This is the
    // tractable form of the 16:10 work that got cut.
    //
    // Exposed as a launcher toggle but defaulted OFF: nearest is the look this
    // game has shipped with, and an engine update should not silently change
    // how anyone's game renders. One click in Video turns it on.
    //
    // NOT exposing launcher_*_affine_filter: that path is gated on
    // g_ws_affine_filter_enabled in the expanded-render code, and Boktai
    // renders the stock 240x160 (widescreen_supported stays 0), so the toggle
    // would do nothing at all.
    opts.launcher_expose_sharp_filter  = true;
    opts.launcher_default_sharp_filter = false;

    // Touch presentation: near-full-screen panels and larger hit targets. The
    // Deck has a touchscreen and this is the first thing here to use it, but it
    // is wrong on a desktop, so it is opt-in per launch rather than hardcoded —
    // one source tree serves both. RunOptions is filled before SDL starts, so
    // the screen cannot be measured here to decide automatically.
    opts.ui_touch_friendly = touch_ui;

    boktai::game_ui_install(opts);   // solar section in the in-game menu

#if defined(RECOMP_LAUNCHER)
    // Run the launcher FIRST: it is where the player picks a ROM/BIOS, sets
    // scale and volume, rebinds inputs — and, via config.ini [Solar], sets the
    // postal code. Starting the weather poll before it would use a stale one.
    if (gbarecomp_launcher_preboot(args, opts)) return 0;   // user quit
#endif

    // CLI beats the persisted setting; an unset code leaves the sensor on the
    // solar hotkeys and makes no network request at all.
    boktai::SolarWeatherConfig cfg = solar_cfg;
    if (cfg.zipcode.empty()) boktai::solar_config_load(&cfg);
    boktai::solar_weather_start(cfg);

    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& s : args) av.push_back(s.data());
    const int rc =
        gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
    boktai::solar_weather_stop();
    return rc;
}

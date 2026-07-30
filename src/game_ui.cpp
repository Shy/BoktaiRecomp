// game_ui.cpp — the Solar section of the in-game settings menu.
//
// The postal code is a RECOMP_RUNTIME_UI_TEXT row: click it and type. That item
// type did not exist in recomp-ui and was added for this, because the
// alternatives were all worse. An INT row is a -/+ pair moving by item->step,
// so a 0..99999 range needs thousands of clicks; a digit-place selector cuts
// that to ~20 but still cannot express "SW1A 1AA" or "K1A 0B1" at all, and a
// CHOICE would mean enumerating every postal code on earth.
//
// The first row is a read-only reading: what the gauge would show right now,
// in bars, next to the raw irradiance. A W/m^2 figure by itself does not answer
// the question the player actually has, which is whether it is worth going
// outside.
//
#include "game_ui.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "solar_weather.h"

#if defined(GBARECOMP_RUNTIME_UI)
#include "recomp_runtime_ui.h"
#endif

namespace boktai {
namespace {

constexpr const char* kKeySource     = "solar.source";
constexpr const char* kKeyManualStep = "solar.manual_step";
constexpr const char* kKeyZip        = "solar.zip";
constexpr const char* kKeyCountry    = "solar.country";
constexpr const char* kKeyFullSun    = "solar.full_sun";
constexpr const char* kKeyPollNow    = "solar.poll_now";
constexpr const char* kKeyReading    = "solar.reading";

// Brightness -> filled bars out of 8, fitted to the levels observed in play:
// 0->0, 8->1, 16->2, 24->3, 31->4, 63->5, 95->6, 127->7, 159->full.
//
// Two segments, because the response has a knee at 4 bars: below that it takes
// ~8 brightness per bar, above it ~32. It also ROUNDS rather than truncates --
// brightness 30 lights 4 bars, not 3. A breakpoint table got that wrong by one
// bar for every value just under a threshold, which is most of the low end
// where the gauge is most sensitive.
int brightness_to_bars(unsigned b) {
    int bars;
    if (b <= 31) {
        bars = static_cast<int>((b + 4) / 8);            // round(b / 8)
    } else {
        bars = 4 + static_cast<int>((b - 31 + 16) / 32); // 4 + round((b-31)/32)
    }
    return bars > 8 ? 8 : bars;
}

// Rewritten in place each draw; see the note on kItems being mutable.
char g_reading_value[128] = "checking…";
char g_reading_desc[192]  = "";

void persist() { solar_config_save(solar_config_current()); }

// Refreshes the read-only status row. Defined after kItems, which it mutates.
void refresh_reading();

int ui_get(const char* key, int* out) {
    if (!key || !out) return 0;
    const std::string k = key;
    if (k == kKeySource) {
        *out = static_cast<int>(solar_source());
    } else if (k == kKeyManualStep) {
        *out = solar_manual_step();
    } else if (k == kKeyFullSun) {
        *out = static_cast<int>(solar_full_sun());
    } else {
        return 0;
    }
    return 1;
}

int ui_set(const char* key, int value) {
    if (!key) return 0;
    const std::string k = key;
    if (k == kKeySource) {
        solar_set_source(value == static_cast<int>(SolarSource::Manual)
                             ? SolarSource::Manual
                             : SolarSource::LiveWeather);
    } else if (k == kKeyManualStep) {
        solar_set_manual_step(value);
        // Picking a level is a clear statement of intent; switch to Manual so
        // the choice actually takes effect instead of being silently ignored
        // while Live weather keeps overwriting it.
        solar_set_source(SolarSource::Manual);
    } else if (k == kKeyFullSun) {
        solar_set_full_sun(static_cast<double>(value));
    } else {
        return 0;
    }
    persist();
    return 1;
}

int ui_get_text(const char* key, char* buf, std::size_t buf_len) {
    if (!key || !buf || buf_len == 0) return 0;
    const std::string k = key;
    std::string v;
    if (k == kKeyReading) {
        refresh_reading();
        v = g_reading_value;
    }
    else if (k == kKeyZip)     v = solar_zipcode();
    else if (k == kKeyCountry) v = solar_config_current().country;
    else return 0;
    std::snprintf(buf, buf_len, "%s", v.c_str());
    return 1;
}

// Trims spaces and upper-cases, so " 11206 " and "sw1a" behave as typed.
std::string normalize_code(const char* value) {
    std::string s = value ? value : "";
    const std::size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t");
    s = s.substr(a, b - a + 1);
    for (auto& c : s) c = static_cast<char>(std::toupper(c));
    return s;
}

int ui_set_text(const char* key, const char* value) {
    if (!key) return 0;
    const std::string k = key;
    if (k == kKeyZip) {
        const std::string z = normalize_code(value);
        // Reject rather than accept-and-mangle, so the row keeps showing the
        // previous working code instead of quietly breaking the sensor.
        if (!solar_set_zipcode(z)) return 0;
        if (!z.empty()) solar_set_source(SolarSource::LiveWeather);
        persist();
        return 1;
    }
    if (k == kKeyCountry) {
        std::string c = normalize_code(value);
        for (auto& ch : c) ch = static_cast<char>(std::tolower(ch));
        if (!solar_set_country(c)) return 0;
        persist();
        return 1;
    }
    return 0;
}

int ui_action(const char* key) {
    if (key && std::string(key) == kKeyPollNow) {
        solar_set_source(SolarSource::LiveWeather);
        solar_poll_now();
        return 1;
    }
    return 0;
}

int ui_enabled(const char* key) {
    if (!key) return 1;
    const std::string k = key;
    // is_enabled is the one callback recomp-ui invokes for every row on every
    // draw, which makes it the cheapest place to keep the live reading current
    // without the run loop reformatting strings for a menu nobody opened.
    if (k == kKeyReading) {
        refresh_reading();
        return 0;                 // read-only: never selectable
    }
    if (k == kKeyManualStep)
        return solar_source() == SolarSource::Manual;
    if (k == kKeyFullSun || k == kKeyPollNow)
        return solar_source() == SolarSource::LiveWeather;
    return 1;
}

#if defined(GBARECOMP_RUNTIME_UI)

const char* const kSourceChoices[] = {"Live weather", "Manual"};

// Static storage: recomp-ui keeps the pointer, and every string must outlive
// the menu (see RunOptions::ui_extra_items). NOT const — the reading row's
// description buffer is rewritten in place as new samples arrive.
RecompRuntimeUiItem kItems[] = {
    {
        /* key */ kKeySource,
        /* section */ "Solar sensor",
        /* label */ "Light source",
        /* description */ "Live weather reads your area's current sunlight. "
                          "Manual holds a fixed level and makes no network "
                          "request.",
        /* type */ RECOMP_RUNTIME_UI_CHOICE,
        /* minimum */ 0, /* maximum */ 1, /* step */ 1,
        /* choices */ kSourceChoices, /* choice_count */ 2,
        /* choice_values */ nullptr,
    },
// The reading row and the two editable codes all need a TEXT row, which
// post-dates some recomp-ui pins. Against an older one the section still offers
// Light source / Full sun / Manual level / Check now, and the postal code stays
// settable via --solar-zip or config.ini [Solar].
#if defined(RECOMP_RUNTIME_UI_HAS_TEXT)
    {
        kKeyReading, "Solar sensor", "Current reading",
        g_reading_desc,
        // TEXT so the value column shows the reading; ui_enabled reports it
        // unselectable, and a disabled row cannot be activated or edited.
        RECOMP_RUNTIME_UI_TEXT,
        0, 0, 0, nullptr, 0, nullptr,
    },
    {
        kKeyZip, "Solar sensor", "Postal code",
        "Click to type. Empty turns the network off entirely. Sent only to "
        "look up your coordinates and local sunlight.",
        RECOMP_RUNTIME_UI_TEXT,
        0, 0, 0, nullptr, 0, nullptr,
    },
    {
        kKeyCountry, "Solar sensor", "Country",
        "Two-letter code for the postal code above: us, ca, gb, de, ...",
        RECOMP_RUNTIME_UI_TEXT,
        0, 0, 0, nullptr, 0, nullptr,
    },
#endif  // RECOMP_RUNTIME_UI_HAS_TEXT — postal code / country need a text row
    {
        kKeyFullSun, "Solar sensor", "Full sun (W/m2)",
        "Irradiance that fills the gauge. Lower it at high latitude or in "
        "winter, where a clear day never reaches 900.",
        RECOMP_RUNTIME_UI_INT,
        300, 1200, 50, nullptr, 0, nullptr,
    },
    {
        kKeyManualStep, "Solar sensor", "Manual level",
        "0 is darkness, 8 is full sun. Matches the number-row keys 1-9.",
        RECOMP_RUNTIME_UI_INT,
        0, 8, 1, nullptr, 0, nullptr,
    },
    {
        kKeyPollNow, "Solar sensor", "Check weather now",
        "Fetches the current reading immediately instead of waiting for the "
        "next poll.",
        RECOMP_RUNTIME_UI_ACTION,
        0, 0, 0, nullptr, 0, nullptr,
    },
};

// Answers "what would the gauge read right now", which is the question the
// player actually has: a W/m^2 figure alone does not say whether it is worth
// going outside. Bars come from the measured response table above.
void refresh_reading() {
    const SolarStatus st = solar_status();
    const int bars = brightness_to_bars(st.brightness);

    if (solar_source() == SolarSource::Manual) {
        std::snprintf(g_reading_value, sizeof(g_reading_value),
                      "%d/8 bars (manual)", bars);
        std::snprintf(g_reading_desc, sizeof(g_reading_desc),
                      "Holding a fixed level. Switch Light source to Live "
                      "weather to follow the sky.");
        return;
    }
    if (solar_zipcode().empty()) {
        std::snprintf(g_reading_value, sizeof(g_reading_value),
                      "no location set");
        std::snprintf(g_reading_desc, sizeof(g_reading_desc),
                      "Enter a postal code below to read your local sunlight.");
        return;
    }
    if (!st.have_sample) {
        std::snprintf(g_reading_value, sizeof(g_reading_value),
                      "checking weather…");
        std::snprintf(g_reading_desc, sizeof(g_reading_desc), "%s",
                      st.last_poll_failed
                          ? "Last lookup failed; retrying."
                          : "Looking up the current reading.");
        return;
    }
    std::snprintf(g_reading_value, sizeof(g_reading_value),
                  "%d/8 bars  ·  %.0f W/m2", bars, st.irradiance);
    std::snprintf(g_reading_desc, sizeof(g_reading_desc),
                  "%s, %.0f%% cloud%s. Full sun here is %.0f W/m2.",
                  st.is_day ? "Daytime" : "Night", st.cloud_cover,
                  st.last_poll_failed ? " (last refresh failed)" : "",
                  solar_full_sun());
}

#else   // !GBARECOMP_RUNTIME_UI — no item array, so nothing to refresh.

void refresh_reading() {}

#endif  // GBARECOMP_RUNTIME_UI

}  // namespace

void game_ui_install(gbarecomp::RunOptions& opts) {
    opts.ui_get      = &ui_get;
    opts.ui_set      = &ui_set;
    opts.ui_action   = &ui_action;
    opts.ui_enabled  = &ui_enabled;
    opts.ui_get_text = &ui_get_text;
    opts.ui_set_text = &ui_set_text;
#if defined(GBARECOMP_RUNTIME_UI)
    refresh_reading();               // populate the row before the first draw
    opts.ui_extra_items      = kItems;
    opts.ui_extra_item_count = sizeof(kItems) / sizeof(kItems[0]);
#endif
}

}  // namespace boktai

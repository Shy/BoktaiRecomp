// solar_weather.h — real-world sunlight for the Gun del Sol, from a zipcode.
//
// Boktai's cartridge measures light with a photodiode. The engine emulates
// that sensor (gbarecomp/src/gba/gba_solar.h) but deliberately owns no light
// source: by ENHANCEMENTS.md Rule 2, sensor emulation is a runner capability
// while "where does light come from" is game policy. This is Boktai's policy,
// installed via RunOptions::solar_provider.
//
// The light quantity is GLOBAL HORIZONTAL IRRADIANCE (GHI) in W/m^2 — the
// power a flat upward-facing surface receives, which is physically what the
// cartridge photodiode integrates. Open-Meteo publishes it as
// `shortwave_radiation`, keyless. Roughly:
//
//     0 W/m^2      night                        -> empty gauge
//     50-150       heavy overcast, daytime      -> 1-3 bars
//     200-400      light overcast / low sun     -> 4-6 bars
//     800-1000     clear sky, midday            -> full gauge
//
// so a full charge genuinely requires a clear sunny day in the player's
// actual location, which is the whole point of the exercise.
//
// PRIVACY: this is strictly opt-in and inert unless a zipcode is supplied.
// When armed it sends that zipcode to api.zippopotam.us (to geocode it) and
// the resulting coordinates to api.open-meteo.com. Neither call carries any
// other data. With no zipcode configured nothing is sent and no thread runs.
//
// Requires libcurl. Without it the translation unit still compiles and
// solar_weather_start() reports unavailable, so the debug keys remain the
// light source.

#ifndef BOKTAI_SOLAR_WEATHER_H
#define BOKTAI_SOLAR_WEATHER_H

#include <cstdint>
#include <string>

namespace boktai {

struct SolarWeatherConfig {
    // Postal code to sample. Empty disables the provider entirely.
    std::string zipcode;
    // Zippopotam country code ("us", "ca", "gb", "de", ...). US ZIPs are the
    // common case; the API covers many others with the same shape.
    std::string country = "us";

    // Irradiance treated as "full sun", i.e. mapped to a saturated gauge.
    // 900 W/m^2 is about clear-sky midday at mid latitudes. Players far from
    // the equator, or in winter, should lower this so a genuinely sunny local
    // day can still fill the gauge.
    double full_sun_irradiance = 900.0;

    // Seconds between polls. Open-Meteo advances `current` every 900 s, so
    // polling faster than that only adds requests, not information.
    unsigned poll_seconds = 600;

    // Brightness reported before the first successful sample (and if the very
    // first fetch fails). 0 keeps the honest answer "we don't know yet, and
    // the sensor is dark" rather than inventing sunlight.
    std::uint8_t startup_brightness = 0;

    // Log every poll: irradiance, cloud cover, day flag, mapped brightness.
    bool verbose = false;
};

// Starts the polling thread. Returns false — without side effects and with a
// reason printed — if the config is disabled, libcurl is missing, or curl
// fails to initialize. Safe to ignore the result: on false, brightness()
// simply keeps reporting startup_brightness.
bool solar_weather_start(const SolarWeatherConfig& cfg);

// ---- live reconfiguration, driven by the in-game menu ----------------------

enum class SolarSource {
    LiveWeather = 0,   // sample the postal code's current irradiance
    Manual      = 1,   // hold a hand-picked level, no network at all
};

void        solar_set_source(SolarSource src);
SolarSource solar_source();

// Manual level on the same 0..8 scale the number-row keys use, mapped through
// the measured gauge response rather than spread evenly over 0..255.
void solar_set_manual_step(int step);   // clamped 0..8
int  solar_manual_step();

// Repoints at a new postal code and polls it immediately. Starts the thread if
// it was never started (e.g. no code was configured at launch), which is what
// makes the setting usable from the menu on a first run. Returns false if the
// code is malformed or libcurl is unavailable.
bool solar_set_zipcode(const std::string& zip);
std::string solar_zipcode();

// Country for the postal code above (Zippopotam code: us, ca, gb, de, ...).
// Also forces a re-geocode, since the same digits mean different places.
bool solar_set_country(const std::string& country);

// Forces a poll now instead of waiting out the interval.
void solar_poll_now();

// Irradiance treated as a full gauge. Lowering it lets a high-latitude or
// winter player still reach full sun on a genuinely clear day.
double solar_full_sun();
void   solar_set_full_sun(double wm2);

// Snapshot of the live configuration, for persisting it.
SolarWeatherConfig solar_config_current();

// Last successful reading, for display. irradiance < 0 means "no sample yet".
struct SolarStatus {
    double       irradiance = -1.0;   // W/m^2
    double       cloud_cover = -1.0;  // %
    bool         is_day = false;
    std::uint8_t brightness = 0;
    bool         have_sample = false;
    bool         last_poll_failed = false;
};
SolarStatus solar_status();

// ---- persistence: config.ini [Solar], next to the executable --------------
// The same player-owned file the launcher seam writes. game.toml is identity
// gated and never written.
bool solar_config_load(SolarWeatherConfig* cfg);
void solar_config_save(const SolarWeatherConfig& cfg);
// Directory holding config.ini. Defaults to "."; main() sets it from argv[0].
void solar_config_set_dir(const std::string& dir);

// The provider handed to RunOptions::solar_provider. Non-blocking: reads a
// cached value written by the polling thread, so it is safe to call once per
// ADC conversion on the guest thread.
std::uint8_t solar_weather_brightness();

// Joins the polling thread. Idempotent.
void solar_weather_stop();

// True once at least one poll has succeeded. For startup logging only.
bool solar_weather_has_sample();

}  // namespace boktai

#endif  // BOKTAI_SOLAR_WEATHER_H

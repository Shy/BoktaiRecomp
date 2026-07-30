// solar_weather.cpp — zipcode -> coordinates -> irradiance -> brightness.
//
// Two keyless services, both queried on a background thread so the guest
// never blocks on the network:
//
//   api.zippopotam.us/<country>/<zip>   -> lat/lon, re-run when the code changes
//   api.open-meteo.com/v1/forecast      -> shortwave_radiation (GHI, W/m^2)
//
// See solar_weather.h for the irradiance-to-gauge reasoning and the privacy
// contract. Nothing here reaches the network unless a postal code is set.
//
// Thread-safety split: solar_weather_brightness() is called once per ADC
// conversion on the GUEST thread, so everything on that path is a relaxed
// atomic and never takes a lock. The mutex guards only the strings and the
// last-reading record, touched by the poll thread and the menu.

#include "solar_weather.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(BOKTAI_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace boktai {
namespace {

// ---- guest-thread hot path (atomics only) ----------------------------------
std::atomic<unsigned> g_weather_brightness{0};   // last live reading
std::atomic<int>      g_source{static_cast<int>(SolarSource::LiveWeather)};
std::atomic<int>      g_manual_step{0};
std::atomic<bool>     g_have_sample{false};
std::atomic<bool>     g_last_poll_failed{false};

// ---- poll thread / menu shared state --------------------------------------
std::mutex              g_mu;
std::condition_variable g_cv;
bool                    g_stop = false;
bool                    g_poke = false;         // "poll now"
unsigned                g_generation = 0;       // bumped when the code changes
std::thread             g_thread;
bool                    g_running = false;
SolarWeatherConfig      g_cfg;
double                  g_last_ghi = -1.0;
double                  g_last_cloud = -1.0;
bool                    g_last_is_day = false;
std::string             g_config_dir = ".";

// Boktai's gauge saturates near brightness 159 and is very steep below 31
// (measured, 8 bars: 0->0, 31->4, 63->5, 95->6, 127->7, 159->full), so
// spreading irradiance over the full 0..255 would waste the top 40% of the
// range on levels the game cannot tell apart.
constexpr double kGaugeFullBrightness = 159.0;

// Manual steps, shaped from that same measurement rather than evenly spread.
// Index 0..8 is the range the engine's SolarBrighter/SolarDimmer hotkeys step.
constexpr unsigned kManualSteps[9] = {0, 8, 16, 24, 31, 63, 95, 127, 159};

std::uint8_t irradiance_to_brightness(double ghi, double full_sun) {
    if (!(full_sun > 0.0)) full_sun = 900.0;
    double f = ghi / full_sun;
    if (!(f > 0.0)) f = 0.0;        // also catches NaN
    if (f > 1.0)    f = 1.0;
    // Linear in irradiance, deliberately: the game's own gauge response is
    // already strongly nonlinear, and pushing a second curve through it made
    // overcast days indistinguishable from clear ones in testing.
    return static_cast<std::uint8_t>(std::lround(f * kGaugeFullBrightness));
}

// A postal code is alphanumeric (plus '-' for ZIP+4). Rejecting everything
// else keeps user input out of URL syntax.
bool postal_code_is_safe(const std::string& s) {
    if (s.empty() || s.size() > 12) return false;
    for (unsigned char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool country_code_is_safe(const std::string& s) {
    if (s.size() < 2 || s.size() > 3) return false;
    for (unsigned char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

// Minimal scalar reader for the two small responses above. Handles
// `"key":123`, `"key": 123`, and `"key": "123"` — Zippopotam quotes its
// coordinates. The needle carries both quotes, so "current" cannot match
// "current_units" and "cloud_cover" cannot match "cloud_cover_low".
bool json_scalar(const std::string& s, const char* key, double& out,
                 std::size_t from = 0) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t k = s.find(needle, from);
    if (k == std::string::npos) return false;
    std::size_t p = s.find(':', k + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '"')) ++p;
    const char* begin = s.c_str() + p;
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    out = v;
    return true;
}

#if defined(BOKTAI_HAVE_CURL)

std::size_t append_body(char* ptr, std::size_t sz, std::size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, sz * nm);
    return sz * nm;
}

bool http_get(const std::string& url, std::string& out, bool verbose) {
    CURL* h = curl_easy_init();
    if (!h) return false;
    out.clear();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &append_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "BoktaiRecomp/1.0");
    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK || status < 200 || status >= 300) {
        if (verbose) {
            // Report whichever layer actually failed; curl_easy_strerror(OK)
            // reads "No error", which is nonsense next to an HTTP 404.
            if (rc != CURLE_OK) {
                std::printf("solar_weather: GET failed: %s\n",
                            curl_easy_strerror(rc));
            } else {
                std::printf("solar_weather: GET returned HTTP %ld\n", status);
            }
            std::fflush(stdout);
        }
        return false;
    }
    return true;
}

bool geocode(const std::string& country, const std::string& zip, bool verbose,
             double& lat, double& lon) {
    const std::string url =
        "https://api.zippopotam.us/" + country + "/" + zip;
    std::string body;
    if (!http_get(url, body, verbose)) return false;
    // lat/lon appear only inside "places", so an unscoped search is fine.
    if (!json_scalar(body, "latitude", lat) ||
        !json_scalar(body, "longitude", lon)) {
        std::printf("solar_weather: could not geocode %s/%s "
                    "(unrecognized response)\n",
                    country.c_str(), zip.c_str());
        std::fflush(stdout);
        return false;
    }
    return true;
}

// Fetches current conditions. `ghi` is the load-bearing value; cloud cover and
// the day flag are reported only, as context for a surprising reading.
bool fetch_conditions(double lat, double lon, bool verbose, double& ghi,
                      double& cloud, double& is_day) {
    char url[256];
    std::snprintf(url, sizeof(url),
                  "https://api.open-meteo.com/v1/forecast"
                  "?latitude=%.4f&longitude=%.4f"
                  "&current=shortwave_radiation,cloud_cover,is_day"
                  "&timezone=auto",
                  lat, lon);
    std::string body;
    if (!http_get(url, body, verbose)) return false;

    // Scope to the "current" object. "current_units" precedes it and holds a
    // STRING for shortwave_radiation ("W/m²"), so an unscoped search would
    // read the units and parse as 0.
    const std::size_t cur = body.find("\"current\":");
    if (cur == std::string::npos) return false;
    if (!json_scalar(body, "shortwave_radiation", ghi, cur)) return false;
    if (!json_scalar(body, "cloud_cover", cloud, cur))  cloud  = -1.0;
    if (!json_scalar(body, "is_day", is_day, cur))      is_day = -1.0;
    return true;
}

// Waits out a fixed delay, returning true if we should stop. Deliberately does
// NOT wake on a postal-code change: this is the debounce, and waking early on
// the very thing it is debouncing would defeat it.
bool wait_settle(unsigned millis) {
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait_for(lk, std::chrono::milliseconds(millis),
                  [] { return g_stop; });
    return g_stop;
}

// How long the postal code must hold still before it is worth two HTTP
// requests. The menu's -/+ moves one digit place per click, so entering a
// five-digit code is ~20 rapid changes; without this each one would spend a
// geocode and a forecast call on a half-typed number.
constexpr unsigned kZipSettleMillis = 1200;

// Waits up to `seconds`, returning true if we should stop. Wakes early on a
// "poll now" poke or a postal-code change so the menu feels immediate.
bool wait_or_stop(unsigned seconds, unsigned& seen_generation) {
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait_for(lk, std::chrono::seconds(seconds), [&] {
        return g_stop || g_poke || g_generation != seen_generation;
    });
    g_poke = false;
    return g_stop;
}

void poll_loop() {
    unsigned seen_generation = 0;
    bool have_coords = false;
    bool settling = false;
    double lat = 0.0, lon = 0.0;
    bool warned_geocode = false, warned_poll = false;

    for (;;) {
        std::string zip, country;
        unsigned poll_seconds;
        double full_sun;
        bool verbose;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_stop) return;
            if (g_generation != seen_generation) {
                seen_generation = g_generation;
                have_coords = false;          // new location, re-geocode
                settling = true;              // ...but not until it holds still
                warned_geocode = warned_poll = false;
            }
            zip = g_cfg.zipcode;
            country = g_cfg.country;
            poll_seconds = g_cfg.poll_seconds;
            full_sun = g_cfg.full_sun_irradiance;
            verbose = g_cfg.verbose;
        }

        // No location, or the player is holding a manual level: nothing to
        // fetch. Stay parked so Manual mode makes literally no requests.
        if (zip.empty() ||
            g_source.load(std::memory_order_relaxed) !=
                static_cast<int>(SolarSource::LiveWeather)) {
            if (wait_or_stop(poll_seconds, seen_generation)) return;
            continue;
        }

        // Let a code that is still being edited come to rest. Re-checking the
        // generation after the delay means a run of clicks costs one lookup at
        // the end, not one per click.
        if (settling) {
            if (wait_settle(kZipSettleMillis)) return;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_generation != seen_generation) continue;  // still moving
            }
            settling = false;
        }

        if (!have_coords) {
            if (geocode(country, zip, verbose, lat, lon)) {
                have_coords = true;
                std::printf("solar_weather: %s %s -> %.4f, %.4f "
                            "(full sun = %.0f W/m^2, poll %us)\n",
                            country.c_str(), zip.c_str(), lat, lon, full_sun,
                            poll_seconds);
                std::fflush(stdout);
            } else {
                if (!warned_geocode) {
                    warned_geocode = true;
                    std::printf("solar_weather: geocoding %s %s failed; "
                                "retrying (the in-game menu still works)\n",
                                country.c_str(), zip.c_str());
                    std::fflush(stdout);
                }
                g_last_poll_failed.store(true, std::memory_order_relaxed);
                if (wait_or_stop(60, seen_generation)) return;
                continue;
            }
        }

        double ghi = 0.0, cloud = -1.0, is_day = -1.0;
        if (fetch_conditions(lat, lon, verbose, ghi, cloud, is_day)) {
            const std::uint8_t b = irradiance_to_brightness(ghi, full_sun);
            const bool first =
                !g_have_sample.exchange(true, std::memory_order_relaxed);
            g_weather_brightness.store(b, std::memory_order_relaxed);
            g_last_poll_failed.store(false, std::memory_order_relaxed);
            warned_poll = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_last_ghi = ghi;
                g_last_cloud = cloud;
                g_last_is_day = is_day > 0.0;
            }
            // Always announce the first reading — it is the confirmation that
            // the whole chain works — then only on request.
            if (first || verbose) {
                std::printf("solar_weather: %.0f W/m^2, cloud %.0f%%, "
                            "day %.0f -> brightness %u\n",
                            ghi, cloud, is_day, static_cast<unsigned>(b));
                std::fflush(stdout);
            }
        } else {
            g_last_poll_failed.store(true, std::memory_order_relaxed);
            if (!warned_poll) {
                // Keep the last good reading rather than dropping to darkness:
                // a dropped Wi-Fi packet is not a sunset.
                warned_poll = true;
                std::printf("solar_weather: poll failed, holding last reading "
                            "(brightness %u)\n",
                            g_weather_brightness.load(std::memory_order_relaxed));
                std::fflush(stdout);
            }
        }
        // Retry sooner while we have never succeeded; Open-Meteo only advances
        // `current` every 900 s, so there is nothing to gain by polling faster
        // than the configured interval once a sample is in hand.
        const unsigned wait_s =
            g_have_sample.load(std::memory_order_relaxed) ? poll_seconds : 30u;
        if (wait_or_stop(wait_s, seen_generation)) return;
    }
}

bool ensure_thread_started() {
    if (g_running) return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::printf("solar_weather: curl_global_init failed\n");
        std::fflush(stdout);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = false;
    }
    g_thread = std::thread(poll_loop);
    g_running = true;
    return true;
}

#endif  // BOKTAI_HAVE_CURL

std::string config_path() { return g_config_dir + "/config.ini"; }

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

bool solar_weather_start(const SolarWeatherConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cfg = cfg;
        if (g_cfg.poll_seconds < 60u) g_cfg.poll_seconds = 60u;
        ++g_generation;
    }
    g_weather_brightness.store(cfg.startup_brightness,
                               std::memory_order_relaxed);

    if (cfg.zipcode.empty()) return false;   // opt-in; stay inert

    if (!postal_code_is_safe(cfg.zipcode) ||
        !country_code_is_safe(cfg.country)) {
        std::printf("solar_weather: rejecting postal code '%s' / country '%s' "
                    "(expected alphanumeric)\n",
                    cfg.zipcode.c_str(), cfg.country.c_str());
        std::fflush(stdout);
        std::lock_guard<std::mutex> lk(g_mu);
        g_cfg.zipcode.clear();
        return false;
    }

#if !defined(BOKTAI_HAVE_CURL)
    std::printf("solar_weather: built without libcurl, so a postal code "
                "cannot be honored; use the in-game menu's Manual mode\n");
    std::fflush(stdout);
    return false;
#else
    return ensure_thread_started();
#endif
}

std::uint8_t solar_weather_brightness() {
    if (g_source.load(std::memory_order_relaxed) ==
        static_cast<int>(SolarSource::Manual)) {
        int step = g_manual_step.load(std::memory_order_relaxed);
        if (step < 0) step = 0;
        if (step > 8) step = 8;
        return static_cast<std::uint8_t>(kManualSteps[step]);
    }
    return static_cast<std::uint8_t>(
        g_weather_brightness.load(std::memory_order_relaxed));
}

void solar_set_source(SolarSource src) {
    g_source.store(static_cast<int>(src), std::memory_order_relaxed);
    // Wake the poll thread: switching back to Live should refresh promptly
    // rather than after however much of the interval happens to remain.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_poke = true;
    }
    g_cv.notify_all();
}

SolarSource solar_source() {
    return static_cast<SolarSource>(g_source.load(std::memory_order_relaxed));
}

void solar_set_manual_step(int step) {
    if (step < 0) step = 0;
    if (step > 8) step = 8;
    g_manual_step.store(step, std::memory_order_relaxed);
}

int solar_manual_step() {
    return g_manual_step.load(std::memory_order_relaxed);
}

bool solar_set_zipcode(const std::string& zip) {
    if (!zip.empty() && !postal_code_is_safe(zip)) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_cfg.zipcode == zip) return true;
        g_cfg.zipcode = zip;
        ++g_generation;          // forces a re-geocode
        g_poke = true;
    }
    g_have_sample.store(false, std::memory_order_relaxed);
    g_cv.notify_all();
    if (zip.empty()) return true;
#if defined(BOKTAI_HAVE_CURL)
    return ensure_thread_started();
#else
    return false;
#endif
}

std::string solar_zipcode() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cfg.zipcode;
}

bool solar_set_country(const std::string& country) {
    if (!country_code_is_safe(country)) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_cfg.country == country) return true;
        g_cfg.country = country;
        ++g_generation;
        g_poke = true;
    }
    g_have_sample.store(false, std::memory_order_relaxed);
    g_cv.notify_all();
#if defined(BOKTAI_HAVE_CURL)
    return ensure_thread_started();
#else
    return false;
#endif
}

void solar_poll_now() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_poke = true;
    }
    g_cv.notify_all();
}

double solar_full_sun() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cfg.full_sun_irradiance;
}

void solar_set_full_sun(double wm2) {
    if (!(wm2 > 0.0)) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cfg.full_sun_irradiance = wm2;
    }
    // Re-map the reading we already have so the gauge responds immediately
    // instead of only at the next poll.
    double ghi;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ghi = g_last_ghi;
    }
    if (ghi >= 0.0) {
        g_weather_brightness.store(irradiance_to_brightness(ghi, wm2),
                                   std::memory_order_relaxed);
    }
}

SolarWeatherConfig solar_config_current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cfg;
}

SolarStatus solar_status() {
    SolarStatus s;
    std::lock_guard<std::mutex> lk(g_mu);
    s.irradiance  = g_last_ghi;
    s.cloud_cover = g_last_cloud;
    s.is_day      = g_last_is_day;
    s.brightness  = solar_weather_brightness();
    s.have_sample = g_have_sample.load(std::memory_order_relaxed);
    s.last_poll_failed = g_last_poll_failed.load(std::memory_order_relaxed);
    return s;
}

void solar_weather_stop() {
    if (!g_running) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_running = false;
#if defined(BOKTAI_HAVE_CURL)
    curl_global_cleanup();
#endif
}

void solar_config_set_dir(const std::string& dir) {
    if (!dir.empty()) g_config_dir = dir;
}

bool solar_config_load(SolarWeatherConfig* cfg) {
    if (!cfg) return false;
    std::ifstream in(config_path());
    if (!in) return false;
    bool in_section = false, found = false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[') {
            // Case-insensitive section match; the launcher writes [Launcher].
            std::string name = t.substr(1, t.find(']') - 1);
            for (auto& ch : name) ch = static_cast<char>(std::tolower(ch));
            in_section = (name == "solar");
            continue;
        }
        if (!in_section) continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        const std::string v = trim(t.substr(eq + 1));
        for (auto& ch : k) ch = static_cast<char>(std::tolower(ch));
        found = true;
        if (k == "zip")               cfg->zipcode = v;
        else if (k == "country")      cfg->country = v;
        else if (k == "full_sun")     cfg->full_sun_irradiance = std::strtod(v.c_str(), nullptr);
        else if (k == "poll")         cfg->poll_seconds = static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "verbose")      cfg->verbose = (v != "0");
        else if (k == "manual_step")  solar_set_manual_step(static_cast<int>(std::strtol(v.c_str(), nullptr, 10)));
        else if (k == "source")       solar_set_source(v == "manual" ? SolarSource::Manual
                                                                    : SolarSource::LiveWeather);
    }
    return found;
}

void solar_config_save(const SolarWeatherConfig& cfg) {
    // Rewrite only our own section, preserving every other line — config.ini
    // is shared with the launcher seam's [Launcher] block and [KeyMap].
    std::vector<std::string> kept;
    {
        std::ifstream in(config_path());
        std::string line;
        bool in_section = false;
        while (std::getline(in, line)) {
            const std::string t = trim(line);
            if (!t.empty() && t.front() == '[') {
                std::string name = t.substr(1, t.find(']') - 1);
                for (auto& ch : name) ch = static_cast<char>(std::tolower(ch));
                in_section = (name == "solar");
            }
            if (!in_section) kept.push_back(line);
        }
    }
    while (!kept.empty() && trim(kept.back()).empty()) kept.pop_back();

    std::ofstream out(config_path(), std::ios::trunc);
    if (!out) return;
    for (const auto& l : kept) out << l << "\n";
    if (!kept.empty()) out << "\n";
    out << "[Solar]\n"
        << "# Real-world sunlight for the solar sensor. An empty zip means no\n"
        << "# network request is ever made. Also settable in the in-game menu.\n"
        << "zip         = " << cfg.zipcode << "\n"
        << "country     = " << cfg.country << "\n"
        << "full_sun    = " << static_cast<long>(cfg.full_sun_irradiance) << "\n"
        << "poll        = " << cfg.poll_seconds << "\n"
        << "source      = "
        << (solar_source() == SolarSource::Manual ? "manual" : "live") << "\n"
        << "manual_step = " << solar_manual_step() << "\n";
}

}  // namespace boktai

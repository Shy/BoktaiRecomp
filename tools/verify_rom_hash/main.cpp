// verify_rom_hash — verify a Boktai 1 dump before spending build time on it.
//
// Unlike the Gen3 targets' stub, this actually hashes the image and validates
// the cartridge header, so a bad dump is caught here rather than at runtime.
//
// Checks, in order of usefulness:
//   1. SHA-1 against the pinned USA dump (the gate the runtime also applies).
//   2. Size is exactly 16 MiB and a power of two (no trim / overdump).
//   3. Header invariants: fixed byte 0x96, complement check, GBA unit code.
//   4. Save chip signature (Boktai: EEPROM_V).
//   5. Seiko RTC signature (SIIRTC_V) — Boktai needs the clock.
//
// Exit 0 on a fully verified dump, 1 on mismatch, 2 on usage / IO error.
//
//   usage: verify_rom_hash <rom_path>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "crc32.h"
#include "gba_rom_header.h"
#include "sha1.h"

namespace {

// Pinned USA dump. Verified 2026-07-28 against a clean cart dump. The
// earlier 92bda1b6... value was an intro-patched image — see BRINGUP.md.
constexpr char kExpectedSha1[] = "7164326283df46a3941ec7b6ceca889cbc40e660";
constexpr std::uint32_t kExpectedCrc32 = 0xE715AC45u;
constexpr std::size_t kExpectedSize = 16u * 1024u * 1024u;

bool find_signature(const std::vector<std::uint8_t>& rom, const char* sig,
                    std::size_t* off_out) {
    const std::size_t n = std::strlen(sig);
    if (rom.size() < n) return false;
    // Nintendo SDK signatures are 4-byte aligned in real ROMs.
    for (std::size_t off = 0; off + n <= rom.size(); off += 4) {
        if (std::memcmp(rom.data() + off, sig, n) == 0) {
            if (off_out) *off_out = off;
            return true;
        }
    }
    return false;
}

void report(const char* label, bool pass) {
    std::printf("  [%s] %s\n", pass ? "PASS" : "FAIL", label);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: verify_rom_hash <rom_path>\n");
        return 2;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::printf("verify_rom_hash: cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    if (rom.size() < 0xC0) {
        std::printf("verify_rom_hash: %s is too small to be a GBA ROM (%zu bytes)\n",
                    argv[1], rom.size());
        return 2;
    }

    const std::string sha = gba::sha1(rom.data(), rom.size()).hex();
    const std::uint32_t crc = gba::crc32(rom.data(), rom.size());
    const gba::RomHeader h = gba::parse_rom(rom.data(), rom.size());
    const bool pow2 = rom.size() && (rom.size() & (rom.size() - 1)) == 0;

    std::printf("rom            %s\n", argv[1]);
    std::printf("size           %zu bytes (%.2f MiB)\n", rom.size(),
                static_cast<double>(rom.size()) / 1048576.0);
    std::printf("sha1           %s\n", sha.c_str());
    std::printf("crc32          0x%08X\n", crc);
    std::printf("title / code   %s / %s   maker %s\n", h.game_title.c_str(),
                h.game_code.c_str(), h.maker_code.c_str());
    std::printf("entry target   0x%08X\n", h.entry_target);
    std::printf("save chip      %s (%s @ 0x%06X)\n",
                gba::save_type_name(h.save_type), h.save_signature.c_str(),
                h.save_signature_offset);

    std::size_t rtc_off = 0;
    const bool has_rtc = find_signature(rom, "SIIRTC_V", &rtc_off);
    if (has_rtc) std::printf("rtc            SIIRTC_V @ 0x%06X\n", rtc_off);

    std::printf("\nchecks:\n");
    const bool sha_ok = (sha == kExpectedSha1);
    report("sha1 matches the pinned USA dump", sha_ok);
    report("crc32 matches", crc == kExpectedCrc32);
    report("size is exactly 16 MiB", rom.size() == kExpectedSize);
    report("size is a power of two (not trimmed / overdumped)", pow2);
    report("header parsed and invariants hold", h.ok);
    report("complement check valid", h.complement_valid);
    report("Nintendo logo present", h.logo_present);
    report("save chip is EEPROM", h.save_type == gba::SaveType::EEPROM);
    report("Seiko RTC signature present", has_rtc);

    const bool all = sha_ok && crc == kExpectedCrc32 &&
                     rom.size() == kExpectedSize && pow2 && h.ok &&
                     h.complement_valid && h.logo_present &&
                     h.save_type == gba::SaveType::EEPROM && has_rtc;

    if (!h.ok && !h.error.empty()) {
        std::printf("\nheader error: %s\n", h.error.c_str());
    }
    if (!sha_ok) {
        std::printf("\nSHA-1 mismatch.\n  expected %s\n  actual   %s\n"
                    "The runtime gates on this and will refuse to launch.\n"
                    "Note: sensor-hack / translation-patched ROMs will fail here\n"
                    "by design — they remove the real solar sensor reads.\n",
                    kExpectedSha1, sha.c_str());
    }
    std::printf("\n%s\n", all ? "VERIFIED: unmodified retail Boktai 1 (USA)."
                              : "NOT VERIFIED: see failures above.");
    return all ? 0 : 1;
}

// builder_core.cpp — see builder_core.h for the design and the two properties
// this file exists to guarantee (assets read in place; build runs in a copy).

#include "builder_core.h"

#include "sha1.h"   // recomp-ui's bundled SHA-1 (cartridge identity)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
  #include <windows.h>
  #define BOKTAI_POPEN  _popen
  #define BOKTAI_PCLOSE _pclose
#else
  #include <unistd.h>
  #define BOKTAI_POPEN  popen
  #define BOKTAI_PCLOSE pclose
#endif
#if defined(__APPLE__)
  #include <mach-o/dyld.h>   // _NSGetExecutablePath: the real path, not argv[0]
#endif

namespace fs = std::filesystem;

namespace boktai::builder {
namespace {

// ---- the game, as data ------------------------------------------------------
const GameSpec kSpec = {
    /* variant        */ "boktai1_usa",
    /* target         */ "BoktaiRecomp",
    /* rom_sha1       */ "7164326283df46a3941ec7b6ceca889cbc40e660",
    /* bios_sha1      */ "300c20df6731a33952ded8c436f7f186d25d3492",
    /* recompile_toml */ "variants/boktai1_usa/symbols/boktai1_usa_recompile.toml",
    // Bumped whenever the game's sources change in a way a player must
    // recompile to see. v2: the solar gauge tracks direct sun instead of plain
    // GHI (dense overcast used to read 4/8 bars), plus the sharp-scaler toggle
    // and --touch-ui. Without a bump the stamp still matches and the wrapper
    // execs the stale binary.
    /* app_version    */ "2",
};

// The build needs headroom for the generated C (~64 MB), the build tree
// (~105 MB) and the linked binary (~15 MB), plus slack for the linker's peak.
constexpr std::uintmax_t kNeedBytes = 400ull * 1024 * 1024;

// Directories and files never worth copying into the work tree. `roms` and the
// cartridge/BIOS patterns are here for the same reason they are in the
// packaging skip lists: so nothing copyrighted is ever duplicated by us, even
// when a developer's checkout happens to contain it.
bool excluded(const std::string& name) {
    static const char* kNames[] = {
        ".git", "build", "flatpak-build", "release-stage", "roms",
        ".github", "docs", "gba_bios.bin",
        // The prebuilt recompiler the payload ships in sources/bin/. It is
        // already usable where it is, so copying 5 MB of it into the work tree
        // every build would be pure waste.
        "bin",
    };
    for (const char* n : kNames)
        if (name == n) return true;
    const std::string ext = fs::path(name).extension().string();
    return ext == ".gba" || ext == ".agb";
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// Quote a path for the shell popen() hands the command to. Paths with spaces
// are normal on every platform this runs on ("Application Support", "Program
// Files"), so this is not optional.
std::string q(const std::string& s) { return "\"" + s + "\""; }

// Where the running executable lives.
//
// argv[0] is NOT good enough, and getting this wrong is silent. When a program is
// started via PATH — which is exactly how a Flatpak, a .desktop entry and Steam
// all start it — argv[0] is the bare command name with no separator, so
// fs::absolute() resolves it against the CURRENT DIRECTORY. The builder then
// looked for its bundled sources next to the cwd: launched from /tmp it reported
// `sources: /tmp`, and launched from a checkout it silently used that checkout
// instead of its own payload. So ask the OS for the real path and keep argv[0]
// only as a last resort.
std::string exe_dir(const char* argv0) {
    std::error_code ec;

#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf))
        return fs::path(std::string(buf, n)).parent_path().string();
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t cap = (std::uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &cap) == 0) {
        // May contain symlinks or '..'; canonicalise so the walk-up below and the
        // sources/ probe both see a real path.
        fs::path real = fs::weakly_canonical(fs::path(buf), ec);
        if (!ec) return real.parent_path().string();
        return fs::path(buf).parent_path().string();
    }
#else
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) return self.parent_path().string();
#endif

    fs::path p = argv0 ? fs::path(argv0) : fs::path();
    // Only trust argv[0] when it actually carries a path; a bare name means it
    // came off PATH and says nothing about where the binary is.
    if (!p.empty() && p.has_parent_path()) {
        fs::path abs = fs::absolute(p, ec);
        if (!ec) return abs.parent_path().string();
    }
    return fs::current_path(ec).string();
}

// Per-user writable base. Inside a Flatpak, XDG_DATA_HOME already points at
// ~/.var/app/<id>/data, so the same code lands in the sandboxed location with
// no special case.
std::string user_data_base() {
    auto env = [](const char* k) -> const char* {
        const char* v = std::getenv(k);
        return (v && v[0]) ? v : nullptr;
    };
#if defined(_WIN32)
    if (const char* v = env("LOCALAPPDATA")) return v;
    if (const char* v = env("USERPROFILE")) return std::string(v) + "\\AppData\\Local";
#elif defined(__APPLE__)
    if (const char* v = env("HOME")) return std::string(v) + "/Library/Application Support";
#else
    if (const char* v = env("XDG_DATA_HOME")) return v;
    if (const char* v = env("HOME")) return std::string(v) + "/.local/share";
#endif
    return ".";
}

// ---- logging ----------------------------------------------------------------
// One append-mode file for the whole run. When a build fails the difference
// between "it didn't work" and something diagnosable is this file.
struct Log {
    std::ofstream out;
    explicit Log(const std::string& path) : out(path, std::ios::app) {}
    void line(const std::string& s) {
        if (out) { out << s << '\n'; out.flush(); }
    }
};

// ---- running a command ------------------------------------------------------
// popen with stderr folded in: compiler diagnostics and ninja progress arrive
// on different streams and both matter. Ninja notices it is not a tty and
// prints one line per edge instead of overwriting with \r, which is exactly the
// format parse_ninja_line() wants.
int run(const std::string& cmd, Log& log,
        const std::function<void(const std::string&)>& on_line) {
    log.line("$ " + cmd);
    FILE* pipe = BOKTAI_POPEN((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        log.line("!! could not start the command");
        return -1;
    }
    std::string buf;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), pipe)) {
        buf.assign(chunk);
        while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r'))
            buf.pop_back();
        log.line(buf);
        if (on_line) on_line(buf);
    }
    const int rc = BOKTAI_PCLOSE(pipe);
    log.line("-- exit " + std::to_string(rc));
    return rc;
}

// Run a command for its exit code only, with output discarded. cmd.exe has no
// /dev/null, so the sink differs per platform.
int quiet_system(const std::string& cmd) {
#if defined(_WIN32)
    return std::system((cmd + " > nul 2>&1").c_str());
#else
    return std::system((cmd + " > /dev/null 2>&1").c_str());
#endif
}

// A CMake generator is only worth asking for if it exists. Ninja gives real
// per-edge progress; without it the build still works, it just reports coarsely.
bool have_ninja() {
    static const int rc = quiet_system("ninja --version");
    return rc == 0;
}

std::string generator_flag() {
    return have_ninja() ? " -G Ninja" : "";
}

}  // namespace

const GameSpec& game_spec() { return kSpec; }

// ---- Paths ------------------------------------------------------------------
std::string Paths::work_src() const {
    return (fs::path(data_root) / "src").string();
}
std::string Paths::game_binary() const {
    std::string name = kSpec.target;
#if defined(_WIN32)
    name += ".exe";
#endif
    return (fs::path(work_src()) / "build" / name).string();
}
std::string Paths::stamp_file() const {
    return (fs::path(data_root) / "build.stamp").string();
}
std::string Paths::log_file() const {
    return (fs::path(data_root) / "build.log").string();
}

Paths default_paths(const char* argv0) {
    Paths p;
    p.src_root = exe_dir(argv0);
    // Escape hatch for an out-of-tree build directory (and for CI, which
    // configures wherever it likes). Checked first so it always wins.
    if (const char* over = std::getenv("BOKTAI_BUILDER_SOURCES"); over && over[0]) {
        p.src_root = over;
        p.data_root = (fs::path(user_data_base()) / "BoktaiRecomp").string();
        if (const char* d = std::getenv("BOKTAI_BUILDER_DATA"); d && d[0])
            p.data_root = d;
        return p;
    }
    // A shipped builder keeps its sources in a `sources/` subdirectory; a
    // developer runs it from a build directory inside the checkout. Walk up
    // until something that looks like this project turns up, so the same binary
    // works both ways.
    {
        std::error_code ec;
        fs::path here = fs::path(p.src_root);
        if (fs::exists(here / "sources" / "CMakeLists.txt", ec)) {
            p.src_root = (here / "sources").string();
        } else {
            for (int up = 0; up < 4; ++up) {
                if (fs::exists(here / "CMakeLists.txt", ec) &&
                    fs::exists(here / "variants", ec)) {
                    p.src_root = here.string();
                    break;
                }
                if (!here.has_parent_path()) break;
                here = here.parent_path();
            }
        }
    }
    p.data_root = (fs::path(user_data_base()) / "BoktaiRecomp").string();
    if (const char* d = std::getenv("BOKTAI_BUILDER_DATA"); d && d[0])
        p.data_root = d;
    return p;
}

// ---- hashing ----------------------------------------------------------------
bool verify_sha1(const std::string& path, const char* want_hex,
                 std::string& actual_hex) {
    actual_hex.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (data.empty()) return false;
    unsigned char digest[20];
    char hex[41];
    recompui_sha1_compute(data.data(), data.size(), digest);
    recompui_sha1_hex(digest, hex);
    actual_hex.assign(hex);
    return want_hex && actual_hex == want_hex;
}

// ---- pre-flight -------------------------------------------------------------
namespace {

// Probe the toolchain by actually compiling something. Checking that a compiler
// binary EXISTS is not enough on macOS: without the Command Line Tools, /usr/bin
// clang is a stub that exists, runs, and fails — so the only honest test is a
// real compile.
Preflight probe_toolchain(const Paths& p) {
    Preflight r;
    std::error_code ec;
    fs::create_directories(p.data_root, ec);
    const fs::path probe = fs::path(p.data_root) / "toolchain_probe.cpp";
    {
        std::ofstream f(probe);
        if (!f) {
            r.error = "Cannot write to " + p.data_root + ".";
            return r;
        }
        f << "#include <string>\nint main(){ return std::string(\"ok\").size()==2?0:1; }\n";
    }
    const fs::path out = fs::path(p.data_root) / "toolchain_probe.out";
    Log log(p.log_file());
    const std::string cxx = [] {
        const char* v = std::getenv("CXX");
        return (v && v[0]) ? std::string(v) : std::string("c++");
    }();
    const int rc = run(cxx + " -std=c++20 -x c++ " + q(probe.string()) +
                       " -o " + q(out.string()), log, nullptr);
    fs::remove(probe, ec);
    fs::remove(out, ec);
    if (rc != 0) {
#if defined(__APPLE__)
        r.error  = "No working C++ compiler. macOS needs Apple's Command Line "
                   "Tools, which cannot be bundled with this app.";
        r.remedy = "xcode-select --install";
#elif defined(_WIN32)
        r.error  = "No working C++ compiler found on PATH.";
        r.remedy = "";
#else
        r.error  = "No working C++ compiler found.";
        r.remedy = "sudo apt install build-essential cmake ninja-build";
#endif
        return r;
    }
    if (quiet_system("cmake --version") != 0) {
        r.error = "CMake is not installed or not on PATH.";
#if defined(__APPLE__)
        r.remedy = "brew install cmake";
#elif !defined(_WIN32)
        r.remedy = "sudo apt install cmake ninja-build";
#endif
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace

Preflight preflight(const Paths& p, const Assets& a) {
    Preflight r;
    std::error_code ec;

    if (!fs::exists(fs::path(p.src_root) / "CMakeLists.txt", ec)) {
        r.error  = "No sources found at " + p.src_root + ".";
        r.remedy = "BOKTAI_BUILDER_SOURCES=/path/to/BoktaiRecomp";
        return r;
    }

    // Fingerprints first: rejecting a JP cartridge or a bad dump belongs here,
    // not three minutes into a compile.
    std::string got;
    if (a.rom.empty() || !fs::exists(a.rom, ec)) {
        r.error = "Select your Boktai cartridge dump.";
        return r;
    }
    if (!verify_sha1(a.rom, kSpec.rom_sha1, got)) {
        r.error = got.empty()
            ? "Could not read the cartridge dump."
            : "That is not Boktai: The Sun Is in Your Hand (USA). Its SHA-1 is "
              + got + ".";
        return r;
    }
    if (a.bios.empty() || !fs::exists(a.bios, ec)) {
        r.error = "Select your Game Boy Advance BIOS dump. This runtime executes "
                  "the real BIOS and ships no substitute.";
        return r;
    }
    if (!verify_sha1(a.bios, kSpec.bios_sha1, got)) {
        r.error = got.empty()
            ? "Could not read the BIOS dump."
            : "That is not a Game Boy Advance BIOS. Its SHA-1 is " + got + ".";
        return r;
    }

    fs::create_directories(p.data_root, ec);
    const fs::space_info space = fs::space(p.data_root, ec);
    if (!ec && space.available < kNeedBytes) {
        r.error = "Not enough free disk space: the build needs about " +
                  std::to_string(kNeedBytes / (1024 * 1024)) + " MB.";
        return r;
    }

    return probe_toolchain(p);
}

// ---- ninja progress ---------------------------------------------------------
bool parse_ninja_line(const std::string& line, int& done, int& total,
                      std::string& file) {
    if (line.size() < 5 || line[0] != '[') return false;
    const auto slash = line.find('/');
    const auto close = line.find(']');
    if (slash == std::string::npos || close == std::string::npos ||
        slash > close)
        return false;
    const std::string a = line.substr(1, slash - 1);
    const std::string b = line.substr(slash + 1, close - slash - 1);
    if (a.empty() || b.empty()) return false;
    for (char c : a) if (!std::isdigit((unsigned char)c)) return false;
    for (char c : b) if (!std::isdigit((unsigned char)c)) return false;
    done  = std::atoi(a.c_str());
    total = std::atoi(b.c_str());
    if (total <= 0) return false;

    // The interesting part of "Building CXX object …/recompiled_07.cpp.o" is the
    // filename, so show the last token that looks like a path.
    //
    // The basename is taken by hand rather than with fs::path::filename(),
    // which only treats '\' as a separator when the HOST is Windows. Ninja under
    // MSYS2 emits backslashes, so a parse that depended on host semantics would
    // print the whole path on one platform and the basename on another.
    file.clear();
    std::istringstream ss(line.substr(close + 1));
    std::string tok;
    while (ss >> tok) {
        const auto cut = tok.find_last_of("/\\");
        if (cut != std::string::npos) file = tok.substr(cut + 1);
    }
    return true;
}

// ---- the build --------------------------------------------------------------
namespace {

struct Reporter {
    const ProgressFn& fn;
    Progress          cur;
    float             lo = 0.0f, hi = 1.0f;

    void phase(Step s, float a, float b, std::string headline) {
        lo = a; hi = b;
        cur.step = s;
        cur.fraction = a;
        cur.headline = std::move(headline);
        cur.detail.clear();
        emit();
    }
    void within(float t, std::string detail) {
        cur.fraction = lo + (hi - lo) * std::clamp(t, 0.0f, 1.0f);
        cur.detail = std::move(detail);
        emit();
    }
    void emit() const { if (fn) fn(cur); }
};

// Copy the sources into the writable work tree, over the top of whatever is
// already there. Deliberately NOT a wipe: the build trees live inside the copy,
// and keeping them turns a post-update rebuild from minutes into seconds.
bool copy_sources(const Paths& p, Reporter& rep, Log& log, std::string& err) {
    std::error_code ec;
    const fs::path from = p.src_root, to = p.work_src();
    fs::create_directories(to, ec);

    // Two passes so progress means something; the count is cheap next to the
    // copy itself.
    std::size_t total = 0, n = 0;
    for (int pass = 0; pass < 2; ++pass) {
        fs::recursive_directory_iterator it(
            from, fs::directory_options::skip_permission_denied, ec);
        if (ec) { err = "Cannot read the bundled sources."; return false; }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path& src = it->path();
            if (excluded(src.filename().string())) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) continue;
            if (pass == 0) { ++total; continue; }

            const fs::path rel = fs::relative(src, from, ec);
            if (ec) { ec.clear(); continue; }
            const fs::path dst = to / rel;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, fs::copy_options::update_existing, ec);
            if (ec) {
                log.line("!! copy failed: " + src.string() + " -> " + ec.message());
                ec.clear();
            }
            if (++n % 64 == 0 && total)
                rep.within((float)n / (float)total,
                           std::to_string(n) + "/" + std::to_string(total) + " files");
        }
        ec.clear();
    }
    log.line("-- copied " + std::to_string(n) + " source files to " + to.string());
    return true;
}

}  // namespace

bool build(const Paths& p, const Assets& a, const ProgressFn& on_progress,
           std::string& err) {
    std::error_code ec;
    fs::create_directories(p.data_root, ec);
    // Truncate: the log describes THIS attempt, which is what a player would be
    // asked to send.
    { std::ofstream fresh(p.log_file(), std::ios::trunc); }
    Log log(p.log_file());
    log.line("== BoktaiRecomp builder, app version " +
             std::string(kSpec.app_version));
    log.line("-- sources: " + p.src_root);
    log.line("-- work:    " + p.work_src());
    log.line("-- ninja:   " + std::string(have_ninja() ? "yes" : "no"));
    // Asset paths are logged; their CONTENTS never leave the player's machine.
    log.line("-- rom:     " + a.rom);
    log.line("-- bios:    " + a.bios);

    Reporter rep{on_progress, {}, 0.0f, 1.0f};

    rep.phase(Step::CopySources, 0.00f, 0.05f, "Preparing a build directory");
    if (!copy_sources(p, rep, log, err)) return false;

    const std::string work = p.work_src();
    const fs::path    eng  = fs::path(work) / "gbarecomp";

    // ---- the recompiler ----
    // A shipped builder brings a prebuilt gba_recompile so the player does not
    // compile the compiler; a developer build falls back to building it.
    rep.phase(Step::Recompiler, 0.05f, 0.20f, "Preparing the recompiler");
    std::string recompiler;
    for (const fs::path cand : {fs::path(p.src_root) / "bin" / "gba_recompile",
                                fs::path(p.src_root) / "bin" / "gba_recompile.exe"}) {
        if (fs::exists(cand, ec)) { recompiler = cand.string(); break; }
    }
    if (recompiler.empty()) {
        const std::string bdir = (eng / "build").string();
        if (run("cmake -B " + q(bdir) + " -S " + q(eng.string()) +
                generator_flag() + " -DCMAKE_BUILD_TYPE=Release", log,
                [&](const std::string&) { rep.within(0.2f, "configuring"); }) != 0) {
            err = "Could not configure the recompiler.";
            return false;
        }
        if (run("cmake --build " + q(bdir) + " --target gba_recompile --parallel",
                log, [&](const std::string& l) {
                    int d = 0, t = 0; std::string f;
                    if (parse_ninja_line(l, d, t, f))
                        rep.within(0.2f + 0.8f * (float)d / (float)t, f);
                }) != 0) {
            err = "Could not build the recompiler.";
            return false;
        }
        for (const fs::path cand : {eng / "build" / "gba_recompile",
                                    eng / "build" / "gba_recompile.exe"}) {
            if (fs::exists(cand, ec)) { recompiler = cand.string(); break; }
        }
    }
    if (recompiler.empty()) {
        err = "The recompiler was not produced.";
        return false;
    }

    // ---- the BIOS ----
    // Translated, not high-level-emulated, so it has to exist before the game
    // links. Small (16 KB) and quick.
    rep.phase(Step::Bios, 0.20f, 0.24f, "Recompiling the BIOS");
    const fs::path bios_out = eng / "src" / "runtime" / "generated_bios";
    if (run(q(recompiler) + " --bios " + q(a.bios) +
            " --config " + q((eng / "bios" / "gba_bios.toml").string()) +
            " --out " + q(bios_out.string()), log, nullptr) != 0) {
        err = "Could not recompile the BIOS.";
        return false;
    }

    // ---- the cartridge ----
    rep.phase(Step::Cartridge, 0.24f, 0.36f, "Recompiling the cartridge");
    const fs::path gen = fs::path(work) / "variants" / kSpec.variant / "generated";
    if (run(q(recompiler) + " --rom " + q(a.rom) +
            " --config " + q((fs::path(work) / kSpec.recompile_toml).string()) +
            " --out " + q(gen.string()), log,
            // Only the recompiler's own "==>" progress markers reach the UI.
            // Echoing every line put things like
            //   auto_jt 0x081AE608 count=5 stride=4 site=0x081AE5EE MOVpc bounded
            // in front of a player, which is meaningless to them and reads as
            // something having gone wrong. Everything still goes to the log.
            [&](const std::string& l) {
                const std::string t = trim(l);
                if (t.rfind("==>", 0) != 0) return;
                std::string d = trim(t.substr(3));
                // Trim the parts that only matter in the log: a long absolute
                // path ("==> wrote /very/long/path/…") and a parenthetical
                // breakdown ("discovered 12555 functions (arm=47 thumb=…)"),
                // which a hard character cut would otherwise leave unbalanced
                // mid-sentence.
                if (const auto sp = d.find(" /"); sp != std::string::npos)
                    d = d.substr(0, sp);
                if (const auto par = d.find(" ("); par != std::string::npos)
                    d = d.substr(0, par);
                rep.within(0.5f, trim(d).substr(0, 72));
            }) != 0) {
        err = "Could not recompile the cartridge.";
        return false;
    }
    if (!fs::exists(gen, ec) || fs::is_empty(gen, ec)) {
        err = "The recompiler produced no output.";
        return false;
    }

    // ---- configure + compile ----
    rep.phase(Step::Configure, 0.36f, 0.40f, "Configuring the build");
    const std::string bdir = (fs::path(work) / "build").string();
    if (run("cmake -B " + q(bdir) + " -S " + q(work) + generator_flag() +
            " -DCMAKE_BUILD_TYPE=Release -DGBAGAME_RECOMP_UI=ON", log,
            nullptr) != 0) {
        err = "Could not configure the build.";
        return false;
    }

    rep.phase(Step::Compile, 0.40f, 0.99f, "Compiling the game");
    if (run("cmake --build " + q(bdir) + " --target " + kSpec.target + " --parallel",
            log, [&](const std::string& l) {
                int d = 0, t = 0; std::string f;
                if (parse_ninja_line(l, d, t, f))
                    rep.within((float)d / (float)t,
                               std::to_string(d) + "/" + std::to_string(t) +
                               (f.empty() ? "" : "   " + f));
            }) != 0) {
        err = "The build failed. The full log is at " + p.log_file() + ".";
        return false;
    }

    if (!fs::exists(p.game_binary(), ec)) {
        err = "The build reported success but produced no game binary.";
        return false;
    }

    stamp_write(p, a);
    rep.phase(Step::Done, 1.00f, 1.00f, "Ready to play");
    return true;
}

// ---- the stamp --------------------------------------------------------------
namespace {

std::string stamp_expected(const Assets& a) {
    std::string rom_hex, bios_hex;
    verify_sha1(a.rom, nullptr, rom_hex);
    verify_sha1(a.bios, nullptr, bios_hex);
    return std::string("version=") + kSpec.app_version +
           "\nrom=" + rom_hex + "\nbios=" + bios_hex + "\n";
}

}  // namespace

void stamp_write(const Paths& p, const Assets& a) {
    std::error_code ec;
    fs::create_directories(p.data_root, ec);
    std::ofstream f(p.stamp_file(), std::ios::trunc);
    if (!f) return;
    // The asset PATHS are recorded so a returning player is not asked to pick
    // their files again. Paths only — never contents.
    f << stamp_expected(a) << "rom_path=" << a.rom << "\nbios_path=" << a.bios
      << '\n';
}

bool stamp_ok(const Paths& p, const Assets& a) {
    std::ifstream f(p.stamp_file());
    if (!f) return false;
    std::string got((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return got.rfind(stamp_expected(a), 0) == 0;
}

bool stamp_read_assets(const Paths& p, Assets& out) {
    std::ifstream f(p.stamp_file());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = trim(line.substr(eq + 1));
        if (key == "rom_path")  out.rom  = val;
        if (key == "bios_path") out.bios = val;
    }
    std::error_code ec;
    return !out.rom.empty() && !out.bios.empty() &&
           fs::exists(out.rom, ec) && fs::exists(out.bios, ec);
}

bool ready_to_play(const Paths& p, const Assets& a) {
    std::error_code ec;
    return fs::exists(p.game_binary(), ec) && stamp_ok(p, a);
}

}  // namespace boktai::builder

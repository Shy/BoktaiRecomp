// builder_main.cpp — the thing a player actually launches.
//
// It is a WRAPPER first and a builder second:
//
//     launch:
//       if a built game exists and the stamp matches -> exec it
//       else                                         -> show the builder, then exec it
//
// So the player adds ONE thing to Steam, once. First launch builds with a
// progress bar; every launch after that boots straight into the game. Gaming
// Mode never sees a missing binary and there is no second icon to add after a
// build. See docs/BUILDER.md.
//
// Modes:
//   (none)            wrapper: play, or build then play
//   --build           headless build, no window (what CI runs)
//   --check           headless pre-flight only
//   --rebuild         force a build even if the stamp matches
//   --paths           print the resolved locations and exit
//   --rom/--bios P    supply assets without the GUI
//
// Anything else is passed through to the game.

#include "builder_core.h"

#include <SDL.h>
#include <SDL_opengl.h>   // glClear/glViewport: SDL's portable way in, no loader needed
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "launcher_debug.h"   // launcher_capture_png(): recomp-ui's framebuffer grab
#include "third_party/tinyfiledialogs.h"

#if defined(_WIN32)
  #include <process.h>
#else
  #include <unistd.h>
#endif

using namespace boktai::builder;

namespace {

// ---- handing off to the game ------------------------------------------------
// On POSIX this REPLACES the process, so Steam keeps tracking the same pid and
// the wrapper costs nothing after the first launch. Windows has no real exec,
// so spawn-and-wait and forward the exit code.
[[noreturn]] void exec_game(const std::string& binary,
                            const std::vector<std::string>& extra) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const auto& s : extra) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
#if defined(_WIN32)
    const intptr_t rc = _spawnv(_P_WAIT, binary.c_str(), argv.data());
    std::exit(rc < 0 ? 1 : (int)rc);
#else
    execv(binary.c_str(), argv.data());
    std::fprintf(stderr, "could not start %s: %s\n", binary.c_str(),
                 std::strerror(errno));
    std::exit(1);
#endif
}

// ---- shared state between the build thread and the frame loop ---------------
struct Shared {
    std::mutex        m;
    Progress          progress;
    std::string       error;      // set on failure
    std::string       remedy;
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
};

void start_build(Shared& sh, const Paths& paths, const Assets& assets) {
    sh.running = true;
    sh.done = false;
    { std::lock_guard<std::mutex> lk(sh.m); sh.error.clear(); sh.remedy.clear(); }
    std::thread([&sh, paths, assets] {
        const Preflight pf = preflight(paths, assets);
        if (!pf.ok) {
            std::lock_guard<std::mutex> lk(sh.m);
            sh.error = pf.error;
            sh.remedy = pf.remedy;
            sh.ok = false;
            sh.done = true;
            sh.running = false;
            return;
        }
        std::string err;
        const bool ok = build(paths, assets, [&sh](const Progress& pr) {
            std::lock_guard<std::mutex> lk(sh.m);
            sh.progress = pr;
        }, err);
        {
            std::lock_guard<std::mutex> lk(sh.m);
            if (!ok) sh.error = err;
        }
        sh.ok = ok;
        sh.done = true;
        sh.running = false;
    }).detach();
}

// ---- asset picking ----------------------------------------------------------
std::string pick(const char* title, const char* const* patterns, int n,
                 const char* desc) {
    const char* r = tinyfd_openFileDialog(title, "", n, patterns, desc, 0);
    return r ? std::string(r) : std::string();
}

// A one-line verdict per asset, so a wrong file is obvious before the wait
// rather than after it.
struct AssetStatus { bool ok = false; std::string line; };

AssetStatus check_asset(const std::string& path, const char* want,
                        const char* what) {
    AssetStatus s;
    if (path.empty()) { s.line = std::string("No ") + what + " selected."; return s; }
    std::string got;
    if (verify_sha1(path, want, got)) {
        s.ok = true;
        s.line = "Verified.";
    } else if (got.empty()) {
        s.line = "Could not read that file.";
    } else {
        s.line = "Wrong file - SHA-1 " + got.substr(0, 16) + "...";
    }
    return s;
}

// Self-test: render a few frames and write the real framebuffer to a PNG, then
// exit. This is the only way to check that the builder's window actually DRAWS
// — the screen a player sees before anything has been built is the one screen no
// other test can reach, and a progress bar nobody can see is worthless. It has
// already earned its keep: it caught overflowing text, truncated buttons, and
// '…' rendering as '?' in the built-in font.
struct SelfTest {
    std::string png;      // empty => not a self-test run
    int         frames = 20;
    // Start the build immediately so the capture lands on the PROGRESS screen.
    // That is the screen a player looks at for several minutes, so it is the one
    // most worth having a picture of; the process exits mid-build, which is safe
    // because the build is incremental and nothing is left half-written.
    bool        building = false;
};

// ---- the window -------------------------------------------------------------
// Small and self-contained on purpose: it must work before anything has been
// built, so it depends on nothing but SDL, ImGui and the built-in font. No asset
// files, so a missing assets/ directory cannot stop a player from building.
//
// That built-in font is ASCII-only, so every string drawn here stays ASCII —
// an em-dash or an ellipsis renders as '?'. The window title (SDL) and the
// stdout lines (a UTF-8 terminal) are not subject to that and keep their
// typography.
int run_gui(const Paths& paths, Assets assets,
            const std::vector<std::string>& passthrough,
            const SelfTest& selftest) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow(
        "Boktai — first-time setup", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1000, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);

    // A Steam Deck has no mouse in Gaming Mode, so every control has to be
    // reachable with the stick and A. Opening the pads explicitly means the
    // first frame already has gamepad nav.
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // Font scale is the HiDPI ratio and nothing more: ImGui lays out in logical
    // points, so scaling by drawable/window is exactly what keeps the text the
    // same PHYSICAL size everywhere. Multiplying on top of that overflowed the
    // window on a Retina display (2.0 * 1.45 = 2.9, and the first paragraph
    // scrolled off the top). The floor covers a Steam Deck, which is 1280x800
    // with no HiDPI scaling but only 7" across, so 1.0 would be too small.
    {
        int ww = 0, dw = 0, wh = 0, dh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        const float hidpi = (ww > 0) ? (float)dw / (float)ww : 1.0f;
        io.FontGlobalScale = std::clamp(hidpi, 1.3f, 2.0f);
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding  = 4.0f;
    st.FramePadding   = ImVec2(10, 7);
    st.ItemSpacing    = ImVec2(10, 9);
    st.ScaleAllSizes(io.FontGlobalScale);
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    Shared sh;
    bool quit = false, finished = false, selftest_ok = false;
    int  frame_count = 0;

    const char* kRomPatterns[]  = {"*.gba", "*.agb", "*.bin"};
    const char* kBiosPatterns[] = {"*.bin", "*.rom"};

    AssetStatus rom_st  = check_asset(assets.rom,  game_spec().rom_sha1,  "cartridge dump");
    AssetStatus bios_st = check_asset(assets.bios, game_spec().bios_sha1, "BIOS dump");

    if (selftest.building && rom_st.ok && bios_st.ok)
        start_build(sh, paths, assets);

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_WINDOWEVENT &&
                e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win))
                quit = true;
        }
        // A build must not be abandoned half-way: a partial build tree is fine
        // (it is incremental) but quitting mid-link wastes the whole wait.
        if (quit && sh.running) quit = false;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("builder", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        Progress pr;
        std::string err, remedy;
        {
            std::lock_guard<std::mutex> lk(sh.m);
            pr = sh.progress;
            err = sh.error;
            remedy = sh.remedy;
        }

        if (sh.done && sh.ok) {
            // Build succeeded: say so, then hand over. Kept as an explicit
            // button rather than an automatic jump so the player sees that it
            // worked and understands the next launch skips all of this.
            ImGui::TextUnformatted("Boktai is built.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "This only happens once. Next time you launch, it goes straight "
                "into the game.");
            ImGui::Spacing();
            if (ImGui::Button("Play", ImVec2(0, 0))) finished = true;
            ImGui::SetItemDefaultFocus();
        } else if (sh.running) {
            ImGui::TextUnformatted(pr.headline.c_str());
            ImGui::Spacing();
            ImGui::ProgressBar(pr.fraction, ImVec2(-1, 0));
            ImGui::Spacing();
            ImGui::TextDisabled("%s", pr.detail.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Compiling a Game Boy Advance game into a native one takes a few "
                "minutes. You only pay this once.");
        } else {
            ImGui::TextUnformatted("Boktai needs to be built once on this machine.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "This app contains no game data. Supply your own cartridge dump "
                "and Game Boy Advance BIOS; they are read where they are and "
                "never copied anywhere.");
            ImGui::Separator();
            ImGui::Spacing();

            // ---- the two assets ----
            auto asset_row = [&](const char* label, std::string& path,
                                 AssetStatus& st, const char* const* pats,
                                 int npats, const char* desc, const char* title,
                                 const char* want, const char* what) {
                ImGui::TextUnformatted(label);
                if (ImGui::Button((std::string("Choose...##") + label).c_str(),
                                  ImVec2(0, 0))) {
                    std::string picked = pick(title, pats, npats, desc);
                    if (!picked.empty()) {
                        path = picked;
                        st = check_asset(path, want, what);
                    }
                }
                ImGui::SameLine();
                if (path.empty()) {
                    ImGui::TextDisabled("nothing selected");
                } else {
                    const ImVec4 col = st.ok ? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
                                             : ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
                    ImGui::TextColored(col, "%s", st.line.c_str());
                    ImGui::TextDisabled("%s", path.c_str());
                }
                ImGui::Spacing();
            };

            asset_row("Boktai cartridge dump (.gba)", assets.rom, rom_st,
                      kRomPatterns, 3, "Game Boy Advance ROM",
                      "Select your Boktai cartridge dump",
                      game_spec().rom_sha1, "cartridge dump");
            asset_row("Game Boy Advance BIOS (16 KB)", assets.bios, bios_st,
                      kBiosPatterns, 2, "Game Boy Advance BIOS",
                      "Select your Game Boy Advance BIOS dump",
                      game_spec().bios_sha1, "BIOS dump");

            if (!err.empty()) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                                   err.c_str());
                if (!remedy.empty()) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("Run this in a terminal, then start this "
                                       "app again:");
                    ImGui::Spacing();
                    ImGui::InputText("##remedy", remedy.data(), remedy.size() + 1,
                                     ImGuiInputTextFlags_ReadOnly);
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Full log: %s", paths.log_file().c_str());
            }

            ImGui::Separator();
            ImGui::Spacing();
            const bool can = rom_st.ok && bios_st.ok;
            if (!can) ImGui::BeginDisabled();
            if (ImGui::Button("Build and play", ImVec2(0, 0)))
                start_build(sh, paths, assets);
            if (can) ImGui::SetItemDefaultFocus();
            if (!can) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(0, 0))) quit = true;
        }

        ImGui::End();

        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(win, &w, &h);
        ImGui::Render();
        glViewport(0, 0, w, h);
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Capture BEFORE the swap: with a double-buffered context the frame just
        // drawn is still the read buffer here. After the swap it is whatever was
        // on screen previously, which on the first frames is uninitialised.
        if (!selftest.png.empty() && ++frame_count >= selftest.frames) {
            selftest_ok = launcher_capture_png(selftest.png.c_str(), w, h);
            SDL_GL_SwapWindow(win);
            break;
        }
        SDL_GL_SwapWindow(win);

        if (finished) break;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (!selftest.png.empty()) {
        if (!selftest_ok) {
            std::fprintf(stderr, "could not write %s\n", selftest.png.c_str());
            return 1;
        }
        std::printf("wrote %s\n", selftest.png.c_str());
        return 0;
    }

    if (finished) exec_game(paths.game_binary(), passthrough);
    return 0;
}

void print_progress_line(const Progress& pr) {
    static int last = -1;
    static std::string last_headline;
    const int pct = (int)(pr.fraction * 100.0f);
    // Dedupe on the percentage, but never swallow a new phase: two phases can
    // start at the same rounded percentage (the BIOS step is only 4% wide), and
    // dropping its headline made the log look like the BIOS was never
    // recompiled at all.
    if (pct == last && pr.detail.empty() && pr.headline == last_headline) return;
    last = pct;
    last_headline = pr.headline;
    std::printf("[%3d%%] %s%s%s\n", pct, pr.headline.c_str(),
                pr.detail.empty() ? "" : " — ", pr.detail.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    Paths paths = default_paths(argc > 0 ? argv[0] : nullptr);
    Assets assets;
    bool headless = false, check_only = false, force = false, show_paths = false;
    bool no_gui = false;
    SelfTest selftest;
    std::vector<std::string> passthrough;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) dst = argv[++i];
        };
        if (a == "--build")        { headless = true; no_gui = true; }
        else if (a == "--check")   { check_only = true; no_gui = true; }
        else if (a == "--rebuild") { force = true; }
        else if (a == "--paths")   { show_paths = true; no_gui = true; }
        else if (a == "--rom")     { next(assets.rom); }
        else if (a == "--bios")    { next(assets.bios); }
        else if (a == "--shot")    { next(selftest.png); force = true; }
        else if (a == "--shot-building") { selftest.building = true; }
        else if (a == "--shot-frames") {
            std::string n; next(n);
            if (!n.empty()) selftest.frames = std::max(1, std::atoi(n.c_str()));
        }
        else if (a == "--builder-help" || a == "-h" || a == "--help") {
            std::printf(
                "BoktaiRecomp builder — builds the game from your own cartridge dump.\n\n"
                "  (no arguments)   play; build first if needed\n"
                "  --build          build without a window\n"
                "  --check          run the pre-flight checks only\n"
                "  --rebuild        build even if it is already up to date\n"
                "  --paths          print the resolved locations\n"
                "  --rom PATH       cartridge dump (skips the picker)\n"
                "  --bios PATH      GBA BIOS dump (skips the picker)\n"
                "  --shot PATH      render the window to a PNG and exit (self-test)\n"
                "  --shot-frames N  frames to settle before --shot (default 20)\n\n"
                "Any other argument is passed through to the game.\n");
            return 0;
        }
        else passthrough.push_back(a);
    }

    if (show_paths) {
        std::printf("sources : %s\n", paths.src_root.c_str());
        std::printf("data    : %s\n", paths.data_root.c_str());
        std::printf("work    : %s\n", paths.work_src().c_str());
        std::printf("game    : %s\n", paths.game_binary().c_str());
        std::printf("stamp   : %s\n", paths.stamp_file().c_str());
        std::printf("log     : %s\n", paths.log_file().c_str());
        return 0;
    }

    // A returning player already told us where their files are; asking again
    // every launch would defeat the point of the wrapper.
    if (assets.rom.empty() || assets.bios.empty()) {
        Assets remembered;
        if (stamp_read_assets(paths, remembered)) {
            if (assets.rom.empty())  assets.rom  = remembered.rom;
            if (assets.bios.empty()) assets.bios = remembered.bios;
        }
    }

    if (check_only) {
        const Preflight pf = preflight(paths, assets);
        if (pf.ok) { std::printf("pre-flight ok\n"); return 0; }
        std::fprintf(stderr, "%s\n", pf.error.c_str());
        if (!pf.remedy.empty()) std::fprintf(stderr, "  try: %s\n", pf.remedy.c_str());
        return 1;
    }

    if (!force && ready_to_play(paths, assets)) {
        if (no_gui) { std::printf("already up to date\n"); return 0; }
        exec_game(paths.game_binary(), passthrough);
    }

    if (headless) {
        const Preflight pf = preflight(paths, assets);
        if (!pf.ok) {
            std::fprintf(stderr, "%s\n", pf.error.c_str());
            if (!pf.remedy.empty())
                std::fprintf(stderr, "  try: %s\n", pf.remedy.c_str());
            return 1;
        }
        std::string err;
        if (!build(paths, assets, print_progress_line, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("built: %s\n", paths.game_binary().c_str());
        return 0;
    }

    return run_gui(paths, assets, passthrough, selftest);
}

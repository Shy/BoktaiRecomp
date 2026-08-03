// test_builder_core.cpp — the builder's two pure pieces.
//
// Both are load-bearing in a way that fails QUIETLY, which is why they are
// tested at all:
//
//   parse_ninja_line  a wrong parse does not crash, it just makes the progress
//                     bar wrong. On a handheld with no terminal, a bar stuck at
//                     0% for five minutes is indistinguishable from a hang.
//   verify_sha1       this is the gate that rejects a JP cartridge or a bad
//                     dump BEFORE the multi-minute build. If it silently
//                     always-passed, the failure would surface as a mysterious
//                     recompiler crash minutes later.
//
// Everything else in builder_core touches the filesystem, spawns compilers and
// takes minutes; that path is covered by actually running `--build`.

#include "builder_core.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace boktai::builder;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}

void expect_ninja(const std::string& line, int want_done, int want_total,
                  const std::string& want_file) {
    int d = -1, t = -1;
    std::string f = "unset";
    const bool got = parse_ninja_line(line, d, t, f);
    check(got && d == want_done && t == want_total && f == want_file,
          "\"" + line.substr(0, 42) + "\"");
    if (got && (d != want_done || t != want_total || f != want_file))
        std::printf("      got %d/%d file=\"%s\"\n", d, t, f.c_str());
}

void expect_not_ninja(const std::string& line) {
    int d = 0, t = 0;
    std::string f;
    check(!parse_ninja_line(line, d, t, f),
          "rejects \"" + line.substr(0, 40) + "\"");
}

}  // namespace

int main() {
    std::printf("parse_ninja_line — accepts real ninja output\n");
    expect_ninja("[412/1180] Building CXX object CMakeFiles/BoktaiRecomp.dir/"
                 "variants/boktai1_usa/generated/recompiled_07.cpp.o",
                 412, 1180, "recompiled_07.cpp.o");
    expect_ninja("[1/1] Linking CXX executable BoktaiRecomp", 1, 1, "");
    expect_ninja("[3/4] ", 3, 4, "");
    // Windows separators, since the same parse runs under MinGW.
    expect_ninja("[7/9] Building CXX object CMakeFiles\\x.dir\\main.cpp.obj",
                 7, 9, "main.cpp.obj");

    std::printf("\nparse_ninja_line — rejects everything else\n");
    expect_not_ninja("");
    expect_not_ninja("ninja: no work to do.");
    expect_not_ninja("-- Configuring done");
    expect_not_ninja("[abc/def] nonsense");
    expect_not_ninja("[0/0] zero total would divide by zero");
    expect_not_ninja("[5] no slash");
    expect_not_ninja("no bracket at all 1/2");
    // A compiler diagnostic that happens to start with a bracket must not be
    // mistaken for progress.
    expect_not_ninja("[-Wunused-variable] warning here");

    std::printf("\nverify_sha1 — the pre-build asset gate\n");
    {
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() / "boktai_sha1_probe";
        { std::ofstream f(tmp, std::ios::binary); f << "abc"; }
        std::string got;
        const char* kAbc = "a9993e364706816aba3e25717850c26c9cd0d89d";
        check(verify_sha1(tmp.string(), kAbc, got), "matches a known digest");
        check(got == kAbc, "reports the digest it computed");
        check(!verify_sha1(tmp.string(), "0000000000000000000000000000000000000000", got),
              "rejects a mismatch");
        check(got == kAbc, "still reports the REAL digest on mismatch");
        std::filesystem::remove(tmp);
        check(!verify_sha1(tmp.string(), kAbc, got), "rejects a missing file");
        check(got.empty(), "reports no digest for a missing file");
    }

    std::printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}

// The launcher's build on Windows, with stand-ins for the recompiler, the
// compiler and the game (tools/check/launcher_stub.cpp): what is compiled
// again and what comes from the cache, the identity that decides a rebuild,
// a locked .exe, the space check, Unicode and shell metacharacters in every
// path, and the real recompiler's Unicode paths. No game data.
//
//   qilin_launcher_build_test <launcher_stub.exe> <qilin.exe> <scratch folder>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#include "build.hpp"
#include "disc.hpp"
#include "kit.hpp"
#include "process.hpp"

namespace fs = std::filesystem;
using namespace qilin::launcher;

namespace {

int failures = 0;

void check(bool ok, const std::string& what, int line) {
    if (ok) return;
    std::printf("FAILED (line %d): %s\n", line, what.c_str());
    std::fflush(stdout);
    ++failures;
}
#define CHECK(x) check((x), #x, __LINE__)

void put(const fs::path& path, std::string_view text, bool append = false) {
    std::ofstream f(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    f << text;
}

int count_lines(const fs::path& path, std::string_view what) {
    std::ifstream f(path);
    int n = 0;
    for (std::string line; std::getline(f, line);) n += line == what;
    return n;
}

std::string file_hash(const fs::path& path) {
    const auto h = sha256_file(path);
    return h ? *h : std::string();
}

void set_env(const wchar_t* name, const wchar_t* value) { SetEnvironmentVariableW(name, value); }

void test_quoting() {
    for (const std::wstring s : {L"plain", L"with space", L"", L"quote\"inside", L"trailing\\", L"trailing space\\", L"a\\\\\"b",
                                 L"%PATH% & | < > ^", L"\x4fdd\x5b58 \x00e9", L"\\\\server\\share\\x y"}) {
        const std::wstring line = L"x.exe " + quote_argument(s);
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(line.c_str(), &argc);
        CHECK(argv && argc == 2 && s == argv[1]);
        if (argv) LocalFree(argv);
    }
}

}  // namespace

int main(int argc, char**) {
    if (argc != 4) {
        std::printf("usage: qilin_launcher_build_test <launcher_stub.exe> <qilin.exe> <scratch folder>\n");
        return 2;
    }
    // The command line as UTF-16 (the scratch folder may be anywhere).
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != 4) return 2;
    const fs::path stub = wargv[1], qilin = wargv[2], scratch = wargv[3];
    LocalFree(wargv);

    test_quoting();

    // A download folder with spaces, a non-ASCII letter and shell characters.
    const fs::path top = scratch / L"kit with spaces \x00e9 & %PATH%";
    std::error_code ec;
    fs::remove_all(top, ec);
    for (const char* dir : {"kit/lib", "kit/include", "kit/toolchain/bin", "user"}) fs::create_directories(top / dir);
    const fs::path launcher = top / "Play Fixture.exe";
    fs::copy_file(stub, launcher);
    fs::copy_file(stub, top / "kit" / "qilin.exe");
    fs::copy_file(stub, top / "kit" / "toolchain" / "bin" / "x86_64-w64-mingw32-clang++.exe");
    put(top / "kit" / "include" / "stub.hpp", "header1");
    put(top / "kit" / "VERSION", "launcher-test\n");
    put(top / "kit" / "game.txt",
        "game=fixturegame\r\ntitle=Fixture\r\nexe=Fixture.exe\r\nboot=FIXTURE.BIN\r\nbuild=Fixture (World)\r\nproduct=T-0\r\ndisc_version=1.000\r\n"
        "package=Fixture--QilinRecomp\r\n");
    const fs::path archive = top / "kit" / "lib" / "libfixture_kit.a";
    put(archive, "fixture");
    put(top / "kit" / "kit.ninja", "cxxflags = -O2 -I$kit/include\nldflags = -static\nlibs = $kit/lib/libfixture_kit.a\n");
    const fs::path disc = top / L"disc %PATH% & \x4fdd\x5b58.cue";
    put(disc, "synthetic cue, no game data");

    auto loaded = load_kit(launcher, top / "user");
    CHECK(loaded.has_value());
    if (!loaded) {
        std::printf("%s\n", loaded.error().c_str());
        return 1;
    }
    const Kit kit = *loaded;
    CHECK(kit.title == "Fixture" && kit.exe == "Fixture.exe" && kit.version == "launcher-test");
    CHECK(kit.wanted_disc() == "Fixture (World), T-0 version 1.000");
    const fs::path calls = kit.work / "tool-calls.txt";
    const auto compiles = [&] { return count_lines(calls, "compile"); };
    const auto links = [&] { return count_lines(calls, "link"); };
#define CHECK_CALLS(c, l)                                                                                   \
    do {                                                                                                    \
        const int got_c = compiles(), got_l = links();                                                      \
        check(got_c == (c) && ((l) < 0 || got_l == (l)),                                                    \
              "tool calls: " + std::to_string(got_c) + " compiles, " + std::to_string(got_l) + " links", __LINE__); \
    } while (0)

    BuildOptions options;
    options.check_disc = false;
    options.need_bytes = 0;
    options.native.clear();
    BuildProgress progress;
    std::atomic<bool> cancel{false};
    const auto build = [&] {
        const auto r = build_game(kit, disc, progress, cancel, options);
        if (!r) std::printf("build: %s\n", r.error().c_str());
        return r.has_value();
    };

    // The first build compiles; the .exe is in place and up to date.
    auto identity = build_identity(kit);
    CHECK(identity.has_value());
    CHECK(!up_to_date(kit, *identity));
    CHECK(build());
    CHECK_CALLS(2, 1);
    CHECK(progress.stage == Stage::Done && progress.total == 2 && progress.done == 2);
    CHECK(fs::is_regular_file(kit.game_exe()));
    CHECK(up_to_date(kit, *identity));
    CHECK(fs::is_regular_file(kit.build_log()));
    std::string hash = file_hash(kit.game_exe());

    // Built again, unchanged: everything from the cache, linked again.
    CHECK(build());
    CHECK_CALLS(2, 2);
    CHECK(progress.cached == 2);

    // A new download folder (no work\ at all) knows the cache.
    fs::remove_all(kit.work);
    CHECK(build());
    CHECK_CALLS(0, 1);  // tool-calls.txt started again with the work folder

    // A changed runtime library (same version, old timestamp): out of date,
    // relinked, nothing compiled.
    put(archive, "same version, new contents", true);
    fs::last_write_time(archive, fs::last_write_time(archive) - std::chrono::hours(24 * 365));
    identity = build_identity(kit);
    CHECK(!up_to_date(kit, *identity));
    CHECK(build());
    CHECK_CALLS(0, 2);
    CHECK(file_hash(kit.game_exe()) != hash);
    CHECK(up_to_date(kit, *identity));

    // A changed header compiles again (whatever its timestamp).
    put(top / "kit" / "include" / "stub.hpp", "header2");
    fs::last_write_time(top / "kit" / "include" / "stub.hpp", fs::last_write_time(archive));
    CHECK(build());
    CHECK_CALLS(2, -1);  // the two sources
    // So does another compiler...
    set_env(L"QILIN_STUB_COMPILER", L"2");
    CHECK(build());
    CHECK_CALLS(4, -1);
    // ...and another CPU (-march=native), which also makes the build out of date.
    std::wstring cpu(512, L'\0');
    cpu.resize(GetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER", cpu.data(), 512));
    set_env(L"PROCESSOR_IDENTIFIER", L"synthetic different processor");
    identity = build_identity(kit);
    CHECK(!up_to_date(kit, *identity));
    CHECK(build());
    CHECK_CALLS(6, -1);
    // Back to the first: still in the cache.
    set_env(L"PROCESSOR_IDENTIFIER", cpu.c_str());
    set_env(L"QILIN_STUB_COMPILER", nullptr);
    put(top / "kit" / "include" / "stub.hpp", "header1");
    CHECK(build());
    CHECK_CALLS(6, -1);
    // Built from scratch: compiled again, cache or not.
    options.use_cache = false;
    CHECK(build());
    CHECK_CALLS(8, -1);
    options.use_cache = true;
    CHECK(fs::is_regular_file(top / "Fixture.exe.previous"));

    // A running (locked) game: the build fails, and the old .exe and its
    // record stay as they were.
    hash = file_hash(kit.game_exe());
    const auto stamp = built_with(kit);
    put(archive, "another update", true);
    {
        const HANDLE locked = CreateFileW(kit.game_exe().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(locked != INVALID_HANDLE_VALUE);
        const auto r = build_game(kit, disc, progress, cancel, options);
        CHECK(!r.has_value());
        CHECK(!r && r.error().find("Can't replace Fixture.exe") != std::string::npos);
        CloseHandle(locked);
    }
    CHECK(file_hash(kit.game_exe()) == hash);
    CHECK(built_with(kit) == stamp);
    CHECK(build());

    // Not enough room: said plainly, with the drive and the amounts.
    options.need_bytes = ~0ull >> 2;
    {
        const auto r = build_game(kit, disc, progress, cancel, options);
        CHECK(!r && r.error().find("free on drive") != std::string::npos);
    }
    options.need_bytes = 0;

    // The cache is trimmed to its newest: with no room, nothing stays.
    options.cache_bytes = 0;
    CHECK(build());
    CHECK(fs::is_empty(kit.cache()));
    options.cache_bytes = 2ull << 30;

    // The game gets the disc's path intact, and its output goes to the logs.
    {
        Command c;
        c.exe = kit.game_exe();
        c.args = {show(disc)};
        c.cwd = kit.top;
        c.out = kit.top / "run.log";
        c.err = kit.top / "run-errors.log";
        c.build_tool = false;
        auto game = Process::start(c);
        CHECK(game.has_value());
        CHECK(game && game->wait() == 0);
        const auto launched = read_file(kit.top / "launched.txt");
        CHECK(launched && *launched == show(disc));
        CHECK(fs::exists(kit.top / "run-errors.log"));
    }

    // The disc check's plain messages for what isn't a disc.
    CHECK(!check_disc(kit, top / "missing.cue").ok);
    CHECK(check_disc(kit, top / "missing.cue").message.starts_with("Can't find"));
    put(top / "notes.txt", "x");
    CHECK(check_disc(kit, top / "notes.txt").message.starts_with("That's not a disc image"));
    CHECK(disc_image_name("a.CUE") && disc_image_name("a.gdi") && disc_image_name("a.chd") && !disc_image_name("a.bin"));
    CHECK(!check_disc(kit, disc).ok);  // a .cue that isn't one

    // The real recompiler: a Unicode boot file and report path.
    {
        const fs::path boot = top / L"\x4fdd\x5b58.bin";
        const fs::path report = top / L"\x4fdd\x5b58.json";
        put(boot, std::string("\x09\x00", 2));
        const auto code = run(tool(qilin, {"info", show(boot), "--json", show(report)}, top, top / "qilin.log"));
        CHECK(code && *code == 3);  // not a supported build
        const auto text = read_file(report);
        CHECK(text && text->find("\"sha256\"") != std::string::npos);
    }

    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("launcher build test: passed\n");
    return 0;
}

// The launcher's build on Windows, Linux and macOS, with stand-ins for the
// recompiler, the compiler and the game (test_stub.cpp): what is compiled
// again and what comes from the cache, the identity that decides a rebuild,
// the space check, Unicode and shell metacharacters in every path, the
// compiler's own leading arguments (zig's "c++"), game.txt's defaults (Toy
// Commander's first downloads), and a second kit with everything game.txt
// can say: a folder as the input, several steps with placeholders, a tool
// whose path has spaces, lifted files in subfolders of gen, the game's own
// arguments and the identify contract's three outcomes. Then the project's
// real recompiler's identify on a Unicode path. On Windows also a locked
// .exe, another CPU and the command line's quoting. No game data.
//
//   qilin_launcher_build_test <test_stub> <qilin> <scratch folder>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "build.hpp"
#include "input.hpp"
#include "kit.hpp"
#include "platform.hpp"
#include "process.hpp"
#include "tidy.hpp"

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

#if defined(_WIN32)
void set_env(const char* name, const char* value) {
    SetEnvironmentVariableW(widen(name).c_str(), value ? widen(value).c_str() : nullptr);
}

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
#else
void set_env(const char* name, const char* value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
#endif

// A copy that can run (the stand-in as each of the kit's programs).
void copy_program(const fs::path& from, const fs::path& to) {
    fs::copy_file(from, to);
    std::error_code ec;
    fs::permissions(to, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: qilin_launcher_build_test <test_stub> <qilin> <scratch folder>\n");
        return 2;
    }
#if defined(_WIN32)
    (void)argv;
    // The command line as UTF-16 (the scratch folder may be anywhere).
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != 4) return 2;
    const fs::path stub = wargv[1], qilin = wargv[2], scratch = wargv[3];
    LocalFree(wargv);

    test_quoting();
    // A download folder with spaces, a non-ASCII letter and shell characters.
    const fs::path top = scratch / fs::path(u8"kit with spaces \u00e9 & %PATH%");
    const std::string exe = "Fixture.exe";
    // The Windows kit's compiler: game.txt's default.
    const fs::path compiler = top / "kit" / "toolchain" / "bin" / "x86_64-w64-mingw32-clang++.exe";
    const std::string compiler_lines;
#else
    const fs::path stub = argv[1], qilin = argv[2], scratch = argv[3];
    // Quotes and a semicolon too: a POSIX name can hold them.
    const fs::path top = scratch / fs::path(u8"kit with spaces \u00e9 & $PATH ;'q' \"d\"");
    const std::string exe = "Fixture";
    // The Linux kit's compiler: zig, run as `zig c++`.
    const fs::path compiler = top / "kit" / "toolchain" / "zig" / "zig";
    const std::string compiler_lines = "compiler=toolchain/zig/zig\ncompiler_args=c++\n";
#endif
    const std::string suffix(executable_suffix());
    std::error_code ec;
    fs::remove_all(top, ec);
    for (const char* dir : {"kit/lib", "kit/include", "user"}) fs::create_directories(top / dir);
    fs::create_directories(compiler.parent_path());
    const fs::path launcher = top / ("Play Fixture" + suffix);
    copy_program(stub, launcher);
    copy_program(stub, top / "kit" / ("qilin" + suffix));
    copy_program(stub, compiler);
    put(top / "kit" / "include" / "stub.hpp", "header1");
    put(top / "kit" / "VERSION", "launcher-test\n");
    put(top / "kit" / "game.txt",
        "game=fixturegame\r\ntitle=Fixture\r\nexe=" + exe + "\r\nboot=FIXTURE.BIN\r\nbuild=Fixture (World)\r\nproduct=T-0\r\n"
        "disc_version=1.000\r\npackage=Fixture--QilinRecomp\r\n" + compiler_lines);
    const fs::path archive = top / "kit" / "lib" / "libfixture_kit.a";
    put(archive, "fixture");
    put(top / "kit" / "build-flags.txt", "cxxflags = -O2 -I$kit/include\nldflags = -static\nlibs = $kit/lib/libfixture_kit.a\n");
    const fs::path disc = top / fs::path(u8"disc %PATH% & \u4fdd\u5b58.cue");
    put(disc, "right: a synthetic cue, no game data");  // what the stub's identify reads

    auto loaded = load_kit(launcher, top / "user");
    CHECK(loaded.has_value());
    if (!loaded) {
        std::printf("%s\n", loaded.error().c_str());
        return 1;
    }
    const Kit kit = *loaded;
    CHECK(kit.title == "Fixture" && kit.exe == exe && kit.version == "launcher-test");
    CHECK(kit.compiler() == compiler);
    CHECK(kit.wanted() == "Fixture (World), T-0 version 1.000");
    // game.txt's defaults: Toy Commander's first downloads.
    CHECK(kit.input == InputKind::File && kit.input_noun == "disc");
    CHECK(kit.input_extensions == std::vector<std::string>({"cue", "gdi", "chd"}));
    CHECK(kit.identify == "{qilin} identify {input} --json --build {game:build}");
    CHECK(kit.steps == std::vector<std::string>({"{qilin} lift {input} --out {gen} --files 64"}));
    CHECK(kit.run_args == "{input}" && kit.compile_memory_mb == 1200 && kit.build_space_mb == 2048 && kit.cache_mb == 2048);
    CHECK(kit.release_tag_prefix == "fixturegame-v" && kit.release_asset_prefix == "Fixture--QilinRecomp-" && kit.repo.empty());
    {
        const auto words = expand_command(kit, kit.steps[0], disc);
        CHECK(words && *words == std::vector<std::string>({show(kit.qilin()), "lift", show(disc), "--out", show(kit.work / "gen"), "--files", "64"}));
        const auto id = expand_command(kit, kit.identify, disc);
        CHECK(id && id->size() == 6 && (*id)[5] == "Fixture (World)");  // {game:build}, spaces and all, one argument
    }
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
    options.check_input = true;  // the stub's identify says "right"
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
    set_env("QILIN_STUB_COMPILER", "2");
    CHECK(build());
    CHECK_CALLS(4, -1);
    int compiled = 4;
#if defined(_WIN32)
    // ...and another CPU (-march=native), which also makes the build out of
    // date. (Linux and macOS ask the system, not the environment.)
    std::wstring cpu(512, L'\0');
    cpu.resize(GetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER", cpu.data(), 512));
    set_env("PROCESSOR_IDENTIFIER", "synthetic different processor");
    identity = build_identity(kit);
    CHECK(!up_to_date(kit, *identity));
    CHECK(build());
    compiled += 2;
    CHECK_CALLS(compiled, -1);
    SetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER", cpu.c_str());
#endif
    // Back to the first: still in the cache.
    set_env("QILIN_STUB_COMPILER", nullptr);
    put(top / "kit" / "include" / "stub.hpp", "header1");
    CHECK(build());
    CHECK_CALLS(compiled, -1);
    // Built from scratch: compiled again, cache or not.
    options.use_cache = false;
    CHECK(build());
    CHECK_CALLS(compiled + 2, -1);
    options.use_cache = true;
    CHECK(fs::is_regular_file(kit.work / (exe + ".previous")));

#if defined(_WIN32)
    // A running (locked) game: the build fails, and the old .exe and its
    // record stay as they were. (Linux and macOS replace a running
    // program's file without trouble.)
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
#endif

    // Not enough room: said plainly, with the drive and the amounts.
    options.need_bytes = ~0ull >> 2;
    {
        const auto r = build_game(kit, disc, progress, cancel, options);
#if defined(_WIN32)
        CHECK(!r && r.error().find("free on drive") != std::string::npos);
#else
        CHECK(!r && r.error().find("free on the disk holding") != std::string::npos);
#endif
    }
    options.need_bytes = 0;

    // The cache is trimmed to its newest: with no room, nothing stays.
    options.cache_bytes = 0;
    CHECK(build());
    CHECK(fs::is_empty(kit.cache()));
    options.cache_bytes = 2ull << 30;

    // The game gets the disc's path intact, and its output goes to the logs.
    {
        const auto c = game_command(kit, disc);
        CHECK(c && c->args == std::vector<std::string>({show(disc)}));
        auto game = Process::start(*c);
        CHECK(game.has_value());
        CHECK(game && game->wait() == 0);
        const auto launched = read_file(kit.work / "launched.txt");  // the stand-in writes beside itself: the game is in work.
        CHECK(launched && *launched == show(disc));
        CHECK(fs::exists(kit.run_errors()));
    }

    // Tidying: what older downloads left behind goes; the release's files,
    // the game, the build and anything of the player's stay.
    {
        put(top / "kit" / "files.txt",
            "kit/game.txt\r\nkit/files.txt\r\nkit/qilin" + suffix + "\r\nkit/VERSION\r\nkit/build-flags.txt\r\nkit/lib/libfixture_kit.a\r\n"
            "kit/include/stub.hpp\r\n" + compiler.lexically_relative(top).generic_string() + "\r\nLICENSES/a/LICENSE.txt\r\n");
        fs::create_directories(top / "LICENSES" / "a");
        fs::create_directories(top / "LICENSES" / "ninja");
        fs::create_directories(top / "kit" / "gone" / "deeper");
        put(top / "LICENSES" / "a" / "LICENSE.txt", "kept");
        put(top / "LICENSES" / "ninja" / "COPYING", "old");
        put(top / "kit" / "play.ps1", "old");
        put(top / "kit" / "ninja.exe", "old");
        put(top / "kit" / "gone" / "deeper" / "x.txt", "old");
        put(top / "mine.txt", "the player's");
        std::vector<fs::path> gone = {top / "LICENSES" / "ninja", top / "kit" / "play.ps1", top / "kit" / "ninja.exe", top / "kit" / "gone"};
#if defined(_WIN32)
        // What the first Windows downloads kept at the top and in work.
        put(top / "Play Fixture.bat", "old");
        put(top / "Fixture.exe", "old");
        put(top / "run.log", "old");
        put(kit.work / "build.ninja", "old");
        put(kit.work / ".ninja_log", "old");
        put(kit.work / "obj" / "lifted_000.o.d", "old");
        put(kit.work / "disc.txt", "\xEF\xBB\xBF" + show(disc) + "\r\n");  // play.ps1's remembered disc
        fs::remove(kit.disc_file(), ec);
        gone.insert(gone.end(), {top / "Play Fixture.bat", top / "Fixture.exe", top / "run.log", kit.work / "build.ninja",
                                 kit.work / ".ninja_log", kit.work / "obj" / "lifted_000.o.d", kit.work / "disc.txt"});
#else
        CHECK(write_file(kit.disc_file(), show(disc)).has_value());  // the launcher's remembered disc stays
#endif
        const Tidied t = tidy_folder(kit);
        CHECK(t.problems.empty());
        for (const fs::path& p : gone) check(!fs::exists(p, ec), "left behind: " + show(p), __LINE__);
        for (const fs::path& p : {top / "kit" / "game.txt", top / "kit" / ("qilin" + suffix), compiler, top / "kit" / "lib" / "libfixture_kit.a",
                                  top / "LICENSES" / "a" / "LICENSE.txt", launcher, disc, top / "mine.txt", kit.game_exe(),
                                  kit.work / "built-with.txt"})
            check(fs::exists(p, ec), "removed: " + show(p), __LINE__);
        const auto remembered = read_file(kit.disc_file());
        CHECK(remembered && *remembered == show(disc));
        CHECK(up_to_date(kit, *build_identity(kit)));
        // A list that doesn't name the kit's own files is the wrong one: nothing goes.
        put(top / "kit" / "files.txt", "README.txt\r\n");
        put(top / "kit" / "stray.txt", "x");
        (void)tidy_folder(kit);
        CHECK(fs::exists(top / "kit" / "stray.txt"));
        fs::remove(top / "kit" / "stray.txt", ec);
        fs::remove(top / "kit" / "files.txt", ec);
    }

    // The input check's plain messages for what isn't a disc, and the
    // identify contract's three outcomes.
    CHECK(!check_input(kit, top / "missing.cue").ok);
    CHECK(check_input(kit, top / "missing.cue").message.starts_with("Can't find"));
    put(top / "notes.txt", "right");
    CHECK(check_input(kit, top / "notes.txt").message.starts_with("That's not your disc. Choose your disc's .cue file"));
    CHECK(check_input(kit, top / "kit").message.starts_with("That's a folder"));
    CHECK(input_name_ok(kit, "a.CUE") && input_name_ok(kit, "a.gdi") && input_name_ok(kit, "a.chd") && !input_name_ok(kit, "a.bin"));
    {
        const InputCheck right = check_input(kit, disc);
        CHECK(right.ok && right.title == "Fixture (World)" && right.id == "T-0" && right.version == "1.000");
        CHECK(right.message == "This is Fixture (World), T-0 version 1.000: the right one.");
        const fs::path other = top / fs::path(u8"other \u00e9.gdi");
        put(other, "wrong");
        const InputCheck wrong = check_input(kit, other);
        CHECK(!wrong.ok && wrong.message.starts_with("This is Other Game (Europe).") && wrong.title == "Other Game (Europe)");
        put(other, "garbage");
        const InputCheck unreadable = check_input(kit, other);
        CHECK(!unreadable.ok && unreadable.message == "Can't read it at all.");
        put(other, "liar");  // ok, but exit 3: a broken tool, said so
        CHECK(!check_input(kit, other).ok && check_input(kit, other).message.find("disagree") != std::string::npos);
        // A build stops at the check, before any step.
        put(other, "wrong");
        const auto r = build_game(kit, other, progress, cancel, options);
        CHECK(!r && r.error().starts_with("This is Other Game (Europe)."));
    }

    // A second kit: everything game.txt can say beyond the defaults.
    {
        const fs::path top2 = scratch / fs::path(u8"folder kit \u00fc 'x'");
        fs::remove_all(top2, ec);
        for (const char* dir : {"kit/lib", "kit/include", "user"}) fs::create_directories(top2 / dir);
        const fs::path compiler2 = top2 / compiler.lexically_relative(top);
        fs::create_directories(compiler2.parent_path());
        const fs::path launcher2 = top2 / ("Play Folder Fixture" + suffix);
        copy_program(stub, launcher2);
        copy_program(stub, top2 / "kit" / ("qilin" + suffix));
        copy_program(stub, top2 / "kit" / ("qilin helper" + suffix));  // a tool whose path has a space
        copy_program(stub, compiler2);
        put(top2 / "kit" / "include" / "stub.hpp", "header1");
        put(top2 / "kit" / "VERSION", "launcher-test\n");
        put(top2 / "kit" / "lib" / "libfolder_kit.a", "folder");
        put(top2 / "kit" / "build-flags.txt", "cxxflags = -O2 -I$kit/include\nlibs = $kit/lib/libfolder_kit.a\n");
        const std::string game_txt =
            "game=foldergame\ntitle=Folder Fixture\nexe=" + exe + "\nbuild=Folder Fixture (World)\npackage=Folder--QilinRecomp\n"
            "repo=example/Folder--QilinRecomp\nrelease_tag_prefix=folder-v\n" + compiler_lines +
            "input=folder\ninput_noun=game folder\n"
            "input_hint=Choose the folder you extracted the disc into, the one holding default.xbe.\n"
            "identify={qilin} identify {input} --json\n"
            "step.1={qilin} lift {input} --out {gen} --files 64\n"
            "step.2=\"{tool:qilin helper}\" sub {input} --out \"{gen}/sub dir\" \"--note=two words\"\n"
            "compile_memory_mb=3000\nbuild_space_mb=1\ncache_mb=64\nrun_args=--data {input} --fullscreen\n"
            "old_files=Play Folder.bat; work/old build.ninja ;../outside.txt\nold_input_file=work/disc-folder.txt\n";
        put(top2 / "kit" / "game.txt", game_txt);
        const fs::path data = top2 / fs::path(u8"game data \u4fdd & %PATH% $HOME");
        fs::create_directories(data / "sys");
        put(data / "identity.txt", "right");

        auto loaded2 = load_kit(launcher2, top2 / "user");
        CHECK(loaded2.has_value());
        if (!loaded2) {
            std::printf("%s\n", loaded2.error().c_str());
            return 1;
        }
        const Kit kit2 = *loaded2;
        CHECK(kit2.input == InputKind::Folder && kit2.input_noun == "game folder" && kit2.input_extensions.empty());
        CHECK(kit2.steps.size() == 2 && kit2.compile_memory_mb == 3000 && kit2.build_space_mb == 1 && kit2.cache_mb == 64);
        CHECK(kit2.release_tag_prefix == "folder-v" && kit2.release_asset_prefix == "Folder--QilinRecomp-" && kit2.repo == "example/Folder--QilinRecomp");
        // An older download's own leftovers (game.txt's old_files and
        // old_input_file): removed, the remembered data taken over, and
        // nothing outside the download folder touched.
        CHECK(kit2.old_files.size() == 2 && kit2.old_input_files.size() == 1);
        {
            fs::create_directories(kit2.work);
            put(top2 / "Play Folder.bat", "old");
            put(kit2.work / "old build.ninja", "old");
            put(top2.parent_path() / "outside.txt", "not the launcher's");
            put(kit2.work / "disc-folder.txt", "\xEF\xBB\xBF" + show(data) + "\r\n");
            std::error_code rm;
            fs::remove(kit2.disc_file(), rm);
            const Tidied t = tidy_folder(kit2);
            CHECK(t.problems.empty());
            CHECK(!fs::exists(top2 / "Play Folder.bat") && !fs::exists(kit2.work / "old build.ninja") && !fs::exists(kit2.work / "disc-folder.txt"));
            CHECK(fs::exists(top2.parent_path() / "outside.txt"));
            const auto taken = read_file(kit2.disc_file());
            CHECK(taken && *taken == show(data));
            fs::remove(kit2.disc_file(), rm);
            fs::remove(top2.parent_path() / "outside.txt", rm);
        }
        CHECK(kit2.the_input() == "the game folder");
        {
            const auto words = expand_command(kit2, kit2.steps[1], data);
            CHECK(words && *words == std::vector<std::string>({show(kit2.kit_tool("qilin helper")), "sub", show(data), "--out",
                                                               show(kit2.work / "gen") + "/sub dir", "--note=two words"}));
        }

        // The identify contract on a folder, and what isn't one.
        CHECK(check_input(kit2, data).ok);
        CHECK(check_input(kit2, data / "identity.txt").message.starts_with("That's a file, not a folder. Choose the folder you extracted"));
        put(data / "identity.txt", "wrong");
        CHECK(!check_input(kit2, data).ok && check_input(kit2, data).message.starts_with("This is Other Game"));
        fs::remove(data / "identity.txt");
        CHECK(!check_input(kit2, data).ok && check_input(kit2, data).message == "Can't read it at all.");
        put(data / "identity.txt", "right");

        // The build: both steps in order, the subfolder's file compiled to
        // the same path under obj, then linked; the second time from the cache.
        BuildOptions options2;
        options2.native.clear();
        const fs::path calls2 = kit2.work / "tool-calls.txt";
        const auto build2 = [&] {
            const auto r = build_game(kit2, data, progress, cancel, options2);
            if (!r) std::printf("build (folder kit): %s\n", r.error().c_str());
            return r.has_value();
        };
        CHECK(build2());
        CHECK(count_lines(calls2, "lift") == 1 && count_lines(calls2, "sub") == 1 && count_lines(calls2, "compile") == 3);
        CHECK(progress.total == 3 && progress.stage == Stage::Done);
        CHECK(fs::is_regular_file(kit2.work / "obj" / "sub dir" / "extra.o"));
        CHECK(fs::is_regular_file(kit2.work / "obj" / "fixture.o"));
        {
            std::ifstream f(calls2);
            std::string first, second;
            std::getline(f, first);
            std::getline(f, second);
            CHECK(first == "lift" && second == "sub");
        }
        CHECK(build2());
        CHECK(count_lines(calls2, "compile") == 3 && progress.cached == 3);
        // A new helper tool means a new build.
        const auto before = build_identity(kit2);
        put(top2 / "kit" / ("qilin helper" + suffix), "x", true);
        const auto after = build_identity(kit2);
        CHECK(before && after && *before != *after);
        // The game gets run_args, the folder's path as one argument.
        {
            const auto c = game_command(kit2, data);
            CHECK(c && c->args == std::vector<std::string>({"--data", show(data), "--fullscreen"}));
            auto game = Process::start(*c);
            CHECK(game && game->wait() == 0);
            const auto launched = read_file(kit2.work / "launched.txt");
            CHECK(launched && *launched == show(data));
        }
        // A step that fails names itself.
        put(top2 / "kit" / "game.txt", game_txt + "step.2=\"{tool:qilin helper}\" sub {input} --out {gen} \"--note=wrong\"\n");
        {
            const auto k = load_kit(launcher2, top2 / "user");
            CHECK(k.has_value());
            const auto r = k ? build_game(*k, data, progress, cancel, options2) : std::expected<void, std::string>();
            CHECK(!r && r.error().find("failed at step 2 of 2") != std::string::npos);
        }
        // game.txt's mistakes are said at start-up, not halfway through a build.
        const auto broken = [&](const std::string& extra, std::string_view says) {
            put(top2 / "kit" / "game.txt", game_txt + extra);
            const auto k = load_kit(launcher2, top2 / "user");
            check(!k && k.error().find(says) != std::string::npos, "game.txt with " + extra + ": " + (k ? std::string("loaded") : k.error()), __LINE__);
        };
        broken("step.4=x\n", "step.4 without step.3");
        broken("step.10=x\n", "step.10");
        broken("step.2={qilin} {nothing}\n", "unknown placeholder {nothing}");
        broken("identify={qilin} \"unclosed\n", "unclosed");
        broken("run_args={game:missing}\n", "unknown placeholder {game:missing}");
        broken("input=disc\n", "input = disc");
        broken("compile_memory_mb=lots\n", "compile_memory_mb");
        put(top2 / "kit" / "game.txt", game_txt);
    }

    // The real recompiler's identify (the contract every project's qilin
    // keeps: ADOPTING.md) on a Unicode path: not the supported build, or not
    // readable at all, said as JSON.
    {
        const fs::path boot = top / fs::path(u8"\u4fdd\u5b58.bin");
        put(boot, std::string("\x09\x00", 2));
        Command c = tool(qilin, {"identify", show(boot), "--json"}, top, top / fs::path(u8"\u4fdd\u5b58.json"));
        c.err = top / "qilin-errors.txt";
        const auto code = run(c);
        CHECK(code && (*code == 2 || *code == 3));
        const auto text = read_file(c.out);
        CHECK(text && text->find("\"ok\": false") != std::string::npos && text->find("\"message\": \"") != std::string::npos);
    }

    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("launcher build test: passed\n");
    return 0;
}

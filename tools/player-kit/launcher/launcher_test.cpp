// The launcher's portable parts: its JSON reader, version comparison, the
// releases list and the kit's text files. No network, no game data.
//
//   qilin_launcher_test json | version | kit
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "json.hpp"
#include "kit_text.hpp"
#include "version.hpp"

using namespace qilin::launcher;

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("FAILED (line %d): %s\n", line, what);
    ++failures;
}
#define CHECK(x) check((x), #x, __LINE__)

void test_json() {
    {
        const auto v = json::parse(R"( {"a": 1, "b": [true, false, null], "c": {"d": "e"}, "n": -12.5e1, "z": 0} )");
        CHECK(v.has_value());
        CHECK(v->is_object());
        CHECK(v->find("a") && v->find("a")->number == 1);
        const json::Value* b = v->find("b");
        CHECK(b && b->is_array() && b->items.size() == 3);
        CHECK(b && b->items[0].is_bool() && b->items[0].boolean);
        CHECK(b && b->items[1].is_bool() && !b->items[1].boolean);
        CHECK(b && b->items[2].is_null());
        CHECK(v->find("c") && v->find("c")->string_or("d", "") == "e");
        CHECK(v->find("n") && std::fabs(v->find("n")->number + 125.0) < 1e-9);
        CHECK(v->find("z") && v->find("z")->number == 0);
        CHECK(!v->find("missing"));
        CHECK(v->string_or("missing", "fallback") == "fallback");
        CHECK(v->bool_or("a", true));  // a number isn't a bool: the fallback
    }
    {
        // Escapes: the simple ones, \u in each UTF-8 length, a surrogate pair.
        const auto v = json::parse(R"(["\"\\\/\b\f\n\r\t", "\u0041\u00e9\u4fdd\ud83d\ude00"])");
        CHECK(v.has_value() && v->items.size() == 2);
        CHECK(v && v->items[0].string == "\"\\/\b\f\n\r\t");
        CHECK(v && v->items[1].string == "A\xC3\xA9\xE4\xBF\x9D\xF0\x9F\x98\x80");
    }
    CHECK(json::parse("[]").has_value() && json::parse("[]")->items.empty());
    CHECK(json::parse("{}").has_value() && json::parse("{}")->members.empty());
    CHECK(json::parse("  \"text\"  ").has_value());
    // Malformed: each one an error, never a guess.
    for (const char* bad : {"", "[", "[1,]", "{\"a\" 1}", "{\"a\":1,}", "[1 2]", "tru", "nul", "\"open", "\"\\x\"", "\"\\u12\"",
                            "\"\\ud800\"", "\"\\udc00\"", "01", "1.", "-", "1e", "[1] [2]", "{1:2}", "\"tab\there\""}) {
        const auto v = json::parse(bad);
        if (v) std::printf("accepted: %s\n", bad);
        CHECK(!v.has_value());
    }
    // Nesting has a limit, so a hostile answer can't exhaust the stack.
    CHECK(!json::parse(std::string(100000, '[')).has_value());
    CHECK(json::parse(std::string(50, '[') + std::string(50, ']')).has_value());
    const auto err = json::parse("[1, x]");
    CHECK(!err && err.error().find("byte 4") != std::string::npos);
}

void test_version() {
    const auto v = parse_version("0.13.0");
    CHECK(v && v->major == 0 && v->minor == 13 && v->patch == 0 && v->ahead == 0);
    CHECK(parse_version("v1.2.3") && parse_version("v1.2.3")->major == 1);
    CHECK(parse_version("0.13.0\r\n").has_value());  // as kit\VERSION is written
    const auto ahead = parse_version("0.13.0-5-gabc1234");
    CHECK(ahead && ahead->ahead == 5);
    CHECK(!parse_version("0.13.0-5-gabc1234", true));  // a tag is exact
    for (const char* bad : {"", "1", "1.2", "1.2.", "a.b.c", "1.2.3x", "1.2.3-", "1.2.3-5", "1.2.3-5-x", "abc1234", "-1.2.3"}) {
        if (parse_version(bad)) std::printf("accepted: %s\n", bad);
        CHECK(!parse_version(bad));
    }
    // Compared part by part, as numbers (0.10.0 is after 0.9.0).
    CHECK(*parse_version("0.10.0") > *parse_version("0.9.0"));
    CHECK(*parse_version("1.0.0") > *parse_version("0.99.99"));
    CHECK(*parse_version("0.13.1") > *parse_version("0.13.0"));
    CHECK(*parse_version("0.13.0") == *parse_version("v0.13.0"));
    // An untagged build after 0.13.0 is newer than it, older than 0.13.1.
    CHECK(*parse_version("0.13.0-2-gdeadbee") > *parse_version("0.13.0"));
    CHECK(*parse_version("0.13.0-2-gdeadbee") < *parse_version("0.13.1"));
    CHECK(to_string(*parse_version("1.20.3")) == "1.20.3");

    // The releases list: this game's tags only, no drafts or pre-releases,
    // the newest by version (not by order), with its download attached.
    const char* releases = R"([
      {"tag_name": "toyracer-v0.9.0", "draft": false, "prerelease": false,
       "assets": [{"name": "ToyRacer--QilinRecomp-0.9.0-windows-x64.zip", "browser_download_url": "https://example.invalid/r", "size": 5}]},
      {"tag_name": "toycommander-v0.14.0", "draft": true, "prerelease": false,
       "assets": [{"name": "ToyCommander--QilinRecomp-0.14.0-windows-x64.zip", "browser_download_url": "https://example.invalid/d"}]},
      {"tag_name": "toycommander-v0.13.2", "draft": false, "prerelease": true,
       "assets": [{"name": "ToyCommander--QilinRecomp-0.13.2-windows-x64.zip", "browser_download_url": "https://example.invalid/p"}]},
      {"tag_name": "toycommander-v0.9.0", "draft": false, "prerelease": false,
       "assets": [{"name": "ToyCommander--QilinRecomp-0.9.0-windows-x64.zip", "browser_download_url": "https://example.invalid/old"}]},
      {"tag_name": "toycommander-v0.13.1", "draft": false, "prerelease": false, "body": "Notes \u00e9",
       "assets": [{"name": "notes.txt", "browser_download_url": "https://example.invalid/n"},
                  {"name": "ToyCommander--QilinRecomp-0.13.1-windows-x64.zip", "browser_download_url": "https://example.invalid/new", "size": 123456789}]},
      {"tag_name": "toycommander-v0.13.5", "draft": false, "prerelease": false, "assets": []},
      {"tag_name": "toycommander-v0.13.6-rc1", "draft": false, "prerelease": false,
       "assets": [{"name": "ToyCommander--QilinRecomp-0.13.6-rc1-windows-x64.zip", "browser_download_url": "https://example.invalid/rc"}]}
    ])";
    const auto newest = newest_release(releases, "toycommander", "ToyCommander--QilinRecomp");
    CHECK(newest && *newest);
    if (newest && *newest) {
        const Release& r = **newest;
        CHECK(r.tag == "toycommander-v0.13.1");
        CHECK(r.asset_url == "https://example.invalid/new");
        CHECK(r.asset_name == "ToyCommander--QilinRecomp-0.13.1-windows-x64.zip");
        CHECK(r.asset_size == 123456789);
        CHECK(r.version > *parse_version("0.13.0"));
        CHECK(!(r.version > *parse_version("0.13.1")));
    }
    const auto racer = newest_release(releases, "toyracer", "ToyRacer--QilinRecomp");
    CHECK(racer && *racer && (*racer)->tag == "toyracer-v0.9.0");
    const auto none = newest_release(releases, "othergame", "Other");
    CHECK(none && !*none);
    CHECK(!newest_release("{\"message\": \"Not Found\"}", "toycommander", "x"));  // an error object, not a list
    CHECK(!newest_release("not json", "toycommander", "x"));
}

void test_kit() {
    const auto about = parse_settings("\xEF\xBB\xBFtitle=Toy Commander\r\nexe=ToyCommander.exe\r\n# a note\r\nnothing here\r\n=empty\r\n"
                                      "build = Toy Commander (USA)\r\n");
    CHECK(about.size() == 3);
    CHECK(about.at("title") == "Toy Commander");
    CHECK(about.at("exe") == "ToyCommander.exe");
    CHECK(about.at("build") == "Toy Commander (USA)");
    const auto ninja = parse_settings("# Written by CMake\ncxxflags = -O2 -DNDEBUG  -I$kit/include\nlibs = $kit/lib/libx_kit.a -lm\n");
    const auto flags = split_flags(ninja.at("cxxflags"), "../kit");
    CHECK(flags.size() == 3);
    CHECK(flags.size() == 3 && flags[0] == "-O2" && flags[1] == "-DNDEBUG" && flags[2] == "-I../kit/include");
    const auto libs = split_flags(ninja.at("libs"), "../kit");
    CHECK(libs.size() == 2 && libs[0] == "../kit/lib/libx_kit.a" && libs[1] == "-lm");
    CHECK(split_flags("", "k").empty());
    CHECK(split_flags("$kit$kit", "$kit").size() == 1 && split_flags("$kit$kit", "$kit")[0] == "$kit$kit");  // no endless replacing
    CHECK(trim("  a b \r\n") == "a b");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view which = argc > 1 ? argv[1] : "";
    if (which.empty() || which == "json") test_json();
    if (which.empty() || which == "version") test_version();
    if (which.empty() || which == "kit") test_kit();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("launcher tests (%s): passed\n", which.empty() ? "all" : argv[1]);
    return 0;
}

// The launcher's portable parts: its JSON reader, version comparison, the
// releases list, the kit's text files and what "Play on iPad" reads from
// Apple's tools (ios_text.hpp). No network, no game data.
//
//   qilin_launcher_test json | version | kit | ios
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "hash.hpp"
#include "ios_text.hpp"
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
    const auto newest = newest_release(releases, "toycommander-v", "ToyCommander--QilinRecomp-");
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
    const auto racer = newest_release(releases, "toyracer-v", "ToyRacer--QilinRecomp-");
    CHECK(racer && *racer && (*racer)->tag == "toyracer-v0.9.0");
    const auto none = newest_release(releases, "othergame-v", "Other-");
    CHECK(none && !*none);
    CHECK(!newest_release("{\"message\": \"Not Found\"}", "toycommander-v", "x-"));  // an error object, not a list
    CHECK(!newest_release("not json", "toycommander-v", "x-"));
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

    // game.txt's commands: split by the launcher's own rules, no shell.
    {
        const auto words = split_command("{qilin} lift  {input} --out \"{gen}/sub dir\" 'it''s' --x=\"a b\"c \"\" C:\\x\\y");
        CHECK(words.has_value());
        CHECK(words && words->size() == 9);
        if (words && words->size() == 9) {
            CHECK((*words)[0] == "{qilin}" && (*words)[1] == "lift" && (*words)[2] == "{input}" && (*words)[3] == "--out");
            CHECK((*words)[4] == "{gen}/sub dir" && (*words)[5] == "its" && (*words)[6] == "--x=a bc" && (*words)[7].empty());
            CHECK((*words)[8] == "C:\\x\\y");  // backslashes stay
        }
        CHECK(!split_command("a \"b").has_value());
        CHECK(!split_command("a 'b").has_value());
        CHECK(split_command("   ").has_value() && split_command("   ")->empty());
        CHECK(split_command("say \"don't\"").has_value() && (*split_command("say \"don't\""))[1] == "don't");
    }
    {
        const auto lookup = [](std::string_view name) -> std::optional<std::string> {
            if (name == "input") return std::string("D:/My Games/\xE4 disc.cue");
            if (name == "tool:qilin-dsp") return std::string("/kit/qilin-dsp");
            return std::nullopt;
        };
        const auto one = expand_placeholders("--in={input}", lookup);
        CHECK(one && *one == "--in=D:/My Games/\xE4 disc.cue");  // spaces stay in one argument
        CHECK(expand_placeholders("{tool:qilin-dsp}", lookup).value_or("") == "/kit/qilin-dsp");
        CHECK(expand_placeholders("{{literal}}", lookup).value_or("") == "{literal}");
        CHECK(expand_placeholders("plain", lookup).value_or("") == "plain");
        CHECK(!expand_placeholders("{nope}", lookup).has_value());
        CHECK(!expand_placeholders("{input", lookup).has_value());
        CHECK(!expand_placeholders("a}b", lookup).has_value());
    }
    {
        const auto ext = split_extensions(".CUE; gdi,chd");
        CHECK(ext.size() == 3 && ext[0] == "cue" && ext[1] == "gdi" && ext[2] == "chd");
        CHECK(list_extensions(ext) == ".cue, .gdi or .chd");
        CHECK(list_extensions({"iso"}) == ".iso");
        CHECK(split_extensions("").empty());
        CHECK(capitalised("disc") == "Disc" && capitalised("ROM set") == "ROM set" && capitalised("").empty());
    }

    // The launcher's own SHA-256 (FIPS 180-4's examples), in pieces too.
    CHECK(sha256_hex(std::string_view()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string_view("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 h;
        const std::string a(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.add(std::string_view(a).substr(0, 1 + i % 7)), h.add(std::string_view(a).substr(0, 999 - i % 7));
        CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");  // a million 'a's
    }
}

// What "Play on iPad" reads from Apple's tools (ios_text.hpp). The samples
// follow the formats as Apple documents them or as the tools print them; the
// ones marked "assumed" in ios_text.hpp are this project's best reading, to
// be confirmed on a Mac with Xcode and an iPad.
void test_ios() {
    using namespace qilin::launcher::ios;

    // `xcrun devicectl list devices --json-output`: one iPad ready, one
    // iPhone unpaired, one iPad without Developer Mode over Wi-Fi, one away,
    // an Apple TV and a simulator (both left out).
    const char* devices = R"js({
      "info": {"arguments": ["devicectl", "list", "devices", "--json-output", "/tmp/d.json"], "commandType": "devicectl.list.devices",
               "jsonVersion": 2, "outcome": "success", "version": "397.21"},
      "result": {"devices": [
        {"capabilities": [], "identifier": "AAAA1111-0000-0000-0000-000000000001",
         "connectionProperties": {"authenticationType": "manualPairing", "isMobileDeviceOnly": false, "pairingState": "paired",
                                  "potentialHostnames": ["00008103-000A.coredevice.local"], "transportType": "wired", "tunnelState": "disconnected"},
         "deviceProperties": {"bootedFromSnapshot": true, "developerModeStatus": "enabled", "ddiServicesAvailable": false,
                              "name": "Player's iPad", "osBuildUpdate": "22B83", "osVersionNumber": "18.1"},
         "hardwareProperties": {"deviceType": "iPad", "marketingName": "iPad Air (5th generation)", "platform": "iOS",
                                "productType": "iPad13,16", "reality": "physical", "udid": "00008103-000A11112222333E"},
         "tags": [], "visibilityClass": "default"},
        {"identifier": "AAAA1111-0000-0000-0000-000000000002",
         "connectionProperties": {"pairingState": "unpaired", "transportType": "wired", "tunnelState": "disconnected"},
         "deviceProperties": {"name": "Work iPhone", "osVersionNumber": "17.6.1"},
         "hardwareProperties": {"deviceType": "iPhone", "platform": "iOS", "productType": "iPhone15,2", "reality": "physical", "udid": "00008120-0002"}},
        {"identifier": "AAAA1111-0000-0000-0000-000000000003",
         "connectionProperties": {"pairingState": "paired", "transportType": "localNetwork", "tunnelState": "disconnected"},
         "deviceProperties": {"developerModeStatus": "disabled", "name": "Kitchen iPad", "osVersionNumber": "17.2"},
         "hardwareProperties": {"deviceType": "iPad", "marketingName": "iPad (10th generation)", "platform": "iOS", "reality": "physical", "udid": "u3"}},
        {"identifier": "AAAA1111-0000-0000-0000-000000000004",
         "connectionProperties": {"pairingState": "paired", "tunnelState": "unavailable"},
         "deviceProperties": {"developerModeStatus": "enabled", "name": "Away iPad", "osVersionNumber": "18.0"},
         "hardwareProperties": {"deviceType": "iPad", "platform": "iOS", "reality": "physical"}},
        {"identifier": "TV", "connectionProperties": {"pairingState": "paired", "transportType": "localNetwork", "tunnelState": "disconnected"},
         "deviceProperties": {"name": "Living Room"}, "hardwareProperties": {"deviceType": "appleTV", "platform": "tvOS", "reality": "physical"}},
        {"identifier": "SIM", "deviceProperties": {"name": "iPad Pro Simulator"}, "hardwareProperties": {"platform": "iOS", "reality": "virtual"}}
      ]}
    })js";
    const auto list = parse_devices(devices);
    CHECK(list.has_value() && list->size() == 4);
    if (list && list->size() == 4) {
        const Device& ready = (*list)[0];  // the ready one first
        CHECK(ready.identifier == "AAAA1111-0000-0000-0000-000000000001");
        CHECK(ready.name == "Player's iPad" && ready.model == "iPad Air (5th generation)" && ready.os_version == "18.1");
        CHECK(ready.udid == "00008103-000A11112222333E" && ready.device_type == "iPad");
        CHECK(ready.ready() && ready.paired() && ready.reachable() && ready.developer_mode_on() && ready.os_major() == 18);
        // Then the reachable ones by name, then the rest.
        const Device& kitchen = (*list)[1];
        CHECK(kitchen.name == "Kitchen iPad" && kitchen.reachable() && kitchen.paired() && !kitchen.developer_mode_on() && !kitchen.ready());
        const Device& phone = (*list)[2];
        CHECK(phone.name == "Work iPhone" && phone.reachable() && !phone.paired() && phone.model == "iPhone15,2");
        const Device& away = (*list)[3];
        CHECK(away.name == "Away iPad" && !away.reachable() && !away.ready());
    }
    // An iPhone on iOS 15 has no Developer Mode to turn on.
    {
        Device old;
        old.os_version = "15.7";
        CHECK(old.developer_mode_on());
        old.os_version = "16.0";
        CHECK(!old.developer_mode_on());
    }
    CHECK(parse_devices(R"({"info": {"outcome": "success"}, "result": {"devices": []}})").has_value());
    CHECK(!parse_devices(R"({"info": {"outcome": "failed"}, "error": {"code": 1}})").has_value());
    CHECK(!parse_devices(R"({"result": {}})").has_value());
    CHECK(!parse_devices("not json").has_value());

    // `security find-identity -v -p codesigning`.
    const char* identities =
        "  1) 0123456789ABCDEF0123456789ABCDEF01234567 \"Apple Development: Jane Doe (A1B2C3D4E5)\"\n"
        "  2) 89ABCDEF0123456789ABCDEF0123456789ABCDEF \"Developer ID Application: Example Ltd (ZZZZZZZZZZ)\"\n"
        "  3) FEDCBA9876543210FEDCBA9876543210FEDCBA98 \"iPhone Developer: Old Name (Q1Q1Q1Q1Q1)\"\n"
        "  4) 0123456789ABCDEF0123456789ABCDEF01234567 \"Apple Development: Jane Doe (A1B2C3D4E5)\"\n"
        "     3 valid identities found\n";
    const auto ids = parse_identities(identities);
    CHECK(ids.size() == 2);
    CHECK(ids.size() == 2 && ids[0].name == "Apple Development: Jane Doe (A1B2C3D4E5)" && ids[0].sha1 == "0123456789ABCDEF0123456789ABCDEF01234567");
    CHECK(ids.size() == 2 && ids[1].name == "iPhone Developer: Old Name (Q1Q1Q1Q1Q1)");
    CHECK(parse_identities("     0 valid identities found\n").empty());

    // `security find-certificate -a -p -c <name>`: a test certificate laid
    // out as Apple's development certificates are (UID, CN, OU = the team,
    // O = its name, C), signed by a stand-in issuer with an OU of its own.
    const char* pem =
        "-----BEGIN CERTIFICATE-----\n"
        "MIICDDCCAbGgAwIBAgIUFt0frHnkTuzME8FxSwfcx5i0Fy8wCgYIKoZIzj0EAwIw\n"
        "QjEUMBIGA1UEAwwLVGVzdCBJc3N1ZXIxCzAJBgNVBAsMAkczMRAwDgYDVQQKDAdU\n"
        "ZXN0IENBMQswCQYDVQQGEwJVUzAeFw0yNjExMjgwMjE3MzNaFw0yNjEyMjgwMjE3\n"
        "MzNaMIGEMRowGAYKCZImiZPyLGQBAQwKSkFORTAwMDAwMTExMC8GA1UEAwwoQXBw\n"
        "bGUgRGV2ZWxvcG1lbnQ6IEphbmUgRG9lIChBMUIyQzNENEU1KTETMBEGA1UECwwK\n"
        "VEVBTTEyMzQ1NjERMA8GA1UECgwISmFuZSBEb2UxCzAJBgNVBAYTAkdCMFkwEwYH\n"
        "KoZIzj0CAQYIKoZIzj0DAQcDQgAE6zpa177jFYS6oE3ij6ohYCDOMhoFwz1sTOwB\n"
        "CMpt9Ze/dGwbs1v9YpLbLWUipfYI5pfK7byIVD9Naxf0ithgNaNCMEAwHQYDVR0O\n"
        "BBYEFG19dSCKZtkA1ZolH/uMqXSmYiQcMB8GA1UdIwQYMBaAFNfWPOp2zJzUrlis\n"
        "B3Y0fCY/arYxMAoGCCqGSM49BAMCA0kAMEYCIQCB31isp+HfjrdSmuKRYtRTIbNo\n"
        "jNJjGm6p4xknU7e7FQIhAILpO6aBVN/IdF5CktzqB03MmQoqYr+Dexl0ZdA4ZdVJ\n"
        "-----END CERTIFICATE-----\n";
    const auto subjects = pem_subjects(pem);
    CHECK(subjects.size() == 1);
    if (subjects.size() == 1) {
        CHECK(subjects[0].common_name == "Apple Development: Jane Doe (A1B2C3D4E5)");
        CHECK(subjects[0].unit == "TEAM123456");  // the subject's, not the issuer's G3
        CHECK(subjects[0].organisation == "Jane Doe");
    }
    CHECK(pem_subjects("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n").empty());  // not a certificate
    {
        const auto teams = identity_teams(ids, subjects);
        CHECK(teams.size() == 1 && teams[0].id == "TEAM123456" && teams[0].name == "Jane Doe" && teams[0].has_certificate && !teams[0].known_free);
    }

    // Xcode's settings (assumed layout), with a personal team and a company's.
    const char* settings = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DVTTextShowLineNumbers</key>
	<true/>
	<key>IDEProvisioningTeamByIdentifier</key>
	<dict>
		<key>0D8E0E9C-0000-0000-0000-000000000000</key>
		<array>
			<dict>
				<key>isFreeProvisioningTeam</key>
				<true/>
				<key>teamID</key>
				<string>TEAM123456</string>
				<key>teamName</key>
				<string>Jane Doe (Personal Team)</string>
				<key>teamType</key>
				<string>Individual</string>
			</dict>
			<dict>
				<key>isFreeProvisioningTeam</key>
				<false/>
				<key>teamID</key>
				<string>CORP000001</string>
				<key>teamName</key>
				<string>Example &amp; Sons Ltd</string>
			</dict>
		</array>
	</dict>
	<key>IDELastBuildDuration</key>
	<real>12.5</real>
	<key>Blob</key>
	<data>
	AAEC
	</data>
</dict>
</plist>
)";
    auto teams = xcode_teams(settings);
    CHECK(teams.size() == 2);
    if (teams.size() == 2) {
        CHECK(teams[0].id == "TEAM123456" && teams[0].name == "Jane Doe (Personal Team)" && teams[0].free && teams[0].known_free);
        CHECK(teams[1].id == "CORP000001" && teams[1].name == "Example & Sons Ltd" && !teams[1].free && teams[1].known_free);
    }
    // Both sources together: one team per identifier, what each knows.
    for (Team& t : identity_teams(ids, subjects)) add_team(teams, std::move(t));
    CHECK(teams.size() == 2 && teams[0].has_certificate && teams[0].name == "Jane Doe (Personal Team)" && !teams[1].has_certificate);
    CHECK(xcode_teams("<plist version=\"1.0\"><dict/></plist>").empty());
    CHECK(xcode_teams("not a plist").empty());

    // The property list reader itself.
    {
        const auto p = parse_plist("<?xml version=\"1.0\"?><!-- note --><plist version=\"1.0\"><array><integer>-42</integer><string>a&lt;b&#x41;&#66;</string>"
                                   "<string/><dict/><array/><false/><date>2026-12-05T10:20:30Z</date></array></plist>");
        CHECK(p.has_value() && p->is_array() && p->items.size() == 7);
        if (p && p->items.size() == 7) {
            CHECK(p->items[0].is_number() && p->items[0].number == -42);
            CHECK(p->items[1].string == "a<bAB");
            CHECK(p->items[2].is_string() && p->items[2].string.empty());
            CHECK(p->items[3].is_object() && p->items[4].is_array() && p->items[5].is_bool() && !p->items[5].boolean);
            CHECK(p->items[6].string == "2026-12-05T10:20:30Z");
        }
        for (const char* bad : {"", "<plist><dict><string>x</string></dict></plist>", "<plist><array><integer>1x</integer></array></plist>",
                                "<plist><string>open</plist>", "<plist><unknown/></plist>", "<plist><string>&bogus;</string></plist>"}) {
            if (parse_plist(bad)) std::printf("accepted: %s\n", bad);
            CHECK(!parse_plist(bad).has_value());
        }
    }

    // Dates.
    CHECK(parse_iso8601("1970-01-01T00:00:00Z") == 0);
    CHECK(parse_iso8601("2026-12-05T10:20:30Z") == 1796466030);
    CHECK(parse_iso8601("2024-02-29T00:00:00Z") == 1709164800);  // a leap day
    CHECK(parse_iso8601("2026-12-05T11:20:30+01:00") == 1796466030);
    CHECK(parse_iso8601("2026-12-05T10:20:30.123Z") == 1796466030);
    for (const char* bad : {"", "2026-12-05", "2026-13-05T10:20:30Z", "2026-12-05T10:20:30", "2026-12-05T10:20:30Zjunk", "26-12-05T10:20:30Z"})
        CHECK(!parse_iso8601(bad).has_value());

    // `security cms -D -i embedded.mobileprovision`: a free team's profile
    // (7 days), then a paid membership's (a year).
    const char* profile = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AppIDName</key>
	<string>XC io github qilinrecomp toycommander r0a1b2c3d</string>
	<key>ApplicationIdentifierPrefix</key>
	<array>
	<string>TEAM123456</string>
	</array>
	<key>CreationDate</key>
	<date>2026-11-28T09:00:00Z</date>
	<key>Platform</key>
	<array>
		<string>iOS</string>
		<string>xrOS</string>
		<string>visionOS</string>
	</array>
	<key>IsXcodeManaged</key>
	<true/>
	<key>DeveloperCertificates</key>
	<array>
		<data>MIIFxjCCBK6gAwIBAgIQ</data>
	</array>
	<key>Entitlements</key>
	<dict>
		<key>application-identifier</key>
		<string>TEAM123456.io.github.qilinrecomp.toycommander.r0a1b2c3d</string>
		<key>get-task-allow</key>
		<true/>
	</dict>
	<key>ExpirationDate</key>
	<date>2026-12-05T09:00:00Z</date>
	<key>Name</key>
	<string>iOS Team Provisioning Profile: io.github.qilinrecomp.toycommander.r0a1b2c3d</string>
	<key>ProvisionedDevices</key>
	<array>
		<string>00008103-000A11112222333E</string>
	</array>
	<key>TeamIdentifier</key>
	<array>
		<string>TEAM123456</string>
	</array>
	<key>TeamName</key>
	<string>Jane Doe</string>
	<key>TimeToLive</key>
	<integer>7</integer>
	<key>UUID</key>
	<string>11111111-2222-3333-4444-555555555555</string>
	<key>Version</key>
	<integer>1</integer>
</dict>
</plist>
)";
    const auto weekly = parse_profile(profile);
    CHECK(weekly.has_value());
    if (weekly) {
        CHECK(weekly->expires == *parse_iso8601("2026-12-05T09:00:00Z") && weekly->created == *parse_iso8601("2026-11-28T09:00:00Z"));
        CHECK(weekly->short_lived());
        CHECK(weekly->team_id == "TEAM123456" && weekly->team_name == "Jane Doe" && weekly->devices == 1);
        CHECK(weekly->app_id == "TEAM123456.io.github.qilinrecomp.toycommander.r0a1b2c3d");
    }
    std::string paid = profile;
    paid.replace(paid.find("2026-12-05T09:00:00Z"), 20, "2027-11-28T09:00:00Z");
    CHECK(parse_profile(paid) && !parse_profile(paid)->short_lived());
    CHECK(!parse_profile("<plist version=\"1.0\"><dict><key>Name</key><string>x</string></dict></plist>").has_value());  // no expiry
    CHECK(!parse_profile("").has_value());

    // The disc's files: a Redump .cue (quoted names), a plain one, a .gdi
    // (quoted and plain names), a .chd.
    const char* cue =
        "FILE \"Toy Commander (USA) (Track 01).bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n"
        "FILE \"Toy Commander (USA) (Track 02).bin\" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n"
        "REM HIGH-DENSITY AREA\r\nFILE \"Toy Commander (USA) (Track 03).bin\" BINARY\r\n  TRACK 03 MODE1/2352\r\n";
    const auto cue_files = disc_files("Toy Commander (USA).cue", cue);
    CHECK(cue_files && cue_files->size() == 4);
    CHECK(cue_files && cue_files->size() == 4 && (*cue_files)[0] == "Toy Commander (USA).cue" &&
          (*cue_files)[1] == "Toy Commander (USA) (Track 01).bin" && (*cue_files)[3] == "Toy Commander (USA) (Track 03).bin");
    const auto plain = disc_files("/discs/game.CUE", "file track 01.bin BINARY\nFILE track02.bin BINARY\nFILE track02.bin BINARY\n");
    CHECK(plain && plain->size() == 3 && (*plain)[0] == "game.CUE" && (*plain)[1] == "track 01.bin" && (*plain)[2] == "track02.bin");
    const char* gdi = "3\r\n1 0 4 2352 track01.bin 0\r\n2 756 0 2352 \"track 02.raw\" 0\r\n3 45000 4 2352 track03.bin 0\r\n";
    const auto gdi_files = disc_files("disc.gdi", gdi);
    CHECK(gdi_files && gdi_files->size() == 4 && (*gdi_files)[2] == "track 02.raw" && (*gdi_files)[3] == "track03.bin");
    CHECK(!disc_files("disc.gdi", "3\n1 0 4 2352 track01.bin 0\n").has_value());  // says 3, lists 1
    CHECK(!disc_files("disc.gdi", "1\n1 0 4\n").has_value());
    CHECK(!disc_files("disc.cue", "REM nothing\n").has_value());
    CHECK(!disc_files("disc.cue", "FILE \"../elsewhere.bin\" BINARY\n").has_value());
    CHECK(!disc_files("disc.cue", "FILE \"/etc/passwd\" BINARY\n").has_value());
    CHECK(!disc_files("disc.cue", "FILE \"C:\\\\x.bin\" BINARY\n").has_value());
    const auto chd = disc_files("Toy Racer.chd", "");
    CHECK(chd && chd->size() == 1 && (*chd)[0] == "Toy Racer.chd");

    // What's in Documents already (assumed format), and the skip.
    const auto remote = parse_remote_files(R"({"info": {"outcome": "success"}, "result": {"files": [
        {"name": "game.cue", "size": 1234, "type": "file"},
        {"relativePath": "Documents/track01.bin", "size": 700000000},
        {"name": "saves", "type": "directory"}]}})");
    CHECK(remote.size() == 2);
    CHECK(already_there(remote, "game.cue", 1234) && !already_there(remote, "game.cue", 1235));
    CHECK(already_there(remote, "track01.bin", 700000000) && !already_there(remote, "track02.bin", 1));
    CHECK(parse_remote_files("not json").empty());

    // The bundle identifier, the iOS kit's layout and the remembered install.
    CHECK(make_bundle_id("toycommander", 0x0a1b2c3du) == "io.github.qilinrecomp.toycommander.r0a1b2c3d");
    CHECK(make_bundle_id("toy_racer!", 0xffffffffu) == "io.github.qilinrecomp.toyracer.rffffffff");
    CHECK(valid_bundle_id(make_bundle_id("toyracer", 7)));
    CHECK(!valid_bundle_id("") && !valid_bundle_id("nodots") && !valid_bundle_id("a..b") && !valid_bundle_id("a.b c") && !valid_bundle_id(".a.b"));
    {
        const auto layout = kit_layout(parse_settings("cxxflags = -O2\nxcode_scheme = Other\n"), "toyracer");
        CHECK(layout.kit_library == "lib/libtoyracer_ios_kit.a" && layout.project == "xcode/QilinGame.xcodeproj" && layout.scheme == "Other");
        CHECK(layout.lifted_setting == "QILIN_LIFTED_LIB" && layout.kit_setting == "QILIN_KIT_LIB" && layout.configuration == "Release");
    }
    {
        Install i;
        i.installed = 1796000000;
        i.expires = 1796604800;
        i.short_lived = true;
        i.team = "TEAM123456";
        i.device = "AAAA1111-0000-0000-0000-000000000001";
        i.device_name = "Player's iPad";
        i.bundle_id = "io.github.qilinrecomp.toycommander.r0a1b2c3d";
        i.app = "/Users/x/Library/Application Support/qilin/toycommander/work/ios/DerivedData/Build/Products/Release-iphoneos/QilinGame.app";
        i.disc = "/Users/x/Discs/Toy Commander (USA).cue";
        const auto back = parse_install(format_install(i));
        CHECK(back.has_value());
        CHECK(back && back->installed == i.installed && back->expires == i.expires && back->short_lived && back->team == i.team &&
              back->device == i.device && back->device_name == i.device_name && back->bundle_id == i.bundle_id && back->app == i.app &&
              back->disc == i.disc);
        CHECK(!parse_install("").has_value());
        CHECK(!parse_install("installed=soon\n").has_value());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view which = argc > 1 ? argv[1] : "";
    if (which.empty() || which == "json") test_json();
    if (which.empty() || which == "version") test_version();
    if (which.empty() || which == "kit") test_kit();
    if (which.empty() || which == "ios") test_ios();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("launcher tests (%s): passed\n", which.empty() ? "all" : argv[1]);
    return 0;
}

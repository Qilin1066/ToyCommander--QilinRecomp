// Checking the player's data (input.hpp).
#include "input.hpp"

#include <system_error>

#include "json.hpp"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

bool input_name_ok(const Kit& kit, const fs::path& path) {
    if (kit.input_extensions.empty()) return true;
    std::string ext = show(path.extension());
    if (ext.starts_with('.')) ext.erase(0, 1);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (const std::string& e : kit.input_extensions)
        if (e == ext) return true;
    return false;
}

InputCheck check_input(const Kit& kit, const fs::path& input) {
    InputCheck out;
    const auto fail = [&](std::string message) {
        out.ok = false;
        out.message = std::move(message);
        return out;
    };
    std::error_code ec;
    const fs::file_status status = fs::status(input, ec);
    if (ec || !fs::exists(status)) return fail("Can't find " + show(input) + ". Choose your " + kit.input_noun + " again.");
    if (kit.input == InputKind::File) {
        if (fs::is_directory(status)) return fail("That's a folder, not your " + kit.input_noun + ". " + kit.input_hint);
        if (!fs::is_regular_file(status) || !input_name_ok(kit, input)) return fail("That's not your " + kit.input_noun + ". " + kit.input_hint);
    } else if (!fs::is_directory(status)) {
        return fail("That's a file, not a folder. " + kit.input_hint);
    }

    // The recompiler's answer.
    const fs::path logs = kit.work / "logs";
    fs::create_directories(logs, ec);
    if (ec) return fail("Can't make the folder " + show(logs) + ": " + ec.message() + ".");
    const auto words = expand_command(kit, kit.identify, input);
    if (!words) return fail("kit/game.txt's identify can't be used (" + words.error() + "). Unpack the download again.");
    Command c = tool(from_utf8(words->front()), {words->begin() + 1, words->end()}, kit.work, logs / "identify.json");
    if (c.exe.is_relative()) c.exe = kit.kit / c.exe;
    c.err = logs / "identify-errors.txt";
    const auto code = run(c);
    const std::string checking = "Checking " + kit.the_input() + " failed";
    if (!code) return fail(checking + ": the recompiler didn't start (" + code.error() + ").");
    const std::string text = read_file(c.out).value_or("");
    const std::size_t open = text.find('{'), close = text.rfind('}');
    auto doc = open != std::string::npos && close != std::string::npos && close > open
                   ? json::parse(std::string_view(text).substr(open, close - open + 1))
                   : std::expected<json::Value, std::string>(std::unexpected(std::string("no JSON")));
    const auto first_error = [&] {
        const std::string errors = tail(c.err, 3);
        return errors.empty() ? std::string() : ": " + std::string(trim(errors.substr(0, errors.find('\n'))));
    };
    if (!doc || !doc->is_object() || !doc->find("ok") || !doc->find("ok")->is_bool())
        return fail(checking + " (the recompiler gave no answer, " + describe_exit(*code) + ")" + first_error() + ".");
    out.title = std::string(doc->string_or("title", ""));
    out.id = std::string(doc->string_or("id", ""));
    out.version = std::string(doc->string_or("version", ""));
    out.message = std::string(doc->string_or("message", ""));
    const bool ok = doc->bool_or("ok", false);
    // Both must agree: a tool that says ok but fails (or the reverse) is broken.
    if (ok != (*code == 0) || (*code != 0 && *code != 2 && *code != 3))
        return fail(checking + " (the recompiler's answer and its " + describe_exit(*code) + " disagree)" + first_error() + ".");
    out.ok = ok;
    if (out.message.empty()) {
        if (ok) out.message = "This is " + (out.title.empty() ? kit.build : out.title) + ": the right " + kit.input_noun + ".";
        else if (*code == 3) out.message = "This isn't " + kit.the_input() + " this download needs: it is for " + kit.wanted() + ".";
        else out.message = "Can't read " + kit.the_input() + ". This download is for " + kit.wanted() + ".";
    }
    return out;
}

}  // namespace qilin::launcher

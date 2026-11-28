// A shortcut to the launcher on the player's desktop (shortcut.hpp).
#include "shortcut.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <iterator>
#include <system_error>

#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

// COM for this thread while it lives (apartment-threaded, as the shell wants).
struct Com {
    bool ok;
    Com() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (ok) CoUninitialize();
    }
};

template <typename T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() {
        if (p) p->Release();
    }
};

std::string hresult(HRESULT hr) { return system_message(static_cast<unsigned long>(hr)); }

// Where an existing shortcut points ("" if it can't be read).
fs::path target_of(const fs::path& link) {
    const Com com;
    Ref<IShellLinkW> shell;
    Ref<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell.p))) ||
        FAILED(shell.p->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file.p))) || FAILED(file.p->Load(link.c_str(), STGM_READ)))
        return {};
    wchar_t path[MAX_PATH * 4] = {};
    if (FAILED(shell.p->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH))) return {};
    return fs::path(path);
}

}  // namespace

const char* shortcut_label() { return "Add a desktop shortcut"; }

fs::path desktop_shortcut(const Kit& kit) {
    PWSTR desktop = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) out = fs::path(desktop) / widen(kit.title + ".lnk");
    CoTaskMemFree(desktop);
    return out;
}

bool has_desktop_shortcut(const Kit& kit) {
    const fs::path link = desktop_shortcut(kit);
    std::error_code ec;
    if (link.empty() || !fs::is_regular_file(link, ec)) return false;
    const fs::path target = target_of(link);
    return !target.empty() && fs::equivalent(target, kit.launcher, ec);
}

std::expected<void, std::string> make_desktop_shortcut(const Kit& kit) {
    const fs::path link = desktop_shortcut(kit);
    if (link.empty()) return std::unexpected(std::string("Windows names no desktop folder"));
    const Com com;
    if (!com.ok) return std::unexpected(std::string("the shell can't be reached"));
    Ref<IShellLinkW> shell;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell.p));
    if (FAILED(hr)) return std::unexpected(hresult(hr));
    shell.p->SetPath(kit.launcher.c_str());
    shell.p->SetWorkingDirectory(kit.top.c_str());
    shell.p->SetIconLocation(kit.launcher.c_str(), 0);
    shell.p->SetDescription(widen("Play " + kit.title).c_str());
    Ref<IPersistFile> file;
    hr = shell.p->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file.p));
    if (SUCCEEDED(hr)) hr = file.p->Save(link.c_str(), TRUE);
    if (FAILED(hr)) return std::unexpected(hresult(hr));
    return {};
}

}  // namespace qilin::launcher

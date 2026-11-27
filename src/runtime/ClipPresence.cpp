#include "PCH.h"
#include "runtime/ClipPresence.h"
#include "runtime/ActionRegistry.h"
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_set>

namespace AnimationInjector::ClipPresence {
namespace {
std::once_flag g_once;
std::vector<bool> g_absent;
std::vector<bool> g_ownFile;                    // an optional action's clip at its own path (loose or in an archive)
std::unordered_set<std::string> g_absentFiles;  // lowercase
std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A path's last part, lowercase, wide (an OAR folder name may hold any character: never through path::string()).
std::wstring Key(std::wstring_view name) {
    std::wstring key(name);
    for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return key;
}
std::wstring FileName(const std::string& file) {
    const auto slash = file.find_last_of("\\/");
    const std::string_view name =
        slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(slash + 1);
    std::wstring wide;
    wide.reserve(name.size());
    for (const char c : name) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return Key(wide);
}
// Every .hkx under the replacers' folders, by name.
std::unordered_set<std::wstring> ReplacerFiles(std::size_t& scanned) {
    namespace fs = std::filesystem;
    std::unordered_set<std::wstring> names;
    const auto animations = fs::current_path() / L"Data" / L"meshes" / L"actors" / L"character" / L"animations";
    for (const wchar_t* folder : {L"OpenAnimationReplacer", L"DynamicAnimationReplacer"}) {
        std::error_code ec;
        const auto root = animations / folder;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            auto name = Key(it->path().filename().wstring());
            if (name.size() > 4 && name.ends_with(L".hkx")) {
                ++scanned;
                names.insert(std::move(name));
            }
        }
    }
    return names;
}
void Build() {
    const int count = Actions::Count();
    g_absent.assign(static_cast<std::size_t>(count), false);
    g_ownFile.assign(static_cast<std::size_t>(count), false);
    std::vector<int> optional;
    for (int a = 0; a < count; ++a)
        if (Actions::Spec(a).optional && !Actions::Spec(a).clips.empty()) optional.push_back(a);
    if (optional.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    std::size_t scanned = 0;
    const auto replaced = ReplacerFiles(scanned);
    int absent = 0;
    for (const int a : optional) {
        const auto& file = Actions::Spec(a).clips.front().file;
        RE::BSResourceNiBinaryStream stream(std::string("Meshes\\Actors\\Character\\") + file);
        g_ownFile[static_cast<std::size_t>(a)] = stream.good();
        if (stream.good() || replaced.contains(FileName(file))) continue;
        g_absent[static_cast<std::size_t>(a)] = true;
        g_absentFiles.insert(Lower(file));
        ++absent;
        logger::debug("[ClipPresence] '{}' ({}): no file installed, not made", Actions::Spec(a).id, file);
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    logger::info("[ClipPresence] {} optional actions: {} installed, {} without a file are left out ({} replacer files "
                 "read in {:.0f} ms)",
        optional.size(), optional.size() - absent, absent, scanned, ms);
}
}
bool NothingToPlay(int action, bool replaced) {
    if (replaced || action < 0 || action >= Actions::Count() || !Actions::Spec(action).optional) return false;
    std::call_once(g_once, Build);
    return static_cast<std::size_t>(action) < g_ownFile.size() && !g_ownFile[static_cast<std::size_t>(action)];
}
bool AbsentFile(const std::string& file) {
    std::call_once(g_once, Build);
    return !g_absentFiles.empty() && g_absentFiles.contains(Lower(file));
}
bool Absent(int action) {
    std::call_once(g_once, Build);
    return action >= 0 && static_cast<std::size_t>(action) < g_absent.size() &&
           g_absent[static_cast<std::size_t>(action)];
}
}

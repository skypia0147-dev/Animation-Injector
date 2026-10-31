#include "PCH.h"
#include "runtime/TdmMovement.h"
#include "compat/TrueDirectionalMovementAPI.h"
#include "Settings.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

namespace AnimationInjector::TdmMovement {
namespace {
std::atomic<TDM_API::IVTDM1*> g_api{nullptr};
std::atomic_bool g_present{false};
// TDM's two settings, re-read at most once a second (an MCM change is written to its settings file).
std::mutex g_settingsLock;
int g_drawn = 2, g_sheathed = 2;  // TDM's own defaults: directional
std::chrono::steady_clock::time_point g_readAt{};
bool g_read = false;

std::string Text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void ReadSettings() {
    const auto now = std::chrono::steady_clock::now();
    if (g_read && now - g_readAt < std::chrono::seconds(1)) return;
    g_read = true;
    g_readAt = now;
    // The MCM defaults, then the player's own values over them (as MCM Helper keeps them).
    const auto data = std::filesystem::current_path() / "Data" / "MCM";
    int drawn = 2, sheathed = 2;
    for (const auto& file : {data / "Config" / "TrueDirectionalMovement" / "settings.ini",
             data / "Settings" / "TrueDirectionalMovement.ini"}) {
        const auto text = Text(file);
        if (const auto v = IniInt(text, "DirectionalMovement", "uDirectionalMovementDrawn")) drawn = *v;
        if (const auto v = IniInt(text, "DirectionalMovement", "uDirectionalMovementSheathed")) sheathed = *v;
    }
    if (drawn != g_drawn || sheathed != g_sheathed)
        logger::info(
            "[TDM] settings: weapon drawn {}, sheathed {} (0 off, 1 vanilla style, 2 directional)", drawn, sheathed);
    g_drawn = drawn;
    g_sheathed = sheathed;
}
}

void Load() {
    if (!FrameworkSettings::Get().followTdm) {
        logger::info("[TDM] movement conditions: the keys, no Rotate-window turn (bFollowTDM = false)");
        return;
    }
    if (!REX::W32::GetModuleHandleW(L"TrueDirectionalMovement.dll")) {
        logger::info("[TDM] movement conditions: the keys, no Rotate-window turn (TDM not loaded)");
        return;
    }
    g_present = true;
    g_api = static_cast<TDM_API::IVTDM1*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V1));
    std::lock_guard lock(g_settingsLock);
    g_read = false;
    ReadSettings();
    logger::info(
        "[TDM] movement conditions and Rotate-window turn: follow TDM's settings (weapon drawn {}, sheathed {}){}",
        g_drawn, g_sheathed, g_api.load() ? "" : ", no target lock API");
}

Mode Current(bool weaponDrawn, bool refresh) {
    if (!g_present) return Mode::kAbsent;
    if (auto* api = g_api.load(); api && api->GetTargetLockState()) return Mode::kTargetLock;
    std::lock_guard lock(g_settingsLock);
    if (refresh) ReadSettings();
    return FromSetting(weaponDrawn ? g_drawn : g_sheathed);
}
}  // namespace AnimationInjector::TdmMovement

#include "PCH.h"
#include "Settings.h"
#include <filesystem>
#include <pugixml.hpp>
#include "declarations/XmlV2.h"
#include "runtime/RagdollProfile.h"

namespace AnimationInjector {

namespace {
struct Ini {
    std::wstring path;
    float Float(const wchar_t* sec, const wchar_t* key, float def) const {
        wchar_t buf[64] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 64, path.c_str());
        if (!buf[0]) return def;
        wchar_t* end = nullptr;
        const float v = std::wcstof(buf, &end);
        return end != buf ? v : def;
    }
    std::int32_t Int(const wchar_t* sec, const wchar_t* key, std::int32_t def) const {
        wchar_t buf[64] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 64, path.c_str());
        if (!buf[0]) return def;
        wchar_t* end = nullptr;
        const long v = std::wcstol(buf, &end, 0);
        return end != buf ? static_cast<std::int32_t>(v) : def;
    }
    bool Bool(const wchar_t* sec, const wchar_t* key, bool def) const {
        wchar_t buf[16] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 16, path.c_str());
        if (!buf[0]) return def;
        return _wcsicmp(buf, L"true") == 0 || buf[0] == L'1';
    }
};
}

FrameworkSettings& FrameworkSettings::Get() {
    static FrameworkSettings s;
    return s;
}

void FrameworkSettings::Load() {
    Ini ini{(std::filesystem::current_path() / "Data/SKSE/Plugins/AnimationInjector.ini").wstring()};
    injectState = ini.Bool(L"Inject", L"bInjectState", true);
    stateIdBase = ini.Int(L"Inject", L"iStateIdBase", 7700);
    wildcardPriority = ini.Int(L"Inject", L"iWildcardPriority", 5000);
    convertAnnotations = ini.Bool(L"Inject", L"bConvertAnnotations", true);
    loadingProgress = ini.Bool(L"Display", L"bLoadingProgress", true);
    preloadOrdinary = ini.Bool(L"Preload", L"bOrdinary", true);
    preloadOrdinaryBudgetMB = std::clamp(ini.Int(L"Preload", L"iOrdinaryBudgetMB", 0), 0, 1 << 20);
    switchZoom = std::clamp(ini.Float(L"Camera", L"fSwitchZoom", 0.5f), -1.0f, 2.0f);
    nativePairAlignment = std::clamp(ini.Int(L"Compatibility", L"iNativePairAlignment", 2), 0, 2);
    pairEndPosition = std::clamp(ini.Int(L"Compatibility", L"iPairEndPosition", 2), 0, 2);
    debug = ini.Bool(L"Debug", L"bDebug", false);
    spaceProbe = ini.Bool(L"Debug", L"bSpaceProbe", false);
    ragdollTrace = ini.Bool(L"Debug", L"bRagdollTrace", false);
    logger::info("[RagdollTrace] enabled={}", ragdollTrace);
    if (debug) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);
    }
    logger::info(
        "[Settings] 프레임워크: inject={} stateIdBase={} priority={} convertAnnotations={} debug={} nativePairAlignment={} "
        "pairEndPosition={}",
        injectState, stateIdBase, wildcardPriority, convertAnnotations, debug, nativePairAlignment, pairEndPosition);
}

namespace Declarations {

// 폴더 INI 하나 → ActionSpec 하나. 섹션은 DirectDodge 시절 그대로 ([Input] [Animation] [Motion] [Condition] [Cancel]).
// Current XML is validated and normalized before constructing runtime declarations.
namespace {
std::uint32_t ParseKey(const char* a_text, std::uint32_t a_def) {
    if (!a_text || !*a_text) return a_def;
    // DirectX 스캔코드 = 키의 물리적 위치. 한/영 입력 상태와 무관하다 (게임은 문자가 아니라 스캔코드를 본다)
    static const std::pair<const char*, std::uint32_t> names[] = {
        {"Esc", 1},
        {"1", 2},
        {"2", 3},
        {"3", 4},
        {"4", 5},
        {"5", 6},
        {"6", 7},
        {"7", 8},
        {"8", 9},
        {"9", 10},
        {"0", 11},
        {"Minus", 12},
        {"Equals", 13},
        {"Backspace", 14},
        {"Tab", 15},
        {"Q", 16},
        {"W", 17},
        {"E", 18},
        {"R", 19},
        {"T", 20},
        {"Y", 21},
        {"U", 22},
        {"I", 23},
        {"O", 24},
        {"P", 25},
        {"LBracket", 26},
        {"RBracket", 27},
        {"Enter", 28},
        {"LCtrl", 29},
        {"A", 30},
        {"S", 31},
        {"D", 32},
        {"F", 33},
        {"G", 34},
        {"H", 35},
        {"J", 36},
        {"K", 37},
        {"L", 38},
        {"Semicolon", 39},
        {"Apostrophe", 40},
        {"Grave", 41},
        {"LShift", 42},
        {"Backslash", 43},
        {"Z", 44},
        {"X", 45},
        {"C", 46},
        {"V", 47},
        {"B", 48},
        {"N", 49},
        {"M", 50},
        {"Comma", 51},
        {"Period", 52},
        {"Slash", 53},
        {"RShift", 54},
        {"NumMul", 55},
        {"LAlt", 56},
        {"Space", 57},
        {"CapsLock", 58},
        {"F1", 59},
        {"F2", 60},
        {"F3", 61},
        {"F4", 62},
        {"F5", 63},
        {"F6", 64},
        {"F7", 65},
        {"F8", 66},
        {"F9", 67},
        {"F10", 68},
        {"NumLock", 69},
        {"ScrollLock", 70},
        {"Num7", 71},
        {"Num8", 72},
        {"Num9", 73},
        {"NumMinus", 74},
        {"Num4", 75},
        {"Num5", 76},
        {"Num6", 77},
        {"NumPlus", 78},
        {"Num1", 79},
        {"Num2", 80},
        {"Num3", 81},
        {"Num0", 82},
        {"NumDot", 83},
        {"F11", 87},
        {"F12", 88},
        {"NumEnter", 156},
        {"RCtrl", 157},
        {"NumDiv", 181},
        {"RAlt", 184},
        {"Home", 199},
        {"Up", 200},
        {"PageUp", 201},
        {"Left", 203},
        {"Right", 205},
        {"End", 207},
        {"Down", 208},
        {"PageDown", 209},
        {"Insert", 210},
        {"Delete", 211},
        {"Mouse1", 256},
        {"Mouse2", 257},
        {"Mouse3", 258},
        {"Mouse4", 259},
        {"Mouse5", 260},
        {"Mouse6", 261},
        {"Mouse7", 262},
        {"MouseWheelUp", 264},
        {"MouseWheelDown", 265},
    };
    for (const auto& [n, v] : names)
        if (_stricmp(a_text, n) == 0) return v;
    char* end = nullptr;
    const long v = std::strtol(a_text, &end, 0);
    return end != a_text ? static_cast<std::uint32_t>(v) : a_def;
}
bool FromXml(
    const std::filesystem::path& a_file, std::vector<ActionSpec>& a_out, std::vector<EventRuleSpec>* a_events) {
    const auto file = PathText(a_file);
    pugi::xml_document doc;
    const auto res = doc.load_file(a_file.c_str());
    if (!res) {
        logger::warn("[Decl] {}: XML 파싱 실패 — {} (offset {})", file, res.description(), res.offset);
        return false;
    }
    if (!doc.child("AnimationSet")) {
        logger::error("[Decl] {}: unsupported XML format; the root must be <AnimationSet version=\"6\">", file);
        return false;
    }
    // A file loads completely or not at all: Link and Flow refer to sibling actions, so a
    // partially loaded file would leave dangling references.
    std::vector<ActionSpec> parsed;
    std::vector<EventRuleSpec> events;
    std::string mod, error;
    if (!XmlV2::Compile(doc, file, parsed, events, mod, error, ResolveKeyName)) {
        logger::error("[Decl:v2] {}: {} — entire file skipped", file, error);
        return false;
    }
    logger::info("[Decl:v2] {}: validated", file);
    logger::info("[Decl] {} (mod '{}'): 액션 {}개, 이벤트 등록 {}개", file, mod, parsed.size(), events.size());
    a_out.insert(a_out.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    if (a_events)
        a_events->insert(
            a_events->end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
    return true;
}
}

std::uint32_t ResolveKeyName(const char* name) { return ParseKey(name, UINT32_MAX); }

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

bool LoadXmlFile(const std::filesystem::path& file, std::vector<ActionSpec>& output) {
    return FromXml(file, output, nullptr);
}
bool LoadXmlFile(
    const std::filesystem::path& file, std::vector<ActionSpec>& output, std::vector<EventRuleSpec>& events) {
    return FromXml(file, output, &events);
}

std::vector<ActionSpec> LoadAll(std::vector<EventRuleSpec>* events) {
    return LoadAllFrom(std::filesystem::current_path() / "Data/SKSE/Plugins/AnimationInjector", events);
}

std::vector<ActionSpec> LoadAllFrom(const std::filesystem::path& root, std::vector<EventRuleSpec>* events) {
    std::vector<ActionSpec> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        logger::warn("[Decl] 액션 폴더 없음: {}", PathText(root));
        return out;
    }
    std::vector<std::filesystem::path> xmls;
    for (const auto& dir : std::filesystem::directory_iterator(root, ec)) {
        if (!dir.is_directory(ec)) continue;
        // Rig ragdoll profiles live beside the action sets; they are not action declarations.
        if (_wcsicmp(dir.path().filename().c_str(), RagdollProfile::kFolder.data()) == 0) continue;
        for (const auto& f : std::filesystem::directory_iterator(dir.path(), ec)) {
            if (!f.is_regular_file(ec)) continue;
            if (_wcsicmp(f.path().extension().c_str(), L".xml") == 0) xmls.push_back(f.path());
        }
    }
    std::sort(xmls.begin(), xmls.end());
    for (const auto& f : xmls) FromXml(f, out, events);
    // Id collision (case-insensitive): the later file in name order replaces the earlier action,
    // which keeps its position. One hash lookup per action.
    std::unordered_map<std::string, std::size_t> kept;
    kept.reserve(out.size());
    std::vector<ActionSpec> unique;
    unique.reserve(out.size());
    for (auto& spec : out) {
        std::string key = spec.id;
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto [it, inserted] = kept.try_emplace(std::move(key), unique.size());
        if (inserted) {
            unique.push_back(std::move(spec));
            continue;
        }
        auto& earlier = unique[it->second];
        logger::warn("[Decl] 액션 id '{}' 중복 — {} 가 {} 를 덮음", spec.id, spec.source, earlier.source);
        earlier = std::move(spec);
    }
    out = std::move(unique);
    // The per-action summary is only logged at debug; its strings are not built otherwise.
    for (const auto& s : out) {
        if (!FrameworkSettings::Get().debug) break;
        std::string wins;
        for (const auto& w : s.cancels)
            wins += fmt::format("[{} mode {} {}~{} {}] ", w.id, w.mode,
                w.openAt >= 0 ? fmt::format("@{:.2f}", w.openAt) : w.openEvent,
                w.closeAt >= 0 ? fmt::format("@end-{:.2f}", w.closeAt) : w.closeEvent,
                w.vanillaMatches.size() ? "vanilla" : "");
        logger::debug(
            "[Decl] 액션 '{}' ← {} : 클립 {}개, 키 {}, 바닐라 전환 {} exit='{}' 블렌드 enter {:.2f} exit {:.2f}, recovery {}",
            s.id, s.source, s.clips.size(), s.trigger.key, wins, s.events.exit, s.enterBlend, s.exitBlend,
            s.recovery.enabled
                ? fmt::format("'{}'{} 입력 {}{}{}", s.recovery.event,
                      s.recovery.at >= 0 ? fmt::format("@{:.2f}", s.recovery.at) : "", s.recovery.move ? "move " : "",
                      s.recovery.sprint ? "sprint " : "", s.recovery.jump ? "jump" : "")
                : std::string("없음"));
        auto slotTxt = [](const SlotWindow& w) {
            return !w.enabled
                       ? std::string("-")
                       : fmt::format("{}~{}", w.onAt >= 0 ? fmt::format("@{:.2f}", w.onAt) : w.onEvent,
                             w.offAt >= 0
                                 ? (w.offRel ? fmt::format("@end-{:.2f}", w.offAt) : fmt::format("@{:.2f}", w.offAt))
                                 : w.offEvent);
        };
        logger::debug("[Decl]   슬롯: rotate {} | iframe {} | hyperArmor {} | cost {}", slotTxt(s.rotate),
            slotTxt(s.iframe), slotTxt(s.hyperArmor),
            s.cost.enabled ? fmt::format("stamina {:.0f} magicka {:.0f} @{}", s.cost.stamina, s.cost.magicka,
                                 s.cost.at >= 0 ? fmt::format("{:.2f}", s.cost.at) : s.cost.event)
                           : std::string("-"));
    }
    return out;
}

}  // namespace Declarations
}  // namespace AnimationInjector

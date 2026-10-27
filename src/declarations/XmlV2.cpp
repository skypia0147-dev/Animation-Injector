#include "XmlV2.h"
#include "runtime/ActionSpec.h"
#include "runtime/PairedRig.h"
#include "runtime/CarrySpec.h"
#include "ConditionFlowXml.h"
#include "Cards.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

// One pass from the AnimationSet document (format 6, docs/설계-형식6-조건카드.md) to runtime declarations: every
// element is validated and then read into its ActionSpec fields; starts and condition cards become the actions'
// triggers and selecting programs (Cards.h). Times keep their textual forms ("@seconds",
// "@end-seconds", "event") between the two steps so that numbers convert exactly as the
// declaration loader always has (strtof and pugixml's as_float on the author's text).
namespace AnimationInjector::XmlV2 {
namespace {
using Node = pugi::xml_node;
using Names = std::initializer_list<std::string_view>;
std::string get(Node n, const char* key, const char* fallback = "") { return n.attribute(key).as_string(fallback); }
bool has(Names names, std::string_view value) { return std::find(names.begin(), names.end(), value) != names.end(); }
std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// The element as the author wrote it: its path and id.
std::string where(Node n) { return n.path() + (n.attribute("id") ? " [" + get(n, "id") + "]" : ""); }
[[noreturn]] void fail(Node n, const std::string& reason) { throw std::runtime_error(where(n) + ": " + reason); }
// What a condition card holds (a start's, a transition's or a QTE result's).
const Names kCardParts = {"Check", "And", "Or", "Row", "True", "False"};
void shape(Node n, Names attrs, Names children = {}) {
    for (auto a : n.attributes())
        if (!has(attrs, a.name())) fail(n, "알 수 없는 속성 '" + std::string(a.name()) + "'");
    for (auto c : n.children()) {
        if (c.type() == pugi::node_element && !has(children, c.name())) fail(c, "지원하지 않는 요소");
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
            fail(n, "요소 안의 텍스트는 허용하지 않습니다");
    }
}
Node one(Node n, const char* name) {
    auto c = n.child(name);
    if (c && c.next_sibling(name)) fail(c, "중복 요소");
    return c;
}
std::string required(Node n, const char* key) {
    auto s = get(n, key);
    if (s.empty()) fail(n, std::string(key) + " 값이 필요합니다");
    return s;
}
void identifier(Node n, const std::string& s) {
    auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (s.empty() || s.size() > 64 || !letter(s[0]) || !std::all_of(s.begin(), s.end(), [&](unsigned char c) {
            return letter(c) || (c >= '0' && c <= '9') || c == '_';
        }))
        fail(n, "ID는 영문으로 시작하는 영문/숫자/밑줄 64자 이내여야 합니다: " + s);
}
double numeric(Node n, std::string_view value, double max = 600, double min = 0) {
    double v = 0;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (value.empty() || ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(v) || v < min ||
        v > max)
        fail(n, "숫자 범위 또는 형식 오류: " + std::string(value));
    return v;
}
void num(Node n, const char* key, double max = 600, double min = 0) {
    if (n.attribute(key)) numeric(n, get(n, key), max, min);
}
void boolean(Node n, const char* key) {
    if (n.attribute(key) && !has({"true", "false"}, get(n, key)))
        fail(n, std::string(key) + "는 true 또는 false여야 합니다");
}
void choice(Node n, const char* key, Names values) {
    if (n.attribute(key) && !has(values, get(n, key)))
        fail(n, std::string(key) + " 값이 지원되지 않습니다: " + get(n, key));
}
void eventName(Node n, const std::string& name) {
    if (name.empty() || name.size() > 256 || name.find_first_of("@|\r\n\t") != std::string::npos)
        fail(n, "이벤트 이름이 잘못되었습니다: " + name);
    // runtime/AnnotationCommands.h: "AI" carries annotation commands (AI.CamShake|strength|duration).
    if (lower(name) == "ai") fail(n, "AI는 주석 명령 전용 이벤트 이름입니다(0.12.274): AI.CamShake|세기|시간");
}
void clip(Node n, const std::string& name) {
    auto s = lower(name);
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s.size() > 1024 || s[0] == '/' || s.find(':') != std::string::npos || !s.ends_with(".hkx"))
        fail(n, "상대 HKX 경로가 필요합니다: " + name);
    // The engine opens animation files through narrow code-page paths: a name outside printable ASCII is
    // never found. Letters, digits and the symbols Windows allows in names are accepted.
    for (const unsigned char c : name)
        if (c < 0x20 || c > 0x7E || std::strchr("*?\"<>|", c))
            fail(n, "HKX 경로는 ASCII 영문·숫자·기호만 씁니다(한글 등은 게임이 찾지 못함): " + name);
    std::size_t begin = 0;
    while (begin < s.size()) {
        auto end = s.find('/', begin);
        auto part = s.substr(begin, end - begin);
        if (part == ".." || part.empty()) fail(n, "상위/빈 경로는 사용할 수 없습니다");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}
std::vector<std::string> list(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < s.size()) {
        auto end = s.find(',', start);
        auto value = s.substr(start, end - start);
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        while (!value.empty() && value.back() == ' ') value.pop_back();
        out.push_back(value);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}
std::vector<std::string> eventList(Node n, const char* attr) {
    auto text = required(n, attr);
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::size_t start = 0;
    do {
        auto end = text.find(',', start);
        auto value = text.substr(start, end == std::string::npos ? end : end - start);
        auto first = value.find_first_not_of(" ");
        auto last = value.find_last_not_of(" ");
        value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
        if (value.empty() || value.size() > 64 ||
            std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            fail(n, "invalid event list item (1..64 characters)");
        if (seen.insert(lower(value)).second) result.push_back(value);
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return result;
}

// "@0.35" → 0.35 (from the start), "@end" → 0 (the end), "@end-0.05" → 0.05 (before the end). An event
// name returns false.
bool ParseAt(const char* text, float& time, bool& relativeToEnd) {
    if (!text || text[0] != '@') return false;
    const char* t = text + 1;
    if (_strnicmp(t, "end", 3) == 0) {
        relativeToEnd = true;
        const char* rest = t + 3;
        time = (*rest == '-') ? std::fabs(std::strtof(rest + 1, nullptr)) : 0.0f;
        return true;
    }
    relativeToEnd = false;
    time = std::strtof(t, nullptr);
    return true;
}
std::vector<std::string> Split(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else if (c != ' ')
            current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}
// vanilla:Attack → the native event name substring its cancel window inherits.
std::string VanillaMatch(const std::string& alias) {
    static const std::pair<const char*, const char*> table[] = {
        {"vanilla:Attack", "attack"},
        {"vanilla:PowerAttack", "attackPowerStart"},
        {"vanilla:Block", "blockStart"},
        {"vanilla:Bash", "bashStart"},
        {"vanilla:Sprint", "sprintStart"},
        {"vanilla:Jump", "jumpStart"},
        {"vanilla:Sneak", "sneakStart"},
    };
    for (const auto& [a, m] : table)
        if (_stricmp(alias.c_str(), a) == 0) return m;
    if (_strnicmp(alias.c_str(), "event:", 6) == 0) return alias.substr(6);
    return "";
}
// A seconds time read as a clock time or an event: Recovery, QTE, Cost, CarryRelease.
void ReadMoment(const std::string& text, std::string& event, float& at) {
    float t;
    bool relative;
    if (ParseAt(text.c_str(), t, relative)) {
        if (!relative) at = t;
    } else
        event = text;
}
// On/off slot (Rotate, IFrame, HyperArmor).
void ReadSlot(SlotWindow& w, const std::string& on, const std::string& off) {
    w.enabled = true;
    float t;
    bool relative;
    if (ParseAt(on.c_str(), t, relative)) {
        if (!relative) w.onAt = t;
    } else
        w.onEvent = on;
    if (ParseAt(off.c_str(), t, relative)) {
        w.offAt = t;
        w.offRel = relative;
    } else
        w.offEvent = off;
}
enum class Basis { Seconds, End, Either };
std::string clock(Node n, const std::string& s, Basis basis) {
    const bool end = s == "end" || s.starts_with("end-");
    if (end) {
        if (basis == Basis::Seconds) fail(n, "현재 런타임은 이 위치에 끝 기준 시각을 지원하지 않습니다");
        if (s != "end") numeric(n, s.substr(4));
    } else {
        if (basis == Basis::End) fail(n, "현재 런타임은 이 위치에 끝 기준 시각만 지원합니다");
        numeric(n, s);
    }
    return "@" + s;
}
std::string time(Node n, const char* name, Basis basis, const char* def = nullptr) {
    const auto s = def ? get(n, name, def) : required(n, name);
    const bool event = s.starts_with("event:");
    std::string out;
    if (event) {
        out = s.substr(6);
        eventName(n, out);
    } else
        out = clock(n, s, basis);
    return out;
}
// A clip time in seconds (or from the end where `basis` allows), never an event: an event moment of a
// function is a global EventRegistry entry (format 3 on).
std::string seconds(Node n, const char* name, Basis basis, const char* def = nullptr) {
    const auto s = def ? get(n, name, def) : required(n, name);
    if (s.starts_with("event:"))
        fail(n, std::string(name) + ": 이벤트 시점은 EventRegistry의 이벤트 기능으로 지정합니다");
    return clock(n, s, basis);
}
template<class T, std::size_t N>
T Pick(const std::pair<std::string_view, T> (&table)[N], std::string_view value, T fallback) {
    for (const auto& [name, v] : table)
        if (name == value) return v;
    return fallback;
}
const std::pair<std::string_view, EventOp> kOps[] = {{"hide", EventOp::kHide}, {"show", EventOp::kShow},
    {"spell", EventOp::kSpell}, {"stateOn", EventOp::kStateOn}, {"stateOff", EventOp::kStateOff},
    {"cost", EventOp::kCost}, {"shake", EventOp::kShake}, {"stopVelocity", EventOp::kStopVelocity},
    {"timeOn", EventOp::kTimeOn}, {"timeOff", EventOp::kTimeOff}, {"iframeOn", EventOp::kIFrameOn},
    {"iframeOff", EventOp::kIFrameOff}, {"armorOn", EventOp::kArmorOn}, {"armorOff", EventOp::kArmorOff},
    {"rotateOn", EventOp::kRotateOn}, {"rotateOff", EventOp::kRotateOff}, {"cancel", EventOp::kCancel},
    {"fireArrow", EventOp::kFireArrow}, {"ragdoll", EventOp::kRagdoll}, {"strike", EventOp::kStrike},
    {"sound", EventOp::kSound}, {"globalSet", EventOp::kGlobalSet}, {"modEvent", EventOp::kModEvent}};
const std::pair<std::string_view, EventTarget> kTargets[] = {{"both", EventTarget::kBoth},
    {"player", EventTarget::kPlayer}, {"initiator", EventTarget::kInitiator}, {"receiver", EventTarget::kReceiver}};
// Where a <Do> stands (0.12.281): a registered event's function holds what it acts with (op, a spell, a ragdoll's
// direction); an action's <EventValues> the numbers it runs with there; a clip-time effect both.
enum class DoUse { kRegistry, kValues, kTimed };
constexpr std::initializer_list<const char*> kNumbers = {"delay", "stamina", "magicka", "strength", "duration",
    "factor", "damage", "force", "ratio", "amount", "radius", "angle", "up", "down", "edge", "volume", "value",
    "number"};
// What a sound, a global and a mod event act with (0.12.296): a registered event's function holds them.
constexpr std::initializer_list<const char*> kOpKinds = {"file", "bone", "restore", "name", "text"};
// A strike's settings that say whom it hits and how (StrikeSpec.h): a registered event's function holds them.
constexpr std::initializer_list<const char*> kStrikeKinds = {"pick", "hit", "hand", "block", "reaction", "without"};
// What a real hit brings along, each named in a strike's `without` to leave it out (0.12.294).
constexpr Names kStrikeExtras = {"enchantment", "poison", "perks", "impact", "hitEvent"};
// A strike (0.12.293): whom it hits and how from `pick`·`hit`·`hand`·`block`, its numbers from the rest.
void strikeSpec(Node e, DoUse use, StrikeSpec& s) {
    choice(e, "pick", {"partner", "combatTarget", "crosshair", "area"});
    choice(e, "hit", {"hostile", "all"});
    choice(e, "hand", {"right", "left", "both"});
    choice(e, "block", {"vanilla", "ignore"});
    choice(e, "reaction", {"none", "stagger", "push"});
    num(e, "ratio", 100);
    num(e, "amount", 100000);
    num(e, "radius", 5000);
    num(e, "angle", 360, 1);
    num(e, "up", 5000);
    num(e, "down", 5000);
    num(e, "edge", 1);
    if (e.attribute("ratio") && e.attribute("amount"))
        fail(e, "strike에는 ratio(무기 데미지 비율)와 amount(고정 피해) 중 하나만 씁니다");
    const auto pick = get(e, "pick", "combatTarget");
    if (use != DoUse::kValues && pick != "area")
        for (const char* n : {"radius", "angle", "up", "down", "edge", "hit"})
            if (e.attribute(n)) fail(e, std::string(n) + "는 pick=\"area\"(범위)에서만 씁니다");
    // After the damage (0.12.294): a stagger's strength (0~1) or a push's force (0~20).
    const auto reaction = get(e, "reaction", "none");
    if (use != DoUse::kValues) {
        if (e.attribute("strength") && reaction != "stagger")
            fail(e, "strike의 strength는 reaction=\"stagger\"(경직)의 세기입니다");
        if (e.attribute("force") && reaction != "push")
            fail(e, "strike의 force는 reaction=\"push\"(밀쳐내기)의 힘입니다");
    }
    if (e.attribute("strength") && e.attribute("strength").as_float(0) > 1.0f)
        fail(e, "strike의 strength(경직 세기)는 0~1입니다");
    const std::string without = get(e, "without");
    for (std::size_t at = 0; at <= without.size();) {
        const auto end = std::min(without.find(',', at), without.size());
        const auto name = without.substr(at, end - at);
        if (!name.empty() && !has(kStrikeExtras, name))
            fail(e, "without에 쓸 수 있는 것은 enchantment·poison·perks·impact·hitEvent입니다: " + name);
        if (name == "enchantment") s.enchantment = false;
        if (name == "poison") s.poison = false;
        if (name == "perks") s.perks = false;
        if (name == "impact") s.impact = false;
        if (name == "hitEvent") s.hitEvent = false;
        at = end + 1;
    }
    s.reaction = reaction == "stagger" ? StrikeSpec::Reaction::kStagger
                 : reaction == "push"  ? StrikeSpec::Reaction::kPush
                                       : StrikeSpec::Reaction::kNone;
    auto value = [&](const char* n, float fallback) {
        const float v = e.attribute(n).as_float(fallback);
        return std::isfinite(v) ? v : fallback;
    };
    s.pick = pick == "partner"     ? StrikeSpec::Pick::kPartner
             : pick == "crosshair" ? StrikeSpec::Pick::kCrosshair
             : pick == "area"      ? StrikeSpec::Pick::kArea
                                   : StrikeSpec::Pick::kCombatTarget;
    s.hostileOnly = get(e, "hit", "hostile") != "all";
    const auto hand = get(e, "hand", "right");
    s.hand = hand == "left"   ? StrikeSpec::Hand::kLeft
             : hand == "both" ? StrikeSpec::Hand::kBoth
                              : StrikeSpec::Hand::kRight;
    s.block = get(e, "block", "vanilla") == "ignore" ? StrikeSpec::Block::kIgnore : StrikeSpec::Block::kVanilla;
    s.base = e.attribute("amount") ? StrikeSpec::Base::kFixed : StrikeSpec::Base::kWeapon;
    s.ratio = std::clamp(value("ratio", 1.0f), 0.0f, 100.0f);
    s.amount = std::clamp(value("amount", 10.0f), 0.0f, 100000.0f);
    s.radius = std::clamp(value("radius", 150.0f), 0.0f, 5000.0f);
    s.angle = std::clamp(value("angle", 60.0f), 1.0f, 360.0f);
    s.up = std::clamp(value("up", 150.0f), 0.0f, 5000.0f);
    s.down = std::clamp(value("down", 60.0f), 0.0f, 5000.0f);
    s.edge = std::clamp(value("edge", 1.0f), 0.0f, 1.0f);
    s.duration = std::clamp(value("duration", 0.0f), 0.0f, 30.0f);
    s.stagger = std::clamp(value("strength", 0.5f), 0.0f, 1.0f);
    s.force = std::clamp(value("force", 5.0f), 0.0f, static_cast<float>(RagdollPush::kMaxForce));
}
EventFunctionSpec eventFunction(Node e, DoUse use) {
    const bool timed = use == DoUse::kTimed;
    if (use == DoUse::kRegistry)
        for (const char* n : kNumbers)
            if (e.attribute(n))
                fail(e,
                    std::string(n) +
                        ": 0.12.281부터 수치는 이 이벤트를 보내는 동작(또는 페어)의 <EventValues>에 씁니다. 등록에는 "
                        "op와 대상(spell의 plugin·form, ragdoll의 direction)만 씁니다");
    if (use == DoUse::kValues) {
        for (const char* n : {"plugin", "form", "direction", "target", "from", "at"})
            if (e.attribute(n))
                fail(e, std::string(n) +
                            ": <EventValues>에는 수치만 씁니다(0.12.281). 마법·래그돌 방향은 이벤트 등록에서 정합니다");
        for (const char* n : kStrikeKinds)
            if (e.attribute(n))
                fail(e, std::string(n) + ": 타격의 대상·방식(pick·hit·hand·block)은 이벤트 등록에서 정합니다");
        for (const char* n : kOpKinds)
            if (e.attribute(n))
                fail(e, std::string(n) +
                            ": 소리 파일·뼈, 전역 값 되돌리기, 모드 이벤트 이름·글은 이벤트 등록에서 정합니다");
        shape(e, {"op", "delay", "stamina", "magicka", "strength", "duration", "factor", "damage", "force", "ratio",
                     "amount", "radius", "angle", "up", "down", "edge", "volume", "value", "number"});
    }
    // A registered event's function changes the actor that sent the event (0.12.273); a pair's clip-time effect
    // names the role it changes (both actors play the clip, so no role sends it).
    if (timed)
        shape(e, {"at", "op", "target", "plugin", "form", "delay", "stamina", "magicka", "strength", "duration",
                     "factor", "damage", "direction", "force", "pick", "hit", "hand", "block", "ratio", "amount",
                     "radius", "angle", "up", "down", "edge", "reaction", "without", "file", "bone", "volume", "value",
                     "restore", "name", "text", "number"});
    else if (use == DoUse::kRegistry)
        shape(e, {"op", "plugin", "form", "direction", "pick", "hit", "hand", "block", "reaction", "without", "file",
                     "bone", "restore", "name", "text"});
    const auto op = required(e, "op");
    choice(e, "op",
        {"hide", "show", "spell", "stateOn", "stateOff", "cost", "shake", "stopVelocity", "timeOn", "timeOff",
            "iframeOn", "iframeOff", "armorOn", "armorOff", "rotateOn", "rotateOff", "cancel", "fireArrow", "ragdoll",
            "strike", "sound", "globalSet", "modEvent"});
    choice(e, "target", {"both", "player", "initiator", "receiver"});
    choice(e, "bone", {"spine", "head", "rightHand", "leftHand", "root"});
    choice(e, "restore", {"end"});
    num(e, "volume", 1);
    num(e, "delay", 60);
    num(e, "stamina", 100000);
    num(e, "magicka", 100000);
    num(e, "strength", 10);
    num(e, "duration", 30);
    num(e, "factor", 5, 0.05);
    choice(e, "direction", {"down", "up", "forward", "backward", "left", "right"});
    num(e, "force", RagdollPush::kMaxForce);
    std::string form = get(e, "form");
    // An action's values for a cost or a time scale are what those functions run with: they must be given.
    if (use == DoUse::kValues && op == "cost" && !e.attribute("stamina") && !e.attribute("magicka"))
        fail(e, "cost에는 stamina 또는 magicka가 필요합니다");
    if (use == DoUse::kValues && op == "timeOn" && !e.attribute("factor")) fail(e, "timeOn에는 factor가 필요합니다");
    if (use == DoUse::kValues) {
        // The registry's spell and ragdoll direction are its own: nothing to read here.
    } else if (op == "spell" || op == "stateOn" || op == "stateOff" || op == "globalSet" ||
               (op == "sound" && !e.attribute("file"))) {
        const auto plugin = required(e, "plugin");
        required(e, "form");
        if (plugin.find_first_of("/\\:") != std::string::npos) fail(e, "plugin에는 파일 이름만 지정합니다");
        if (form.starts_with("0x") || form.starts_with("0X")) form.erase(0, 2);
        if (form.empty() || form.size() > 8 ||
            !std::all_of(form.begin(), form.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            fail(e, "form은 16진수 ID여야 합니다");
    } else if (e.attribute("plugin") || e.attribute("form"))
        fail(e, "plugin·form은 spell·stateOn·stateOff·sound·globalSet에서만 씁니다");
    // A shot fires when its event or clip time comes (0.12.303): the arrow leaves with no draw, `damage` percent of
    // a full draw's damage (1~1000, default 100).
    if (op == "fireArrow") {
        num(e, "damage", 1000, 1);
        if (e.attribute("delay")) fail(e, "fireArrow는 delay를 쓰지 않습니다(이벤트·시각에 바로 발사)");
    } else if (e.attribute("damage"))
        fail(e, "damage는 fireArrow에서만 씁니다");
    if (op != "ragdoll" && e.attribute("direction")) fail(e, "direction은 ragdoll에서만 씁니다");
    if (op != "ragdoll" && op != "strike" && e.attribute("force")) fail(e, "force는 ragdoll·strike에서만 씁니다");
    // A sound, a global and a mod event (0.12.296).
    if (op == "sound") {
        if (use != DoUse::kValues && e.attribute("file") && (e.attribute("plugin") || e.attribute("form")))
            fail(e, "sound에는 file(소리 파일) 또는 plugin·form(등록 사운드) 중 하나만 씁니다");
        if (use != DoUse::kValues && !e.attribute("file") && !e.attribute("plugin"))
            fail(e, "sound에는 file(소리 파일) 또는 plugin·form(등록 사운드)이 필요합니다");
        const std::string path = get(e, "file");
        if (path.find(':') != std::string::npos || path.starts_with("\\") || path.starts_with("/") ||
            path.find("..") != std::string::npos)
            fail(e, "sound의 file은 Data 아래 상대 경로입니다(sound\\fx\\…\\a.wav)");
    } else if (e.attribute("file") || e.attribute("bone") || e.attribute("volume"))
        fail(e, "file·bone·volume은 sound에서만 씁니다");
    if (op != "globalSet" && (e.attribute("value") || e.attribute("restore")))
        fail(e, "value·restore는 globalSet에서만 씁니다");
    if (op == "modEvent") {
        if (use != DoUse::kValues && get(e, "name").empty()) fail(e, "modEvent에는 name이 필요합니다");
    } else if (e.attribute("name") || e.attribute("text") || e.attribute("number"))
        fail(e, "name·text·number는 modEvent에서만 씁니다");
    if (op != "strike")
        for (const char* n : {"pick", "hit", "hand", "block", "ratio", "amount", "radius", "angle", "up", "down",
                 "edge", "reaction", "without"})
            if (e.attribute(n)) fail(e, std::string(n) + "는 strike(타격)에서만 씁니다");
    auto number = [](pugi::xml_attribute attr, float fallback, float maximum) {
        const float value = attr.as_float(fallback);
        return std::isfinite(value) ? std::clamp(value, 0.0f, maximum) : fallback;
    };
    EventFunctionSpec f;
    f.op = Pick(kOps, op, EventOp::kSpell);
    // A registered event's function changes its sender; a clip-time effect without a target changes both actors.
    f.target = timed ? Pick(kTargets, get(e, "target", "both"), EventTarget::kBoth) : EventTarget::kSender;
    f.delay = number(e.attribute("delay"), 0, 60);
    f.plugin = get(e, "plugin");
    f.form = form;
    if (op == "ragdoll") {
        const auto direction = get(e, "direction", "down");
        for (std::size_t i = 0; i < RagdollPush::kNames.size(); ++i)
            if (RagdollPush::kNames[i] == direction) f.direction = static_cast<RagdollPush::Direction>(i);
        if (e.attribute("force")) f.force = number(e.attribute("force"), 1, RagdollPush::kMaxForce);
    }
    if (op == "fireArrow") f.arrowDamage = e.attribute("damage").as_float(100.0f) / 100.0f;
    if (op == "strike") strikeSpec(e, use, f.strike);
    if (op == "sound") {
        f.file = get(e, "file");
        f.bone = get(e, "bone", "spine");
        f.volume = number(e.attribute("volume"), 1.0f, 1.0f);
    }
    if (op == "globalSet") {
        const float value = e.attribute("value").as_float(0.0f);
        f.value = std::isfinite(value) ? value : 0.0f;
        f.restore = get(e, "restore") == "end";
    }
    if (op == "modEvent") {
        f.name = get(e, "name");
        f.text = get(e, "text");
        const float n = e.attribute("number").as_float(0.0f);
        f.number = std::isfinite(n) ? n : 0.0f;
    }
    f.stamina = number(e.attribute("stamina"), 0, 100000);
    f.magicka = number(e.attribute("magicka"), 0, 100000);
    f.strength = number(e.attribute("strength"), 1, 10);
    f.duration = number(e.attribute("duration"), 0.5f, 30);
    f.factor = std::max(0.05f, number(e.attribute("factor"), 1, 5));
    return f;
}
RecoverySpec recoveryDeclaration(Node r) {
    shape(r, {"at", "blend"});
    num(r, "blend", 5);
    RecoverySpec out;
    out.enabled = true;
    ReadMoment(time(r, "at", Basis::Seconds), out.event, out.at);
    out.move = true;  // movement input ends the action; sprint and jump do not
    out.sprint = false;
    out.jump = false;
    out.blend = r.attribute("blend").as_float(-1.0f);
    return out;
}
// A connection read once the pair's own settings are known (partnerTo needs a pair).
struct LinkDeclaration {
    std::string to, partnerTo, event;
    std::uint32_t key = 0, gamepad = 0;        // the key transition's key (an <Inputs> name resolved, or written here)
    GameControl control = GameControl::kNone;  // or a game control (0.12.292)
    bool partner = false;
    pugi::xml_attribute blend;
    std::string mode, open, close;
    bool timed = false;
};
// The window of a vanilla transition or a Transition: buffered/window/always and its open and close times. No
// from, until or buffer = always (the whole clip).
struct WindowTimes {
    std::string mode, open, close;
    bool timed = false;
};
struct InputInfo {
    Node node;
    std::uint32_t key = 0;                     // DX scan code (+256 mouse)
    std::uint32_t gamepad = 0;                 // the engine's gamepad button code
    GameControl control = GameControl::kNone;  // a game control instead of a key and button (0.12.292)
    bool hasKey = false;
};
// Where a key is given: an <Inputs> name (input="…") or the key itself (key="G" gamepad="A").
struct KeyRef {
    std::string input;  // the <Inputs> name, if one was used
    std::uint32_t key = 0, gamepad = 0;
    GameControl control = GameControl::kNone;
    bool given = false;
};
// Gamepad buttons by name: the engine's codes for the device (the d-pad and buttons are XInput masks, the
// triggers 9 and 10). The editor's button list uses the same names.
constexpr std::pair<const char*, std::uint32_t> kGamepadButtons[] = {{"Up", 0x1}, {"Down", 0x2}, {"Left", 0x4},
    {"Right", 0x8}, {"Start", 0x10}, {"Back", 0x20}, {"LS", 0x40}, {"RS", 0x80}, {"LB", 0x100}, {"RB", 0x200},
    {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LT", 9}, {"RT", 10}};
struct Compiler {
    KeyResolver resolve;
    std::map<std::string, std::string> actions;
    std::set<std::string> singleActions;
    std::set<std::string> putdownActions;  // Actions a Carry names as its putdown
    View fileView = View::kThird;          // <AnimationSet view> (runtime/ViewSpec.h)
    std::map<std::string, InputInfo> inputs;
    std::set<std::string> actionIds, inputIds;
    std::vector<std::string> declared;  // every Action/Pair id in document order
    std::vector<bool> pairs;            // declared[i] is a Pair
    Cards::Shared shared;               // the root's <Condition id> cards
    std::set<std::string> taken;        // action and start ids (lower case), for the selecting actions' names
    // Selecting actions a transition or a QTE result goes through (T5), after the document's actions.
    struct Trailing {
        std::string id;
        Cards::Compiled compiled;
    };
    std::vector<Trailing> trailing;
    std::map<std::string, std::string> made;         // their ids -> "pair" or "action"
    std::map<std::string, std::string> byCondition;  // a shared card -> the selecting action holding it alone
    // An event QTE result's action (0.12.249): the registry is read before the actions, so it is checked after them.
    std::vector<std::pair<Node, std::string>> eventQteTargets;
    // A key the runtime will use: a table name or code:<1..265>. The resolver reports
    // unknown names as 0 or as a value above 265 (UINT32_MAX in the DLL); both fail here,
    // so a typo cannot become a key code that no input event ever matches.
    std::uint32_t keyCode(Node n, const std::string& value) {
        if (value.starts_with("code:")) {
            const auto v = numeric(n, value.substr(5), 265, 1);
            if (std::floor(v) != v) fail(n, "키 코드는 정수여야 합니다");
            return static_cast<std::uint32_t>(v);
        }
        if (value.empty() || value == "None" ||
            !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isalnum(ch) != 0; }) ||
            (value.size() > 1 && std::isdigit(static_cast<unsigned char>(value[0]))) || !resolve)
            fail(n, "알 수 없는 키 이름: " + value);
        const auto code = resolve(value.c_str());
        if (code < 1 || code > 265) fail(n, "알 수 없는 키 이름: " + value);
        return code;
    }
    // Empty and None mean "no keyboard or mouse key" where a gamepad button may be used instead.
    std::uint32_t optionalKeyCode(Node n, const std::string& value) {
        return value.empty() || value == "None" ? 0 : keyCode(n, value);
    }
    // A gamepad button: a name (kGamepadButtons) or code:<1..65535>; 0 when the attribute is absent.
    std::uint32_t gamepadCode(Node n, const char* attr = "gamepad") {
        if (!n.attribute(attr)) return 0;
        const auto value = get(n, attr);
        for (const auto& [name, code] : kGamepadButtons)
            if (_stricmp(value.c_str(), name) == 0) return code;
        if (value.starts_with("code:")) {
            const auto v = numeric(n, value.substr(5), 65535, 1);
            if (std::floor(v) != v) fail(n, "패드 버튼 코드는 정수여야 합니다");
            return static_cast<std::uint32_t>(v);
        }
        fail(n, "알 수 없는 패드 버튼: " + value +
                    " (A B X Y LB RB LT RT LS RS Start Back Up Down Left Right 또는 code:숫자)");
    }
    // A game control by its RE::UserEvents name (0.12.292): every device's binding at once.
    GameControl gameControl(Node n) {
        const auto value = get(n, "control");
        const auto control = ParseGameControl(value);
        if (control == GameControl::kNone) {
            std::string names;
            for (int i = 1; i < kGameControlCount; ++i) (names += ' ') += kGameControlNames[i];
            fail(n, "알 수 없는 게임 동작: " + value + " (" + names.substr(1) + ")");
        }
        return control;
    }
    // input="<Inputs name>", key=/gamepad= or control= (a game control) written here; only one of them.
    KeyRef keyRef(Node n) {
        KeyRef r;
        const bool named = n.attribute("input"), direct = n.attribute("key") || n.attribute("gamepad"),
                   control = n.attribute("control");
        if (named + direct + control > 1) fail(n, "input, key·gamepad, control 중 하나만 씁니다");
        if (named) {
            inputRef(n);
            r.input = get(n, "input");
            const auto& in = inputs.at(r.input);
            r.key = in.hasKey ? in.key : 0;
            r.gamepad = in.gamepad;
            r.control = in.control;
        } else if (direct) {
            r.key = optionalKeyCode(n, get(n, "key"));
            r.gamepad = gamepadCode(n);
            if (!r.key && !r.gamepad) fail(n, "키 또는 패드 버튼이 필요합니다");
        } else if (control)
            r.control = gameControl(n);
        r.given = named || direct || control;
        return r;
    }
    void inputRef(Node n, const char* attr = "input") {
        if (n.attribute(attr) && !inputs.contains(get(n, attr))) fail(n, "정의되지 않은 입력: " + get(n, attr));
    }
    void actionRef(Node n, const char* attr) {
        if (!actions.contains(required(n, attr))) fail(n, "정의되지 않은 액션: " + get(n, attr));
    }
    void input(Node n) {
        shape(n, {"id", "key", "gamepad", "control"});
        auto id = required(n, "id");
        identifier(n, id);
        if (!inputIds.insert(lower(id)).second) fail(n, "입력 ID 중복");
        InputInfo info{n};
        if (n.attribute("control")) {
            if (n.attribute("key") || n.attribute("gamepad")) fail(n, "control은 key·gamepad와 함께 쓰지 않습니다");
            info.control = gameControl(n);
            inputs.emplace(id, info);
            return;
        }
        if (auto key = n.attribute("key")) {
            info.key = keyCode(n, key.value());
            info.hasKey = true;
        }
        info.gamepad = gamepadCode(n);
        if (!n.attribute("key") && !info.gamepad) fail(n, "키, 패드 버튼 또는 control이 필요합니다");
        inputs.emplace(id, info);
    }
    // One <QTE>: an action's (links: its transitions are collected there) or an event's (no links: started by
    // its event, 0.12.248).
    QteSpec qte(Node q, Node owner, bool pair, std::set<std::string>& qteIds, std::vector<LinkDeclaration>* links) {
        if (!links) {
            // Started by its event (0.12.248): at once, for the player, with no action to go to.
            shape(q,
                {"id", "key", "gamepad", "speed", "successWindow", "perfectWindow", "scale", "x", "y", "anchor",
                    "delay", "reducedMotion", "hidden", "type", "pressCount", "perfectBefore", "control", "stopEvent",
                    "feedback", "channel", "design"},
                {"Success", "Failure", "Perfect", "Input"});
        } else
            shape(q,
                {"id", "at", "key", "gamepad", "inputOwner", "eventSource", "speed", "successWindow", "perfectWindow",
                    "scale", "x", "y", "anchor", "delay", "reducedMotion", "hidden", "type", "pressCount",
                    "perfectBefore", "control", "stopEvent", "replaceActive", "feedback", "group", "channel", "design"},
                {"Success", "Failure", "Perfect", "Input"});
        if (get(q, "group").size() > 128) fail(q, "QTE group name is too long");
        // Channels run together (runtime Qte::kChannels = 4).
        num(q, "channel", 3);
        if (q.attribute("channel") && q.attribute("channel").as_double() != q.attribute("channel").as_uint())
            fail(q, "channel must be an integer");
        if (q.attribute("control")) gameControl(q);
        boolean(q, "replaceActive");
        num(q, "feedback", 10, .05);
        if (q.attribute("stopEvent")) {
            const auto stop = required(q, "stopEvent");
            eventName(q, stop);
            if (std::any_of(stop.begin(), stop.end(), [](unsigned char c) { return c <= 32 || c == 60 || c == 62; }))
                fail(q, "invalid stopEvent");
        }
        if (q.attribute("control") && get(q, "type") == "keySequence")
            fail(q, "control input does not support keySequence");
        choice(q, "anchor", {"screen", "center", "topLeft", "topRight", "bottomLeft", "bottomRight"});
        choice(q, "type", {"splitArc", "timingBar", "rotatingNeedle", "holdRelease", "rapidPress", "keySequence"});
        // A splitArc's look (0.12.246): the shrinking circle or the converging lines.
        choice(q, "design", {"circle", "converge"});
        if (q.attribute("design") && get(q, "type", "splitArc") != "splitArc")
            fail(q, "design is for type=\"splitArc\"");
        num(q, "pressCount", 100, 1);
        num(q, "perfectBefore", 1);
        if (q.attribute("pressCount") && q.attribute("pressCount").as_double() != q.attribute("pressCount").as_uint())
            fail(q, "pressCount must be an integer");
        auto id = required(q, "id");
        identifier(q, id);
        if (!qteIds.insert(lower(id)).second) fail(q, "duplicate QTE id");
        num(q, "speed", 10, .1);
        num(q, "successWindow", .5, .001);
        num(q, "perfectWindow", .5);
        if (q.attribute("perfectWindow").as_float(.026f) > q.attribute("successWindow").as_float(.085f))
            fail(q, "perfectWindow exceeds successWindow");
        const double minPosition = get(q, "anchor", "screen") == "screen" ? 0 : -1;
        num(q, "scale", 4, .25);
        num(q, "x", 1, minPosition);
        num(q, "y", 1, minPosition);
        num(q, "delay", 10);
        boolean(q, "reducedMotion");
        boolean(q, "hidden");
        choice(q, "inputOwner", {"player", "self", "partner"});
        choice(q, "eventSource", {"self", "partner", "both"});
        if (!pair &&
            (get(q, "inputOwner") == "partner" || get(q, "eventSource") != "" && get(q, "eventSource") != "self"))
            fail(q, "partner QTE requires Pair");
        QteSpec qte;
        if (links)
            ReadMoment(time(q, "at", Basis::Seconds), qte.event, qte.at);
        else
            qte.at = 0;
        // control and keySequence replace the single key, so it is neither read nor validated
        // there (the Grapple migration writes key="LMB" beside control="rightAttack").
        const bool keyUsed = get(q, "control").empty() && get(q, "type") != "keySequence";
        // E is the default only when the attribute is absent; key="" means gamepad only.
        const std::string keyName = q.attribute("key") ? q.attribute("key").value() : "E";
        const auto code = keyUsed ? optionalKeyCode(q, keyName) : 0u;
        const auto pad = gamepadCode(q);
        if (keyUsed && !code && !pad) fail(q, "QTE requires a valid key or gamepad button");
        qte.key = code;
        unsigned inputCount = 0;
        bool allKeys = true, allPads = true;
        for (auto input : q.children("Input")) {
            shape(input, {"key", "gamepad"});
            const auto inputCode = optionalKeyCode(input, get(input, "key"));
            const auto inputPad = gamepadCode(input);
            allKeys &= inputCode != 0;
            allPads &= inputPad != 0;
            if (!inputCode && !inputPad) fail(input, "Input requires a valid key or gamepad button");
            if (qte.sequenceCount < qte.sequence.size()) qte.sequence[qte.sequenceCount++] = {inputCode, inputPad};
            ++inputCount;
        }
        if (get(q, "type") == "keySequence") {
            if (inputCount < 2 || inputCount > 8 || (!allKeys && !allPads))
                fail(q, "keySequence requires 2-8 Inputs and a complete keyboard/mouse or gamepad sequence");
        } else if (inputCount)
            fail(q, "Input children require keySequence");
        int result = 0;
        for (const char* name : {"Success", "Failure", "Perfect"}) {
            auto& outcome = qte.results[result++];
            auto r = one(q, name);
            outcome.recipient = r.attribute("recipient").as_string("self");
            outcome.soundPlugin = r.attribute("soundPlugin").as_string();
            outcome.soundForm = r.attribute("soundForm").as_string();
            outcome.event = r.attribute("event").as_string();
            if (!r) continue;
            if (links)
                shape(
                    r, {"event", "recipient", "to", "partnerTo", "condition", "soundPlugin", "soundForm"}, kCardParts);
            else
                shape(r, {"event", "to", "partnerTo", "condition", "soundPlugin", "soundForm"}, kCardParts);
            if (r.attribute("soundPlugin") || r.attribute("soundForm")) {
                required(r, "soundPlugin");
                const auto form = required(r, "soundForm");
                auto digits = std::string_view(form);
                if (digits.starts_with("0x") || digits.starts_with("0X")) digits.remove_prefix(2);
                if (digits.empty() || digits.size() > 8 ||
                    digits.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
                    fail(r, "invalid soundForm");
            }
            choice(r, "recipient", {"self", "partner", "both"});
            if (!links && Cards::HasCard(r)) {
                // An event QTE's result through a condition card (T5): the action it picks starts at once.
                if (r.attribute("event")) fail(r, "choose event OR to");
                if (r.attribute("partnerTo")) fail(r, "partnerTo는 to(동작)와 함께만 씁니다");
                outcome.to = through(r, "QTE_" + id + "_" + name);
                eventQteTargets.emplace_back(r, outcome.to);
                continue;
            }
            if (links && Cards::HasCard(r)) {
                // Through a condition card (T5): the result's event starts the selecting action that holds it.
                if (r.attribute("event")) fail(r, "choose event OR to");
                if (r.attribute("recipient")) fail(r, "transition recipient is fixed to the initiator");
                if (r.attribute("partnerTo")) fail(r, "partnerTo는 to(동작)와 함께만 씁니다");
                const auto event = "AI_" + get(owner, "id") + "_QTE_" + id + "_" + name;
                LinkDeclaration link;
                link.event = event;
                link.to = through(r, get(owner, "id") + "_" + id + "_" + name);
                links->push_back(std::move(link));
                outcome.event = event;
                continue;
            }
            if (r.attribute("to") && r.attribute("event")) fail(r, "choose event OR to");
            if (!pair && get(r, "recipient") != "" && get(r, "recipient") != "self")
                fail(r, "partner result requires Pair");
            if (r.attribute("event")) eventName(r, required(r, "event"));
            if (r.attribute("partnerTo") && !r.attribute("to")) fail(r, "partnerTo requires to");
            if (r.attribute("to") && !links) {
                // An event's QTE starts the action itself: at once, or once the engine hands a body getting up
                // back (runtime/GetUpHandoff).
                outcome.to = required(r, "to");
                eventQteTargets.emplace_back(r, outcome.to);
                // Asked during a pair (0.12.264), the partner goes to its own ordinary action.
                if (r.attribute("partnerTo")) {
                    outcome.partnerTo = required(r, "partnerTo");
                    eventQteTargets.emplace_back(r, outcome.partnerTo);
                }
            } else if (r.attribute("to")) {
                actionRef(r, "to");
                if (r.attribute("recipient")) fail(r, "transition recipient is fixed to the initiator");
                if (r.attribute("partnerTo")) {
                    actionRef(r, "partnerTo");
                    if (!pair || actions.at(get(r, "to")) == "pair" || actions.at(get(r, "partnerTo")) == "pair")
                        fail(r, "partnerTo requires pair to two ordinary actions");
                }
                // The result sends this event; a connection on it starts the destination.
                const auto event = "AI_" + get(owner, "id") + "_QTE_" + id + "_" + name;
                LinkDeclaration link;
                link.event = event;
                link.to = get(r, "to");
                link.partner = static_cast<bool>(r.attribute("partnerTo"));
                link.partnerTo = get(r, "partnerTo");
                links->push_back(std::move(link));
                outcome.event = event;
            }
        }
        qte.type = ParseQteType(get(q, "type", "splitArc"));
        qte.pressCount = q.attribute("pressCount").as_uint(10);
        qte.perfectBefore = q.attribute("perfectBefore").as_float(.5f);
        qte.id = id;
        qte.gamepad = pad;
        qte.eventSource = get(q, "eventSource", "self");
        // Unset, a pair QTE started by one actor's event is that actor's (0.12.263): the role whose clip sends the
        // event is the role that sees the prompt. A timed QTE or one heard from both stays the player's in any role.
        qte.inputOwner = get(q, "inputOwner",
            pair && !qte.event.empty() && qte.eventSource != "both" ? qte.eventSource.c_str() : "player");
        qte.speed = q.attribute("speed").as_float(1);
        qte.successWindow = q.attribute("successWindow").as_float(.085f);
        qte.perfectWindow = q.attribute("perfectWindow").as_float(.026f);
        qte.scale = q.attribute("scale").as_float(1);
        qte.anchor = ParseQteAnchor(get(q, "anchor", "screen"));
        const float origin = qte.anchor == QteAnchor::Screen ? .5f : 0;
        qte.x = q.attribute("x").as_float(origin);
        qte.y = q.attribute("y").as_float(origin);
        qte.delay = q.attribute("delay").as_float(0);
        qte.reducedMotion = q.attribute("reducedMotion").as_bool(false);
        qte.hidden = q.attribute("hidden").as_bool(false);
        qte.control = get(q, "control");
        qte.stopEvent = get(q, "stopEvent");
        qte.replaceActive = q.attribute("replaceActive").as_bool();
        qte.feedback = q.attribute("feedback").as_float(1.35f);
        qte.channel = static_cast<std::uint8_t>(std::min(3u, q.attribute("channel").as_uint(0)));
        qte.design = get(q, "design", "circle") == "converge" ? QteDesign::Converge : QteDesign::Circle;
        return qte;
    }
    // <EventRegistry>: every event name of the file, with its functions (none: the name is only registered
    // in the graphs, for an action's window, QTE or trigger).
    void registry(Node r, const std::string& sourceName, std::vector<EventRuleSpec>& out) {
        shape(r, {}, {"Event"});
        std::set<std::string> names;
        for (auto e : r.children("Event")) {
            shape(e, {"name"}, {"Do", "QTE"});
            const auto name = required(e, "name");
            eventName(e, name);
            if (!names.insert(lower(name)).second) fail(e, "같은 이벤트를 두 번 등록했습니다: " + name);
            EventRuleSpec rule;
            rule.name = name;
            rule.source = sourceName;
            for (auto d : e.children("Do")) rule.functions.push_back(eventFunction(d, DoUse::kRegistry));
            if (rule.functions.size() > 64) fail(e, "이벤트 하나에 기능은 64개까지입니다");
            std::set<std::string> qteIds;
            for (auto q : e.children("QTE")) rule.qtes.push_back(qte(q, {}, false, qteIds, nullptr));
            if (rule.qtes.size() > 4) fail(e, "이벤트 하나에 QTE는 4개까지입니다");
            out.push_back(std::move(rule));
        }
        if (out.size() > 4096) fail(r, "이벤트 등록은 파일당 4096개까지입니다");
    }
    // <EventValues> (0.12.281): the numbers this action's or pair's clip runs registered functions with. Which
    // registry function each one gives numbers to is matched once every file is read (EventValuesMatch.h).
    std::vector<EventValuesSpec> eventValues(Node n) {
        shape(n, {}, {"Event"});
        std::vector<EventValuesSpec> out;
        std::set<std::string> names;
        for (auto e : n.children("Event")) {
            shape(e, {"name"}, {"Do"});
            const auto name = required(e, "name");
            eventName(e, name);
            if (!names.insert(lower(name)).second) fail(e, "같은 이벤트의 값을 두 번 썼습니다: " + name);
            EventValuesSpec values;
            values.event = name;
            for (auto d : e.children("Do")) values.functions.push_back(eventFunction(d, DoUse::kValues));
            if (values.functions.empty()) fail(e, "값이 없는 이벤트입니다: <Do op=\"…\" …/>를 쓰세요");
            if (values.functions.size() > 64) fail(e, "이벤트 하나에 기능은 64개까지입니다");
            out.push_back(std::move(values));
        }
        return out;
    }
    // from, until and buffer on a <Transition> or <Vanilla> (its shape is checked by the caller).
    WindowTimes window(Node n, bool link) {
        boolean(n, "buffer");
        // from: the clip start unless written; until: the clip end unless written.
        WindowTimes out;
        out.mode = get(n, "buffer", "true") == "true" ? "buffered" : "window";
        out.open = time(n, "from", Basis::Seconds, "0");
        out.close = time(n, "until", link ? Basis::Either : Basis::End, "end");
        out.timed = true;
        auto a = get(n, "from", "0"), b = get(n, "until", "end");
        if (link && !a.starts_with("event:") && !b.starts_with("event:") && !b.starts_with("end") &&
            numeric(n, a) >= numeric(n, b))
            fail(n, "열림은 닫힘보다 빨라야 합니다");
        return out;
    }
    // Clip-time functions (<Do at>) and the camera restore (<Camera>), written directly in the action; a pair's
    // interruptOnHit.
    void features(Node n, PairFeatureSpec& out, bool pair) {
        boolean(n, "interruptOnHit");
        const auto c = one(n, "Camera");
        if (!c && !n.child("Do") && !n.attribute("interruptOnHit")) return;
        out.enabled = true;
        if (c) {
            shape(c, {"reset", "restore", "timeout"});
            choice(c, "restore", {"end", "event"});
            num(c, "timeout", 60, 0.1);
            if (c.attribute("reset")) eventName(c, required(c, "reset"));
            out.cameraReset = get(c, "reset");
            out.cameraRestoreOnEnd = get(c, "restore", "end") != "event";
            const float timeout = c.attribute("timeout").as_float(10.0f);
            out.cameraTimeout = std::isfinite(timeout) ? std::clamp(timeout, 0.1f, 60.0f) : 10.0f;
        }
        out.interruptOnThirdPartyHit = n.attribute("interruptOnHit").as_bool(false);
        for (auto d : n.children("Do")) {
            if (!pair) {
                // An ordinary action's clip-time function acts on the actor playing it (0.12.274).
                if (d.attribute("target") || d.attribute("from"))
                    fail(d,
                        "일반 동작의 시각 기능은 그 동작을 재생하는 액터에게 적용됩니다: target·from을 쓰지 않습니다");
                const auto op = get(d, "op");
                if (!op.empty() &&
                    !has({"hide", "show", "spell", "stateOn", "stateOff", "shake", "stopVelocity", "timeOn", "timeOff",
                             "ragdoll", "strike", "sound", "globalSet", "modEvent", "fireArrow"},
                        op))
                    fail(d, "일반 동작의 시각 기능이 아닙니다: " + op +
                                " (무적·슈퍼아머·회전·비용은 IFrame·HyperArmor·Rotate·Cost, cancel은 페어 전용)");
            } else if (get(d, "op") == "fireArrow")
                fail(d, "활 발사는 플레이어가 재생하는 일반 동작(Action)에서만 씁니다");
            TimedEffectSpec timed;
            timed.at = static_cast<float>(numeric(d, required(d, "at"), 600));
            timed.function = eventFunction(d, DoUse::kTimed);
            timed.at = d.attribute("at").as_float(timed.at);
            out.timed.push_back(std::move(timed));
        }
    }
    // One <Action>/<Pair> (format 6): its clip and features. The start that is its own (T1) is applied by the
    // caller, which then Finalizes it.
    ActionSpec action(Node n, const std::string& sourceName) {
        const bool pair = std::string_view(n.name()) == "Pair";
        shape(n,
            {"id", "clip", "view", "firstPerson", "air", "attack", "enter", "block", "safety", "lostBlend", "reorient",
                "interruptOnHit"},
            {"Exit", "Rotate", "IFrame", "HyperArmor", "Cost", "MoveCancel", "Vanilla", "Transition", "FindPartner",
                "Carry", "QTE", "CarryRelease", "EventValues", "Do", "Camera"});
        if (!pair) {
            for (const char* only : {"lostBlend", "reorient", "interruptOnHit"})
                if (n.attribute(only)) fail(n, std::string(only) + "는 페어 전용 속성입니다");
            if (n.child("FindPartner") || n.child("Carry")) fail(n, "액션 종류에 맞지 않는 요소입니다");
        }
        // Only the putdown Action a Carry names; the Carry checks that it has this slot.
        if (auto r = one(n, "CarryRelease"); r && !putdownActions.contains(get(n, "id")))
            fail(r, "CarryRelease는 운반(Carry)의 putdown이 가리키는 Action에만 씁니다");
        if (pair &&
            (n.child("Vanilla") || n.child("Rotate") || n.child("IFrame") || n.child("HyperArmor") || n.child("Cost")))
            fail(n, "페어 역할 기능은 <Do at>으로 선언해야 합니다");
        ActionSpec s;
        s.id = get(n, "id");
        s.source = sourceName;
        // The camera view (runtime/ViewSpec.h): the file's unless written here. Pairs, carries and a carry's
        // putdown play in third person whatever the file says.
        const bool thirdOnly = pair || putdownActions.contains(get(n, "id"));
        s.view.view = thirdOnly ? View::kThird : fileView;
        if (n.attribute("view")) {
            const auto view = ParseView(get(n, "view"));
            if (!view) fail(n, "view는 third, first, both 중 하나입니다");
            if (thirdOnly && *view != View::kThird)
                fail(n, "페어·운반(내려놓기 포함)은 3인칭(view=\"third\")으로만 재생됩니다");
            s.view.view = *view;
        }
        if (n.attribute("firstPerson")) {
            const auto start = ParseFirstPersonStart(get(n, "firstPerson"));
            if (!start) fail(n, "firstPerson은 block 또는 switch입니다");
            if (s.view.view != View::kThird) fail(n, "firstPerson은 3인칭 액션(view=\"third\")에만 씁니다");
            s.view.firstPerson = *start;
        }
        {
            auto f = required(n, "clip");
            clip(n, f);
            ClipVariant v;
            v.file = f;
            s.clips.push_back(std::move(v));
        }
        // Every root input is declared on every action, without changing its ID.
        for (const auto& [id, info] : inputs)
            s.inputs.push_back({id, info.hasKey ? info.key : 0, info.gamepad, info.control});
        s.trigger.key = 0;  // no key of its own: a start gives it one (T1)
        // The HKX's own nature, on every way in: whether it plays in the air.
        choice(n, "air", {"both", "only"});
        s.condition.inAir = get(n, "air") == "both" ? 1 : get(n, "air") == "only" ? 2 : 0;
        // While it plays, these vanilla events are blocked; a start's block while it can start is the start's.
        if (n.attribute("block")) {
            s.suppress.events = eventList(n, "block");
            s.suppress.when = SuppressSpec::When::kActive;
        }
        if (n.attribute("attack")) {
            auto data = required(n, "attack");
            eventName(n, data);
            s.attack.enabled = true;
            s.attack.data = data;
        }
        num(n, "enter", 5);
        s.enterBlend = n.attribute("enter").as_float(-1.0f);
        {
            // The Exit trigger: seconds before the clip's end, or an annotation event.
            std::string at = "@end-0.05", event;
            if (auto e = one(n, "Exit")) {
                shape(e, {"at", "blend"});
                num(e, "blend", 5);
                auto t = time(e, "at", Basis::End, "end-0.05");
                if (t.starts_with("@"))
                    at = t;
                else {
                    at.clear();
                    event = t;
                }
                s.exitBlend = e.attribute("blend").as_float(-1.0f);
            }
            float t;
            bool relative;
            if (ParseAt(at.c_str(), t, relative)) s.exitTriggerBeforeEnd = relative ? t : -1.0f;
            if (!event.empty()) {
                s.events.exit = event;
                s.exitTriggerBeforeEnd = -1.0f;
            }
        }
        for (auto name : {"Rotate", "IFrame", "HyperArmor"})
            if (auto slot = one(n, name)) {
                auto& window = std::string_view(name) == "Rotate"   ? s.rotate
                               : std::string_view(name) == "IFrame" ? s.iframe
                                                                    : s.hyperArmor;
                if (std::string_view(name) == "Rotate" && slot.attribute("locked")) {
                    shape(slot, {"locked"});
                    boolean(slot, "locked");
                    s.rotationLocked = slot.attribute("locked").as_bool(false);
                    // locked="false" still makes rotation a slot for the whole clip
                    if (!s.rotationLocked) ReadSlot(window, "@0", "@end");
                    continue;
                }
                shape(slot, {"from", "until"});
                const auto on = seconds(slot, "from", Basis::Seconds, "0");
                const auto off = seconds(slot, "until", Basis::Either, "end");
                ReadSlot(window, on, off);
            }
        std::vector<LinkDeclaration> links;  // QTE results first, then transitions, in document order
        std::set<std::string> qteIds;
        for (auto q : n.children("QTE")) s.qtes.push_back(qte(q, n, pair, qteIds, &links));
        if (auto c = one(n, "Cost")) {
            shape(c, {"at", "stamina", "magicka"});
            num(c, "stamina", 100000);
            num(c, "magicka", 100000);
            s.cost.enabled = true;
            ReadMoment(seconds(c, "at", Basis::Seconds, "0"), s.cost.event, s.cost.at);
            s.cost.stamina = c.attribute("stamina").as_float(0.0f);
            s.cost.magicka = c.attribute("magicka").as_float(0.0f);
        }
        if (auto r = one(n, "CarryRelease")) {
            shape(r, {"at"});
            s.carryRelease.enabled = true;
            ReadMoment(time(r, "at", Basis::Seconds), s.carryRelease.event, s.carryRelease.at);
        }
        if (auto r = one(n, "MoveCancel")) s.recovery = recoveryDeclaration(r);
        if (auto v = one(n, "EventValues")) s.eventValues = eventValues(v);
        if (pair) s.safetyDuration = 30.0f;  // a pair's clip is long; a written value replaces it
        if (n.attribute("safety")) {
            num(n, "safety", 600, 0.001);
            s.safetyDuration = n.attribute("safety").as_float(s.safetyDuration);
        }
        // Clip-time functions and the camera restore (0.12.260 any action, 0.12.274 an ordinary action's on the
        // actor playing it); third-party interruption stays a pair's.
        features(n, s.pairFeatures, pair);
        if (pair) {
            auto& paired = s.paired;
            paired.enabled = true;
            if (auto t = one(n, "FindPartner")) {
                shape(t, {"range", "angle", "rig"});
                num(t, "range", 10000, 0.001);
                num(t, "angle", 360, 0.001);
                if (t.attribute("rig") && PairedRig::FromName(get(t, "rig")) == PairedRig::Kind::kUnsupported)
                    fail(t, "알 수 없는 대상 스켈레톤: " + get(t, "rig"));
                paired.targetRig = PairedRig::FromName(get(t, "rig", "character"));
                paired.range = t.attribute("range").as_float(150.0f);
                paired.angle = t.attribute("angle").as_float(60.0f);
            }
            if (n.attribute("lostBlend")) {
                numeric(n, get(n, "lostBlend"), 5, 0.01);
                const float lossBlend = n.attribute("lostBlend").as_float(-1.0f);
                paired.lossBlend =
                    std::isfinite(lossBlend) && lossBlend > 0.0f ? std::clamp(lossBlend, 0.01f, 5.0f) : -1.0f;
            }
            // A pair played from behind turns neither actor to face the other (the editor sets it from the HKX).
            boolean(n, "reorient");
            s.paired.reorientSupport = n.attribute("reorient").as_bool(true);
            if (auto c = one(n, "Carry")) carry(c, n, s);
        }
        std::set<std::string> windows, bindings;
        unsigned index = 0;
        for (auto t : n.children()) {
            const std::string kind = t.name();
            if (kind != "Vanilla" && kind != "Transition") continue;
            const bool link = kind == "Transition";
            shape(t,
                link ? Names{"to", "condition", "partnerTo", "input", "key", "gamepad", "control", "event", "blend",
                           "from", "until", "buffer"}
                     : Names{"id", "to", "blend", "from", "until", "buffer"},
                link ? kCardParts : Names{});
            LinkDeclaration connect;
            CancelSpec ws;
            std::string targets;
            KeyRef key;
            if (link) {
                if (Cards::HasCard(t)) {
                    // Through a condition card (T5): to the selecting action that holds it.
                    if (t.attribute("blend"))
                        fail(t, "조건 카드를 거치는 전환에는 blend를 쓸 수 없습니다(런타임 미지원)");
                    if (t.attribute("partnerTo")) fail(t, "partnerTo는 to(동작)와 함께만 씁니다");
                    connect.to = through(t, get(n, "id") + "_Transition");
                } else {
                    actionRef(t, "to");
                    connect.to = get(t, "to");
                    num(t, "blend", 5);
                    if (t.attribute("blend") &&
                        (pair || get(n, "id") == get(t, "to") || !singleActions.contains(get(t, "to"))))
                        fail(t, "Transition blend는 일반 단일 HKX 대상에만 지원합니다");
                    if (t.attribute("partnerTo")) {
                        actionRef(t, "partnerTo");
                        if (!pair || actions.at(get(t, "to")) == "pair" || actions.at(get(t, "partnerTo")) == "pair")
                            fail(t, "partnerTo에는 페어에서 양쪽 일반 액션으로 가는 연결만 지정합니다");
                        connect.partner = true;
                        connect.partnerTo = get(t, "partnerTo");
                    }
                }
                connect.blend = t.attribute("blend");
                key = keyRef(t);
                if (key.given == static_cast<bool>(t.attribute("event")))
                    fail(t, "Transition에는 키(input, key·gamepad 또는 control)와 event 중 하나가 필요합니다");
                if (t.attribute("event")) {
                    eventName(t, required(t, "event"));
                    connect.event = get(t, "event");
                }
            } else {
                // A vanilla transition's id names its window events; cancel1, cancel2, … unless written.
                auto id = get(t, "id", ("cancel" + std::to_string(++index)).c_str());
                identifier(t, id);
                if (!windows.insert(lower(id)).second) fail(t, "바닐라 전환 ID 중복");
                ws.id = id;
                num(t, "blend", 5);
                ws.blend = t.attribute("blend").as_float(-1.0f);
                for (auto& name : list(required(t, "to"))) {
                    if (!has({"Attack", "PowerAttack", "Block", "Bash", "Sprint", "Jump", "Sneak"}, name) &&
                        !name.starts_with("event:"))
                        fail(t, "지원하지 않는 바닐라 동작: " + name +
                                    " (Attack PowerAttack Block Bash Sprint Jump Sneak 또는 event:검색어)");
                    if (!targets.empty()) targets += ",";
                    targets += name.starts_with("event:") ? name : "vanilla:" + name;
                }
            }
            // No from, until or buffer: the whole clip (a key transition or a vanilla transition).
            WindowTimes times;
            if (t.attribute("from") || t.attribute("until") || t.attribute("buffer"))
                times = window(t, link);
            else if (!link)
                times.mode = "always";
            if (t.attribute("event")) {
                auto signature = "event:" + get(t, "event");
                if (!bindings.insert(signature).second) fail(t, "같은 이벤트에 여러 연결이 있어 모호합니다");
            } else if (key.given) {
                std::vector<std::string> channels;
                if (key.key) channels.push_back("key:" + std::to_string(key.key));
                if (key.gamepad) channels.push_back("pad:" + std::to_string(key.gamepad));
                if (key.control != GameControl::kNone)
                    channels.push_back("control:" + std::string(GameControlName(key.control)));
                for (auto& channel : channels)
                    if (!bindings.insert(channel).second) fail(t, "같은 키의 연결이 중복되어 모호합니다");
            }
            if (link) {
                connect.key = key.key;
                connect.gamepad = key.gamepad;
                connect.control = key.control;
                connect.mode = times.mode;
                connect.open = times.open;
                connect.close = times.close;
                connect.timed = times.timed;
                links.push_back(std::move(connect));
                continue;
            }
            // A vanilla transition's window.
            for (const auto& to : Split(targets))
                if (const auto match = VanillaMatch(to); !match.empty()) ws.vanillaMatches.push_back(match);
            const std::string mode = times.mode.empty() ? "buffered" : times.mode;
            ws.mode = (_stricmp(mode.c_str(), "always") == 0) ? 0 : (_stricmp(mode.c_str(), "window") == 0) ? 1 : 2;
            float at;
            bool relative;
            const std::string open = times.timed ? times.open : "@0", close = times.timed ? times.close : "@end";
            if (ParseAt(open.c_str(), at, relative))
                ws.openAt = relative ? -1.0f : at;
            else {
                ws.openEvent = open;
                ws.openAt = -1.0f;
            }
            if (ParseAt(close.c_str(), at, relative))
                ws.closeAt = relative ? at : -1.0f;
            else {
                ws.closeEvent = close;
                ws.closeAt = -1.0f;
            }
            s.cancels.push_back(std::move(ws));
        }
        for (const auto& l : links) {
            ActionLinkSpec next;
            next.blend = l.blend.as_float(-1.0f);
            next.action = l.to;
            next.partnerAction = l.partnerTo;
            if (l.partner && (next.partnerAction.empty() || !s.paired.enabled)) continue;
            next.event = l.event;
            next.key = l.key;
            next.gamepad = l.gamepad;
            next.control = l.control;
            next.buffered = (l.mode.empty() ? std::string("buffered") : l.mode) == "buffered";
            auto timing = [&](const std::string& text, bool close, std::string& event, float& at) {
                float value;
                bool relative;
                at = -1;
                if (ParseAt(text.c_str(), value, relative) && std::isfinite(value)) {
                    if (!relative || close) {
                        at = value;
                        if (relative) next.closeRelativeToEnd = true;
                    }
                } else
                    event = text;
            };
            if (l.timed) {
                timing(l.open, false, next.openEvent, next.openAt);
                timing(l.close, true, next.closeEvent, next.closeAt);
            }
            if (next.action.empty() ||
                (next.event.empty() && !next.key && !next.gamepad && next.control == GameControl::kNone))
                continue;
            s.links.push_back(std::move(next));
        }
        return s;
    }
    // A pair's <Carry> (the carrier lifts its partner, puts it down by a key through a one-actor putdown Action).
    void carry(Node c, Node n, ActionSpec& s) {
        shape(c, {"bone", "targetRoot", "putdown", "input", "key", "gamepad", "control", "target", "part", "weapon"},
            {"Hold", "Fix"});
        // A drawn weapon: sheathed (default) or sheathe (put away with no animation as the lift starts).
        choice(c, "weapon", {"sheathed", "sheathe"});
        for (auto key : {"bone", "targetRoot"})
            if (!ValidCarryBone(required(c, key))) fail(c, "본 이름은 앞뒤 공백·제어 문자 없이 1~256바이트여야 합니다");
        // Carry stage 3: how the target is held after the lift.
        choice(c, "target", {"pose", "physics"});
        if (c.attribute("part")) {
            if (get(c, "target", "pose") != "physics") fail(c, "part는 target=\"physics\"에서만 씁니다");
            if (!ValidCarryBone(get(c, "part"))) fail(c, "본 이름은 앞뒤 공백·제어 문자 없이 1~256바이트여야 합니다");
        }
        // The carrier alone puts the target down: a plain one-actor Action that only the carry starts (no start
        // names it, T1), with one CarryRelease moment (docs/설계-운반-래그돌-내려놓기.md).
        const auto putdown = required(c, "putdown");
        actionRef(c, "putdown");
        auto destination = n.parent().find_child_by_attribute("Action", "id", putdown.c_str());
        if (!destination || !singleActions.contains(putdown))
            fail(c, "putdown은 시작 입력·조건 카드가 없는 한 사람 Action이어야 합니다");
        if (!destination.child("CarryRelease"))
            fail(c, "putdown Action에 대상 놓기 시점 <CarryRelease at=\"...\"/>가 필요합니다");
        const auto key = keyRef(c);
        if (!key.given) fail(c, "운반에는 내려놓기 키(input, key·gamepad 또는 control)가 필요합니다");
        auto& carry = s.carry;
        carry.enabled = true;
        carry.bone = get(c, "bone");
        carry.targetRoot = get(c, "targetRoot");
        carry.putdownAction = putdown;
        carry.key = key.key ? key.key : 0xFFFFFFFF;
        carry.gamepad = key.gamepad;
        carry.control = key.control;
        carry.mode = get(c, "target", "pose") == "physics" ? CarryTargetMode::kPhysics : CarryTargetMode::kPose;
        carry.part = get(c, "part");
        const auto weapon = get(c, "weapon", "sheathed");
        carry.weapon = weapon == "sheathe" ? CarryWeapon::kSheathe : CarryWeapon::kSheathed;
        // The moment the target is fixed to the carrier bone during the lift (seconds only).
        if (auto f = one(c, "Fix")) {
            shape(f, {"at"});
            numeric(f, required(f, "at"));
            carry.fixAt = f.attribute("at").as_float(-1.0f);
        }
        // Carrier upper-body hold pose (carry stage 2).
        if (auto h = one(c, "Hold")) {
            shape(h, {"blendIn", "blendOut"}, {"Bone"});
            num(h, "blendIn", 5);
            num(h, "blendOut", 5);
            auto& hold = carry.hold;
            hold.pose = "lift-end";
            hold.blendIn = h.attribute("blendIn").as_float(0.2f);
            hold.blendOut = h.attribute("blendOut").as_float(0.2f);
            std::set<std::string> names;
            bool positive = false;
            for (auto b : h.children("Bone")) {
                shape(b, {"name", "weight"});
                const auto name = required(b, "name");
                if (!ValidCarryBone(name)) fail(b, "본 이름은 앞뒤 공백·제어 문자 없이 1~256바이트여야 합니다");
                if (!names.insert(lower(name)).second) fail(b, "같은 뼈가 두 번 지정되었습니다: " + name);
                num(b, "weight", 1);
                positive |= !b.attribute("weight") || b.attribute("weight").as_double() > 0;
                hold.bones.push_back({name, b.attribute("weight").as_float(1.0f)});
            }
            if (!positive) fail(h, "무게가 0보다 큰 Bone이 하나 이상 필요합니다");
        }
    }

    // ---- Starts (docs/설계-형식6-조건카드.md 2.3, T1-T7) ----
    void startShape(Node st) {
        shape(st,
            {"id", "to", "condition", "input", "key", "gamepad", "control", "event", "actor", "target", "range",
                "angle", "onKeyUp", "cooldown", "block", "blockWhen"},
            kCardParts);
        num(st, "cooldown");
        choice(st, "blockWhen", {"startable", "active", "both"});
        if (st.attribute("blockWhen") && !st.attribute("block")) fail(st, "blockWhen은 block과 함께 씁니다");
        if (!Cards::HasCard(st) && !st.attribute("to"))
            fail(st, "시작에는 to(동작), condition(함께 쓰는 조건 카드) 또는 조건 카드가 필요합니다");
    }
    // A start's trigger, cooldown and block on the action it holds: the HKX's own (T1) or a selecting one (T3, T4).
    // `pair`: what it starts is a Pair (a combat target is a pair's).
    void applyStart(ActionSpec& s, Node st, bool pair) {
        auto& trigger = s.trigger;
        const auto key = keyRef(st);
        if (key.given == static_cast<bool>(st.attribute("event")))
            fail(st, "키(input, key·gamepad 또는 control)와 event 중 하나만 지정합니다");
        trigger.inputId = key.input;
        trigger.key = key.key;
        trigger.gamepad = key.gamepad;
        trigger.control = key.control;
        // A start choosing among non-pair actions (0.12.299: a selecting one whose card plays no Pair) reads its
        // conditions on a target too: the nearest actor ahead (range, angle) or the combat target, key or event.
        const bool choosing = !pair && !s.flow.nodes.empty();
        if (st.attribute("event")) {
            eventName(st, required(st, "event"));
            trigger.onEvent = get(st, "event");
        } else if (st.attribute("actor"))
            fail(st, "actor는 이벤트 시작에서만 사용합니다");
        else if (st.attribute("target") && !choosing)
            fail(st, "target은 이벤트 시작이나 일반 동작을 고르는 조건 카드 시작에서만 사용합니다");
        choice(st, "actor", {"both", "player", "npc"});
        choice(st, "target", {"nearest", "combatTarget"});
        const bool combat = get(st, "target") == "combatTarget";
        if (combat && !choosing && (!pair || !st.attribute("event")))
            fail(st, "combatTarget은 페어 이벤트 시작이나 일반 동작을 고르는 조건 카드 시작에만 사용할 수 있습니다");
        if ((st.attribute("range") || st.attribute("angle")) && (!choosing || combat))
            fail(st, "range·angle은 일반 동작을 고르는 조건 카드 시작의 전방 대상에만 씁니다");
        num(st, "range", 2000);
        num(st, "angle", 360);
        trigger.combatTarget = combat;
        trigger.frontTarget = choosing && !combat;
        trigger.targetRange = st.attribute("range").as_float(trigger.targetRange);
        trigger.targetAngle = st.attribute("angle").as_float(trigger.targetAngle);
        if (trigger.targetRange <= 0.0f || trigger.targetAngle <= 0.0f) fail(st, "range·angle은 0보다 커야 합니다");
        boolean(st, "onKeyUp");
        trigger.onKeyUp = st.attribute("onKeyUp").as_bool(false);
        if (!trigger.onEvent.empty()) {
            const std::string who = get(st, "actor", "both");
            trigger.onEventPlayer = who == "player" || who == "both";
            trigger.onEventNpc = who == "npc" || who == "both";
        }
        if (st.attribute("cooldown")) s.condition.cooldown = st.attribute("cooldown").as_float(0.0f);
        if (st.attribute("block")) {
            const auto events = eventList(st, "block");
            const auto when = get(st, "blockWhen", "startable");
            const auto parsed = when == "active" ? SuppressSpec::When::kActive
                                : when == "both" ? SuppressSpec::When::kBoth
                                                 : SuppressSpec::When::kStartable;
            if (!s.suppress.events.empty()) {
                // The HKX's while-playing block and its own start's block of the same events: both at once.
                if (events != s.suppress.events || parsed != SuppressSpec::When::kStartable)
                    fail(st, "그 HKX의 재생 중 차단(block)과 시작의 차단을 함께 쓰려면 같은 이벤트여야 합니다");
                s.suppress.when = SuppressSpec::When::kBoth;
            } else {
                s.suppress.events = events;
                s.suppress.when = parsed;
            }
        }
    }
    // A selecting action (T3-T6): no clip, it picks by its card; its start's settings if a start holds it.
    ActionSpec selecting(
        const std::string& id, const Cards::Compiled& compiled, Node start, const std::string& sourceName) {
        const bool pair = compiled.playsPair;
        ActionSpec s;
        s.id = id;
        s.source = sourceName;
        s.view.view = pair ? View::kThird : fileView;
        for (const auto& [name, info] : inputs)
            s.inputs.push_back({name, info.hasKey ? info.key : 0, info.gamepad, info.control});
        s.trigger.key = 0;
        s.condition.inAir = 1;           // the picked HKX's air rule decides (T6)
        s.exitTriggerBeforeEnd = 0.05f;  // as every action's default Exit
        s.flowActions = declared;
        s.flow = compiled.program;
        if (pair) {
            s.paired.enabled = true;
            s.safetyDuration = 30.0f;
        }
        if (start) applyStart(s, start, pair);
        return s;
    }
    std::string fresh(const std::string& base) {
        std::string id = base.substr(0, 64);
        for (int i = 2; taken.contains(lower(id)); ++i) {
            const auto suffix = std::to_string(i);
            id = base.substr(0, 64 - suffix.size()) + suffix;
        }
        taken.insert(lower(id));
        return id;
    }
    Cards::Compiled card(Node owner) {
        Cards::Compiled out;
        std::string error;
        if (!Cards::Compile(owner, declared, pairs, shared, out, error)) throw std::runtime_error(error);
        return out;
    }
    // T5: the selecting action a transition or a QTE result going through a card reaches (one per shared card).
    std::string through(Node owner, const std::string& base) {
        const bool sharedOnly = owner.attribute("condition") && !owner.first_child();
        const auto name = get(owner, "condition");
        if (sharedOnly)
            if (auto it = byCondition.find(name); it != byCondition.end()) return it->second;
        const auto id = fresh(sharedOnly ? name + "_Flow" : base);
        auto compiled = card(owner);
        made.emplace(id, compiled.playsPair ? "pair" : "action");
        trailing.push_back({id, std::move(compiled)});
        if (sharedOnly) byCondition[name] = id;
        return id;
    }
};
// A name made from a start's trigger for its selecting action: letters, digits and _ only (it names graph events).
std::string triggerName(Node st) {
    std::string raw = get(st, "event");
    if (raw.empty()) raw = get(st, "key");
    if (raw.empty()) raw = get(st, "gamepad");
    if (raw.empty()) raw = get(st, "control");
    if (raw.empty()) raw = get(st, "input");
    std::string out = "Start_";
    for (const unsigned char c : raw) out += std::isalnum(c) ? static_cast<char>(c) : '_';
    return out;
}
bool CompileFormat6(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey) {
    error.clear();
    try {
        auto root = source.document_element();
        std::size_t roots = 0;
        for (auto n : source.children())
            if (n.type() == pugi::node_element) ++roots;
        if (roots != 1) fail(root, "루트 요소는 하나여야 합니다");
        if (std::string_view(root.name()) != "AnimationSet") fail(root, "AnimationSet 루트가 필요합니다");
        if (get(root, "version") != "6") fail(root, "형식 6(version=\"6\")만 읽습니다");
        shape(root, {"version", "id", "view"}, {"Inputs", "EventRegistry", "Start", "Condition", "Action", "Pair"});
        identifier(root, required(root, "id"));
        Compiler c{resolveKey};
        if (root.attribute("view")) {
            const auto view = ParseView(get(root, "view"));
            if (!view) fail(root, "view는 third, first, both 중 하나입니다");
            c.fileView = *view;
        }
        if (auto inputs = one(root, "Inputs")) {
            shape(inputs, {}, {"Input"});
            for (auto i : inputs.children("Input")) c.input(i);
        }
        // Names first: actions, starts and shared cards, so that cards and starts can name any of them.
        std::set<std::string> conditionIds;
        for (auto a : root.children()) {
            const std::string kind = a.name();
            if (a.type() != pugi::node_element) continue;
            if (kind == "Action" || kind == "Pair") {
                auto id = required(a, "id");
                identifier(a, id);
                if (!c.taken.insert(lower(id)).second) fail(a, "ID 중복: " + id);
                if (kind == "Action") c.singleActions.insert(id);
                if (auto carry = a.child("Carry"); kind == "Pair" && carry.attribute("putdown"))
                    c.putdownActions.insert(get(carry, "putdown"));
                c.actions.emplace(id, kind == "Pair" ? "pair" : "action");
                c.declared.push_back(id);
                c.pairs.push_back(kind == "Pair");
            } else if (kind == "Start" && a.attribute("id")) {
                const auto id = get(a, "id");
                identifier(a, id);
                if (!c.taken.insert(lower(id)).second) fail(a, "ID 중복: " + id);
            } else if (kind == "Condition") {
                shape(a, {"id"}, kCardParts);
                const auto id = required(a, "id");
                identifier(a, id);
                if (!conditionIds.insert(lower(id)).second) fail(a, "조건 카드 ID 중복: " + id);
                c.shared.emplace(id, a);
            }
        }
        std::vector<EventRuleSpec> registry;
        if (auto r = one(root, "EventRegistry")) c.registry(r, sourceName, registry);
        // Ways into each action (T2): starts, transitions, QTE results, card rows and carry putdowns that name it.
        std::map<std::string, int> ways;
        auto count = [&](Node n, const char* attr) {
            if (const auto v = get(n, attr); !v.empty()) ++ways[lower(v)];
        };
        auto walk = [&](auto&& self, Node at) -> void {
            for (auto x : at.children()) {
                if (x.type() != pugi::node_element) continue;
                if (has({"Start", "Transition", "Success", "Failure", "Perfect", "Row", "True", "False"}, x.name())) {
                    count(x, "to");
                    count(x, "partnerTo");
                } else if (std::string_view(x.name()) == "Carry")
                    count(x, "putdown");
                self(self, x);
            }
        };
        walk(walk, root);
        // Where each start goes (T1-T4).
        struct Plan {
            Node start;
            std::string id;  // a selecting action's (T3, T4); empty: the start is the HKX's own (T1)
            Cards::Compiled compiled;
        };
        std::vector<Plan> plans;
        std::map<std::string, Node> attached;  // action id (lower) -> its own start
        for (auto st : root.children("Start")) {
            c.startShape(st);
            Plan p{st};
            const bool card = Cards::HasCard(st);
            const auto to = get(st, "to");
            if (!card) {
                c.actionRef(st, "to");
                if (c.putdownActions.contains(to))
                    fail(st, "운반의 내려놓기 동작(putdown)은 시작으로 시작하지 않습니다: " + to);
                const bool held = st.attribute("cooldown").as_float(0.0f) > 0.0f;
                const bool onlyWay = ways[lower(to)] == 1;
                if (!attached.contains(lower(to)) && (!held || onlyWay)) {
                    attached[lower(to)] = st;
                    plans.push_back(std::move(p));
                    continue;
                }
            } else if (!to.empty() && c.putdownActions.contains(to))
                fail(st, "운반의 내려놓기 동작(putdown)은 시작으로 시작하지 않습니다: " + to);
            p.id = st.attribute("id") ? get(st, "id") : c.fresh(card ? triggerName(st) : to + "_Start");
            p.compiled = c.card(st);
            plans.push_back(std::move(p));
        }
        // The actions in document order (T7): each HKX with the start that is its own, each selecting action at
        // its start's place, then the ones transitions and QTE results go through.
        std::vector<ActionSpec> actions;
        auto finish = [&](ActionSpec&& s) {
            s.Finalize();
            if (s.paired.enabled && s.enterBlend < 0) s.enterBlend = 0.2f;
            actions.push_back(std::move(s));
        };
        std::size_t plan = 0;
        for (auto n : root.children()) {
            const std::string_view kind = n.name();
            if (kind == "Action" || kind == "Pair") {
                auto s = c.action(n, sourceName);
                if (auto it = attached.find(lower(get(n, "id"))); it != attached.end())
                    c.applyStart(s, it->second, kind == "Pair");
                finish(std::move(s));
            } else if (kind == "Start") {
                auto& p = plans[plan++];
                if (!p.id.empty()) finish(c.selecting(p.id, p.compiled, p.start, sourceName));
            }
        }
        for (auto& t : c.trailing) finish(c.selecting(t.id, t.compiled, {}, sourceName));
        for (const auto& [node, to] : c.eventQteTargets) {
            const auto it = c.actions.find(to);
            const auto made = c.made.find(to);
            const auto kind = it != c.actions.end() ? it->second : made != c.made.end() ? made->second : "";
            if (kind.empty()) fail(node, "정의되지 않은 액션: " + to);
            if (kind != "action") fail(node, "이벤트 QTE의 to는 일반 동작만 가리킵니다: " + to);
        }
        if ((c.declared.empty() && registry.empty()) || actions.size() > 256)
            fail(root, "1~256개 액션 또는 이벤트 등록이 필요합니다(조건으로 고르는 시작·전환도 하나씩 셉니다)");
        mod = get(root, "id");
        output.insert(output.end(), std::make_move_iterator(actions.begin()), std::make_move_iterator(actions.end()));
        events.insert(events.end(), std::make_move_iterator(registry.begin()), std::make_move_iterator(registry.end()));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey) {
    return CompileFormat6(source, sourceName, output, events, mod, error, resolveKey);
}
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::string& mod, std::string& error, KeyResolver resolveKey) {
    std::vector<EventRuleSpec> events;
    return Compile(source, sourceName, output, events, mod, error, resolveKey);
}
}

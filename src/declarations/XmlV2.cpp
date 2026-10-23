#include "XmlV2.h"
#include "runtime/ActionSpec.h"
#include "runtime/PairedRig.h"
#include "runtime/CarrySpec.h"
#include "ConditionFlowXml.h"
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

// One pass from the AnimationSet document (format 4) to runtime
// declarations: every element is
// validated and then read into its ActionSpec fields. Times keep their textual forms ("@seconds",
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
[[noreturn]] void fail(Node n, const std::string& reason) {
    throw std::runtime_error(n.path() + (n.attribute("id") ? " [" + get(n, "id") + "]" : "") + ": " + reason);
}
void shape(Node n, Names attrs, Names children = {}) {
    // 0.12.278: an event moment waits for its event only; "if it does not come by" is gone.
    for (auto gone : {"fallback", "fromFallback", "untilFallback"})
        if (n.attribute(gone))
            fail(n, std::string(gone) +
                        "(이벤트가 안 오면)는 없어졌습니다(0.12.278): 이벤트로 두면 그 이벤트가 올 때만 동작합니다");
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
// A seconds time read as a clock time or an event: Recovery, QTE, FireArrow, Cost, CarryRelease.
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
// A clip moves only by its HKX's AMR annotations ("animmotion"/"animrotation", 0.12.231): a declaration no
// longer moves the actor, holds it in place, or picks a clip by the movement input.
void noMotion(Node n) {
    if (auto m = n.child("Motion"))
        fail(m, "Motion은 없어졌습니다(0.12.231): 이동은 HKX의 AMR 주석(animmotion)으로만 합니다. 움직이지 않게 하려면 "
                "주석을 지우세요");
    if (auto c = n.child("Clips"))
        fail(c,
            "Clips(방향별 클립)는 없어졌습니다(0.12.231): 방향마다 Action을 두고 이동 입력 방향 조건(Flow)으로 고르세요");
    for (auto key : {"direction", "defaultDirection", "strict"})
        if (auto st = n.child("Start"); st && st.attribute(key))
            fail(st, std::string("Start ") + key +
                         "는 없어졌습니다(0.12.231): 방향은 이동 입력 방향 조건(Flow)으로 고르세요");
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
    {"fireArrow", EventOp::kFireArrow}, {"ragdoll", EventOp::kRagdoll}};
const std::pair<std::string_view, EventTarget> kTargets[] = {{"both", EventTarget::kBoth},
    {"player", EventTarget::kPlayer}, {"initiator", EventTarget::kInitiator}, {"receiver", EventTarget::kReceiver}};
// How a shot leaves the bow (0.12.268): mode "drawn" (default) releases the drawn bow as the player would;
// "direct" launches at full power with no draw, `damage` percent of a full draw's damage (1~1000, default 100).
FireArrowShot fireArrowShot(Node e) {
    choice(e, "mode", {"drawn", "direct"});
    num(e, "damage", 1000, 1);
    FireArrowShot shot;
    shot.mode = get(e, "mode", "drawn") == "direct" ? FireArrowMode::kDirect : FireArrowMode::kDrawn;
    if (e.attribute("damage")) {
        if (shot.mode != FireArrowMode::kDirect) fail(e, "damage는 mode=\"direct\"(바로 발사)에서만 씁니다");
        shot.damage = e.attribute("damage").as_float(100.0f) / 100.0f;
    }
    return shot;
}
// One <Do>: a function of a registered event, or (`timed`, with `at`) of a pair's clip time.
EventFunctionSpec eventFunction(Node e, bool timed) {
    if (has({"lock", "unlock"}, get(e, "op"))) fail(e, "op=\"lock\"·\"unlock\"(조작 잠금)은 없어졌습니다(0.12.238)");
    // An event's function changes the actor that sent the event (0.12.273): no file used another target or a
    // sender filter, and a player-only function already does nothing for anyone else. A pair's clip-time
    // effect has no sender, so it keeps its target.
    if (!timed && (e.attribute("target") || e.attribute("from")))
        fail(e, "이벤트 기능의 target·from은 없어졌습니다(0.12.273): 기능은 이벤트를 보낸 액터에게 적용됩니다");
    if (e.attribute("parameters"))
        fail(e, "parameters는 없어졌습니다(0.12.274): 주석 숫자로 흔들려면 HKX 주석 AI.CamShake|세기|시간을 씁니다");
    // 0.12.275: a function's own space condition is gone (a space split is a condition branch), and a pair's
    // clip-time effect names the role it changes: both actors play the clip, so no role sends it.
    for (const char* gone : {"space", "minSpace", "maxSpace"})
        if (e.attribute(gone))
            fail(e,
                "space·minSpace·maxSpace는 없어졌습니다(0.12.275): 공간으로 나누려면 조건 분기(Flow의 space)를 씁니다");
    if (timed && e.attribute("from"))
        fail(e,
            "시각 기능의 from은 없어졌습니다(0.12.275): 적용할 쪽은 target(initiator·receiver·both·player)으로 정합니다");
    if (timed && has({"sender", "partner"}, get(e, "target")))
        fail(e,
            "시각 기능의 target은 initiator·receiver·both·player입니다(0.12.275): 클립 시각에는 보낸 액터가 없습니다");
    // 0.12.278: the ragdoll is its own function; a spell no longer frees a pair's physics hold.
    if (e.attribute("releasePhysics"))
        fail(e, "releasePhysics는 없어졌습니다(0.12.278): 래그돌은 op=\"ragdoll\"(direction·force)을 씁니다");
    if (timed)
        shape(e, {"at", "op", "target", "plugin", "form", "delay", "stamina", "magicka", "strength", "duration",
                     "factor", "mode", "damage", "direction", "force"});
    else
        shape(e, {"op", "plugin", "form", "delay", "stamina", "magicka", "strength", "duration", "factor", "mode",
                     "damage", "direction", "force"});
    const auto op = required(e, "op");
    choice(e, "op",
        {"hide", "show", "spell", "stateOn", "stateOff", "cost", "shake", "stopVelocity", "timeOn", "timeOff",
            "iframeOn", "iframeOff", "armorOn", "armorOff", "rotateOn", "rotateOff", "cancel", "fireArrow", "ragdoll"});
    choice(e, "target", {"both", "player", "initiator", "receiver"});
    num(e, "delay", 60);
    num(e, "stamina", 100000);
    num(e, "magicka", 100000);
    num(e, "strength", 10);
    num(e, "duration", 30);
    num(e, "factor", 5, 0.05);
    choice(e, "direction", {"down", "up", "forward", "backward", "left", "right"});
    num(e, "force", RagdollPush::kMaxForce);
    std::string form = get(e, "form");
    if (op == "spell" || op == "stateOn" || op == "stateOff") {
        const auto plugin = required(e, "plugin");
        required(e, "form");
        if (plugin.find_first_of("/\\:") != std::string::npos) fail(e, "plugin에는 파일 이름만 지정합니다");
        if (form.starts_with("0x") || form.starts_with("0X")) form.erase(0, 2);
        if (form.empty() || form.size() > 8 ||
            !std::all_of(form.begin(), form.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            fail(e, "form은 16진수 ID여야 합니다");
    } else if (e.attribute("plugin") || e.attribute("form"))
        fail(e, "plugin·form은 spell·stateOn·stateOff에서만 씁니다");
    if (op == "fireArrow") {
        if (timed) fail(e, "시간 기준 활 발사는 동작의 FireArrow at으로 지정합니다");
    } else if (e.attribute("mode") || e.attribute("damage"))
        fail(e, "mode·damage는 fireArrow에서만 씁니다");
    if (op != "ragdoll" && (e.attribute("direction") || e.attribute("force")))
        fail(e, "direction·force는 ragdoll에서만 씁니다");
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
    if (op == "fireArrow") f.arrow = fireArrowShot(e);
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
    std::uint32_t key = 0, gamepad = 0;  // the key transition's key (an <Inputs> name resolved, or written here)
    bool partner = false;
    pugi::xml_attribute blend;
    std::string mode, open, close;
    bool timed = false;
};
// The window of a Cancel or Connect: buffered/window/always and its open and close times. No <Window> = always
// (the whole clip).
struct WindowTimes {
    std::string mode, open, close;
    bool timed = false;
};
struct InputInfo {
    Node node;
    std::uint32_t key = 0;      // DX scan code (+256 mouse)
    std::uint32_t gamepad = 0;  // the engine's gamepad button code
    bool hasKey = false;
};
// Where a key is given: an <Inputs> name (input="…") or the key itself (key="G" gamepad="A"), format 4.
struct KeyRef {
    std::string input;  // the <Inputs> name, if one was used
    std::uint32_t key = 0, gamepad = 0;
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
    // input="<Inputs name>" or key=/gamepad= written here; never both.
    KeyRef keyRef(Node n) {
        KeyRef r;
        const bool named = n.attribute("input"), direct = n.attribute("key") || n.attribute("gamepad");
        if (named && direct) fail(n, "input과 key·gamepad 중 하나만 씁니다");
        if (named) {
            inputRef(n);
            r.input = get(n, "input");
            const auto& in = inputs.at(r.input);
            r.key = in.hasKey ? in.key : 0;
            r.gamepad = in.gamepad;
        } else if (direct) {
            r.key = optionalKeyCode(n, get(n, "key"));
            r.gamepad = gamepadCode(n);
            if (!r.key && !r.gamepad) fail(n, "키 또는 패드 버튼이 필요합니다");
        }
        r.given = named || direct;
        return r;
    }
    void inputRef(Node n, const char* attr = "input") {
        if (n.attribute(attr) && !inputs.contains(get(n, attr))) fail(n, "정의되지 않은 입력: " + get(n, attr));
    }
    void actionRef(Node n, const char* attr) {
        if (!actions.contains(required(n, attr))) fail(n, "정의되지 않은 액션: " + get(n, attr));
    }
    void input(Node n) {
        shape(n, {"id", "key", "gamepad"});
        auto id = required(n, "id");
        identifier(n, id);
        if (!inputIds.insert(lower(id)).second) fail(n, "입력 ID 중복");
        InputInfo info{n};
        if (auto key = n.attribute("key")) {
            info.key = keyCode(n, key.value());
            info.hasKey = true;
        }
        info.gamepad = gamepadCode(n);
        if (!n.attribute("key") && !info.gamepad) fail(n, "키 또는 패드 버튼이 필요합니다");
        inputs.emplace(id, info);
    }
    // One <QTE>: an action's (links: its transitions are collected there) or an event's (no links: started by
    // its event, 0.12.248).
    QteSpec qte(Node q, Node owner, bool pair, std::set<std::string>& qteIds, std::vector<LinkDeclaration>* links) {
        if (q.attribute("step")) fail(q, "QTE step은 콤보와 함께 없어졌습니다(형식 4)");
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
        choice(q, "control", {"rightAttack"});
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
                shape(r, {"event", "recipient", "to", "partnerTo", "soundPlugin", "soundForm"});
            else
                shape(r, {"event", "to", "partnerTo", "soundPlugin", "soundForm"});
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
            if (e.attribute("values"))
                fail(
                    e, "values는 없어졌습니다(0.12.274): 흔들림 세기·시간은 HKX 주석 AI.CamShake|세기|시간으로 씁니다");
            shape(e, {"name"}, {"Do", "QTE"});
            const auto name = required(e, "name");
            eventName(e, name);
            if (!names.insert(lower(name)).second) fail(e, "같은 이벤트를 두 번 등록했습니다: " + name);
            EventRuleSpec rule;
            rule.name = name;
            rule.source = sourceName;
            for (auto d : e.children("Do")) rule.functions.push_back(eventFunction(d, false));
            if (rule.functions.size() > 64) fail(e, "이벤트 하나에 기능은 64개까지입니다");
            std::set<std::string> qteIds;
            for (auto q : e.children("QTE")) rule.qtes.push_back(qte(q, {}, false, qteIds, nullptr));
            if (rule.qtes.size() > 4) fail(e, "이벤트 하나에 QTE는 4개까지입니다");
            out.push_back(std::move(rule));
        }
        if (out.size() > 4096) fail(r, "이벤트 등록은 파일당 4096개까지입니다");
    }
    WindowTimes window(Node n, bool link) {
        if (n.attribute("always"))
            fail(n, "always는 없어졌습니다(형식 4): <Window>를 쓰지 않으면 클립 전체에서 받습니다");
        shape(n, {"from", "until", "buffer"});
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
    void effects(Node n, PairFeatureSpec& out) {
        if (auto old = n.child("Effect"))
            fail(old, "Effect는 없어졌습니다: 이벤트 기능은 EventRegistry, 시간 효과는 <Do at=\"초\">로 씁니다");
        shape(n, {"interruptOnThirdPartyHit"}, {"Camera", "Do"});
        boolean(n, "interruptOnThirdPartyHit");
        out.enabled = true;
        if (auto c = one(n, "Camera")) {
            shape(c, {"reset", "restore", "timeout"});
            choice(c, "restore", {"end", "event"});
            num(c, "timeout", 60, 0.1);
            if (c.attribute("reset")) eventName(c, required(c, "reset"));
            out.cameraReset = get(c, "reset");
            out.cameraRestoreOnEnd = get(c, "restore", "end") != "event";
            const float timeout = c.attribute("timeout").as_float(10.0f);
            out.cameraTimeout = std::isfinite(timeout) ? std::clamp(timeout, 0.1f, 60.0f) : 10.0f;
        }
        out.interruptOnThirdPartyHit = n.attribute("interruptOnThirdPartyHit").as_bool(false);
        for (auto d : n.children("Do")) {
            TimedEffectSpec timed;
            timed.at = static_cast<float>(numeric(d, required(d, "at"), 600));
            timed.function = eventFunction(d, true);
            timed.at = d.attribute("at").as_float(timed.at);
            out.timed.push_back(std::move(timed));
        }
    }
    ActionSpec action(Node n, const std::string& sourceName) {
        if (auto old = n.child("Events"))
            fail(old, "Events extra는 없어졌습니다: 그래프에 등록할 이벤트는 EventRegistry의 Event로 씁니다");
        const bool pair = std::string_view(n.name()) == "Pair";
        noMotion(n);
        for (const char* old : {"Step", "Transition"})
            if (auto c = n.child(old))
                fail(c,
                    std::string(old) +
                        "는 콤보와 함께 없어졌습니다(형식 4): HKX마다 <Action>을 두고 <Connect to=\"다음\" key=\"…\">로 이으세요");
        if (auto old = n.child("Lock")) fail(old, "Lock(조작 잠금 구간)은 없어졌습니다(0.12.238)");
        if (auto old = n.child("EndPosition"))
            fail(old, "EndPosition은 없어졌습니다(0.12.255): 페어 종료 위치 보정은 모든 페어에 자동으로 걸리며 "
                      "INI [Compatibility] iPairEndPosition으로 끄거나 범위를 정합니다");
        shape(n, {"id", "clip", "view", "firstPerson"},
            {"Start", "Require", "Attack", "Enter", "Exit", "Rotate", "IFrame", "HyperArmor", "Cost", "Recovery",
                "Cancel", "Connect", "Advanced", "Target", "OnPartnerLost", "Effects", "Flow", "Carry", "Suppress",
                "FireArrow", "QTE", "CarryRelease"});
        if (!pair && (n.child("Target") || n.child("OnPartnerLost") || n.child("Carry")))
            fail(n, "액션 종류에 맞지 않는 요소입니다");
        // Only the putdown Action a Carry names; the Carry checks that it has this slot.
        if (auto r = one(n, "CarryRelease"); r && !putdownActions.contains(get(n, "id")))
            fail(r, "CarryRelease는 운반(Carry)의 putdown이 가리키는 Action에만 씁니다");
        if (pair &&
            (n.child("Cancel") || n.child("Rotate") || n.child("IFrame") || n.child("HyperArmor") || n.child("Cost")))
            fail(n, "페어 역할 기능은 Effects로 선언해야 합니다");
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
        if (auto flow = one(n, "Flow")) {
            std::vector<std::string> names;
            for (auto a : n.parent().children())
                if (has({"Action", "Pair"}, a.name())) names.push_back(required(a, "id"));
            ConditionFlowXml::Program program;
            std::string error;
            if (!ConditionFlowXml::Read(flow, names, program, error)) fail(flow, error);
            for (const auto& predicate : program.predicates)
                if (predicate.test == ConditionFlowXml::Test::kSpace ||
                    predicate.test == ConditionFlowXml::Test::kGround) {
                    if (!pair) fail(flow, "공간 검사는 상대 검색 기준을 가진 Pair에 선언하세요");
                    bool pairedCandidate = false;
                    for (auto a : n.parent().children())
                        if (get(a, "id") == names[predicate.candidate])
                            pairedCandidate = std::string_view(a.name()) == "Pair";
                    if (!pairedCandidate) fail(flow, "공간 검사 candidate는 Pair여야 합니다");
                }
            for (const auto& node : program.nodes)
                if (node.kind == ConditionFlow::Kind::kAction && names[node.action] != get(n, "id")) {
                    for (auto a : n.parent().children())
                        if (get(a, "id") == names[node.action]) {
                            if (a.child("Flow")) fail(flow, "Flow 목적지에 또 다른 Flow를 연결할 수 없습니다");
                            if (!pair && std::string_view(a.name()) == "Pair")
                                fail(flow, "페어 목적지를 선택하는 Flow는 상대 검색 기준을 가진 Pair에 선언하세요");
                        }
                }
            // The runtime program names every action of the set; destinations resolve by name.
            s.flowActions = declared;
            if (!ConditionFlowXml::Read(flow, s.flowActions, s.flow, error)) fail(flow, error);
        }
        if (n.attribute("clip")) {
            auto f = required(n, "clip");
            clip(n, f);
            ClipVariant v;
            v.file = f;
            s.clips.push_back(std::move(v));
        } else {
            // A selecting action (0.12.244): its Flow sends every route to another action, so it never plays
            // a clip of its own and needs none. It is never injected or preloaded; only its start gates
            // (Start, Require, Suppress) and its Flow mean anything.
            const auto flow = n.child("Flow");
            if (!flow)
                fail(n, "clip이 필요합니다. clip을 생략할 수 있는 것은 Flow가 모든 경로를 다른 동작으로 보내는 고르기 "
                        "전용 동작뿐입니다(0.12.244)");
            for (const auto& node : s.flow.nodes)
                if (node.kind == ConditionFlow::Kind::kAction && s.flowActions[node.action] == s.id)
                    fail(flow, "clip이 없는 동작 '" + s.id +
                                   "'의 Flow가 자기 자신을 재생합니다. 자기 HKX를 재생하려면 clip을 씁니다");
            for (auto c : n.children())
                if (c.type() == pugi::node_element && !has({"Start", "Require", "Suppress", "Flow"}, c.name()))
                    fail(c, std::string("clip이 없는 고르기 전용 동작에는 Start·Require·Suppress·Flow만 씁니다: ") +
                                c.name());
        }
        // Every root input is declared on every action, without changing its ID.
        for (const auto& [id, info] : inputs) s.inputs.push_back({id, info.hasKey ? info.key : 0, info.gamepad});
        std::string startActor, startTarget;
        auto& trigger = s.trigger;
        trigger.key = 0;  // no key of its own: an <Inputs> reference, a key written here or an event
        if (auto st = one(n, "Start")) {
            shape(st, {"input", "key", "gamepad", "event", "actor", "target", "onKeyUp"});
            const auto key = keyRef(st);
            if (key.given == static_cast<bool>(st.attribute("event")))
                fail(st, "키(input 또는 key·gamepad)와 event 중 하나만 지정합니다");
            trigger.inputId = key.input;
            trigger.key = key.key;
            trigger.gamepad = key.gamepad;
            if (st.attribute("event")) {
                eventName(st, required(st, "event"));
                trigger.onEvent = get(st, "event");
            } else if (st.attribute("actor") || st.attribute("target"))
                fail(st, "actor/target은 이벤트 시작에서만 사용합니다");
            choice(st, "actor", {"both", "player", "npc"});
            choice(st, "target", {"nearest", "combatTarget"});
            if (get(st, "target") == "combatTarget" && (!pair || !st.attribute("event")))
                fail(st, "combatTarget은 페어 이벤트 시작에만 사용할 수 있습니다");
            boolean(st, "onKeyUp");
            startActor = get(st, "actor");
            startTarget = get(st, "target");
            trigger.onKeyUp = st.attribute("onKeyUp").as_bool(false);
        }
        if (auto r = one(n, "Require")) {
            // 0.12.276: the action's own nature only; a state check is a condition branch's test
            // (tools/require_to_flow.py moves an older file's).
            for (const char* gone : {"weapon", "blockWhileAttacking", "stamina"})
                if (r.attribute(gone))
                    fail(r, std::string(gone) +
                                "는 없어졌습니다(0.12.276): 조건 분기의 검사(무기 뽑음·공격 상태·스태미나)를 씁니다");
            shape(r, {"air", "cooldown"});
            choice(r, "air", {"deny", "allow", "only"});
            num(r, "cooldown");
            auto& c = s.condition;
            const std::string air = get(r, "air", "deny");
            c.inAir = _stricmp(air.c_str(), "allow") == 0 ? 1 : _stricmp(air.c_str(), "only") == 0 ? 2 : 0;
            c.cooldown = r.attribute("cooldown").as_float(0.0f);
        }
        if (auto e = one(n, "Suppress")) {
            shape(e, {"events", "when"});
            choice(e, "when", {"startable", "active", "both"});
            const auto when = get(e, "when", "startable");
            s.suppress.when = when == "active" ? SuppressSpec::When::kActive
                              : when == "both" ? SuppressSpec::When::kBoth
                                               : SuppressSpec::When::kStartable;
            s.suppress.events = eventList(e, "events");
        }
        if (auto a = one(n, "Attack")) {
            shape(a, {"data"});
            auto data = required(a, "data");
            eventName(a, data);
            s.attack.enabled = true;
            s.attack.data = data;
        }
        if (auto e = one(n, "Enter")) {
            shape(e, {"blend"});
            num(e, "blend", 5);
            s.enterBlend = e.attribute("blend").as_float(-1.0f);
        }
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
                    if (pair) fail(slot, "Rotate locked는 일반 액션 전용입니다");
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
        std::vector<LinkDeclaration> links;  // QTE results first, then Connects, in document order
        std::set<std::string> qteIds;
        for (auto q : n.children("QTE")) s.qtes.push_back(qte(q, n, pair, qteIds, &links));
        if (auto f = one(n, "FireArrow")) {
            if (pair) fail(f, "FireArrow is player-only and requires an Action");
            shape(f, {"at", "onRelease", "mode", "damage"});
            boolean(f, "onRelease");
            s.fireArrow.enabled = true;
            s.fireArrow.shot = fireArrowShot(f);
            const bool direct = s.fireArrow.shot.mode == FireArrowMode::kDirect;
            // A direct shot has no draw to release: it fires at its time (an event shot is the registry's).
            if (direct && f.attribute("onRelease").as_bool(false))
                fail(f, "mode=\"direct\"(바로 발사)는 당김이 없어 onRelease를 쓸 수 없습니다");
            s.fireArrow.onRelease = !direct && f.attribute("onRelease").as_bool(true);
            if (f.attribute("at")) {
                std::string unused;
                ReadMoment(seconds(f, "at", Basis::Seconds), unused, s.fireArrow.at);
            }
            if (direct && s.fireArrow.at < 0)
                fail(f, "mode=\"direct\"(바로 발사)에는 at이 필요합니다(이벤트 발사는 EventRegistry의 fireArrow)");
            if (s.fireArrow.at < 0 && !s.fireArrow.onRelease)
                fail(f,
                    "FireArrow에는 at 또는 onRelease=\"true\"가 필요합니다(이벤트 발사는 EventRegistry의 fireArrow)");
        }
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
        if (auto r = one(n, "Recovery")) s.recovery = recoveryDeclaration(r);
        if (pair) s.safetyDuration = 30.0f;  // a pair's clip is long; written values replace it (Advanced)
        // An ordinary action restores the player's camera too (0.12.260) and has clip-time effects on the actor
        // playing it (0.12.274): the ones its own slots do not cover. Third-party interruption stays a pair's.
        if (!pair)
            if (auto e = one(n, "Effects")) {
                if (e.attribute("interruptOnThirdPartyHit"))
                    fail(e, "제3자 피격 중단(interruptOnThirdPartyHit)은 페어 전용입니다");
                for (auto d : e.children("Do")) {
                    if (d.attribute("target") || d.attribute("from"))
                        fail(d,
                            "일반 동작의 시각 기능은 그 동작을 재생하는 액터에게 적용됩니다: target·from을 쓰지 않습니다");
                    const auto op = get(d, "op");
                    if (!op.empty() && !has({"hide", "show", "spell", "stateOn", "stateOff", "shake", "stopVelocity",
                                                "timeOn", "timeOff"},
                                           op))
                        fail(d, "일반 동작의 시각 기능이 아닙니다: " + op +
                                    " (무적·슈퍼아머·회전·비용은 IFrame·HyperArmor·Rotate·Cost, 활 발사는 FireArrow, "
                                    "cancel은 페어 전용)");
                }
                effects(e, s.pairFeatures);
            }
        if (pair) {
            auto& paired = s.paired;
            paired.enabled = true;
            if (auto t = one(n, "Target")) {
                shape(t, {"range", "angle", "rig"});
                num(t, "range", 10000, 0.001);
                num(t, "angle", 360, 0.001);
                if (t.attribute("rig") && PairedRig::FromName(get(t, "rig")) == PairedRig::Kind::kUnsupported)
                    fail(t, "알 수 없는 대상 스켈레톤: " + get(t, "rig"));
                paired.targetRig = PairedRig::FromName(get(t, "rig", "character"));
                paired.range = t.attribute("range").as_float(150.0f);
                paired.angle = t.attribute("angle").as_float(60.0f);
            }
            if (auto l = one(n, "OnPartnerLost")) {
                shape(l, {"blend"});
                numeric(l, required(l, "blend"), 5, 0.01);
                const float lossBlend = l.attribute("blend").as_float(-1.0f);
                paired.lossBlend =
                    std::isfinite(lossBlend) && lossBlend > 0.0f ? std::clamp(lossBlend, 0.01f, 5.0f) : -1.0f;
            }
            if (auto e = one(n, "Effects")) effects(e, s.pairFeatures);
            if (auto c = one(n, "Carry")) {
                if (c.attribute("release"))
                    fail(c, "내려놓기는 이제 putdown(한 사람 Action)과 그 안의 CarryRelease 시점으로 지정합니다");
                if (c.attribute("alignStart")) fail(c, "alignStart는 없어졌습니다(형식 4): 쓰이지 않던 값입니다");
                if (get(c, "weapon") == "keep" || c.attribute("weaponShow"))
                    fail(c, "weapon=\"keep\"과 weaponShow는 없어졌습니다(0.12.238): 무기를 든 채 들려면 "
                            "weapon=\"sheathe\"(들기 시작 때 넣기)를 쓰세요");
                shape(c, {"bone", "targetRoot", "putdown", "input", "key", "gamepad", "target", "part", "weapon"},
                    {"Hold", "Fix"});
                // A drawn weapon: sheathed (default) or sheathe (put away with no animation as the lift starts).
                choice(c, "weapon", {"sheathed", "sheathe"});
                for (auto key : {"bone", "targetRoot"})
                    if (!ValidCarryBone(required(c, key)))
                        fail(c, "본 이름은 앞뒤 공백·제어 문자 없이 1~256바이트여야 합니다");
                // Carry stage 3: how the target is held after the lift.
                choice(c, "target", {"pose", "physics"});
                if (c.attribute("part")) {
                    if (get(c, "target", "pose") != "physics") fail(c, "part는 target=\"physics\"에서만 씁니다");
                    if (!ValidCarryBone(get(c, "part")))
                        fail(c, "본 이름은 앞뒤 공백·제어 문자 없이 1~256바이트여야 합니다");
                }
                // The carrier alone puts the target down: a plain one-actor Action that only the carry starts,
                // with one CarryRelease moment (docs/설계-운반-래그돌-내려놓기.md).
                const auto putdown = required(c, "putdown");
                actionRef(c, "putdown");
                auto destination = n.parent().find_child_by_attribute("Action", "id", putdown.c_str());
                if (!destination || !singleActions.contains(putdown) || destination.child("Start") ||
                    destination.child("Flow"))
                    fail(c, "putdown은 시작 입력·조건 분기·방향 클립이 없는 한 사람 Action이어야 합니다");
                if (!destination.child("CarryRelease"))
                    fail(c, "putdown Action에 대상 놓기 시점 <CarryRelease at=\"...\"/>가 필요합니다");
                const auto key = keyRef(c);
                if (!key.given) fail(c, "운반에는 내려놓기 키(input 또는 key·gamepad)가 필요합니다");
                auto& carry = s.carry;
                carry.enabled = true;
                carry.bone = get(c, "bone");
                carry.targetRoot = get(c, "targetRoot");
                carry.putdownAction = putdown;
                carry.key = key.key ? key.key : 0xFFFFFFFF;
                carry.gamepad = key.gamepad;
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
                    if (h.attribute("pose")) fail(h, "Hold pose는 없어졌습니다(형식 4): 들기의 마지막 자세만 씁니다");
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
        }
        if (auto a = one(n, "Advanced")) {
            if (a.attribute("graph"))
                fail(a, "graph는 없어졌습니다(형식 4): 일반 동작은 FullBody, 페어는 RootBehaviorGraph에 들어갑니다");
            shape(a, {"safety", "getToMarkTime", "markErrorThreshold", "reorientSupport", "applyMotionFromRoot"});
            if (a.attribute("safety")) {
                num(a, "safety", 600, 0.001);
                s.safetyDuration = a.attribute("safety").as_float(s.safetyDuration);
            }
            for (auto key : {"getToMarkTime", "markErrorThreshold", "reorientSupport", "applyMotionFromRoot"})
                if (a.attribute(key) && !pair) fail(a, "페어 전용 고급 속성입니다");
            boolean(a, "reorientSupport");
            // 0.12.259: getToMarkTime, markErrorThreshold and applyMotionFromRoot are read and ignored (older files
            // still carry them). The start alignment times itself by the enter blend, the vanilla tolerance never
            // needs changing, and motion from the root turned the start alignment off; the values stay the vanilla
            // pair's (ActionSpec defaults). reorientSupport stays: a pair played from behind needs it off, and the
            // editor sets it from the HKX's start pose.
            if (pair) s.paired.reorientSupport = a.attribute("reorientSupport").as_bool(true);
        }
        std::set<std::string> windows, bindings;
        unsigned index = 0;
        for (auto t : n.children()) {
            const std::string kind = t.name();
            if (kind != "Cancel" && kind != "Connect") continue;
            const bool link = kind == "Connect";
            shape(t,
                link ? Names{"to", "partnerTo", "input", "key", "gamepad", "event", "blend"}
                     : Names{"id", "to", "blend"},
                {"Window"});
            LinkDeclaration connect;
            CancelSpec ws;
            std::string targets;
            KeyRef key;
            if (link) {
                actionRef(t, "to");
                connect.to = get(t, "to");
                num(t, "blend", 5);
                connect.blend = t.attribute("blend");
                if (t.attribute("blend"))
                    for (auto a : n.parent().children())
                        if (get(a, "id") == get(t, "to") && a.child("Flow"))
                            fail(t, "Flow 목적지의 명시적 Connect blend는 아직 지원하지 않습니다");
                if (t.attribute("blend") &&
                    (pair || get(n, "id") == get(t, "to") || !singleActions.contains(get(t, "to"))))
                    fail(t, "Connect blend는 일반 단일 HKX 대상에만 지원합니다");
                if (t.attribute("partnerTo")) {
                    actionRef(t, "partnerTo");
                    if (!pair || actions.at(get(t, "to")) == "pair" || actions.at(get(t, "partnerTo")) == "pair")
                        fail(t, "partnerTo에는 페어에서 양쪽 일반 액션으로 가는 연결만 지정합니다");
                    connect.partner = true;
                    connect.partnerTo = get(t, "partnerTo");
                }
                key = keyRef(t);
                if (key.given == static_cast<bool>(t.attribute("event")))
                    fail(t, "Connect에는 키(input 또는 key·gamepad)와 event 중 하나가 필요합니다");
                if (t.attribute("event")) {
                    eventName(t, required(t, "event"));
                    connect.event = get(t, "event");
                }
            } else {
                // A vanilla cancel's id names its window events; cancel1, cancel2, … unless written.
                auto id = get(t, "id", ("cancel" + std::to_string(++index)).c_str());
                identifier(t, id);
                if (!windows.insert(lower(id)).second) fail(t, "바닐라 전환 ID 중복");
                ws.id = id;
                num(t, "blend", 5);
                ws.blend = t.attribute("blend").as_float(-1.0f);
                auto to = required(t, "to");
                for (auto& name : list(to))
                    if (!has({"vanilla:Attack", "vanilla:PowerAttack", "vanilla:Block", "vanilla:Bash",
                                 "vanilla:Sprint", "vanilla:Jump", "vanilla:Sneak"},
                            name) &&
                        !name.starts_with("event:"))
                        fail(t, "지원하지 않는 취소 대상: " + name);
                targets = to;
            }
            // No <Window>: the whole clip (a key transition or a vanilla cancel).
            WindowTimes times;
            if (auto w = one(t, "Window"))
                times = window(w, link);
            else if (!link)
                times.mode = "always";
            if (t.attribute("event")) {
                auto signature = "event:" + get(t, "event");
                if (!bindings.insert(signature).second) fail(t, "같은 이벤트에 여러 연결이 있어 모호합니다");
            } else if (key.given) {
                std::vector<std::string> channels;
                if (key.key) channels.push_back("key:" + std::to_string(key.key));
                if (key.gamepad) channels.push_back("pad:" + std::to_string(key.gamepad));
                for (auto& channel : channels)
                    if (!bindings.insert(channel).second) fail(t, "같은 키의 연결이 중복되어 모호합니다");
            }
            if (link) {
                connect.key = key.key;
                connect.gamepad = key.gamepad;
                connect.mode = times.mode;
                connect.open = times.open;
                connect.close = times.close;
                connect.timed = times.timed;
                links.push_back(std::move(connect));
                continue;
            }
            // A Cancel (to vanilla actions) window.
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
        trigger.eventCombatTarget = startTarget == "combatTarget";
        if (!trigger.onEvent.empty()) {
            const std::string who = startActor.empty() ? "both" : startActor;
            trigger.onEventPlayer = _stricmp(who.c_str(), "player") == 0 || _stricmp(who.c_str(), "both") == 0;
            trigger.onEventNpc = _stricmp(who.c_str(), "npc") == 0 || _stricmp(who.c_str(), "both") == 0;
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
            if (next.action.empty() || (next.event.empty() && !next.key && !next.gamepad)) continue;
            s.links.push_back(std::move(next));
        }
        s.Finalize();
        if (s.paired.enabled && s.enterBlend < 0) s.enterBlend = 0.2f;
        return s;
    }
};
}
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey) {
    error.clear();
    try {
        auto root = source.document_element();
        std::size_t roots = 0;
        for (auto n : source.children())
            if (n.type() == pugi::node_element) ++roots;
        if (roots != 1) fail(root, "루트 요소는 하나여야 합니다");
        if (std::string_view(root.name()) != "AnimationSet") fail(root, "AnimationSet 루트가 필요합니다");
        const auto version = get(root, "version");
        if (version == "2" || version == "3")
            fail(root,
                "형식 " + version +
                    "은 더 읽지 않습니다: version=\"4\"로 바꾸세요(tools/convert_v4.py가 바꿉니다, docs/계획-문법개편-v4.md)");
        if (version != "4") fail(root, "지원하지 않는 XML 버전");
        if (auto old = root.child("Combo"))
            fail(old,
                "Combo는 없어졌습니다(형식 4): HKX마다 <Action>을 두고 <Connect to=\"다음\" key=\"…\">로 이으세요");
        if (auto old = root.child("External"))
            fail(old, "External은 없어졌습니다(0.12.238): 연결(Connect)·조건 분기는 같은 파일의 동작만 가리킵니다");
        shape(root, {"version", "id", "view"}, {"Inputs", "Action", "Pair", "EventRegistry"});
        identifier(root, required(root, "id"));
        Compiler c{resolveKey};
        std::vector<EventRuleSpec> registry;
        if (auto r = one(root, "EventRegistry")) c.registry(r, sourceName, registry);
        if (root.attribute("view")) {
            const auto view = ParseView(get(root, "view"));
            if (!view) fail(root, "view는 third, first, both 중 하나입니다");
            c.fileView = *view;
        }
        if (auto inputs = one(root, "Inputs")) {
            shape(inputs, {}, {"Input"});
            for (auto i : inputs.children("Input")) c.input(i);
        }
        std::size_t count = 0;
        for (auto a : root.children()) {
            const std::string kind = a.name();
            if (kind == "Inputs" || kind == "EventRegistry" || a.type() != pugi::node_element) continue;
            auto id = required(a, "id");
            identifier(a, id);
            if (!c.actionIds.insert(lower(id)).second) fail(a, "액션 ID 중복");
            if (kind == "Action") c.singleActions.insert(id);
            if (auto carry = a.child("Carry"); kind == "Pair" && carry.attribute("putdown"))
                c.putdownActions.insert(get(carry, "putdown"));
            c.actions.emplace(id, kind == "Pair" ? "pair" : "action");
            c.declared.push_back(id);
            ++count;
        }
        for (const auto& [node, to] : c.eventQteTargets) {
            const auto it = c.actions.find(to);
            if (it == c.actions.end()) fail(node, "정의되지 않은 액션: " + to);
            if (it->second != "action") fail(node, "이벤트 QTE의 to는 일반 동작만 가리킵니다: " + to);
        }
        if ((!count && registry.empty()) || count > 256) fail(root, "1~256개 액션 또는 이벤트 등록이 필요합니다");
        std::vector<ActionSpec> actions;
        for (auto n : root.children())
            if (has({"Action", "Pair"}, n.name())) actions.push_back(c.action(n, sourceName));
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
namespace AnimationInjector::XmlV2 {
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::string& mod, std::string& error, KeyResolver resolveKey) {
    std::vector<EventRuleSpec> events;
    return Compile(source, sourceName, output, events, mod, error, resolveKey);
}
}

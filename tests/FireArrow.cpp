#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/FireArrowState.h"
#include "runtime/FireArrowExecution.h"
#include "core/FireArrowResolver.h"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace AnimationInjector;
namespace {
unsigned checks = 0;
struct FakeEquipment {
    int bow = 1;
    bool leftBow = true;
    std::vector<bool> queried;
    int* GetEquippedObject(bool left) {
        queried.push_back(left);
        return left == leftBow ? &bow : nullptr;
    }
};
struct FakeEngine {
    std::vector<std::string> calls;
    bool available = true, released = false, accept = true, interrupt = false, releaseFires = false;
    bool ReleaseAction() {
        calls.push_back("input");
        available = !interrupt;
        released = releaseFires;
        return accept;
    }
    void CloseCharge() { calls.push_back("charge"); }
    bool Available() { return available; }
    bool ArrowReleased() { return released; }
    void DeliverArrowEvent() {
        calls.push_back("arrow");
        released = true;
    }
};
// The engine side of a direct shot (0.12.268): what it was asked, in order.
struct FakeDirect {
    std::vector<std::string> calls;
    bool bow = true, launches = true, god = false;
    int ammo = 3;
    float damage = 0.0f;
    bool HasBow() {
        calls.push_back("bow");
        return bow;
    }
    int AmmoCount() {
        calls.push_back("ammo");
        return ammo;
    }
    bool Launch(float d) {
        calls.push_back("launch");
        damage = d;
        return launches;
    }
    bool GodMode() { return god; }
    void TakeArrow() {
        calls.push_back("take");
        --ammo;
    }
};
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
std::vector<unsigned char> memory;
std::map<std::uintptr_t, std::uintptr_t> ranges;
bool Range(std::uintptr_t start, std::uintptr_t& end) {
    auto it = ranges.find(start);
    if (it == ranges.end()) return false;
    end = it->second;
    return true;
}
std::uint32_t Read(std::ifstream& f) {
    std::uint32_t v = 0;
    f.read(reinterpret_cast<char*>(&v), 4);
    Check(bool(f), "fixture readable");
    return v;
}
bool Valid(const std::string& slot, const char* kind = "Action") {
    std::string xml = "<AnimationSet version='6' id='Test'><" + std::string(kind) + " id='A' clip='a.hkx'>" + slot +
                      "</" + kind + "></AnimationSet>";
    pugi::xml_document input;
    input.load_string(xml.c_str());
    std::vector<ActionSpec> out;
    std::string mod, error;
    return XmlV2::Compile(input, "test", out, mod, error, [](const char*) -> std::uint32_t { return 34; });
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        spdlog::set_level(spdlog::level::warn);
        Check(argc == 2 || argc == 3, "port path required (native fixture optional)");
        Check(FireArrowDrawReady(RE::ATTACK_STATE_ENUM::kBowAttached),
            "attached bow permits shot independent of transient weapon drawn flag");
        Check(FireArrowDrawReady(RE::ATTACK_STATE_ENUM::kBowDrawn),
            "drawn bow permits shot independent of transient weapon drawn flag");
        for (unsigned state = 0; state <= 15; ++state) {
            if (state == 9 || state == 10) continue;
            Check(!FireArrowDrawReady(static_cast<RE::ATTACK_STATE_ENUM>(state)),
                "reset, released and unrelated attack states reject a late queued shot");
        }
        FakeEquipment equipment;
        auto hands = FireArrowEquipment(equipment);
        Check(hands.first == &equipment.bow && !hands.second && equipment.queried == std::vector<bool>{true, false},
            "native left-slot bow retained alongside right slot");
        equipment.leftBow = false;
        hands = FireArrowEquipment(equipment);
        Check(!hands.first && hands.second == &equipment.bow, "right-slot bow also retained");
        FakeEngine engine;
        auto completion = CompleteFireArrow(true, engine);
        Check(completion.completed && engine.calls == std::vector<std::string>{"input", "charge", "arrow"},
            "automatic shot closes original charge before native arrow handler");
        engine = {};
        completion = CompleteFireArrow(false, engine);
        Check(completion.completed && engine.calls == std::vector<std::string>{"arrow"},
            "early mouse up completes arrow without repeating input or charge");
        engine = {};
        engine.releaseFires = true;
        completion = CompleteFireArrow(true, engine);
        Check(completion.completed && engine.calls == std::vector<std::string>{"input", "charge"},
            "synchronous native arrow is not dispatched twice");
        engine = {};
        engine.interrupt = true;
        completion = CompleteFireArrow(true, engine);
        Check(!completion.completed && engine.calls == std::vector<std::string>{"input", "charge"},
            "death/ragdoll during release prevents arrow delivery");
        engine = {};
        engine.accept = false;
        completion = CompleteFireArrow(true, engine);
        Check(!completion.accepted && completion.completed,
            "input acceptance is separate from native arrow handler completion");
        for (auto slot :
            {"<FireArrow at='0'/>", "<FireArrow at='0.67'/>", "<FireArrow/>", "<FireArrow at='1' onRelease='false'/>"})
            Check(Valid(slot), "valid timing");
        // A direct shot's aim (0.12.271): angles toward the crosshair's target, the ray starting beside the shooter.
        {
            constexpr float pi = 3.14159265f;
            const auto same = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };
            const RE::NiPoint3 o{0, 0, 0};
            auto [x, z] = AimAngles(o, {0, 10, 0});
            Check(same(x, 0) && same(z, 0), "ahead (+Y): level, heading 0");
            std::tie(x, z) = AimAngles(o, {10, 0, 0});
            Check(same(z, pi / 2), "+X: heading quarter turn");
            std::tie(x, z) = AimAngles(o, {0, -10, 0});
            Check(same(z, pi), "behind: heading half turn");
            std::tie(x, z) = AimAngles(o, {-10, 0, 0});
            Check(same(z, 3 * pi / 2), "-X: heading in [0, 2pi)");
            std::tie(x, z) = AimAngles(o, {0, 10, 10});
            Check(same(x, -pi / 4), "up is a negative pitch");
            const auto start = AimRayStart({0, -300, 100}, {0, 1, 0}, {0, 0, 100});
            Check(same(start.y, 0) && same(start.z, 100), "the ray starts beside the shooter, not behind them");
            const auto ahead = AimRayStart({0, 300, 100}, {0, 1, 0}, {0, 0, 100});
            Check(same(ahead.y, 300), "a camera already past the shooter starts at the camera");
        }
        // A direct shot: launched at full power, its damage scaled, one arrow taken (not in god mode).
        {
            FakeDirect direct;
            Check(CompleteDirectShot(0.8f, direct) == DirectShotResult::kLaunched &&
                      direct.calls == std::vector<std::string>{"bow", "ammo", "launch", "take"} && direct.ammo == 2 &&
                      direct.damage == 0.8f,
                "direct shot launches with the damage and takes one arrow");
            direct = {};
            direct.god = true;
            Check(CompleteDirectShot(1.0f, direct) == DirectShotResult::kLaunched && direct.ammo == 3,
                "god mode keeps the arrow");
            direct = {};
            direct.ammo = 0;
            Check(CompleteDirectShot(1.0f, direct) == DirectShotResult::kNoAmmo &&
                      direct.calls == std::vector<std::string>{"bow", "ammo"},
                "no arrow: nothing launched");
            direct = {};
            direct.bow = false;
            Check(CompleteDirectShot(1.0f, direct) == DirectShotResult::kNoBow && direct.calls.size() == 1,
                "no bow: nothing asked further");
            direct = {};
            direct.launches = false;
            Check(CompleteDirectShot(1.0f, direct) == DirectShotResult::kLaunchFailed && direct.ammo == 3,
                "a failed launch takes no arrow");
        }
        // 0.12.283: the engine set the damage at the no-draw power (0.35 in game): the percent is of a full draw's.
        {
            auto same = [](float a, float b) { return std::fabs(a - b) < 0.001f; };
            Check(same(DirectShotDamage(8.32f, 0.35f, 0.5f), 8.32f / 0.35f * 0.5f), "50% of a full draw's damage");
            Check(same(DirectShotDamage(8.32f, 0.35f, 1.0f), 8.32f / 0.35f), "100%: a full draw's damage");
            Check(same(DirectShotDamage(20.0f, 1.0f, 1.5f), 30.0f), "already at full power: as launched");
            Check(same(DirectShotDamage(20.0f, 0.0f, 1.0f), 20.0f), "no power read: as launched");
        }
        for (auto slot : {"<FireArrow at='0.6' mode='direct'/>", "<FireArrow at='0.6' mode='direct' damage='80'/>",
                 "<FireArrow at='0.6' mode='direct' damage='1000'/>", "<FireArrow mode='drawn'/>"})
            Check(Valid(slot), "valid direct shot");
        for (auto slot : {"<FireArrow mode='direct'/>", "<FireArrow at='0.6' mode='direct' onRelease='true'/>",
                 "<FireArrow at='0.6' damage='80'/>", "<FireArrow at='0.6' mode='direct' damage='0'/>",
                 "<FireArrow at='0.6' mode='direct' damage='1001'/>", "<FireArrow at='0.6' mode='full'/>"})
            Check(!Valid(slot), "invalid direct shot rejected");
        // An event shot is a registry function (EventRegistry fireArrow), not an action slot.
        for (auto slot : {"<FireArrow onRelease='false'/>", "<FireArrow at=''/>", "<FireArrow at='-1'/>",
                 "<FireArrow at='NaN'/>", "<FireArrow at='inf'/>", "<FireArrow at='end'/>", "<FireArrow at='event:'/>",
                 "<FireArrow at='event:DodgeShotFire'/>", "<FireArrow at='0' fallback='1'/>",
                 "<FireArrow at='0' actor='npc'/>", "<FireArrow at='0'/><FireArrow at='1'/>"})
            Check(!Valid(slot), "invalid timing/duplicate rejected");
        Check(!Valid("<FireArrow at='0'/>", "Pair"), "Pair explicitly rejected");
        pugi::xml_document timings;
        Check(timings.load_string(R"(<AnimationSet version="6" id="FireTiming">
          <Action id="Seconds" clip="a.hkx"><FireArrow at="0.67" onRelease="false"/></Action>
          <Action id="Release" clip="a.hkx"><FireArrow/></Action>
          <Action id="First" clip="a.hkx"><FireArrow at="0.5"/><Transition to="Second" key="E"/></Action>
          <Action id="Second" clip="b.hkx"/>
        </AnimationSet>)"),
            "timing fixture XML");
        const auto timingPath = std::filesystem::temp_directory_path() / "AnimationInjectorFireArrow-timing-cases.xml";
        Check(timings.save_file(timingPath.c_str()), "timing fixture saved");
        std::vector<ActionSpec> timed;
        Check(Declarations::LoadXmlFile(timingPath.c_str(), timed) && timed.size() == 4,
            "timings pass actual declarations loader");
        Check(timed[0].fireArrow.enabled && std::fabs(timed[0].fireArrow.at - 0.67f) < 0.0001f &&
                  !timed[0].fireArrow.onRelease,
            "numeric runtime timing retained");
        Check(timed[1].fireArrow.enabled && timed[1].fireArrow.at < 0 && timed[1].fireArrow.onRelease,
            "release-only shot");
        Check(timed[2].fireArrow.enabled && std::fabs(timed[2].fireArrow.at - 0.5f) < 0.0001f &&
                  !timed[3].fireArrow.enabled,
            "a connected action keeps its own slots");
        pugi::xml_document directDoc;
        Check(directDoc.load_string(R"(<AnimationSet version="6" id="FireDirect">
          <EventRegistry>
            <Event name="ShotA"><Do op="fireArrow"/></Event>
            <Event name="ShotB"><Do op="fireArrow"/></Event>
          </EventRegistry>
          <Action id="Timed" clip="a.hkx"><FireArrow at="0.6" mode="direct" damage="150"/>
            <EventValues><Event name="ShotA"><Do op="fireArrow" mode="direct" damage="80"/></Event></EventValues></Action>
          <Start id="Pick" key="E" to="Timed"><Check kind="sneaking"/></Start>
        </AnimationSet>)"),
            "direct fixture XML");
        const auto directPath = std::filesystem::temp_directory_path() / "AnimationInjectorFireArrow-direct.xml";
        Check(directDoc.save_file(directPath.c_str()), "direct fixture saved");
        {
            std::vector<ActionSpec> direct;
            std::vector<EventRuleSpec> rules;
            Check(
                Declarations::LoadXmlFile(directPath.c_str(), direct, rules) && direct.size() == 2 && rules.size() == 2,
                "direct shots pass the declarations loader");
            const auto& f = direct[0].fireArrow;
            Check(f.shot.mode == FireArrowMode::kDirect && std::fabs(f.shot.damage - 1.5f) < 0.0001f && !f.onRelease &&
                      std::fabs(f.at - 0.6f) < 0.0001f,
                "a timed direct shot: percent read as a multiplier, no release shot");
            // 0.12.281: the event shot's way is the action's: the registry holds a drawn shot by default.
            Check(rules[0].functions[0].arrow == FireArrowShot{} && direct[0].eventValues.size() == 1 &&
                      direct[0].eventValues[0].functions[0].arrow.mode == FireArrowMode::kDirect &&
                      std::fabs(direct[0].eventValues[0].functions[0].arrow.damage - 0.8f) < 0.0001f,
                "an event's direct shot is the action's");
            Check(rules[1].functions[0].arrow == FireArrowShot{}, "no mode: the drawn shot as before");
            Check(!direct[0].SelectsOnly(), "an action with its own clip plays it");
            // 0.12.269: a selecting action passes the injected-graph start check (it is never injected).
            Check(direct[1].SelectsOnly(), "an action with only a Flow selects");
        }
        std::vector<ActionSpec> specs;
        std::vector<EventRuleSpec> events;
        Check(Declarations::LoadXmlFile(argv[1], specs, events), "DDS port parses");
        Check(specs.size() == 7, "DDS dispatcher and six direction actions");
        for (const auto& spec : specs)
            if (!spec.SelectsOnly())  // the dispatcher only picks (format 5: a start into a flow)
                Check(spec.fireArrow.enabled && spec.fireArrow.onRelease && spec.fireArrow.at < 0, "release shot kept");
        const auto shot =
            std::find_if(events.begin(), events.end(), [](const auto& e) { return e.name == "DodgeShotFire"; });
        Check(shot != events.end() && shot->functions.size() == 1 && shot->functions[0].op == EventOp::kFireArrow &&
                  shot->functions[0].target == EventTarget::kSender,
            "the event shot is a registry function");
        DeclarationCatalog catalog;
        catalog.Build(specs, {"DodgeShotFire"});
        Check(std::count(catalog.events.begin(), catalog.events.end(), "DodgeShotFire") == 1,
            "the registry alone registers the annotation");
        FireArrowStep step;
        FireArrowCycle cycle;
        Check(!cycle.CanRelease(cycle.serial), "no shot on load without draw");
        cycle.Press();
        step.Reset(cycle.serial);
        step.Request();
        step.Request();
        Check(step.Take(), "event and fallback enqueue one request");
        Check(!step.Take(), "single consumption");
        step.Request();
        Check(!step.Take(), "late annotation cannot repeat fallback");
        Check(cycle.Automatic(step.draw), "first release accepted");
        Check(!cycle.Automatic(step.draw), "second step cannot shoot same physical draw twice");
        Check(!cycle.NativeRelease() && !cycle.NativeRelease(), "later physical up cannot repeat automatic release");
        cycle.Press();
        step.Reset(cycle.serial);
        Check(cycle.NativeRelease(), "manual early release passes");
        step.Request();
        Check(step.Take() && !cycle.Automatic(step.draw), "later annotation skips manually fired arrow");
        Check(cycle.Claim(step.draw) == FireArrowCycle::Shot::kManual,
            "native mouse release still needs the renamed HKX arrow handler");
        Check(
            cycle.Claim(step.draw) == FireArrowCycle::Shot::kNone, "later annotation cannot repeat manual completion");
        cycle.Press();
        step.Reset(cycle.serial);
        Check(cycle.Claim(step.draw) == FireArrowCycle::Shot::kAutomatic,
            "held native press permits automatic completion without reading transient held bits");
        Check(!cycle.NativeRelease(), "automatic completion does not repeat on physical up");
        cycle.Press();
        step.Reset(cycle.serial);
        cycle.ObserveArrow();
        Check(cycle.Claim(step.draw) == FireArrowCycle::Shot::kNone, "native arrow event already completed this draw");
        cycle.Reset();
        Check(cycle.NativeRelease(), "unmatched mouse up passes on load");
        Check(cycle.Claim(cycle.serial) == FireArrowCycle::Shot::kNone, "unmatched mouse up cannot create an arrow");
        cycle.Press();
        step.Reset(cycle.serial);
        step.Request();
        step.Cancel();
        Check(!step.Take(), "finish/cancel retires queued event");
        step.Reset(cycle.serial);
        step.Request();
        Check(step.Take(), "next step resets one-shot slot");
        cycle.Press();
        Check(!cycle.CanRelease(step.draw), "old step cannot fire a new draw");
        step.Reset(cycle.serial);
        cycle.Reset();
        Check(!cycle.CanRelease(step.draw), "load invalidates outstanding draw");
        Check(cycle.NativeRelease(), "load clears duplicate guard without injecting input");
        // 0.12.272: a draw the clip made (no press) shoots once the caller sees the drawn state.
        cycle.Reset();
        Check(cycle.Claim(cycle.serial) == FireArrowCycle::Shot::kNone && cycle.ClipDraw(cycle.serial),
            "a draw the clip made shoots without a press");
        cycle.Press();
        step.Reset(cycle.serial);
        Check(!cycle.ClipDraw(step.draw), "a held press is not a clip draw");
        cycle.NativeRelease();
        Check(!cycle.ClipDraw(step.draw), "an early release waiting for its arrow is not a clip draw");
        Check(cycle.Claim(step.draw) == FireArrowCycle::Shot::kManual, "that release still completes as manual");
        Check(!cycle.ClipDraw(step.draw + 1), "another draw's ticket is refused");
        // The native fixture is captured game code, kept out of the public source.
        if (argc == 2) {
            std::cout << "FireArrow checks passed: " << checks << " (native AE fixture absent: skipped)\n";
            return 0;
        }
        std::ifstream input(argv[2], std::ios::binary);
        Check(bool(input), "native fixture opened");
        Check(Read(input) == 0x46495245, "fixture magic");
        const auto start = Read(input), press = Read(input), release = Read(input), dispatch = Read(input),
                   finish = Read(input), count = Read(input);
        memory.resize(0x1000000);
        const auto base = reinterpret_cast<std::uintptr_t>(memory.data());
        for (unsigned i = 0; i < count; ++i) {
            const auto rva = Read(input), size = Read(input);
            Check(rva + size < memory.size(), "bounded body");
            input.read(reinterpret_cast<char*>(memory.data() + rva), size);
            Check(bool(input), "native code read");
            ranges[base + rva] = base + rva + size;
        }
        const auto result = FireArrowResolver::Resolve(base + start, Range);
        Check(result && result.pressCall == base + press && result.releaseCall == base + release &&
                  result.dispatch == base + dispatch && result.finishCharge == base + finish,
            "real AE engine sequence resolves exactly");
        Check(FireArrowResolver::Validate(result, base + 0x5b40b0, Range),
            "native source action pipeline and charge fingerprint verified");
        Check(!FireArrowResolver::Validate(result, base + start, Range),
            "unrelated function cannot certify action dispatch");
        const auto chargeByte = memory[finish + 0x15];
        memory[finish + 0x15] = 0x90;
        Check(!FireArrowResolver::Validate(result, base + 0x5b40b0, Range), "changed charge finalizer rejected");
        memory[finish + 0x15] = chargeByte;
        const auto saved = memory[release];
        memory[release] = 0x90;
        Check(!FireArrowResolver::Resolve(base + start, Range), "modified release site rejected");
        memory[release] = saved;
        memory[release + 1] ^= 1;
        Check(!FireArrowResolver::Validate(result, base + 0x5b40b0, Range), "prepatch target change rejected");
        memory[release + 1] ^= 1;
        ranges.erase(base + finish);
        Check(!FireArrowResolver::Resolve(base + start, Range), "unverified charge function rejected");
        Check(!FireArrowResolver::Resolve(base + start, nullptr), "missing code reader rejected");
        std::cout << "FireArrow checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}

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
    std::string xml = "<AnimationSet version='4' id='Test'><" + std::string(kind) + " id='A' clip='a.hkx'>" + slot +
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
        // An event shot is a registry function (EventRegistry fireArrow), not an action slot.
        for (auto slot : {"<FireArrow onRelease='false'/>", "<FireArrow at=''/>", "<FireArrow at='-1'/>",
                 "<FireArrow at='NaN'/>", "<FireArrow at='inf'/>", "<FireArrow at='end'/>", "<FireArrow at='event:'/>",
                 "<FireArrow at='event:DodgeShotFire'/>", "<FireArrow at='0' fallback='1'/>",
                 "<FireArrow at='0' actor='npc'/>", "<FireArrow at='0'/><FireArrow at='1'/>"})
            Check(!Valid(slot), "invalid timing/duplicate rejected");
        Check(!Valid("<FireArrow at='0'/>", "Pair"), "Pair explicitly rejected");
        pugi::xml_document timings;
        Check(timings.load_string(R"(<AnimationSet version="4" id="FireTiming">
          <Action id="Seconds" clip="a.hkx"><FireArrow at="0.67" onRelease="false"/></Action>
          <Action id="Release" clip="a.hkx"><FireArrow/></Action>
          <Action id="First" clip="a.hkx"><FireArrow at="0.5"/><Connect to="Second" key="E"/></Action>
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
        std::vector<ActionSpec> specs;
        std::vector<EventRuleSpec> events;
        Check(Declarations::LoadXmlFile(argv[1], specs, events), "DDS port parses");
        Check(specs.size() == 7, "DDS dispatcher and six direction actions");
        for (const auto& spec : specs)
            Check(spec.fireArrow.enabled && spec.fireArrow.onRelease && spec.fireArrow.at < 0, "release shot kept");
        const auto shot =
            std::find_if(events.begin(), events.end(), [](const auto& e) { return e.name == "DodgeShotFire"; });
        Check(shot != events.end() && shot->functions.size() == 1 && shot->functions[0].op == EventOp::kFireArrow &&
                  shot->functions[0].from == EventFrom::kPlayer,
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

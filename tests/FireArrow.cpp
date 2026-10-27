#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/FireArrowExecution.h"
#include <iostream>
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
        Check(argc == 2, "port path required");
        FakeEquipment equipment;
        auto hands = FireArrowEquipment(equipment);
        Check(hands.first == &equipment.bow && !hands.second && equipment.queried == std::vector<bool>{true, false},
            "native left-slot bow retained alongside right slot");
        equipment.leftBow = false;
        hands = FireArrowEquipment(equipment);
        Check(!hands.first && hands.second == &equipment.bow, "right-slot bow also retained");
        // 0.12.303: a shot is the direct one, at its event or clip time; its damage is its only setting.
        for (auto slot : {"<Do at='0' op='fireArrow'/>", "<Do at='0.67' op='fireArrow'/>",
                 "<Do at='0.6' op='fireArrow' damage='80'/>", "<Do at='0.6' op='fireArrow' damage='1000'/>",
                 "<Do at='0.2' op='fireArrow'/><Do at='0.6' op='fireArrow' damage='50'/>",
                 "<EventValues><Event name='Fire'><Do op='fireArrow' damage='50'/></Event></EventValues>"})
            Check(Valid(slot), "valid shot");
        for (auto slot :
            {"<FireArrow/>", "<Do op='fireArrow'/>", "<Do at='' op='fireArrow'/>", "<Do at='-1' op='fireArrow'/>",
                "<Do at='end' op='fireArrow'/>", "<Do at='0.6' op='fireArrow' damage='0'/>",
                "<Do at='0.6' op='fireArrow' damage='1001'/>", "<Do at='0.6' op='fireArrow' mode='direct'/>",
                "<Do at='0.6' op='fireArrow' onRelease='true'/>", "<Do at='0.6' op='fireArrow' delay='0.1'/>",
                "<EventValues><Event name='Fire'><Do op='fireArrow' delay='0.1'/></Event></EventValues>"})
            Check(!Valid(slot), "invalid shot rejected");
        Check(!Valid("<Do at='0' op='fireArrow'/>", "Pair"), "Pair explicitly rejected");
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
        pugi::xml_document directDoc;
        Check(directDoc.load_string(R"(<AnimationSet version="6" id="FireDirect">
          <EventRegistry>
            <Event name="ShotA"><Do op="fireArrow"/></Event>
            <Event name="ShotB"><Do op="fireArrow"/></Event>
          </EventRegistry>
          <Action id="Timed" clip="a.hkx"><Do at="0.6" op="fireArrow" damage="150"/>
            <EventValues><Event name="ShotA"><Do op="fireArrow" damage="80"/></Event></EventValues></Action>
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
                "shots pass the declarations loader");
            const auto& timed = direct[0].pairFeatures.timed;
            Check(timed.size() == 1 && timed[0].function.op == EventOp::kFireArrow &&
                      std::fabs(timed[0].function.arrowDamage - 1.5f) < 0.0001f &&
                      std::fabs(timed[0].at - 0.6f) < 0.0001f,
                "a clip-time shot: a timed effect, its percent read as a multiplier");
            // 0.12.281: the registry holds the shot, the action its damage there.
            Check(rules[0].functions[0].arrowDamage == 1.0f && direct[0].eventValues.size() == 1 &&
                      std::fabs(direct[0].eventValues[0].functions[0].arrowDamage - 0.8f) < 0.0001f,
                "an event's shot damage is the action's");
            Check(!direct[0].SelectsOnly(), "an action with its own clip plays it");
            // 0.12.269: a selecting action passes the injected-graph start check (it is never injected).
            Check(direct[1].SelectsOnly(), "an action with only a Flow selects");
        }
        std::vector<ActionSpec> specs;
        std::vector<EventRuleSpec> events;
        Check(Declarations::LoadXmlFile(argv[1], specs, events), "DDS port parses");
        Check(specs.size() == 7, "DDS dispatcher and six direction actions");
        const auto shot =
            std::find_if(events.begin(), events.end(), [](const auto& e) { return e.name == "DodgeShotFire"; });
        Check(shot != events.end() && shot->functions.size() == 1 && shot->functions[0].op == EventOp::kFireArrow &&
                  shot->functions[0].target == EventTarget::kSender,
            "the event shot is a registry function");
        DeclarationCatalog catalog;
        catalog.Build(specs, {"DodgeShotFire"});
        Check(std::count(catalog.events.begin(), catalog.events.end(), "DodgeShotFire") == 1,
            "the registry alone registers the annotation");
        std::cout << "FireArrow checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}

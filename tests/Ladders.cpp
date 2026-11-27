// <Locomotion> (runtime/LadderSpec.h, docs/설계-이동사다리-완충칸.md section 7): what the parser reads, what it rejects,
// which file's is used (the locomotion file's alone), and the gait a split blender follows (runtime/GaitShare.h).
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/EventRegistrySpec.h"
#include "runtime/GaitShare.h"
#include "runtime/LadderSpec.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace AnimationInjector;
namespace {
unsigned checks = 0;
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
struct Parsed {
    bool ok = false;
    std::vector<ActionSpec> actions;
    std::vector<EventRuleSpec> events;
    LocomotionSpec locomotion;
    TreeUse Use(TreeWeapon w) const { return locomotion.Use(w); }
    bool Applies(LadderSet s) const { return locomotion.Applies(s); }
    std::string error;
};
Parsed Parse(const std::string& body) {
    const std::string xml = "<AnimationSet version='6' id='Test'>" + body + "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::string mod;
    p.ok =
        XmlV2::Compile(doc, "test.xml", p.actions, p.events, p.locomotion, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
bool Rejected(const std::string& body) { return !Parse(body).ok; }
bool Near(float a, float b, float within = 1e-3f) { return std::fabs(a - b) < within; }
void WriteFile(const std::filesystem::path& file, const std::string& id, const std::string& body) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<AnimationSet version=\"6\" id=\"" << id << "\">" << body
        << "</AnimationSet>\n";
}
// The share after `seconds` of 60 fps frames in one state.
float Run(float share, bool running, float speed, float walk, float seconds) {
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f)
        share = Gait::NextShare(share, running, speed, walk, 1.0f / 60.0f);
    return share;
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / (L"ai-ladders-" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    try {
        // Names: XML values, blender names, case-sensitive values.
        Check(LadderBlenderName(LadderSet::k1HM, LadderDirection::kForward) == "1HM_ForwardBlend", "1HM forward");
        Check(LadderBlenderName(LadderSet::kSheathed, LadderDirection::kBackwardLeft) == "MT_BackwardLeftBlend",
            "Sheathed is the MT_ prefix");
        Check(LadderBlenderName(LadderSet::kMagicCast, LadderDirection::kForwardRight) == "MagicCast_ForwardRightBlend",
            "MagicCast forward right");
        Check(
            ParseLadderSet("CrossBow") == LadderSet::kCrossBow && !ParseLadderSet("crossbow") && !ParseLadderSet("MT"),
            "set values are the XML names, case-sensitive");
        Check(LadderNames::kSets.size() == kLadderSetCount, "the set count");
        for (std::size_t i = 0; i < LadderNames::kSets.size(); ++i)
            Check(ParseLadderSet(LadderSetName(static_cast<LadderSet>(i))) == static_cast<LadderSet>(i),
                "every set name round-trips");
        for (std::size_t i = 0; i < LadderNames::kDirections.size(); ++i)
            Check(ParseLadderDirection(LadderDirectionName(static_cast<LadderDirection>(i))) ==
                      static_cast<LadderDirection>(i),
                "every direction name round-trips");
        Check(LadderBlenderName(LadderSet::kBlock2HM, LadderDirection::kLeft) == "2HM_LeftBlend" &&
                  LadderBlenderName(LadderSet::kBlockSneak, LadderDirection::kBackward) == "Sneak_BackwardBlend" &&
                  LadderBlenderName(LadderSet::kBlock2HW, LadderDirection::kRight) == "2HW_RightBlend" &&
                  LadderBlenderName(LadderSet::kH2H, LadderDirection::kForward) == "H2H_ForwardBlend",
            "Block sets name the ordinary blenders; the own trees' sets their own");
        Check(LadderInBlock(LadderSet::kBlock1HM) && LadderInBlock(LadderSet::kBlockH2H) &&
                  !LadderInBlock(LadderSet::kSneak) && !LadderInBlock(LadderSet::k2HM),
            "only the Block sets are the block graph's");
        Check(TreeKindOf(TreeWeapon::kH2H).from == "1HM" && TreeKindOf(TreeWeapon::kH2H).handType == 0 &&
                  TreeKindOf(TreeWeapon::k2HW).from == "2HM" && TreeKindOf(TreeWeapon::k2HW).handType == 6 &&
                  TreeKindOf(TreeWeapon::kH2H).set == LadderSet::kH2H &&
                  TreeKindOf(TreeWeapon::k2HW).blockSet == LadderSet::kBlock2HW,
            "what each weapon's tree copies");

        // An empty <Locomotion/> is every set of the vanilla trees; no file declares nothing.
        {
            const auto none = Parse(R"(<Action id="A" clip="a.hkx"/>)");
            Check(none.ok && none.locomotion.Empty() && !none.Applies(LadderSet::k1HM), "no <Locomotion>: vanilla");
            const auto all = Parse("<Locomotion/>");
            Check(all.ok && all.actions.empty() && all.events.empty() && !all.locomotion.Empty(),
                "a locomotion-only file parses");
            Check(all.locomotion.sourceName == "test.xml", "it names its file");
            for (const auto set : {LadderSet::k1HM, LadderSet::k2HM, LadderSet::kBow, LadderSet::kCrossBow,
                     LadderSet::kMagic, LadderSet::kMagicCast, LadderSet::kSheathed, LadderSet::kSneak,
                     LadderSet::kBlock1HM, LadderSet::kBlock2HM, LadderSet::kBlockBow, LadderSet::kBlockSneak})
                Check(all.Applies(set), "every vanilla set applies");
            Check(!all.Applies(LadderSet::k2HW) && !all.Applies(LadderSet::kBlock2HW) &&
                      !all.Applies(LadderSet::kH2H) && !all.Applies(LadderSet::kBlockH2H),
                "an own tree's sets need the tree");
            Check(all.Use(TreeWeapon::k2HW) == TreeUse::kVanilla && all.Use(TreeWeapon::kH2H) == TreeUse::kVanilla,
                "trees default to vanilla");
            const auto with = Parse(R"(<Action id="A" clip="a.hkx"/><Locomotion/>)");
            Check(with.ok && with.actions.size() == 1 && with.Applies(LadderSet::kBow), "next to an action");
        }
        // <Stance side from full walk run> (11-23).
        {
            const auto p = Parse(
                R"(<Locomotion><Stance side="right" walk="Animations\bowdrawn_walk"/><Stance side="left" from="10" full="30" run="Animations\L_run"/></Locomotion>)");
            Check(p.ok, p.error.c_str());
            const auto& right = p.locomotion.Stance(StanceSide::kRight);
            const auto& left = p.locomotion.Stance(StanceSide::kLeft);
            Check(right.declared && Near(right.from, 25.0f) && Near(right.full, 45.0f) &&
                      right.walk == "Animations\\bowdrawn_walk" && right.run.empty(),
                "a right stance with its default range");
            Check(left.declared && Near(left.from, 10.0f) && Near(left.full, 30.0f) && left.walk.empty() &&
                      left.run == "Animations\\L_run",
                "a left stance with its range and run");
            Check(p.locomotion.AnyStance() && p.Applies(LadderSet::k1HM), "a stance keeps every set split");
            Check(StanceFile(right.walk, LadderDirection::kForwardLeft) == "Animations\\bowdrawn_walkForwardLeft.hkx",
                "the direction is appended to the prefix");
            Check(StanceFile("", LadderDirection::kForward).empty(), "no prefix, no file");
            Check(Near(StanceShare(right, 20.0f), 0.0f) && Near(StanceShare(right, 35.0f), 0.5f) &&
                      Near(StanceShare(right, 60.0f), 1.0f),
                "the share rises from `from` to `full`");
            Check(LocomotionSpec::StanceSet(LadderSet::k1HM) && LocomotionSpec::StanceSet(LadderSet::kSheathed) &&
                      !LocomotionSpec::StanceSet(LadderSet::kSneak) && !LocomotionSpec::StanceSet(LadderSet::kBlockBow),
                "not while sneaking or blocking");
            Check(!Parse(R"(<Locomotion/>)").locomotion.AnyStance(), "no stance by default");
            Check(Rejected(R"(<Locomotion><Stance side="up" walk="a"/></Locomotion>)"), "side is right or left");
            Check(Rejected(R"(<Locomotion><Stance side="right"/></Locomotion>)"), "a walk or a run is needed");
            Check(Rejected(R"(<Locomotion><Stance side="right" walk="a" from="40" full="30"/></Locomotion>)"),
                "full above from");
            Check(Rejected(R"(<Locomotion><Stance side="right" walk="a"/><Stance side="right" run="b"/></Locomotion>)"),
                "one per side");
            Check(Rejected(R"(<Locomotion><Stance side="right" walk="Animations\a_walkforward.hkx"/></Locomotion>)"),
                "a prefix, not a file");
            Check(
                Rejected(R"(<Locomotion><Stance side="right" walk="a" set="Bow"/></Locomotion>)"), "unknown attribute");
        }
        // <Except set>.
        {
            const auto p = Parse(R"(<Locomotion><Except set="Sneak"/><Except set="BlockSneak"/></Locomotion>)");
            Check(p.ok && !p.Applies(LadderSet::kSneak) && !p.Applies(LadderSet::kBlockSneak) &&
                      p.Applies(LadderSet::k1HM) && p.Applies(LadderSet::kBlock1HM),
                "excepted sets stay vanilla, the others apply");
            Check(Rejected(R"(<Locomotion><Except set="sneak"/></Locomotion>)"), "set is case-sensitive");
            Check(Rejected(R"(<Locomotion><Except set="MT"/></Locomotion>)"), "a blender prefix is no set");
            Check(Rejected(R"(<Locomotion><Except/></Locomotion>)"), "set is required");
            Check(Rejected(R"(<Locomotion><Except set="Bow" direction="left"/></Locomotion>)"), "unknown attribute");
            Check(Rejected(R"(<Locomotion><Except set="Bow"/><Except set="Bow"/></Locomotion>)"), "one per set");
        }
        // <Tree weapon use>.
        {
            const auto shared = Parse(R"(<Locomotion><Tree weapon="2HW" use="2HM"/></Locomotion>)");
            Check(shared.ok && shared.Use(TreeWeapon::k2HW) == TreeUse::kShared &&
                      shared.Use(TreeWeapon::kH2H) == TreeUse::kVanilla && !shared.Applies(LadderSet::k2HW) &&
                      shared.Applies(LadderSet::k2HM),
                "use=2HM: the 2HM blenders are the axes' too");
            const auto own = Parse(R"(<Locomotion><Tree weapon="2HW" use="own"/><Tree weapon="H2H" use="own"/>
                <Except set="BlockH2H"/></Locomotion>)");
            Check(own.ok && own.Use(TreeWeapon::k2HW) == TreeUse::kOwn && own.Use(TreeWeapon::kH2H) == TreeUse::kOwn &&
                      own.Applies(LadderSet::k2HW) && own.Applies(LadderSet::kBlock2HW) &&
                      own.Applies(LadderSet::kH2H) && !own.Applies(LadderSet::kBlockH2H),
                "own trees bring their sets");
            Check(Rejected(R"(<Locomotion><Tree weapon="2hw" use="own"/></Locomotion>)"), "weapon is case-sensitive");
            Check(Rejected(R"(<Locomotion><Tree weapon="1HM" use="own"/></Locomotion>)"), "weapon is 2HW or H2H");
            Check(Rejected(R"(<Locomotion><Tree weapon="2HW" use="true"/></Locomotion>)"), "use is own or 2HM");
            Check(Rejected(R"(<Locomotion><Tree weapon="H2H" use="2HM"/></Locomotion>)"), "use 2HM is for 2HW only");
            Check(Rejected(R"(<Locomotion><Tree weapon="2HW"/></Locomotion>)"), "use is required");
            Check(Rejected(R"(<Locomotion><Tree weapon="2HW" use="own"/><Tree weapon="2HW" use="2HM"/></Locomotion>)"),
                "one tree per weapon in a file");
        }
        // The earlier formats are not read.
        Check(Rejected(
                  R"(<Locomotion><Ladder set="1HM" direction="forward" walkUntil="111" runFrom="325"/></Locomotion>)"),
            "ladder rows are gone");
        Check(Rejected(R"(<Locomotion smoothing="0.1"/>)"), "smoothing is gone");
        Check(Rejected(R"(<Locomotion runUpperBody="true"/>)"), "runUpperBody is gone");
        Check(Rejected(R"(<Locomotion axes="own"/>)"), "axes is gone");
        Check(Rejected("<Locomotion/><Locomotion/>"), "one Locomotion per file");
        Check(Rejected(R"(<Locomotion><Rung set="1HM"/></Locomotion>)"), "unknown child");
        Check(Rejected(R"(<Locomotion>text</Locomotion>)"), "text in Locomotion");
        Check(Rejected(""), "a file with nothing is still rejected");

        // The gait (runtime/GaitShare.h). Walk speed 80, run 350 (forward) and 195 (backward).
        {
            using namespace Gait;
            const float dt = 1.0f / 60.0f;
            Check(Near(Run(0.0f, false, 80.0f, 80.0f, 2.0f), 0.0f), "a steady walk is the walk");
            Check(Near(Run(1.0f, true, 350.0f, 80.0f, 2.0f), 1.0f), "a steady run is the run");
            Check(Near(Run(1.0f, true, 195.0f, 80.0f, 2.0f), 1.0f), "a steady backward run is the run");
            Check(
                Near(Run(0.0f, false, 104.0f, 104.0f, 2.0f), 0.0f) && Near(Run(1.0f, true, 254.0f, 104.0f, 2.0f), 1.0f),
                "whatever the walk speed is (SpeedMult 130)");
            // Run -> walk: the state flips while the speed is still the run's; nothing moves until the speed falls.
            Check(Near(NextShare(1.0f, false, 350.0f, 80.0f, dt), 1.0f), "walking at run speed: still the run");
            Check(Near(NextShare(1.0f, false, 200.0f, 80.0f, dt), 1.0f), "above 2.2x the walk speed: still the run");
            const float easing = NextShare(1.0f, false, 80.0f, 80.0f, dt);
            Check(easing < 1.0f && easing > 0.8f, "at the walk speed it eases, not jumps");
            Check(Run(1.0f, false, 80.0f, 80.0f, 0.12f) < 0.45f && Run(1.0f, false, 80.0f, 80.0f, 0.12f) > 0.3f,
                "about a third left after one time constant");
            Check(Near(Run(1.0f, false, 80.0f, 80.0f, 1.0f), 0.0f), "and lands on the walk");
            // Walk -> run.
            const float rising = Run(0.0f, true, 350.0f, 80.0f, 0.06f);
            Check(rising > 0.55f && rising < 0.72f, "a walk eases into a run with the short constant");
            Check(Near(Run(0.0f, true, 350.0f, 80.0f, 0.6f), 1.0f), "and lands on the run");
            // A stop: running, the speed falling to 0.
            Check(Near(NextShare(1.0f, true, 144.0f, 80.0f, dt), 1.0f), "1.8x the walk speed: the run");
            Check(Near(NextShare(1.0f, true, 100.0f, 80.0f, dt), 1.0f - dt / kGaitSeconds), "below: linear");
            Check(Near(Run(1.0f, true, 81.0f, 80.0f, 1.0f), kStopRunShare), "a stop keeps the stop's share of the run");
            Check(Near(NextShare(0.6f, true, 0.0f, 80.0f, dt), 0.6f - dt / kGaitSeconds),
                "standing above it: the way down goes on");
            Check(Near(Run(0.6f, true, 0.0f, 80.0f, 1.0f), kStopRunShare) &&
                      Near(Run(0.6f, false, 0.0f, 80.0f, 1.0f), kStopRunShare),
                "and stops there, whatever the state says");
            Check(Near(Run(0.1f, false, 0.0f, 80.0f, 1.0f), 0.1f) && Near(Run(0.0f, true, 0.0f, 80.0f, 1.0f), 0.0f),
                "standing below it: the share stays");
            Check(Near(NextShare(1.4f, true, 350.0f, 80.0f, dt), 1.0f) &&
                      NextShare(-0.3f, false, 80.0f, 80.0f, dt) >= 0.0f,
                "out of range shares are clamped");
            Check(Near(Run(0.0f, true, 300.0f, 0.0f, 1.0f), 1.0f) && Near(Run(1.0f, false, 300.0f, 0.0f, 1.0f), 0.0f),
                "no walk speed: the state alone");
        }

        // <VariableRegistry> (runtime/GraphVariables.h): what the parser reads and rejects.
        {
            const auto parse = [](const std::string& body, std::vector<GraphVariables::Registered>& variables) {
                const std::string xml = "<AnimationSet version='6' id='Test'>" + body + "</AnimationSet>";
                pugi::xml_document doc;
                doc.load_string(xml.c_str());
                std::vector<ActionSpec> actions;
                std::vector<EventRuleSpec> events;
                LocomotionSpec locomotion;
                std::string mod, error;
                return XmlV2::Compile(
                    doc, "test.xml", actions, events, locomotion, variables, mod, error, Declarations::ResolveKeyName);
            };
            const auto rejected = [&](const std::string& body) {
                std::vector<GraphVariables::Registered> v;
                return !parse("<VariableRegistry>" + body + "</VariableRegistry>", v) && v.empty();
            };
            std::vector<GraphVariables::Registered> v;
            Check(parse(R"(<VariableRegistry><Variable name="iMy_Move" type="int" source="moveInput"/>)"
                        R"(<Variable name="bMy_Flag" type="bool"/><Variable name="fMy_Speed" type="float"/>)"
                        "</VariableRegistry>",
                      v) &&
                      v.size() == 3,
                "a file may hold only a variable registry");
            Check(v[0].name == "iMy_Move" && v[0].type == GraphVariables::Type::kInt &&
                      v[0].source == GraphVariables::Source::kMoveInput && v[0].file == "test.xml",
                "a moveInput variable");
            Check(v[1].type == GraphVariables::Type::kBool && v[1].source == GraphVariables::Source::kNone &&
                      v[2].type == GraphVariables::Type::kFloat,
                "names alone, by type");
            Check(rejected(""), "an empty registry is rejected");
            Check(rejected(R"(<Variable name="a" />)"), "type is required");
            Check(rejected(R"(<Variable name="a" type="string"/>)"), "type is bool, int or float");
            Check(rejected(R"(<Variable name="a.b" type="int"/>)"), "names are letters, digits and underscores");
            Check(rejected(R"(<Variable name="a" type="int"/><Variable name="A" type="bool"/>)"),
                "one name once, ignoring case");
            Check(rejected(R"(<Variable name="a" type="int" source="keys"/>)"), "a source is moveInput or strafeInput");
            {
                std::vector<GraphVariables::Registered> s;
                Check(parse(R"(<VariableRegistry><Variable name="iMy_Strafe" type="int" source="strafeInput"/>)"
                            "</VariableRegistry>",
                          s) &&
                          s.size() == 1 && s[0].source == GraphVariables::Source::kStrafeInput,
                    "a strafeInput variable");
                Check(rejected(R"(<Variable name="a" type="bool" source="strafeInput"/>)"), "strafeInput is an int");
            }
            Check(rejected(R"(<Variable name="a" type="float" source="moveInput"/>)"), "moveInput is an int");
            // MoveDir: 0 forward, 1 back, 2 left, 3 right, 4 none -> 1 forward, 2 right, 3 back, 4 left, 0 none.
            Check(GraphVariables::MoveInputValue(0) == 1 && GraphVariables::MoveInputValue(3) == 2 &&
                      GraphVariables::MoveInputValue(1) == 3 && GraphVariables::MoveInputValue(2) == 4 &&
                      GraphVariables::MoveInputValue(4) == 0 && GraphVariables::MoveInputValue(-1) == 0,
                "the movement input's values go clockwise from forward");
        }

        // Through the loader: the locomotion file's <Locomotion> alone is used, whole; no other file adds to it.
        {
            const std::string mine = R"(<Locomotion><Tree weapon="H2H" use="own"/><Except set="Sneak"/></Locomotion>)";
            WriteFile(root / L"A Mod" / L"a.xml", "A",
                R"(<Locomotion><Tree weapon="2HW" use="2HM"/><Except set="Bow"/></Locomotion>)");
            WriteFile(root / L"Z Mod" / L"z.xml", "Z", "<Locomotion/>");
            // The name alone, in another folder, is not the locomotion file.
            WriteFile(root / L"Other" / L"Locomotion.xml", "Other", "<Locomotion/>");
            std::vector<EventRuleSpec> events;
            {
                LocomotionSpec none;
                const auto actions = Declarations::LoadAllFrom(root, &events, &none);
                Check(actions.empty() && events.empty() && none.Empty(),
                    "without the locomotion file no file's <Locomotion> is used");
            }
            WriteFile(root / L"locomotion" / L"LOCOMOTION.XML", "Mine", mine);  // names ignoring case
            LocomotionSpec locomotion;
            const auto actions = Declarations::LoadAllFrom(root, &events, &locomotion);
            Check(actions.empty() && events.empty(), "locomotion files add no actions");
            Check(!locomotion.Empty() && !locomotion.Applies(LadderSet::kSneak) && locomotion.Applies(LadderSet::kBow),
                "the locomotion file's exceptions, and only they");
            Check(locomotion.sourceName.find("LOCOMOTION.XML") != std::string::npos, "it names that file");
            Check(locomotion.Use(TreeWeapon::kH2H) == TreeUse::kOwn &&
                      locomotion.Use(TreeWeapon::k2HW) == TreeUse::kVanilla,
                "its trees, not another file's");
            // A locomotion file with an action in it is skipped whole.
            WriteFile(root / L"locomotion" / L"LOCOMOTION.XML", "Mine", R"(<Action id="A" clip="a.hkx"/>)" + mine);
            LocomotionSpec mixed;
            Check(Declarations::LoadAllFrom(root, &events, &mixed).empty() && mixed.Empty(),
                "the locomotion file holds the locomotion and nothing else");
            // A rejected locomotion file declares nothing; the others still do not step in.
            WriteFile(root / L"locomotion" / L"LOCOMOTION.XML", "Mine",
                R"(<Locomotion><Ladder set="Bow" direction="left" walkUntil="100" runFrom="300"/></Locomotion>)");
            LocomotionSpec rejected;
            Declarations::LoadAllFrom(root, &events, &rejected);
            Check(rejected.Empty(), "a rejected locomotion file leaves the game vanilla");
            // One file on its own (the checker): its <Locomotion> whatever its name.
            std::vector<ActionSpec> one;
            LocomotionSpec fileOwn;
            Check(Declarations::LoadXmlFile(root / L"A Mod" / L"a.xml", one, events, fileOwn) &&
                      !fileOwn.Applies(LadderSet::kBow) && fileOwn.Use(TreeWeapon::k2HW) == TreeUse::kShared,
                "LoadXmlFile returns the file's own <Locomotion>");
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
    std::filesystem::remove_all(root, ignored);
    std::cout << "ladder checks passed: " << checks << '\n';
    return 0;
}

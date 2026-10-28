// Action ids are the file's own: the runtime names each action after its AnimationSet (XmlV2::Compile qualifier,
// Settings.cpp), so two mods that kept the editor's CustomAction1 each play their own clip. Before, the later file
// in name order replaced the earlier one's action, and its starts reached the other mod's clip.
#include "Settings.h"
#include "declarations/XmlV2.h"
#include <algorithm>
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
void Write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" << text << '\n';
}
// The same ids in every mod: a start through a card (its selecting action Start_Go), two actions, a QTE whose
// result goes to the second, and a check of the last action finished.
std::string Mod(const std::string& set, const std::string& folder) {
    return "<AnimationSet version=\"6\" id=\"" + set +
           "\">"
           "<Start event=\"Go\"><Check kind=\"previousAction\" actions=\"CustomAction2\" not=\"true\"/>"
           "<True to=\"CustomAction1\"/></Start>"
           "<Action id=\"CustomAction1\" clip=\"Animations\\" +
           folder +
           "\\One.hkx\">"
           "<QTE id=\"Q\" at=\"0.5\" key=\"E\"><Success to=\"CustomAction2\"/></QTE></Action>"
           "<Action id=\"CustomAction2\" clip=\"Animations\\" +
           folder +
           "\\Two.hkx\"/>"
           "</AnimationSet>";
}
const ActionSpec* Find(const std::vector<ActionSpec>& specs, const char* id) {
    const auto it = std::find_if(specs.begin(), specs.end(), [&](const ActionSpec& s) { return s.id == id; });
    return it != specs.end() ? &*it : nullptr;
}
bool Compiles(const std::string& text, std::string_view qualifier, std::vector<ActionSpec>& out) {
    pugi::xml_document doc;
    doc.load_string(text.c_str());
    std::string mod, error;
    return XmlV2::Compile(doc, "test", out, mod, error, [](const char*) { return 34u; }, qualifier);
}
}

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / (L"ai-action-names-" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    try {
        Write(root / "A" / "a.xml", Mod("ModA", "A"));
        Write(root / "B" / "b.xml", Mod("ModB", "B"));
        // The same set id again (a mod installed twice): loaded beside it as ModB_2.
        Write(root / "C" / "c.xml", Mod("ModB", "C"));
        const auto specs = Declarations::LoadAllFrom(root);
        Check(specs.size() == 9, "three files, three actions each: none replaced");
        const auto* a1 = Find(specs, "ModA_CustomAction1");
        const auto* b1 = Find(specs, "ModB_CustomAction1");
        const auto* c1 = Find(specs, "ModB_2_CustomAction1");
        Check(a1 && b1 && c1, "each file's CustomAction1 under its set");
        Check(a1->clips.size() == 1 && a1->clips[0].file == "Animations\\A\\One.hkx", "A's action plays A's clip");
        Check(b1->clips.size() == 1 && b1->clips[0].file == "Animations\\B\\One.hkx", "B's action plays B's clip");
        Check(c1->clips.size() == 1 && c1->clips[0].file == "Animations\\C\\One.hkx", "the copy plays its own clip");
        Check(
            a1->clips[0].enterEvent == "AI_ModA_CustomAction1_Enter" && a1->events.exit == "AI_ModA_CustomAction1_Exit",
            "graph events carry the set");
        // The card's selecting action (made from the trigger) and what it reaches are the file's own.
        const auto* go = Find(specs, "ModA_Start_Go");
        Check(go && Find(specs, "ModB_Start_Go") && Find(specs, "ModB_2_Start_Go"), "each file's selecting action");
        Check(
            std::find(go->flowActions.begin(), go->flowActions.end(), "ModA_CustomAction1") != go->flowActions.end() &&
                std::none_of(go->flowActions.begin(), go->flowActions.end(),
                    [](const std::string& a) { return a.starts_with("ModB"); }),
            "the start reaches its own file's actions");
        const auto& history = go->flow.predicates;
        Check(history.size() == 1 && history[0].actionNames == std::vector<std::string>{"ModA_CustomAction2"},
            "the last action finished names the file's own");
        // The QTE result: its event and the action it starts.
        Check(a1->links.size() == 1 && a1->links[0].action == "ModA_CustomAction2" &&
                  a1->links[0].event == "AI_ModA_CustomAction1_QTE_Q_Success",
            "the QTE result goes to the file's own action");
        Check(a1->qtes.size() == 1 && a1->qtes[0].results[0].event == "AI_ModA_CustomAction1_QTE_Q_Success",
            "the QTE result's event carries the set");

        // Underscores can still make the same name (set X_Y's Z and set X's Y_Z): the later file is not loaded.
        const auto clash = root / "clash";
        Write(clash / "1" / "1.xml", "<AnimationSet version=\"6\" id=\"X_Y\"><Start key=\"G\" to=\"Z\"/>"
                                     "<Action id=\"Z\" clip=\"Animations\\T\\A.hkx\"/></AnimationSet>");
        Write(clash / "2" / "2.xml", "<AnimationSet version=\"6\" id=\"X\"><Start key=\"G\" to=\"Y_Z\"/>"
                                     "<Action id=\"Y_Z\" clip=\"Animations\\T\\B.hkx\"/></AnimationSet>");
        const auto clashed = Declarations::LoadAllFrom(clash);
        Check(clashed.size() == 1 && clashed[0].id == "X_Y_Z" && clashed[0].clips[0].file == "Animations\\T\\A.hkx",
            "the first file keeps its action, the second is skipped whole");

        // The last action finished names only the file's actions.
        std::vector<ActionSpec> out;
        Check(!Compiles("<AnimationSet version=\"6\" id=\"S\"><Start key=\"G\">"
                        "<Check kind=\"previousAction\" actions=\"Elsewhere\"/><True to=\"A\"/></Start>"
                        "<Action id=\"A\" clip=\"Animations\\T\\A.hkx\"/></AnimationSet>",
                  "S", out),
            "another file's action is not named");
        // Without a qualifier the ids stay as written (tools that read a declaration alone).
        Check(Compiles(Mod("ModA", "A"), {}, out) && Find(out, "CustomAction1") &&
                  Find(out, "CustomAction1")->clips[0].enterEvent == "AI_CustomAction1_Enter",
            "no qualifier, the written ids");
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
    std::filesystem::remove_all(root, ignored);
    std::cout << "action name checks passed: " << checks << '\n';
    return 0;
}

// Declaration folders and files named outside the ANSI code page (code review 2026-09-30 R-14): loading
// them must not throw. path::string() threw there, and the declarations load in the background, so the
// exception came back uncaught in the first reader's thread and ended the game.
#include "Settings.h"
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
void Write(const std::filesystem::path& file, const char* id) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<AnimationSet version=\"6\" id=\"" << id
        << "\"><Inputs><Input id=\"Use\" key=\"G\"/></Inputs><Start input=\"Use\" to=\"" << id << "\"/><Action id=\""
        << id << "\" clip=\"Animations\\Test\\A.hkx\"/></AnimationSet>\n";
}
}

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / (L"ai-declaration-paths-" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    try {
        // Thai is in no East Asian or Western code page; Korean is outside every non-Korean one.
        const auto thai = root / L"กข Mod" / L"กข.xml";
        const auto korean = root / L"그래플" / L"그래플.xml";
        Write(thai, "ThaiFolder");
        Write(korean, "KoreanFolder");
        std::vector<EventRuleSpec> events;
        const auto actions = Declarations::LoadAllFrom(root, &events);
        Check(actions.size() == 2, "both files load");
        // The source names the file in UTF-8 (the log is UTF-8); the action is named after its set.
        const auto thaiText = Declarations::PathText(thai);
        const bool named = std::any_of(actions.begin(), actions.end(),
            [&](const ActionSpec& a) { return a.id == "ThaiFolder_ThaiFolder" && a.source == thaiText; });
        Check(named, "the source is the UTF-8 path");
        Check(thaiText.find("\xE0\xB8\x81") != std::string::npos, "PathText is UTF-8");
        // A broken file in such a folder is skipped with its log line, not thrown.
        {
            std::ofstream broken(root / L"กข Mod" / L"brokenก.xml", std::ios::binary);
            broken << "<AnimationSet version=\"6\" id=\"Broken\"><Action";
        }
        Check(Declarations::LoadAllFrom(root).size() == 2, "a broken file is skipped");
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
    std::filesystem::remove_all(root, ignored);
    std::cout << "declaration path checks passed: " << checks << '\n';
    return 0;
}

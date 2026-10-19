// RagdollTrace::Recorder: bounded recording, the JSON Lines file HKX_Edit reads back, and exact
// float round trips (the replay compares against these numbers).
#include "core/RagdollTrace.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::cerr << "RagdollTrace check failed: " << what << '\n';
        std::exit(1);
    }
}
std::vector<std::string> Lines(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}
// The numbers of one step line, "[a,b,...]"; null reads as NaN.
std::vector<float> Numbers(const std::string& line) {
    std::vector<float> out;
    std::stringstream in(line.substr(1, line.size() - 2));
    for (std::string item; std::getline(in, item, ',');)
        out.push_back(item == "null" ? std::numeric_limits<float>::quiet_NaN() : std::strtof(item.c_str(), nullptr));
    return out;
}
}

int main() {
    using namespace AnimationInjector::RagdollTrace;
    Header header;
    header.rig = "chicken";
    header.profile = R"(D:\Skyrim\mods\Combo "Test"\chicken.xml)";
    header.anchor = "Neck2";
    header.actorScale = 1.3f;
    header.follow = 1.0f;
    header.gravity = {0, 0, -686.0f};
    header.bodies = {{"Pelvis", true, 13.0f}, {"Neck2", false, 6.5f}};
    header.joints = {{"Neck0", "Pelvis", 4898.6f, 0.377f, {-0.26f, 0.26f}, {-0.7f, 0.7f}}};
    header.widened = {{"LThigh", "Pelvis", "cone", 0.785f, 1.056f}, {"LFoot", "LCalf", "hingeMin", -0.6f, -0.93f}};

    Recorder recorder;
    const Pose frame{{1, 2, 3}, {1, 0, 0, 0}};
    const std::vector<BodyState> two{{{{10, 20, 30}, {1, 0, 0, 0}}, {1.0f / 3.0f, -2, 3}, {0.1f, 0.2f, 0.3f}},
        {{{-1, -2, -3}, {0, 1, 0, 0}}, {}, {}}};
    recorder.Add(0.01f, frame, two);
    Check(!recorder.Active() && recorder.Steps() == 0, "Add before Begin records nothing");

    recorder.Begin(header, 3);
    Check(recorder.Active(), "Begin with bodies activates");
    for (int i = 0; i < 4; ++i) recorder.Add(0.01f, frame, two);
    recorder.Add(0.01f, frame, std::span(two).first(1));
    Check(recorder.Steps() == 3 && recorder.Dropped() == 2, "capacity and body-count mismatch drop, never grow");

    const auto file = std::filesystem::temp_directory_path() / "AnimationInjectorRagdollTrace.jsonl";
    Check(recorder.Write(file), "Write succeeds");
    const auto lines = Lines(file);
    Check(lines.size() == 4, "header line + one line per step");
    const auto& h = lines[0];
    Check(h.front() == '{' && h.back() == '}', "header is one JSON object");
    for (const char* part : {R"("format":"AnimationInjector.RagdollTrace")", R"("rig":"chicken")",
             R"("profile":"D:\\Skyrim\\mods\\Combo \"Test\"\\chicken.xml")", R"("anchor":"Neck2")", R"("steps":3)",
             R"("dropped":2)", R"("bone":"Pelvis","simulated":true,"mass":13)", R"("bone":"Neck2","simulated":false)",
             R"("a":"Neck0","b":"Pelvis","friction":4898.6)", R"("gravity":[0,0,-686])",
             R"("widened":[{"a":"LThigh","b":"Pelvis","limit":"cone","from":0.785,"to":1.056},{"a":"LFoot")"})
        Check(h.find(part) != std::string::npos, part);

    const auto step = Numbers(lines[3]);
    Check(step.size() == 2 + 7 + 13 * 2, "step = time, delta, attachment, 13 per body");
    Check(step[0] == 0.01f + 0.01f + 0.01f && step[1] == 0.01f, "time sums the recorded deltas");
    Check(step[2] == 1 && step[4] == 3 && step[5] == 1, "attachment position then rotation w first");
    Check(step[9] == 10 && step[16] == 1.0f / 3.0f && step[21] == 0.3f, "body floats read back exactly");
    Check(step[22] == -1 && step[26] == 1, "second body follows the first");

    recorder.Clear();
    recorder.Begin(header, 1);
    recorder.Add(std::numeric_limits<float>::quiet_NaN(), frame, two);
    Check(recorder.Write(file), "Write after Begin again");
    const auto nan = Numbers(Lines(file)[1]);
    Check(std::isnan(nan[0]) && std::isnan(nan[1]), "non-finite numbers are written as null");

    recorder.Clear();
    Check(!recorder.Active() && recorder.Steps() == 0 && recorder.Dropped() == 0, "Clear resets");
    std::filesystem::remove(file);
    std::cout << "RagdollTrace: " << checks << " checks passed\n";
    return 0;
}

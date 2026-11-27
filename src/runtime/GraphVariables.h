#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Graph variables a declaration reads (1.1.0): <Check kind="graphVariable" variable="MCO_nextattack" type="int" .../>.
// Every variable such a check names goes into the third-person character master graphs and their project's variable
// lookup (core/GraphInjector, core/EventMaps), as the OAR signals do, so a clip's annotation can set it (Payload
// Interpreter's @SGVI|MCO_nextattack|2) without a behavior patch and the check can read it. A graph that already has
// the variable (an MCO-patched one) keeps its own; one of another type is left alone and logged.
namespace AnimationInjector::GraphVariables {
enum class Type : std::uint8_t { kBool, kInt, kFloat };
struct Variable {
    std::string name;
    Type type = Type::kInt;
};
// <VariableRegistry> (1.1.0): a graph variable a file registers by name, as <EventRegistry> registers event names.
//   <Variable name="iMyMod_State" type="int"/>                     the name only: another plugin, an annotation or
//                                                                  an Open Animation Replacer condition uses it
//   <Variable name="iMyMod_MoveInput" type="int" source="moveInput"/>   the runtime keeps it at the player's movement
//                                                                  input (runtime/InputVariables.h)
//   <Variable name="iMyMod_Strafe" type="int" source="strafeInput"/>    the same, but 0 while the keys turn the
//                                                                  character instead of moving it sideways (True
//                                                                  Directional Movement's directional setting)
// Apart from the game so the declaration parser and its tests can use it.
enum class Source : std::uint8_t { kNone, kMoveInput, kStrafeInput };
struct Registered {
    std::string name;
    Type type = Type::kInt;
    Source source = Source::kNone;
    std::string file;  // declaration file, for logs
};
// A registered variable's name: 1-64 of A-Z a-z 0-9 _ (a behavior variable's name, read by OAR as written).
inline bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
// The value of a moveInput variable for the keys' direction (MoveDir: 0 forward, 1 back, 2 left, 3 right, 4 none):
// 0 none, 1 forward, 2 right, 3 back, 4 left (clockwise from forward).
constexpr int MoveInputValue(int moveDir) {
    return moveDir == 0 ? 1 : moveDir == 3 ? 2 : moveDir == 1 ? 3 : moveDir == 2 ? 4 : 0;
}
// The variables the loaded declarations register (every file's <VariableRegistry>, each name once) and their checks
// name (names compare without case; the first type is kept and another logged), in that order. Built on first use
// (any thread).
const std::vector<Variable>& All();
}

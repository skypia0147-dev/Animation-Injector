#pragma once
#include <cstdint>
#include <string>
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
// The variables the loaded declarations' checks name, each once (names compare without case; the first check's type
// is kept and another type logged), in declaration order. Built on first use (any thread).
const std::vector<Variable>& All();
}

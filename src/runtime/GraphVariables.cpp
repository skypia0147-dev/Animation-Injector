#include "PCH.h"
#include "runtime/GraphVariables.h"
#include "runtime/ActionRegistry.h"
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace AnimationInjector::GraphVariables {
namespace {
std::vector<Variable> g_all;
std::once_flag g_built;
Type TypeOf(const std::string& name) {
    return name == "bool" ? Type::kBool : name == "float" ? Type::kFloat : Type::kInt;
}
}
const std::vector<Variable>& All() {
    std::call_once(g_built, [] {
        std::vector<Variable> out;
        std::unordered_map<std::string, std::size_t> byName;
        for (int a = 0; a < Actions::Count(); ++a)
            for (const auto& p : Actions::Spec(a).flow.predicates) {
                if (p.test != ConditionFlowXml::Test::kGraphVariable) continue;
                std::string key = p.variable;
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto type = TypeOf(p.variableType);
                const auto [it, added] = byName.try_emplace(key, out.size());
                if (added)
                    out.push_back({p.variable, type});
                else if (out[it->second].type != type)
                    logger::error("[GraphVariable] '{}' is read as {} in '{}' but as another type before; the first is "
                                  "kept",
                        p.variable, p.variableType, Actions::Spec(a).id);
            }
        if (!out.empty()) logger::info("[GraphVariable] {} graph variables read by checks", out.size());
        g_all = std::move(out);
    });
    return g_all;
}
}

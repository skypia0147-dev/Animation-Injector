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
        // Registered names first (<VariableRegistry>): a file with no check still gets its variables.
        for (const auto& r : Actions::RegisteredVariables()) {
            std::string key = r.name;
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto [it, added] = byName.try_emplace(key, out.size());
            if (added)
                out.push_back({r.name, r.type});
            else if (out[it->second].type != r.type)
                logger::error(
                    "[GraphVariable] '{}' is registered with another type in '{}'; the first is kept", r.name, r.file);
        }
        const auto registered = out.size();
        for (int a = 0; a < Actions::Count(); ++a)
            for (const auto& p : Actions::Spec(a).flow.predicates) {
                // An existence check asks whether a behavior patch made the variable: adding it would answer yes.
                if (p.test != ConditionFlowXml::Test::kGraphVariable || p.compare == "exists") continue;
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
        if (!out.empty())
            logger::info("[GraphVariable] {} graph variables ({} registered, {} more read by checks)", out.size(),
                registered, out.size() - registered);
        g_all = std::move(out);
    });
    return g_all;
}
}

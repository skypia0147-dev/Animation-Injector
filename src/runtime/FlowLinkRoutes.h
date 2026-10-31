#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include "runtime/ConditionFlow.h"

namespace AnimationInjector {
// Called once after declarations are finalized, before graph construction.
template<class Specs>
void ExpandFlowLinkRoutes(Specs& actions) {
    // Build distinct transition events per link and candidate at load time.
    for (auto& source : actions) {
        if (source.paired.enabled || source.clips.size() != 1) continue;
        const auto count = source.links.size();
        for (std::size_t j = 0; j < count; ++j) {
            const auto original = source.links[j];
            const auto host =
                std::find_if(actions.begin(), actions.end(), [&](const auto& s) { return s.id == original.action; });
            if (host == actions.end() || host->flow.nodes.empty()) continue;
            std::vector<std::string> visited;
            for (const auto& node : host->flow.nodes) {
                if (node.kind != ConditionFlow::Kind::kAction || node.action < 0 ||
                    node.action >= static_cast<int>(host->flowActions.size()))
                    continue;
                const auto& name = host->flowActions[node.action];
                if (std::find(visited.begin(), visited.end(), name) != visited.end()) continue;
                visited.push_back(name);
                auto dest = std::find_if(actions.begin(), actions.end(), [&](const auto& s) { return s.id == name; });
                if (dest == actions.end() || dest->paired.enabled || dest->clips.size() != 1) continue;
                auto route = original;
                route.action = name;
                route.flowParent = static_cast<int>(j);
                route.enterEvent = original.enterEvent + "_Candidate" + std::to_string(node.action);
                route.airEnterEvent = route.enterEvent + "Air";
                source.links.push_back(std::move(route));
            }
        }
    }
}
}

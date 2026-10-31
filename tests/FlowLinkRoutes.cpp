#include "runtime/FlowLinkRoutes.h"
#include "runtime/PairFeatureSpec.h"
#include <iostream>
#include <stdexcept>

struct Spec {
    std::string id;
    struct {
        bool enabled = false;
    } paired;
    std::vector<int> clips{0};
    struct {
        std::vector<AnimationInjector::ConditionFlow::Node> nodes;
    } flow;
    std::vector<std::string> flowActions;
    std::vector<AnimationInjector::ActionLinkSpec> links;
};
int main() {
    using namespace AnimationInjector;
    std::vector<Spec> specs(5);
    for (int i = 0; i < 5; ++i) specs[i].id = "A" + std::to_string(i);
    ActionLinkSpec link;
    link.action = "A1";
    link.enterEvent = "Link";
    link.airEnterEvent = "LinkAir";
    link.blend = .7f;
    specs[0].links.push_back(link);
    specs[1].flowActions = {"A0", "A1", "A2", "A3", "A4"};
    for (int i : {0, 1, 2, 2, 3, 4}) {
        ConditionFlow::Node node;
        node.kind = ConditionFlow::Kind::kAction;
        node.action = i;
        specs[1].flow.nodes.push_back(node);
    }
    specs[3].paired.enabled = true;
    specs[4].clips.push_back(1);
    ExpandFlowLinkRoutes(specs);
    // The source itself is a destination too (10-31: a transition into the same action restarts it).
    if (specs[0].links.size() != 4)
        throw std::runtime_error("deduplication or unsupported destination filtering failed");
    for (int i = 0; i <= 2; ++i) {
        const auto& route = specs[0].links[i + 1];
        if (route.action != "A" + std::to_string(i) || route.flowParent != 0 || route.blend != .7f ||
            route.enterEvent != "Link_Candidate" + std::to_string(i) || route.airEnterEvent != route.enterEvent + "Air")
            throw std::runtime_error("candidate transition binding failed");
    }
    if (specs[0].links[0].enterEvent != "Link" || specs[0].links[0].flowParent != -1)
        throw std::runtime_error("source input mutated");
    std::cout << "Immutable candidate events, blend, deduplication and destination filtering passed\n";
}

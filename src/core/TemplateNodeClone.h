#pragma once
#include <cstdint>
#include <limits>

namespace AnimationInjector::TemplateNodeClone {
// Native hkbNode copying marks the source as a template and the result as a
// runtime clone. A new independent graph declaration must preserve the source's
// original classification and receive its own identity during symbol linking.
template<class Node, class Clone>
Node* Detached(Node* source, Clone clone) {
    if (!source) return nullptr;
    const auto originalState = source->cloneState;
    struct Restore {
        Node* source;
        decltype(originalState) state;
        ~Restore() { source->cloneState = state; }
    } restore{source, originalState};
    auto* node = clone(source);
    if (!node || node == source) return nullptr;
    node->id = std::numeric_limits<std::uint16_t>::max();
    node->cloneState = Node::CloneState::kDefault;
    return node;
}
}

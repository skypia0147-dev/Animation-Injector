#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/TemplateNodeClone.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

struct Node {
    enum class CloneState { kDefault, kTemplate, kClone, kShareable };
    CloneState cloneState = CloneState::kDefault;
    std::uint16_t id = 123;
    const void* binding = nullptr;
};

int main() {
    using AnimationInjector::TemplateNodeClone::Detached;
    for (auto state : {Node::CloneState::kDefault, Node::CloneState::kTemplate, Node::CloneState::kShareable}) {
        int nativeBinding = 0;
        Node source{state, 123, &nativeBinding}, result;
        auto* clone = Detached(&source, [&](Node* value) {
            // Native hkbNode copy marks the source and shares a referenced binding.
            result = *value;
            value->cloneState = Node::CloneState::kTemplate;
            result.cloneState = Node::CloneState::kClone;
            return &result;
        });
        assert(clone == &result && clone != &source);
        assert(source.cloneState == state && source.id == 123 && source.binding == &nativeBinding);
        assert(result.id == 0xFFFF && result.cloneState == Node::CloneState::kDefault);
        // Subsequent binding replacement must change only the new declaration.
        int detachedBinding = 1;
        clone->binding = &detachedBinding;
        assert(source.binding == &nativeBinding);
    }
    Node source;
    auto nullClone = [](Node* value) -> Node* {
        value->cloneState = Node::CloneState::kTemplate;
        return nullptr;
    };
    assert(Detached(&source, nullClone) == nullptr);
    assert(source.cloneState == Node::CloneState::kDefault && source.id == 123);
    assert(Detached(&source, [](Node* value) {
        value->cloneState = Node::CloneState::kTemplate;
        return value;
    }) == nullptr);
    assert(source.cloneState == Node::CloneState::kDefault && source.id == 123);
    bool invoked = false;
    assert(Detached<Node>(nullptr,
               [&](Node*) -> Node* {
                   invoked = true;
                   return nullptr;
               }) == nullptr &&
           !invoked);
    try {
        Detached(&source, [](Node* value) -> Node* {
            value->cloneState = Node::CloneState::kTemplate;
            throw std::runtime_error("simulated clone failure");
        });
        assert(false);
    } catch (const std::runtime_error&) {
        assert(source.cloneState == Node::CloneState::kDefault);
    }
    std::cout << "Native template clone source restoration, identity and binding isolation checks passed\n";
}

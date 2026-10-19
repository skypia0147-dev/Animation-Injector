#pragma once
#include <cstdint>
#include <optional>
#include "core/PairedClipIndex.h"

namespace AnimationInjector::AnimationFileLifetime {
struct Identity {
    std::uintptr_t character = 0, clip = 0, sync = 0;
    bool operator==(const Identity&) const = default;
};
// Stack-local ownership across synchronous Deactivate -> Unload. No engine
// pointers survive the callback, and independent animation workers do not mix.
class ExitScope {
public:
    ExitScope(Identity key, std::uint16_t file, bool held)
        : previous(current), identity(key), index(file), valid(held && key.character && key.clip && file != 0xFFFF) {
        current = this;
    }
    ExitScope(const ExitScope&) = delete;
    ExitScope& operator=(const ExitScope&) = delete;
    ~ExitScope() { current = previous; }
    static std::optional<std::uint16_t> Find(Identity key) {
        for (auto* scope = current; scope; scope = scope->previous)
            if (scope->valid && scope->identity == key) return scope->index;
        return {};
    }

private:
    static inline thread_local ExitScope* current = nullptr;
    ExitScope* previous;
    Identity identity;
    std::uint16_t index;
    bool valid;
};
template<class NativeCall>
decltype(auto) Unload(Identity key, std::uint16_t& index, NativeCall&& native) {
    PairedClipIndex::Borrow borrowed;
    if (const auto owned = ExitScope::Find(key)) borrowed.Set(index, *owned);
    return native();
}
void Install();
}

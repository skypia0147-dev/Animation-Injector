#pragma once
#include "PCH.h"
#include <mutex>
#include "core/PairedClipIndex.h"

namespace AnimationInjector::PairedClipBinding {
// Borrow the current character's base index for the synchronous native mark
// query, then restore the shared template on every return path.
class MarkScope {
public:
    MarkScope(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context);
    MarkScope(const MarkScope&) = delete;
    MarkScope& operator=(const MarkScope&) = delete;

private:
    std::unique_lock<std::recursive_mutex> lock;
    PairedClipIndex::Borrow index;
};
// Installed after plugin loading, before OAR's synchronized selection executes.
void Install();
}

#ifdef NDEBUG
#undef NDEBUG
#endif
// Execute production hooks against the native paired/ordinary cleanup order.
#include "../src/core/AnimationFileLifetime.cpp"
#include <array>
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector::AnimationFileLifetime;
namespace {
RE::hkbCharacter actor, other;
RE::hkbContext context{&actor};
RE::AnimationFileManagerSingleton manager;
std::array<int, 4096> refs{};
int releases = 0;
bool oar = false, ordinary = false, restoreEarly = false, throwNative = false;
constexpr std::uint16_t base = 1827, replacement = 2668;
std::int32_t NativeUnload(RE::AnimationFileManagerSingleton*, const RE::hkbContext&, RE::hkbClipGenerator* clip,
    RE::BSSynchronizedClipGenerator*) {
    if (throwNative) throw std::runtime_error("native test failure");
    assert(clip->userData != 0);
    assert(refs[clip->animationBindingIndex] > 0);
    --refs[clip->animationBindingIndex];
    ++releases;
    clip->userData = 0;
    return 37;
}
void InnerCleanup(RE::hkbClipGenerator* clip, const RE::hkbContext& ctx) {
    if (ordinary && oar && restoreEarly) clip->animationBindingIndex = base;
    if (clip->userData) assert(FileUnload::thunk(&manager, ctx, clip, nullptr) == 37);
    if (oar) clip->animationBindingIndex = base;
}
void ParentCleanup(RE::BSSynchronizedClipGenerator* sync, const RE::hkbContext& ctx) {
    auto* clip = sync->clipGenerator;
    if (!clip) return;
    const auto held = clip->userData;
    clip->userData = 0;
    ClipDeactivate::thunk(clip, ctx);
    clip->userData = held;
    assert(FileUnload::thunk(&manager, ctx, clip, sync) == 37);
}
}
int main() {
    FileUnload::original = NativeUnload;
    ClipDeactivate::original = InnerCleanup;
    SyncDeactivate::original = ParentCleanup;
    // Reproduce the reported bug when the outer ownership scope is absent.
    refs[base] = 4;
    refs[replacement] = 1;
    oar = true;
    RE::hkbClipGenerator broken{replacement, 12};
    RE::BSSynchronizedClipGenerator brokenSync{&broken};
    ParentCleanup(&brokenSync, context);
    assert(refs[base] == 3 && refs[replacement] == 1);
    // The shared original preload reference must survive repeated replaced
    // playback, with each replacement reference released exactly once.
    for (bool paired : {false, true})
        for (bool replaced : {false, true}) {
            ordinary = !paired;
            oar = replaced;
            refs.fill(0);
            refs[base] = 4;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto file = replaced ? replacement : base;
                ++refs[file];
                RE::hkbClipGenerator clip{file, 12};
                RE::BSSynchronizedClipGenerator sync{&clip};
                const int before = releases;
                if (paired)
                    SyncDeactivate::thunk(&sync, context);
                else
                    ClipDeactivate::thunk(&clip, context);
                assert(releases == before + 1 && refs[base] == 4 && refs[replacement] == 0);
                assert(clip.userData == 0 && clip.animationBindingIndex == base);
                assert(!ExitScope::Find(Key(context, &clip, paired ? &sync : nullptr)));
            }
        }
    // Ordinary cleanup also tolerates a plugin restoring the index early.
    ordinary = true;
    oar = true;
    restoreEarly = true;
    RE::hkbClipGenerator clip{replacement, 12};
    ++refs[replacement];
    ClipDeactivate::thunk(&clip, context);
    assert(refs[base] == 4 && refs[replacement] == 0);
    restoreEarly = false;
    // Pending loads have the same ownership and no extra Queue is required.
    ordinary = false;
    clip = {replacement, 8};
    ++refs[replacement];
    RE::BSSynchronizedClipGenerator sync{&clip};
    SyncDeactivate::thunk(&sync, context);
    assert(refs[base] == 4 && refs[replacement] == 0);
    // Exact identity matching protects unrelated clips, actors and mark queries.
    const auto key = Key(context, &clip, &sync);
    {
        ExitScope outer(key, replacement, true);
        assert(!ExitScope::Find({key.character + 1, key.clip, key.sync}));
        assert(!ExitScope::Find({key.character, key.clip + 1, key.sync}));
        assert(!ExitScope::Find({key.character, key.clip, 0}));
        {
            ExitScope nested(key, 1900, true);
            assert(*ExitScope::Find(key) == 1900);
        }
        assert(*ExitScope::Find(key) == replacement);
        // The inner native cleanup has userData=0 and must not own an Unload.
        {
            ExitScope inner(Key(context, &clip, nullptr), base, false);
            assert(*ExitScope::Find(key) == replacement);
        }
        clip = {base, 12};
        throwNative = true;
        try {
            FileUnload::thunk(&manager, context, &clip, &sync);
            assert(false);
        } catch (const std::runtime_error&) {
        }
        assert(clip.animationBindingIndex == base);
        throwNative = false;
    }
    assert(!ExitScope::Find(key));
    // Standalone preloader/mark unloads pass through, even with no scope.
    clip = {base, 12};
    const int before = releases;
    assert(FileUnload::thunk(&manager, context, &clip, nullptr) == 37);
    assert(releases == before + 1 && refs[base] == 3);
    {
        ExitScope invalid(key, 0xFFFF, true);
        assert(!ExitScope::Find(key));
    }
    RE::BSSynchronizedClipGenerator empty{nullptr};
    SyncDeactivate::thunk(&empty, context);
    std::cout
        << "Production release hooks: repeated ordinary/paired OAR on/off, shared preload refs, pending loads, nesting, identity isolation and restoration passed\n";
}

#ifdef NDEBUG
#undef NDEBUG
#endif
// Exercise the actual loader hooks, including nested engine/plugin callbacks.
#include "../src/core/AnimationLoadObserver.cpp"
#include <iostream>

using namespace AnimationInjector;
using namespace AnimationInjector::AnimationLoadObserver;
namespace {
int queued = 0, loaded = 0, unloaded = 0;
int queueResult = 0, unloadResult = 37;
bool loadResult = false;
std::function<void()> duringQueue, duringLoad, duringUnload;
RE::AnimationFileManagerSingleton manager;
RE::hkbCharacter character, otherCharacter;
RE::hkbContext context{&character}, otherContext{&otherCharacter};
RE::hkbClipGenerator clip;
std::int32_t NativeQueue(RE::AnimationFileManagerSingleton* self, const RE::hkbContext&, RE::hkbClipGenerator*,
    RE::BSSynchronizedClipGenerator*) {
    assert(self == &manager);
    ++queued;
    if (duringQueue) {
        auto callback = std::exchange(duringQueue, {});
        callback();
    }
    return queueResult;
}
bool NativeLoad(RE::AnimationFileManagerSingleton* self, const RE::hkbContext&, RE::hkbClipGenerator*,
    RE::BSSynchronizedClipGenerator*) {
    assert(self == &manager);
    ++loaded;
    if (duringLoad) {
        auto callback = std::exchange(duringLoad, {});
        callback();
    }
    return loadResult;
}
std::int32_t NativeUnload(RE::AnimationFileManagerSingleton* self, const RE::hkbContext&, RE::hkbClipGenerator*,
    RE::BSSynchronizedClipGenerator*) {
    assert(self == &manager);
    ++unloaded;
    if (duringUnload) {
        auto callback = std::exchange(duringUnload, {});
        callback();
    }
    return unloadResult;
}
auto State() { return LoadingProgress::Read(); }
void Reset() {
    ResetProgress();
    LoadingProgress::Reset();
}
int RequestAnimation() { return Queue::thunk(&manager, context, &clip, nullptr); }
bool Complete() { return Load::thunk(&manager, context, &clip, nullptr); }
int Release() { return Unload::thunk(&manager, context, &clip, nullptr); }
}
int main() {
    REL::testVtable[1] = reinterpret_cast<std::uintptr_t>(NativeQueue);
    REL::testVtable[2] = reinterpret_cast<std::uintptr_t>(NativeLoad);
    REL::testVtable[3] = reinterpret_cast<std::uintptr_t>(NativeUnload);
    Install();
    assert(REL::testVtable[1] == reinterpret_cast<std::uintptr_t>(Queue::thunk));
    assert(REL::testVtable[2] == reinterpret_cast<std::uintptr_t>(Load::thunk));
    assert(REL::testVtable[3] == reinterpret_cast<std::uintptr_t>(Unload::thunk));

    // Regression: a cached clip has no animationControl yet. Repeated activation
    // must not show a loading job or retain a phantom 20-second pending request.
    queueResult = 1;
    for (int i = 0; i < 100; ++i) {
        assert(RequestAnimation() == 1);
        assert(Release() == 37);
    }
    assert(!clip.animationControl && !State().visible && State().total == 0 && requests.empty());
    assert(queued == 100 && unloaded == 100);  // Hooks do not acquire/release extra references.

    queueResult = 0;
    assert(RequestAnimation() == 0);
    assert(State().visible && State().pending == 1 && State().total == 1);
    assert(RequestAnimation() == 0);  // Duplicate notifications do not double-count a request.
    assert(State().pending == 1 && State().total == 1);
    loadResult = false;
    assert(!Complete() && State().pending == 1);
    loadResult = true;
    assert(Complete() && !clip.animationControl);
    assert(State().ready == 1 && State().pending == 0 && requests.empty());
    assert(LoadingProgress::Advance(0).visible);  // Fast genuine loads are still shown.
    for (int i = 0; i < 11; ++i) LoadingProgress::Advance(.1f);
    assert(!State().visible);
    assert(Release() == 37 && State().cancelled == 0);

    Reset();
    RequestAnimation();
    queueResult = 1;
    assert(RequestAnimation() == 1 && State().ready == 1 && State().pending == 0);

    Reset();
    queueResult = 0;
    RequestAnimation();
    // A false Load result means pending, not an invented failure timeout.
    loadResult = false;
    for (int i = 0; i < 210; ++i) {
        assert(!Complete());
        LoadingProgress::Advance(.1f);
    }
    assert(State().pending == 1 && State().failed == 0);
    assert(Release() == 37 && State().pending == 0 && State().cancelled == 1);
    loadResult = true;
    Complete();
    assert(State().ready == 0);  // A late completion cannot revive an unloaded request.

    Reset();
    RequestAnimation();
    Queue::thunk(&manager, otherContext, &clip, nullptr);
    assert(State().pending == 2);
    Complete();
    assert(State().pending == 1 && State().ready == 1);
    Unload::thunk(&manager, otherContext, &clip, nullptr);
    assert(State().pending == 0 && State().cancelled == 1);

    Reset();
    // OAR's fake objects may have no valid name. Read only their vtable word.
    std::uintptr_t fakeObject = 0;
    Queue::thunk(&manager, context, reinterpret_cast<RE::hkbClipGenerator*>(&fakeObject), nullptr);
    RE::hkbClipGenerator pair;
    pair.name = {"AI_Pair_Clip"};
    RE::hkbClipGenerator preloader;
    preloader.name = {"AI_Pair_Clip_Preload"};
    RE::hkbClipGenerator vanilla;
    vanilla.name = {"Idle"};
    RE::BSSynchronizedClipGenerator sync;
    Queue::thunk(&manager, context, &pair, nullptr);
    Queue::thunk(&manager, context, &preloader, nullptr);
    Queue::thunk(&manager, context, &vanilla, nullptr);
    Queue::thunk(&manager, context, &clip, &sync);
    Queue::thunk(&manager, context, nullptr, nullptr);
    Queue::thunk(&manager, RE::hkbContext{}, &clip, nullptr);
    assert(State().total == 0 && requests.empty());

    // Completion can re-enter while Queue is still executing. Count a genuine
    // request once and finish it without waiting for a playback controller.
    duringQueue = [] { assert(Complete()); };
    RequestAnimation();
    assert(State().visible && State().ready == 1 && State().pending == 0);
    Reset();
    queueResult = 1;
    duringQueue = [] { assert(Complete()); };
    RequestAnimation();
    assert(!State().visible && requests.empty());

    Reset();
    queueResult = 0;
    duringQueue = [] { Reset(); };
    RequestAnimation();
    assert(!State().visible && requests.empty());
    RequestAnimation();
    duringLoad = [] {
        Reset();
        RequestAnimation();
    };
    Complete();
    assert(State().pending == 1 && State().ready == 0);  // Old-save callback ignored.
    Complete();
    assert(State().pending == 0 && State().ready == 1);

    Reset();
    RequestAnimation();
    duringUnload = [] { RequestAnimation(); };
    Release();
    assert(State().pending == 1 && State().cancelled == 1);  // Address reused in teardown.
    Complete();
    assert(State().pending == 0 && State().ready == 1);

    Reset();
    queueResult = -1;
    assert(RequestAnimation() == -1 && !State().visible && requests.empty());
    Reset();
    queueResult = 0;
    RequestAnimation();
    ResetProgress();
    assert(State().pending == 0 && State().cancelled == 1 && requests.empty());
    Complete();
    assert(State().ready == 0);

    Reset();
    queueResult = 0;
    RequestAnimation();
    LoadingProgress::GameLoaded();
    assert(!LoadingProgress::ObservesNativeRequests());
    Complete();  // A request admitted during loading can finish after world entry.
    assert(State().ready == 1 && State().pending == 0);
    const int beforeGameplay = queued;
    for (int i = 0; i < 10; ++i) {
        assert(RequestAnimation() == 0);
        assert(Complete());
        assert(Release() == 37);
    }
    assert(queued == beforeGameplay + 10);  // Playback/I/O still run, without UI jobs.
    assert(State().total == 1 && requests.empty());
    LoadingProgress::Advance(0);
    for (int i = 0; i < 11; ++i) LoadingProgress::Advance(.1f);
    assert(!State().visible);
    RequestAnimation();
    Complete();
    Release();
    assert(!State().visible && State().total == 1);

    Reset();
    duringQueue = [] { LoadingProgress::GameLoaded(); };
    RequestAnimation();
    assert(!State().visible && requests.empty());  // Boundary changed inside native Queue.
    ResetProgress();
    LoadingProgress::Reset(true);
    RequestAnimation();
    assert(State().pending == 1 && State().visible);  // A new save permits initial work again.
    Complete();
    Release();
    // Disabled display must not admit accounting, including across save resets.
    LoadingProgress::SetEnabled(false);
    assert(!LoadingProgress::IsActive() && !LoadingProgress::ObservesNativeRequests());
    LoadingProgress::Reset(true);
    LoadingProgress::GameLoaded();
    assert(LoadingProgress::Begin(LoadingProgress::Phase::kAnimation) == 0);
    assert(LoadingProgress::BeginNativeAnimation() == 0);
    assert(!LoadingProgress::IsActive() && !LoadingProgress::ObservesNativeRequests());
    RequestAnimation();
    Complete();
    Release();
    assert(State().total == 0 && requests.empty());
    LoadingProgress::SetEnabled(true);
    auto token = LoadingProgress::Begin(LoadingProgress::Phase::kAnimation);
    assert(token && LoadingProgress::IsActive());
    LoadingProgress::Finish(token, LoadingProgress::Outcome::kReady);
    LoadingProgress::GameLoaded();
    LoadingProgress::Advance(0);
    for (int i = 0; i < 11; ++i) LoadingProgress::Advance(.1f);
    assert(!LoadingProgress::IsActive());
    assert(LoadingProgress::Begin(LoadingProgress::Phase::kAnimation) == 0);
    LoadingProgress::Reset(true);
    assert(LoadingProgress::IsActive());
    std::cout
        << "Native loading observer: cache, completion, cancellation, OAR exclusion, reentrancy, save reload and no gameplay popups passed\n";
}

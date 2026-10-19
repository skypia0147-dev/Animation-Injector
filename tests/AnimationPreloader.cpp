#ifdef NDEBUG
#undef NDEBUG
#endif
// Exercise production acquisition, maintenance and teardown with a deterministic native loader.
#include "../src/core/AnimationPreloader.cpp"
#include <iostream>
using namespace AnimationInjector::AnimationPreloader;

struct Manager : RE::AnimationFileManagerSingleton {
    RE::hkaAnimation animation;
    RE::hkaAnimationBinding binding{{&animation}};
    int queues = 0, loads = 0, unloads = 0, references = 0;
    bool allowLoad = true, reportReadyWithoutBinding = false;
    int blocked = -1;  // a binding index Load never finishes
    auto* Slot(const RE::hkbContext& ctx, RE::hkbClipGenerator* clip) {
        return BindingSet(ctx.character)->bindings._data[clip->animationBindingIndex];
    }
    int Queue(const RE::hkbContext& ctx, RE::hkbClipGenerator* clip, void*) override {
        assert(clip->userData == 0);
        ++queues;
        ++references;
        const bool loaded = reportReadyWithoutBinding || static_cast<bool>(Slot(ctx, clip)->binding);
        clip->userData = loaded ? 12 : 8;
        return loaded ? 1 : 0;
    }
    bool Load(const RE::hkbContext& ctx, RE::hkbClipGenerator* clip, void*) override {
        assert(clip->userData != 0);
        ++loads;
        if (!allowLoad || clip->animationBindingIndex == blocked) return false;
        Slot(ctx, clip)->binding = {&binding};
        clip->userData = 12;
        return true;
    }
    int Unload(const RE::hkbContext&, RE::hkbClipGenerator* clip, void*) override {
        assert(clip->userData != 0 && references > 0);
        ++unloads;
        --references;
        clip->userData = 0;
        return 0;
    }
};
int main(int argc, char** argv) {
    Manager manager;
    RE::AnimationFileManagerSingleton::instance = &manager;
    RE::Strings strings;
    strings.animationNames = {"pair.hkx"};
    RE::Data data{{&strings}};
    RE::hkbAnimationBindingWithTriggers slot;
    RE::hkbAnimationBindingWithTriggers* slots[] = {&slot};
    RE::hkbAnimationBindingSet set{{1, slots}};
    RE::Setup setup{{&data}, {&set}};
    RE::hkbBehaviorGraph graph, otherGraph;
    RE::hkbCharacter character{{&graph}, {&setup}, {&set}};
    RE::hkbContext ctx{&character};
    files.push_back({"pair.hkx", "loader"});
    Prepare(&graph, ctx, 0.01f);
    assert(manager.queues == 1 && manager.loads == 0);
    assert(AnimationInjector::LoadingProgress::Read().pending == 1);
    Prepare(&graph, ctx, 0.01f);
    assert(slot.binding && manager.loads == 1);
    auto& state = characters.at(reinterpret_cast<std::uintptr_t>(&character));
    assert(state.jobs[0].finished);
    assert(AnimationInjector::LoadingProgress::Read().pending == 0);
    Prepare(&graph, ctx, 1);
    assert(manager.queues == 1 && manager.unloads == 0);
    RE::hkbClipGenerator failedClip;
    failedClip.animationName = "pair.hkx";
    RecoverFailedMark(ctx, &failedClip);  // A resident file is not a data-loss failure.
    Prepare(&graph, ctx, .01f);
    assert(manager.queues == 1);
    for (int repair = 1; repair <= 3; ++repair) {
        slot.binding = {};
        const auto checks = AnimationInjector::NodeFactory::residencyChecks;
        for (int frame = 0; frame < 1000; ++frame) Prepare(&graph, ctx, .016f);
        assert(manager.queues == repair);
        assert(AnimationInjector::NodeFactory::residencyChecks == checks);
        RequestCheck();
        RequestCheck();  // Lifecycle events coalesce into one audit.
        Prepare(&graph, ctx, .01f);
        assert(AnimationInjector::NodeFactory::residencyChecks == checks + 1);
        assert(manager.queues == repair);  // Audit alone does not reload.
        failedClip.animationName = "other.hkx";
        RecoverFailedMark(ctx, &failedClip);
        assert(!state.jobs[0].recover);
        failedClip.animationName = "pair.hkx";
        RecoverFailedMark(ctx, &failedClip);
        RecoverFailedMark(ctx, &failedClip);
        assert(manager.queues == repair);  // Never call engine loading inside GetMark.
        Prepare(&graph, ctx, .01f);
        assert(manager.queues == repair + 1 && manager.unloads == repair && manager.references == 1);
        Prepare(&graph, ctx, .01f);
        assert(slot.binding && state.jobs[0].finished);
    }
    slot.binding = {};
    RecoverFailedMark(ctx, &failedClip);
    Prepare(&graph, ctx, 1);
    assert(manager.queues == 4);  // Bounded recovery, even with repeated failed starts.
    slot.binding = {&manager.binding};
    RequestCheck();
    Prepare(&graph, ctx, .01f);
    assert(state.jobs[0].repairs == 0);  // A healthy lifecycle audit renews the budget.
    slot.binding = {};
    RecoverFailedMark(ctx, &failedClip);
    Prepare(&graph, ctx, .01f);
    Prepare(&graph, ctx, .01f);
    assert(manager.queues == 5 && slot.binding);
    Release(state.jobs[0], ctx, &manager, "test-teardown");
    assert(manager.references == 0 && manager.unloads == manager.queues);
    characters.clear();

    manager.allowLoad = false;
    slot.binding = {};
    Prepare(&graph, ctx, 0.01f);
    Prepare(&graph, ctx, 5.0f);
    assert(manager.references == 0 && manager.unloads == manager.queues);
    auto& retry = characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0];
    assert(retry.retryPending && retry.retryDelay == 1 && !retry.loader);
    RE::UI::paused = true;
    Prepare(&graph, ctx, 60);
    assert(manager.queues == 6 && retry.retryDelay == 1);
    RE::UI::paused = false;
    Prepare(&graph, ctx, .5f);
    assert(manager.queues == 6);
    Prepare(&graph, ctx, .5f);
    assert(manager.queues == 7 && manager.references == 1);
    Prepare(&graph, ctx, 5);
    assert(retry.retryPending && retry.retryDelay == 2 && manager.references == 0);
    manager.allowLoad = true;
    Prepare(&graph, ctx, 2);
    Prepare(&graph, ctx, .01f);
    assert(slot.binding && retry.finished && retry.timeouts == 0 && manager.references == 1);
    Release(retry, ctx, &manager, "test-teardown");
    assert(!retry.retryPending && manager.references == 0 && manager.queues == manager.unloads);
    characters.clear();
    slot.binding = {};
    // Teardown during a retry delay must cancel without another acquisition.
    manager.allowLoad = false;
    Prepare(&graph, ctx, .01f);
    Prepare(&graph, ctx, 5);
    auto& cancelled = characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0];
    assert(cancelled.retryPending);
    Release(cancelled, ctx, &manager, "test-teardown");
    characters.clear();
    assert(manager.references == 0);

    manager.allowLoad = true;
    manager.reportReadyWithoutBinding = true;
    Prepare(&graph, ctx, 0.01f);
    assert(!characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0].finished);
    Prepare(&graph, ctx, 0.01f);
    assert(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0].finished);
    Release(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0], ctx, &manager, "test-teardown");
    characters.clear();
    manager.reportReadyWithoutBinding = false;

    slot.binding = {&manager.binding};
    manager.allowLoad = true;
    Prepare(&graph, ctx, 0.01f);  // Native already-loaded result is valid only with a resident binding.
    const int queues = manager.queues;
    character.behaviorGraph = {&otherGraph};
    Prepare(&otherGraph, ctx, 1);
    assert(manager.queues == queues);  // Never reuse a loader with a replacement graph identity.
    character.behaviorGraph = {&graph};
    Release(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0], ctx, &manager, "test-teardown");
    assert(manager.references == 0);
    characters.clear();
    // Matching paths alone must not preload human-only clips into a chicken.
    using namespace AnimationInjector::PairedRig;
    // Real hkbCharacterStringData.name from Skyrim - Animations.bsa, not the
    // race's ChickenProject.hkx filename. Accept an extracted name for asset QA.
    strings.name = argc == 2 ? argv[1] : "ChickenCharater";
    const int beforeChicken = manager.queues;
    Prepare(&graph, ctx, .01f);
    assert(manager.queues == beforeChicken);
    assert(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs.empty());
    characters.clear();
    files[0].rigs |= Mask(Kind::kChicken);
    Prepare(&graph, ctx, .01f);
    assert(manager.queues == beforeChicken + 1);
    Release(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0], ctx, &manager, "chicken-teardown");
    assert(manager.references == 0);
    characters.clear();
    strings.name = "WolfProject";
    Prepare(&graph, ctx, .01f);
    assert(characters.empty() && manager.queues == beforeChicken + 1);
    static_assert(CharacterName("ChickenCharater") == Kind::kChicken);
    static_assert(CharacterName("chickencharater") == Kind::kChicken);
    static_assert(CharacterName("DefaultFemale") == Kind::kCharacter);
    static_assert(CharacterName("DefaultMale") == Kind::kCharacter);
    static_assert(CharacterName("ChickenProject") == Kind::kUnsupported);
    static_assert(CharacterName("ChickenCharater2") == Kind::kUnsupported);
    static_assert(ProjectPath("Actors/ambient/chicken/ChickenProject.hkx") == Kind::kChicken);
    static_assert(ProjectPath("Actors\\Character\\DefaultMale.hkx") == Kind::kCharacter);
    static_assert(ProjectPath("Actors/chicken/ChickenCharater.hkx") == Kind::kUnsupported);
    static_assert(ProjectPath("Actors/chicken/DefaultChicken.hkx") == Kind::kUnsupported);
    static_assert(SupportsRole(Kind::kChicken, Kind::kCharacter, 1));
    static_assert(SupportsRole(Kind::kChicken, Kind::kChicken, 0));
    static_assert(!SupportsRole(Kind::kChicken, Kind::kCharacter, 0));
    static_assert(!SupportsRole(Kind::kChicken, Kind::kChicken, 1));
    static_assert(!SupportsRole(Kind::kChicken, Kind::kUnsupported, 0));
    static_assert(SupportsRole(Kind::kCharacter, Kind::kCharacter, 0));
    for (const auto& profile : profiles) {
        const int before = manager.queues;
        strings.name = profile.character;
        files[0].rigs = Mask(profile.kind);
        Prepare(&graph, ctx, .01f);
        assert(manager.queues == before + 1);
        Release(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0], ctx, &manager, "profile-teardown");
        assert(manager.references == 0);
        characters.clear();
        files[0].rigs = 0;
        Prepare(&graph, ctx, .01f);
        assert(manager.queues == before + 1);
        assert(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs.empty());
        characters.clear();
    }
    // A broken asset settles as failed after four attempts, balancing every
    // native reference. It cannot hold the loading overlay open indefinitely.
    AnimationInjector::LoadingProgress::Reset();
    strings.name = "DefaultMale";
    files[0].rigs = Mask(Kind::kCharacter);
    slot.binding = {};
    manager.allowLoad = false;
    Prepare(&graph, ctx, .01f);
    for (int attempt = 1; attempt <= 4; ++attempt) {
        Prepare(&graph, ctx, 5.0f);
        if (attempt < 4) {
            assert(AnimationInjector::LoadingProgress::Read().pending == 1);
            Prepare(&graph, ctx, static_cast<float>(1u << (attempt - 1)));
        }
    }
    const auto failed = AnimationInjector::LoadingProgress::Read();
    assert(failed.failed == 1 && failed.pending == 0 && failed.ready == 0);
    assert(manager.references == 0 && manager.queues == manager.unloads);
    const int stoppedQueues = manager.queues;
    Prepare(&graph, ctx, 60);
    assert(manager.queues == stoppedQueues);
    characters.clear();

    // Load resets cancel old UI work, without unloading a live graph. A still
    // pending binding is re-counted on its next valid engine update.
    AnimationInjector::LoadingProgress::Reset();
    Prepare(&graph, ctx, .01f);
    ResetProgress();
    AnimationInjector::LoadingProgress::Reset();
    Prepare(&graph, ctx, .01f);
    assert(AnimationInjector::LoadingProgress::Read().pending == 1);
    Release(characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs[0], ctx, &manager, "reset-teardown");
    assert(AnimationInjector::LoadingProgress::Read().pending == 0);
    assert(manager.references == 0);
    characters.clear();

    // Large cold sets are queued over several graph updates, without missing
    // the denominator or treating budget wait as a failed load.
    AnimationInjector::LoadingProgress::Reset();
    files.clear();
    strings.animationNames.clear();
    std::vector<RE::hkbAnimationBindingWithTriggers> manySlots(65);
    std::vector<RE::hkbAnimationBindingWithTriggers*> manyPointers;
    for (int i = 0; i < 65; ++i) {
        const auto path = "pair" + std::to_string(i) + ".hkx";
        files.push_back({path, "loader"});
        strings.animationNames.push_back(path);
        manyPointers.push_back(&manySlots[i]);
    }
    set.bindings = {65, manyPointers.data()};
    manager.allowLoad = true;
    const int beforeBudget = manager.queues;
    Prepare(&graph, ctx, .01f);
    assert(manager.queues - beforeBudget == 16);
    assert(AnimationInjector::LoadingProgress::Read().total == 65);
    for (int i = 0; i < 6; ++i) Prepare(&graph, ctx, .01f);
    assert(manager.queues - beforeBudget == 65 && AnimationInjector::LoadingProgress::Read().ready == 65);
    for (auto& job : characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs)
        Release(job, ctx, &manager, "budget-teardown");
    assert(manager.references == 0 && manager.queues == manager.unloads);
    characters.clear();
    AnimationInjector::LoadingProgress::GameLoaded();
    AnimationInjector::LoadingProgress::Advance(0);
    for (int i = 0; i < 11; ++i) AnimationInjector::LoadingProgress::Advance(.1f);
    assert(!AnimationInjector::LoadingProgress::Read().visible);
    const int beforeCell = manager.queues;
    for (auto& nextSlot : manySlots) nextSlot.binding = {};
    Prepare(&graph, ctx, .01f);  // New cell/graph still preloads with the UI session closed.
    for (int i = 0; i < 6; ++i) Prepare(&graph, ctx, .01f);
    assert(manager.queues - beforeCell == 65);
    for (auto& job : characters.at(reinterpret_cast<std::uintptr_t>(&character)).jobs) {
        assert(job.finished && job.progress == 0);
        Release(job, ctx, &manager, "silent-cell-teardown");
    }
    assert(!AnimationInjector::LoadingProgress::Read().visible);
    assert(manager.references == 0 && manager.queues == manager.unloads);
    characters.clear();
    std::cout
        << "Production preloader: no completed polling, lifecycle audits, targeted failed-start recovery, bounded retries, 47 rigs and silent cell preparation passed\n";
    // One character per binding set prepares its files, and the references stay with the set: the
    // player takes a set over from an NPC by moving its jobs, a follower takes over the jobs a torn-down
    // holder left (a save load tears every graph down), and nothing is released or queued again. A
    // follower's failed start recovers through the holder.
    files.clear();
    files.push_back({"pair.hkx", "loader"});
    strings.animationNames = {"pair.hkx"};
    strings.name = "DefaultMale";
    set.bindings = {1, slots};
    slot.binding = {};
    holders.clear();
    parked.clear();
    RE::hkbBehaviorGraph npcGraph, otherNpcGraph, playerGraph;
    RE::hkbCharacter npc{{&npcGraph}, {&setup}, {&set}}, otherNpc{{&otherNpcGraph}, {&setup}, {&set}},
        player{{&playerGraph}, {&setup}, {&set}};
    RE::hkbContext npcCtx{&npc}, otherCtx{&otherNpc}, playerCtx{&player};
    RE::Actor playerActor{true};
    AnimationInjector::ClipHooks::actors = {{&player, &playerActor}};
    auto keyOf = [](RE::hkbCharacter& c) { return reinterpret_cast<std::uintptr_t>(&c); };
    const auto setKey = reinterpret_cast<std::uintptr_t>(&set);
    const int shareBase = manager.queues, unloadBase = manager.unloads;
    Prepare(&npcGraph, npcCtx, .01f);
    Prepare(&npcGraph, npcCtx, .01f);
    assert(manager.queues == shareBase + 1 && slot.binding && manager.references == 1);
    Prepare(&otherNpcGraph, otherCtx, .01f);
    assert(manager.queues == shareBase + 1 && characters.at(keyOf(otherNpc)).jobs.empty());
    Prepare(&playerGraph, playerCtx, .01f);
    assert(holders.at(setKey) == keyOf(player) && characters.at(keyOf(player)).jobs.size() == 1);
    assert(characters.at(keyOf(npc)).jobs.empty() && characters.at(keyOf(player)).settled);
    assert(manager.queues == shareBase + 1 && manager.unloads == unloadBase && manager.references == 1);
    Prepare(&npcGraph, npcCtx, .01f);
    assert(manager.queues == shareBase + 1 && manager.references == 1);
    // The holder's graph goes: its jobs wait for the set's next character, references kept.
    DeactivateHook::thunk(&playerGraph, playerCtx);
    assert(manager.references == 1 && !holders.contains(setKey) && parked.at(setKey).jobs.size() == 1);
    Prepare(&otherNpcGraph, otherCtx, .01f);
    assert(holders.at(setKey) == keyOf(otherNpc) && parked.empty() && characters.at(keyOf(otherNpc)).settled);
    assert(manager.queues == shareBase + 1 && manager.references == 1);
    // After a load the new graph of the same character takes its set's jobs back.
    DeactivateHook::thunk(&otherNpcGraph, otherCtx);
    RE::hkbBehaviorGraph reloadedGraph;
    otherNpc.behaviorGraph = {&reloadedGraph};
    Prepare(&reloadedGraph, otherCtx, .01f);
    assert(holders.at(setKey) == keyOf(otherNpc) && parked.empty() && manager.queues == shareBase + 1);
    slot.binding = {};
    RE::hkbClipGenerator followerClip;
    followerClip.animationName = "pair.hkx";
    RecoverFailedMark(npcCtx, &followerClip);
    assert(characters.at(keyOf(otherNpc)).jobs[0].recover);
    Prepare(&reloadedGraph, otherCtx, .01f);
    assert(manager.queues == shareBase + 2 && manager.references == 1);
    Prepare(&reloadedGraph, otherCtx, .01f);
    assert(slot.binding && characters.at(keyOf(otherNpc)).settled);
    // A file the engine dropped while the jobs waited is loaded again when they are taken over.
    DeactivateHook::thunk(&reloadedGraph, otherCtx);
    slot.binding = {};
    Prepare(&reloadedGraph, otherCtx, .01f);
    assert(manager.queues == shareBase + 3 && manager.references == 1 && !characters.at(keyOf(otherNpc)).settled);
    // A holder that stops updating without an observed teardown hands its jobs to a follower.
    int silent = 0;
    while (holders.at(setKey) != keyOf(npc) && silent < 700) {
        Prepare(&npcGraph, npcCtx, .016f);
        ++silent;
    }
    assert(silent > 600 && silent < 610);
    assert(characters.at(keyOf(npc)).jobs.size() == 1 && characters.at(keyOf(otherNpc)).jobs.empty());
    assert(manager.queues == shareBase + 3 && manager.references == 1);
    Prepare(&npcGraph, npcCtx, .016f);
    assert(characters.at(keyOf(npc)).settled && slot.binding);
    // A freed set's address reused by another project: its old jobs are not taken over.
    DeactivateHook::thunk(&npcGraph, npcCtx);
    assert(parked.at(setKey).project == "DefaultMale");
    strings.name = "DefaultFemale";
    characters.clear();
    player.behaviorGraph = {&playerGraph};
    Prepare(&playerGraph, playerCtx, .01f);
    assert(parked.empty() && manager.queues == shareBase + 4 && characters.at(keyOf(player)).jobs.size() == 1);
    // The old project's reference went with it; the new one is released here.
    Release(characters.at(keyOf(player)).jobs[0], playerCtx, &manager, "share-teardown");
    assert(manager.references == 1);
    manager.references = 0;
    manager.unloads = manager.queues;
    characters.clear();
    holders.clear();
    parked.clear();
    strings.name = "DefaultMale";
    AnimationInjector::ClipHooks::actors.clear();
    std::cout
        << "Production preloader: one holder per binding set, references kept across teardown, player and follower takeover, recovery passed\n";
    // The engine loads queued files one after another: a file behind others is not timed out while
    // the set makes progress, only once nothing became ready for 5 seconds.
    {
        files.clear();
        files.push_back({"a.hkx", "loader"});
        files.push_back({"b.hkx", "loader"});
        strings.animationNames = {"a.hkx", "b.hkx"};
        RE::hkbAnimationBindingWithTriggers first, second;
        RE::hkbAnimationBindingWithTriggers* pairSlots[] = {&first, &second};
        set.bindings = {2, pairSlots};
        manager.allowLoad = true;
        manager.blocked = 1;
        const int stallQueues = manager.queues;
        Prepare(&graph, ctx, .01f);
        assert(manager.queues == stallQueues + 2);
        Prepare(&graph, ctx, 3.0f);
        auto& stall = characters.at(reinterpret_cast<std::uintptr_t>(&character));
        assert(stall.jobs[0].finished && !stall.jobs[1].finished);
        const int unloadsBefore = manager.unloads;
        Prepare(&graph, ctx, 3.0f);  // 6 s queued, 3 s since progress: still waiting
        assert(!stall.jobs[1].retryPending && stall.jobs[1].queued && manager.unloads == unloadsBefore);
        Prepare(&graph, ctx, 3.0f);  // 6 s without progress: timed out
        assert(stall.jobs[1].retryPending && manager.unloads == unloadsBefore + 1);
        manager.blocked = -1;
        Release(stall.jobs[0], ctx, &manager, "stall-teardown");
        characters.clear();
        holders.clear();
        parked.clear();
        set.bindings = {1, slots};
        strings.animationNames = {"pair.hkx"};
        manager.references = 0;
        manager.unloads = manager.queues;
    }
    // Behind other requests (OAR's preloads after a save load): no timeout while the engine's queue moves, however
    // long the wait; once the queue itself stops for 5 seconds, the waiting file times out as before.
    {
        files.clear();
        files.push_back({"a.hkx", "loader"});
        strings.animationNames = {"a.hkx"};
        RE::hkbAnimationBindingWithTriggers only;
        RE::hkbAnimationBindingWithTriggers* oneSlot[] = {&only};
        set.bindings = {1, oneSlot};
        manager.allowLoad = false;
        manager.queuedAnimations.assign(100, 0);
        const int waitQueues = manager.queues, unloadsBefore = manager.unloads;
        Prepare(&graph, ctx, .01f);
        assert(manager.queues == waitQueues + 1);
        auto& wait = characters.at(reinterpret_cast<std::uintptr_t>(&character));
        for (int frame = 0; frame < 40; ++frame) {  // 20 s: one engine file read every half second
            manager.queuedAnimations.pop_back();
            ++manager.currentAnimation.fileInfo.crc32Filename;
            Prepare(&graph, ctx, .5f);
        }
        assert(!wait.jobs[0].retryPending && wait.jobs[0].queued && manager.unloads == unloadsBefore);
        assert(manager.queues == waitQueues + 1);  // never released and queued again
        manager.queuedAnimations.push_back(0);     // a request added behind: not a move
        Prepare(&graph, ctx, 3.0f);
        assert(!wait.jobs[0].retryPending);
        Prepare(&graph, ctx, 3.0f);  // the queue has stood still for 6 s: timed out
        assert(wait.jobs[0].retryPending && manager.unloads == unloadsBefore + 1);
        manager.allowLoad = true;
        manager.queuedAnimations.clear();
        manager.currentAnimation = {};
        characters.clear();
        holders.clear();
        parked.clear();
        set.bindings = {1, slots};
        strings.animationNames = {"pair.hkx"};
        manager.references = 0;
        manager.unloads = manager.queues;
    }
    std::cout << "Production preloader: timeouts only when the set and the engine's queue stop making progress "
                 "passed\n";
    files.clear();
    using namespace AnimationInjector;
    Actions::SpecType crossRig;
    crossRig.paired.targetRig = PairedRig::Kind::kChicken;
    crossRig.clips = {{"Animations\\AIHumanChicken\\ChickenCarry_Lower.hkx", "lead"},
        {"Animations\\AIHumanChicken\\ChickenCarry_Lower.hkx", "support"}};
    Actions::specs = {crossRig};
    Install();
    assert(files.size() == 2);
    assert(files[0].path == crossRig.clips[0].file);
    assert(files[0].rigs == PairedRig::Mask(PairedRig::Kind::kCharacter));
    assert(files[1].path == "..\\..\\character\\Animations\\AIHumanChicken\\ChickenCarry_Lower.hkx");
    assert(files[1].rigs == PairedRig::Mask(PairedRig::Kind::kChicken));
    std::cout
        << "Production preload install: same HKX via role-specific paths, no human/chicken path mixing or duplicate jobs passed\n";

    // 0.12.224: ordinary action files after the paired ones, for the character and the first-person
    // project; one file named twice (any case) is one job.
    {
        Actions::SpecType dodge, pair, stop;
        dodge.paired.enabled = false;
        dodge.clips = {{"Animations\\Dodge\\A.hkx", "dodgeA"}, {"Animations\\Dodge\\B.hkx", "dodgeB"}};
        pair.clips = {{"Animations\\Pair\\P.hkx", "lead"}, {"Animations\\Pair\\P.hkx", "support"}};
        stop.paired.enabled = false;
        stop.clips = {{"animations\\dodge\\a.HKX", "again"}, {"Animations\\Stop\\C.hkx", "stop"}};
        Actions::specs = {dodge, pair, stop};
        Install();  // no first-person copies on disk here
        const auto third = PairedRig::Mask(PairedRig::Kind::kCharacter), both = third | kFirstPerson;
        assert(files.size() == 4);
        assert(files[0].path == "Animations\\Pair\\P.hkx" && files[0].paired);
        assert(files[1].path == "Animations\\Dodge\\A.hkx" && !files[1].paired && files[1].rigs == third);
        assert(files[1].loaderName == "dodgeA_Preload");
        assert(files[2].path == "Animations\\Dodge\\B.hkx" && files[3].path == "Animations\\Stop\\C.hkx");
        assert(ProjectMask("FirstPerson") == kFirstPerson && ProjectMask("firstperson") == kFirstPerson);
        assert(ProjectMask("DefaultMale") == PairedRig::Mask(PairedRig::Kind::kCharacter) && ProjectMask(nullptr) == 0);

        // A budget keeps declaration order; a file whose size cannot be read counts as the average.
        const std::vector<OrdinaryClip> list = {{"a", "la"}, {"b", "lb"}, {"c", "lc"}, {"d", "ld"}};
        const auto size = [](const std::string& file) -> std::optional<std::uint64_t> {
            if (file == "c") return std::nullopt;  // counts as (40 + 30 + 10) / 3 = 26
            return file == "a" ? 40 : file == "b" ? 30 : 10;
        };
        Actions::specs = {};
        const auto none = [](const std::string&) { return false; };
        assert(BuildFiles(list, 80, size, none) == 1 && files.size() == 3 && files[2].path == "d");
        assert(BuildFiles(list, 0, size, none) == 0 && files.size() == 4);  // 0: no limit, sizes never read
        // 0.12.225: the projects follow the action's view; first person only where the copy is (a missing
        // file would stall the engine's queue), and a file no project takes is no job.
        const std::vector<OrdinaryClip> viewed = {
            {"a", "la", View::kThird}, {"b", "lb", View::kBoth}, {"c", "lc", View::kFirst}, {"d", "ld", View::kFirst}};
        BuildFiles(viewed, 0, size, [](const std::string& file) { return file != "d"; });
        assert(files.size() == 3 && files[0].rigs == third && files[1].rigs == both && files[2].rigs == kFirstPerson);

        // The first-person project takes the ordinary files only; the third person takes both.
        files.clear();
        files.push_back({"P.hkx", "lp"});
        files.push_back({"O.hkx", "lo", {}, both, false});
        strings.animationNames = {"P.hkx", "O.hkx"};
        RE::hkbAnimationBindingWithTriggers p, o;
        RE::hkbAnimationBindingWithTriggers* twoSlots[] = {&p, &o};
        set.bindings = {2, twoSlots};
        characters.clear();
        holders.clear();
        parked.clear();
        strings.name = "FirstPerson";
        Prepare(&graph, ctx, .01f);
        auto& firstPerson = characters.at(reinterpret_cast<std::uintptr_t>(&character));
        assert(firstPerson.jobs.size() == 1 && files[firstPerson.jobs[0].file].path == "O.hkx");
        characters.clear();
        holders.clear();
        strings.name = "DefaultMale";
        Prepare(&graph, ctx, .01f);
        auto& thirdPerson = characters.at(reinterpret_cast<std::uintptr_t>(&character));
        assert(thirdPerson.jobs.size() == 2 && files[thirdPerson.jobs[0].file].path == "P.hkx");
        characters.clear();
        holders.clear();
        parked.clear();
    }
    std::cout << "Production preload install: ordinary files after paired ones, first person, budget passed\n";
}

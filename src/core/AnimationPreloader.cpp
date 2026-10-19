#include "PCH.h"
#include "core/AnimationPreloader.h"
#include "core/LoadingProgress.h"
#include "core/NodeFactory.h"
#include "core/AnimationBindingWithTriggers.h"
#include "core/ClipHooks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/PairedRig.h"
#include "Settings.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace AnimationInjector::AnimationPreloader {
namespace {
struct File {
    std::string path, loaderName;
    // These real, non-playing clip objects belong to our process-lifetime
    // NodeFactory pool. Reuse them only after native Unload clears userData.
    std::vector<RE::hkbClipGenerator*> available;
    std::uint64_t rigs = PairedRig::Mask(PairedRig::Kind::kCharacter);
    bool paired = true;  // else an ordinary action's file
};
// The first-person project ("FirstPerson") holds the ordinary actions' files only; it is not a pair rig.
constexpr std::uint64_t kFirstPerson = std::uint64_t{1} << 63;
static_assert(PairedRig::profiles.size() < 63, "rig masks must leave the first-person bit free");
std::uint64_t ProjectMask(const char* project) {
    if (!project) return 0;
    if (PairedRig::Equal(project, "FirstPerson")) return kFirstPerson;
    return PairedRig::Mask(PairedRig::CharacterName(project));
}
struct Job {
    std::size_t file = 0;
    RE::hkbClipGenerator* loader = nullptr;
    bool queued = false, finished = false;
    float elapsed = 0.0f;
    unsigned repairs = 0;
    unsigned timeouts = 0;
    bool retryPending = false;
    float retryDelay = 0.0f;
    bool repairLimitReported = false;
    bool recover = false;
    LoadingProgress::Token progress = 0;
};
// The engine's file queue at one look: the file it is reading and how many requests wait behind it.
struct EngineQueueView {
    std::uint64_t head = 0;
    std::uint32_t waiting = 0;
    bool seen = false;
};
// Whether the engine's queue moved since `last` (which becomes now). It reads one file after another, about one a
// frame, so another file being read or fewer waiting means it is working through earlier requests: ours may wait
// behind many (OAR queues about 1,200 at the main menu and the rest is read after a save load, about 10 s; measured
// 2026-10-03), which is not a stall. Read without the engine's lock: a torn read only looks like one more move.
bool EngineQueueMoved(EngineQueueView& last, const RE::AnimationFileManagerSingleton& manager) {
    const auto& info = manager.currentAnimation.fileInfo;
    const EngineQueueView now{(std::uint64_t{info.crc32Filename} << 32) | info.crc32Path,
        static_cast<std::uint32_t>(manager.queuedAnimations.size()), true};
    const bool moved = last.seen && (now.head != last.head || now.waiting < last.waiting);
    last = now;
    return moved;
}
struct CharacterJobs {
    std::uintptr_t graph = 0, bindings = 0;
    std::uint64_t rig = 0;
    bool player = false;
    bool mismatchReported = false;
    bool built = false;  // jobs made while this character holds its binding set
    bool followerReported = false;
    std::uint64_t updates = 0;        // this character's graph updates seen here
    std::uint64_t holderUpdates = 0;  // follower: the holder's count at its last change
    unsigned holderSilent = 0;        // follower: own updates since that change
    std::vector<Job> jobs;
    bool checkRequested = false;
    bool settled = false;
    float sinceProgress = 0.0f;  // seconds since one of the jobs became ready or the engine's queue moved
    EngineQueueView engine;      // the engine's queue at this character's last update
};
// A binding set's jobs whose holder's graph went (a save load tears every graph down). Their native
// references are kept, as OAR keeps its preloads: releasing them let the engine drop the files, and
// the next load read every paired file from disk again (about 8.6 s with 1,640 files). The next
// character of the set takes them over as they are.
struct Parked {
    std::string project;
    std::vector<Job> jobs;
    bool settled = false;
};
std::unordered_map<std::uintptr_t, Parked> parked;  // binding set -> jobs
std::vector<File> files;
// Numeric identities are compared only; never dereferenced after a callback.
std::unordered_map<std::uintptr_t, CharacterJobs> characters;
// Binding set -> the character whose jobs hold that set's paired files. Characters of one project
// share their binding set, and a native Queue reference keeps a binding resident for every character
// using it, so one of them prepares the files (the player when it uses the set) and the others
// follow. Every character used to queue every paired file itself: with thousands of pairs, each NPC
// that came into range allocated a loader per file and queued them all again.
std::unordered_map<std::uintptr_t, std::uintptr_t> holders;
std::recursive_mutex mutex;

bool IsPlayer(const RE::hkbContext& ctx) {
    auto* actor = ClipHooks::ActorForContext(ctx);
    return actor && actor->IsPlayerRef();
}

RE::hkbAnimationBindingSet* BindingSet(RE::hkbCharacter* ch) {
    auto* set = ch ? static_cast<RE::hkbAnimationBindingSet*>(ch->animationBindingSet.get()) : nullptr;
    if (!set && ch && ch->setup) set = ch->setup->animationBindingSet.get();
    return set;
}
RE::hkbClipGenerator* Acquire(std::size_t file, int index) {
    auto& pool = files[file];
    if (pool.available.empty())
        return NodeFactory::MakeClip(
            pool.loaderName.c_str(), pool.path.c_str(), static_cast<std::uint16_t>(index), nullptr, nullptr, false);
    auto* loader = pool.available.back();
    pool.available.pop_back();
    loader->animationBindingIndex = static_cast<std::uint16_t>(index);
    return loader;
}
// A successful past Queue/Load does not guarantee that this binding is still resident.
std::optional<bool> Resident(RE::hkbAnimationBindingSet* set, int index) {
    if (index < 0 || index >= set->bindings._size) return std::nullopt;
    auto* slot = set->bindings._data[index];
    if (!NodeFactory::SameVTable(slot, RE::VTABLE_hkbAnimationBindingWithTriggers)) return std::nullopt;
    return slot->binding && slot->binding->animation;
}
void Release(Job& job, const RE::hkbContext& ctx, RE::AnimationFileManagerSingleton* manager, const char* reason,
    bool settleProgress = true) {
    if (settleProgress) LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kCancelled);
    job.retryPending = false;
    job.retryDelay = 0;
    auto* loader = job.loader;
    if (!loader) return;
    if (job.queued) {
        const auto result = manager->Unload(ctx, loader, nullptr);
        logger::trace("[Preload] release character={:X} file='{}' reason={} result={} userData={}",
            reinterpret_cast<std::uintptr_t>(ctx.character), files[job.file].path, reason, result, loader->userData);
    }
    if (loader->userData == 0 && !loader->animationControl)
        files[job.file].available.push_back(loader);
    else
        logger::warn("[Preload] loader not recycled: native state is not clear");
    job.loader = nullptr;
    job.queued = false;
    job.finished = true;
    job.retryPending = false;
    job.retryDelay = 0;
}

void Prepare(RE::hkbBehaviorGraph* graph, const RE::hkbContext& ctx, float delta) {
    if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) return;
    auto* ch = ctx.character;
    if (!ch || graph != ch->behaviorGraph.get() || !std::isfinite(delta) || delta <= 0.0f) return;
    auto* setup = ch->setup.get();
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    const char* project = strings ? strings->name.c_str() : nullptr;
    auto* set = BindingSet(ch);
    auto* manager = RE::AnimationFileManagerSingleton::GetSingleton();
    if (!set || !manager || !set->bindings._data) return;
    std::lock_guard lock(mutex);
    const auto key = reinterpret_cast<std::uintptr_t>(ch);
    auto found = characters.find(key);
    if (found == characters.end()) {
        const std::uint64_t rig = ProjectMask(project);
        if (!rig) return;
        CharacterJobs state;
        state.graph = reinterpret_cast<std::uintptr_t>(graph);
        state.bindings = reinterpret_cast<std::uintptr_t>(set);
        state.rig = rig;
        state.player = IsPlayer(ctx);
        found = characters.emplace(key, std::move(state)).first;
    }
    auto& state = found->second;
    // Never apply a stale loader to a replacement graph or binding set.
    if (state.graph != reinterpret_cast<std::uintptr_t>(graph) ||
        state.bindings != reinterpret_cast<std::uintptr_t>(set)) {
        if (!state.mismatchReported) {
            state.mismatchReported = true;
            logger::error(
                "[Preload] character={:X} graph/binding identity changed without observed teardown; preload skipped",
                key);
            for (auto& job : state.jobs) LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kFailed);
        }
        return;
    }
    ++state.updates;
    auto& holder = holders[state.bindings];
    if (holder != key) {
        // A holder whose graph stopped updating (its teardown not observed) is replaced after this
        // follower's own 600 updates, about 10 seconds; both stop while the game is paused.
        constexpr unsigned kSilentLimit = 600;
        const auto current = characters.find(holder);
        if (current != characters.end()) {
            if (current->second.updates != state.holderUpdates) {
                state.holderUpdates = current->second.updates;
                state.holderSilent = 0;
            } else
                ++state.holderSilent;
        }
        if (current == characters.end() || (state.player && !current->second.player) ||
            state.holderSilent > kSilentLimit) {
            // The jobs move with the set: a native reference counts for the set, whichever character
            // of it queued the file, so nothing is released or queued again.
            if (current != characters.end()) {
                logger::info("[Preload] character={:X}{} takes the binding set from character={:X}{} ({} files)", key,
                    state.player ? " (player)" : "", holder,
                    state.holderSilent > kSilentLimit ? ", no longer updating" : "", current->second.jobs.size());
                state.jobs = std::move(current->second.jobs);
                current->second.jobs.clear();
                state.settled = current->second.settled;
                state.built = current->second.built;
                current->second.built = false;
                current->second.settled = false;
            }
            holder = key;
            state.holderSilent = 0;
        }
    }
    if (holder != key) {
        auto& current = characters.at(holder);
        if (!current.followerReported) {
            current.followerReported = true;
            logger::info("[Preload] character={:X} shares the binding set of character={:X}; its files are held once",
                key, holder);
        }
        return;
    }
    if (!state.built) {
        state.built = true;
        if (const auto kept = parked.find(state.bindings); kept != parked.end()) {
            const bool same = project && kept->second.project == project;
            if (same) {
                state.jobs = std::move(kept->second.jobs);
                state.settled = kept->second.settled;
                // A file the engine dropped anyway is loaded again now, not at a failed start.
                std::size_t dropped = 0;
                for (auto& job : state.jobs)
                    if (job.finished && job.loader &&
                        !Resident(set, job.loader->animationBindingIndex).value_or(true)) {
                        job.recover = true;
                        job.repairs = 0;
                        ++dropped;
                    }
                if (dropped) state.settled = false;
                logger::info("[Preload] character={:X} takes over {} held files of set={:X} ({} dropped by "
                             "the engine, loading them again)",
                    key, state.jobs.size(), state.bindings, dropped);
            } else
                // Another project at a freed set's address: the old project's references went with it.
                logger::warn("[Preload] set={:X} now belongs to '{}' (held for '{}'); preparing again", state.bindings,
                    project ? project : "?", kept->second.project);
            parked.erase(kept);
        }
    }
    if (state.jobs.empty() && !state.settled) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (!(files[i].rigs & state.rig)) continue;
            const int index = NodeFactory::FindAnimationName(strings->animationNames, files[i].path.c_str());
            if (index < 0 || index > 32767 || index >= set->bindings._size || !set->bindings._data[index]) continue;
            auto* loader = Acquire(i, index);
            state.jobs.push_back({i, loader});
            state.jobs.back().progress = LoadingProgress::Begin(LoadingProgress::Phase::kAnimation);
        }
        state.settled = state.jobs.empty();
    }
    // Completed graphs have no periodic residency scan. Lifecycle events
    // request one audit; a failed native mark alone can request recovery.
    if (state.settled && !state.checkRequested) return;
    const bool audit = state.checkRequested;
    state.checkRequested = false;
    // The engine loads queued files one after another: a file waiting behind others is not stuck. A job times
    // out only when for as long no job of the set became ready and the engine's queue did not move (a request
    // that never completes holds up every one behind it; releasing ours lets the queue go on).
    state.sinceProgress += delta;
    if (EngineQueueMoved(state.engine, *manager)) state.sinceProgress = 0;
    unsigned queues = 0;
    for (auto& job : state.jobs) {
        if ((!job.finished || job.retryPending) && !job.progress)
            job.progress = LoadingProgress::Begin(LoadingProgress::Phase::kAnimation);
        if (!job.loader) {
            if (!job.retryPending) continue;
            job.retryDelay -= delta;
            if (job.retryDelay > 0) continue;
            const int index = NodeFactory::FindAnimationName(strings->animationNames, files[job.file].path.c_str());
            if (index < 0 || index > 32767 || index >= set->bindings._size || !set->bindings._data[index]) {
                if (++job.timeouts >= 4) {
                    job.retryPending = false;
                    job.finished = true;
                    LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kFailed);
                    logger::error(
                        "[Preload] retry index unavailable character={:X} file='{}'", key, files[job.file].path);
                } else
                    job.retryDelay = 8.0f;
                continue;
            }
            job.loader = Acquire(job.file, index);
            job.retryPending = false;
            job.finished = false;
            job.elapsed = 0;
            logger::info(
                "[Preload] retry character={:X} file='{}' timeouts={}", key, files[job.file].path, job.timeouts);
        }
        if (job.finished) {
            if (!audit && !job.recover) continue;
            const int index = job.loader->animationBindingIndex;
            const auto resident = Resident(set, index);
            if (!resident) continue;
            if (*resident) {
                job.recover = false;
                if (audit) {
                    job.repairs = 0;
                    job.repairLimitReported = false;
                }
                continue;
            }
            if (!job.recover) {
                logger::debug("[Preload] lifecycle audit: missing character={:X} file='{}'; waiting for failed start",
                    key, files[job.file].path);
                continue;
            }
            job.recover = false;
            if (job.repairs >= 3) {
                if (!job.repairLimitReported) {
                    job.repairLimitReported = true;
                    logger::error("[Preload] repair limit reached character={:X} file='{}' index={}", key,
                        files[job.file].path, index);
                    job.progress = LoadingProgress::Begin(LoadingProgress::Phase::kAnimation);
                    LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kFailed);
                }
                continue;
            }
            ++job.repairs;
            logger::warn("[Preload] failed-start recovery character={:X} file='{}' index={} userData={} repair={}/3",
                key, files[job.file].path, index, job.loader->userData, job.repairs);
            // Balance our old Queue before reacquiring. Never assign an engine binding or userData.
            Release(job, ctx, manager, "binding-lost");
            job.loader = Acquire(job.file, index);
            job.finished = false;
            job.elapsed = 0;
            job.progress = LoadingProgress::Begin(LoadingProgress::Phase::kAnimation);
        }
        if (!job.queued) {
            // Stagger cold acquisitions for large declaration sets. Waiting
            // for this budget is not a native I/O timeout.
            if (queues >= 16) continue;
            ++queues;
            job.elapsed = 0;
            const auto result = manager->Queue(ctx, job.loader, nullptr);
            job.queued = true;
            logger::trace("[Preload] queue character={:X} project='{}' file='{}' index={} result={} userData={}", key,
                project, files[job.file].path, job.loader->animationBindingIndex, result, job.loader->userData);
            // Queue=1 is the native already-loaded result. Keep its reference
            // until graph teardown so GetMark can read the asset immediately.
            if (result == 1 && Resident(set, job.loader->animationBindingIndex).value_or(false)) {
                job.finished = true;
                job.timeouts = 0;
                state.sinceProgress = 0;
                LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kReady);
                logger::trace("[Preload] ready character={:X} project='{}' file='{}' already-loaded", key, project,
                    files[job.file].path);
            }
            continue;
        }
        job.elapsed += delta;
        const bool loaded = manager->Load(ctx, job.loader, nullptr);
        if (loaded && Resident(set, job.loader->animationBindingIndex).value_or(false)) {
            job.finished = true;
            job.timeouts = 0;
            state.sinceProgress = 0;
            LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kReady);
            logger::trace("[Preload] ready character={:X} project='{}' file='{}' elapsed={:.3f} userData={}", key,
                project, files[job.file].path, job.elapsed, job.loader->userData);
        } else if (job.elapsed >= 5.0f && state.sinceProgress >= 5.0f) {
            Release(job, ctx, manager, "timeout", false);
            job.timeouts = std::min(job.timeouts + 1, 4u);
            if (job.timeouts >= 4) {
                LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kFailed);
                logger::error(
                    "[Preload] load failed after 4 attempts character={:X} file='{}'", key, files[job.file].path);
                continue;
            }
            job.retryDelay = static_cast<float>(1u << (job.timeouts - 1));
            job.retryPending = true;
            logger::warn("[Preload] retry scheduled character={:X} file='{}' delay={:.1f}", key, files[job.file].path,
                job.retryDelay);
        }
    }
    const bool settled = std::all_of(
        state.jobs.begin(), state.jobs.end(), [](const Job& job) { return job.finished && !job.retryPending; });
    if (settled && !state.settled && !state.jobs.empty())
        logger::info("[Preload] settled character={:X} files={} failed={} periodicChecks=off", key, state.jobs.size(),
            std::count_if(state.jobs.begin(), state.jobs.end(), [](const Job& job) { return !job.loader; }));
    state.settled = settled;
}
struct UpdateHook {
    static void thunk(RE::hkbBehaviorGraph* graph, const RE::hkbContext& ctx, float delta) {
        Prepare(graph, ctx, delta);
        original(graph, ctx, delta);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct DeactivateHook {
    static void thunk(RE::hkbBehaviorGraph* graph, const RE::hkbContext& ctx) {
        if (ctx.character && graph == ctx.character->behaviorGraph.get()) {
            std::lock_guard lock(mutex);
            const auto key = reinterpret_cast<std::uintptr_t>(ctx.character);
            auto found = characters.find(key);
            if (found != characters.end() && found->second.graph == reinterpret_cast<std::uintptr_t>(graph) &&
                found->second.bindings == reinterpret_cast<std::uintptr_t>(BindingSet(ctx.character))) {
                auto& state = found->second;
                // The set keeps its references (see Parked); another character of it, or this one
                // after a load, takes the jobs over.
                if (state.built && (!state.jobs.empty() || state.settled)) {
                    const auto* setup = ctx.character->setup.get();
                    const auto* data = setup ? setup->data.get() : nullptr;
                    const auto* strings = data ? data->stringData.get() : nullptr;
                    auto& kept = parked[state.bindings];
                    kept.project = strings && strings->name.c_str() ? strings->name.c_str() : "";
                    kept.jobs = std::move(state.jobs);
                    kept.settled = state.settled;
                }
                if (const auto holder = holders.find(state.bindings); holder != holders.end() && holder->second == key)
                    holders.erase(holder);
                characters.erase(found);
            }
        }
        original(graph, ctx);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void RequestCheck() {
    std::lock_guard lock(mutex);
    for (auto& [key, state] : characters) state.checkRequested = true;
}
void RecoverFailedMark(const RE::hkbContext& ctx, RE::hkbClipGenerator* clip) {
    if (!ctx.character || !clip || clip->animationControl) return;
    std::lock_guard lock(mutex);
    const auto found = characters.find(reinterpret_cast<std::uintptr_t>(ctx.character));
    auto* set = BindingSet(ctx.character);
    if (found == characters.end() || !set || !set->bindings._data) return;
    if (found->second.graph != reinterpret_cast<std::uintptr_t>(ctx.character->behaviorGraph.get()) ||
        found->second.bindings != reinterpret_cast<std::uintptr_t>(set))
        return;
    // A follower's binding set is prepared by its holder.
    const auto holder = holders.find(found->second.bindings);
    const auto owner = holder != holders.end() ? characters.find(holder->second) : found;
    if (owner == characters.end() || owner->second.bindings != found->second.bindings) return;
    auto& state = owner->second;
    // Match our preloaded file as well as the current project index. OAR or
    // unrelated native clips must not cause recovery of an arbitrary slot.
    for (auto& job : state.jobs) {
        if (!job.finished || !job.loader || job.retryPending || job.recover ||
            job.loader->animationBindingIndex != clip->animationBindingIndex || !clip->animationName.c_str() ||
            _stricmp(files[job.file].path.c_str(), clip->animationName.c_str()) != 0)
            continue;
        const auto resident = Resident(set, clip->animationBindingIndex);
        if (!resident || *resident) return;
        job.recover = true;
        state.settled = false;
        return;
    }
}
void ResetProgress() {
    std::lock_guard lock(mutex);
    for (auto& [key, state] : characters)
        for (auto& job : state.jobs) LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kCancelled);
    for (auto& [set, kept] : parked)
        for (auto& job : kept.jobs) LoadingProgress::Finish(job.progress, LoadingProgress::Outcome::kCancelled);
}
namespace {
// An ordinary action's clip: its file and the name of the loader clip made for it.
struct OrdinaryClip {
    std::string file, loader;
    View view = View::kThird;  // its action's (runtime/ViewSpec.h)
};
// The file list: paired files first (each with the rigs that use it), then ordinary files in declaration
// order until `budget` bytes of file size (0: no limit). `size` reads a file's size; a file it cannot read
// counts as the average of those it can. An ordinary file goes to the projects of its action's view: the
// third-person ones, and the first-person one only when `firstPerson` finds the copy there. That project
// reads its names under Actors\Character\_1stPerson, and a request for a file that is not there never
// completes and holds up the files queued after it (the engine reads them one after another). Returns the
// ordinary files left out by the budget.
std::size_t BuildFiles(const std::vector<OrdinaryClip>& ordinary, std::uint64_t budget,
    const std::function<std::optional<std::uint64_t>(const std::string&)>& size,
    const std::function<bool(const std::string&)>& firstPerson) {
    files.clear();
    std::unordered_map<std::string, std::size_t> byPath;  // lowercase path -> files index
    const auto lower = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const auto add = [&](const std::string& path, const std::string& loader, std::uint64_t mask, bool paired) {
        const auto [it, added] = byPath.try_emplace(lower(path), files.size());
        if (added)
            files.push_back({path, loader, {}, mask, paired});
        else {
            files[it->second].rigs |= mask;
            files[it->second].paired |= paired;
        }
    };
    for (int a = 0; a < Actions::Count(); ++a) {
        const auto& spec = Actions::Spec(a);
        if (!spec.paired.enabled) continue;
        for (const auto& clip : spec.clips)
            for (const auto rig : {PairedRig::Kind::kCharacter, spec.paired.targetRig})
                add(PairedRig::ClipPath(clip.file, rig), clip.clipName + "_Preload", PairedRig::Mask(rig), true);
    }
    const auto mask = [&](const OrdinaryClip& clip) {
        std::uint64_t bits = InThird(clip.view) ? PairedRig::Mask(PairedRig::Kind::kCharacter) : 0;
        if (InFirst(clip.view) && firstPerson(clip.file)) bits |= kFirstPerson;
        return bits;
    };
    if (!budget) {
        for (const auto& clip : ordinary)
            if (const auto bits = mask(clip)) add(clip.file, clip.loader, bits, false);
        return 0;
    }
    std::vector<std::optional<std::uint64_t>> sizes;
    std::uint64_t known = 0, knownCount = 0;
    for (const auto& clip : ordinary) {
        sizes.push_back(size(clip.file));
        if (sizes.back()) {
            known += *sizes.back();
            ++knownCount;
        }
    }
    const std::uint64_t average = knownCount ? known / knownCount : 0;
    std::uint64_t used = 0;
    std::size_t left = 0;
    for (std::size_t i = 0; i < ordinary.size(); ++i) {
        const auto bytes = sizes[i].value_or(average);
        if (used + bytes > budget) {
            ++left;
            continue;
        }
        used += bytes;
        if (const auto bits = mask(ordinary[i])) add(ordinary[i].file, ordinary[i].loader, bits, false);
    }
    return left;
}
// A declared clip's size on disk (Data\Meshes\Actors\Character\<file>), or nothing for a file in an archive.
std::optional<std::uint64_t> LooseFileSize(const std::string& file) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(std::filesystem::path("Data/Meshes/Actors/Character") / file, error);
    return error ? std::nullopt : std::optional<std::uint64_t>(bytes);
}
// A declared clip's first-person copy, loose or in an archive (the engine's own file lookup).
bool InFirstPerson(const std::string& file) {
    RE::BSResourceNiBinaryStream stream(std::string("Meshes\\Actors\\Character\\_1stPerson\\") + file);
    if (stream.good()) return true;
    logger::warn("[Preload] no first-person copy of '{}' (Meshes\\Actors\\Character\\_1stPerson): not preloaded", file);
    return false;
}
}
void Install() {
    const auto& settings = FrameworkSettings::Get();
    if (!settings.injectState) return;
    std::vector<OrdinaryClip> ordinary;
    if (settings.preloadOrdinary)
        for (int a = 0; a < Actions::Count(); ++a) {
            const auto& spec = Actions::Spec(a);
            if (spec.paired.enabled) continue;
            for (const auto& clip : spec.clips)
                ordinary.push_back({clip.file, clip.clipName + "_Preload", spec.view.view});
        }
    const auto budget = static_cast<std::uint64_t>(settings.preloadOrdinaryBudgetMB) << 20;
    const auto left = BuildFiles(ordinary, budget, LooseFileSize, InFirstPerson);
    if (files.empty()) return;
    REL::Relocation<std::uintptr_t> graph{RE::VTABLE_hkbBehaviorGraph[0]};
    UpdateHook::original = graph.write_vfunc(5, UpdateHook::thunk);
    DeactivateHook::original = graph.write_vfunc(7, DeactivateHook::thunk);
    const auto paired = std::count_if(files.begin(), files.end(), [](const File& file) { return file.paired; });
    std::string note;
    if (!settings.preloadOrdinary)
        note = " (ordinary preloading off)";
    else if (left)
        note = " (" + std::to_string(left) + " past the " + std::to_string(settings.preloadOrdinaryBudgetMB) +
               " MB budget load when played)";
    const auto firstPerson =
        std::count_if(files.begin(), files.end(), [](const File& file) { return file.rigs & kFirstPerson; });
    logger::info("[Preload] graph Update/Deactivate installed: {} paired + {} ordinary files ({} in first person){}",
        paired, files.size() - paired, firstPerson, note);
}
}

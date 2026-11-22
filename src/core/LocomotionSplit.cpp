#include "core/LocomotionSplit.h"
#include "core/LocomotionDrive.h"
#include "core/LocomotionToggle.h"
#include "core/AnimDataRegistry.h"
#include "core/BlenderLayout.h"
#include "core/NodeFactory.h"
#include "core/OwnLocomotion.h"
#include "core/VanillaLadders.h"
#include "runtime/ActionRegistry.h"
#include "Settings.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace AnimationInjector::LocomotionSplit {
namespace {
using Havok::BlenderGeneratorChildLayout;
using Havok::BlenderGeneratorLayout;
using Havok::ManualSelectorLayout;
using Havok::NodeNamed;
using Havok::Readable;
using Havok::StateTaggingLayout;

constexpr std::uint8_t kDontConvertAnnotations = 16;  // hkbClipGenerator::FLAG_DONT_CONVERT_ANNOTATIONS_TO_TRIGGERS

struct RawArr {
    void* data;
    std::int32_t size;
    std::int32_t capFlags;
};
struct Child {
    RE::hkbNode* node;
    void* info;
};
// The node's children as the engine lists them (hkbNode virtuals 8 and 9), as GraphInjector's ListChildren.
int ListChildren(RE::hkbNode* node, std::vector<Child>& buffer) {
    using GetMaxFn = int (*)(RE::hkbNode*, int);
    using GetChildrenFn = void (*)(RE::hkbNode*, int, RawArr*);
    auto** vt = *reinterpret_cast<void***>(node);
    const int max = reinterpret_cast<GetMaxFn>(vt[8])(node, 0);
    if (max <= 0 || max > 4096) return 0;
    buffer.assign(static_cast<std::size_t>(max), Child{});
    RawArr arr{
        buffer.data(), 0, static_cast<std::int32_t>(static_cast<std::uint32_t>(max) | NodeFactory::kDontDealloc)};
    struct ChildrenOut {
        RawArr* arr;
        std::int32_t n;
        std::int32_t pad;
    } out{&arr, 0, 0};
    reinterpret_cast<GetChildrenFn>(vt[9])(node, 0, reinterpret_cast<RawArr*>(&out));
    return std::clamp(arr.size, 0, max);
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A clip in the new ladder: the node, the vanilla clip it came from and its total playback multiplier.
struct Slot {
    RE::hkbClipGenerator* clip = nullptr;
    std::string sourceName;  // the vanilla clip name (animation data key)
    float rate = 1.0f;
};

std::string RateTag(float rate) { return fmt::format("{}", static_cast<int>(std::lround(rate * 1000.0f))); }

// ── On/off (Toggle): each patched blender's child weights with the split on and off ──────────────────────────────
//   on   the vanilla slots far below the axis in their order (-1e6 + index), the walk clip at kWalkAxis, the run clip
//        at kRunAxis: the axis (kMinAxis .. kRunAxis) lies between the two clips only
//   off  the vanilla slots at their own weights, the two clips far above (1e6, 1e6 + 1): vanilla
// The array is ascending either way and no slot is added or dropped: a live blender's per-instance arrays are sized
// to its children. By "graph|blender" (lowered): blenders of one name differ between graphs (Shout_Behavior's
// MT_ForwardRightBlend has four slots, MT_RootBehavior's five).
struct Values {
    float on;
    float off;
};
constexpr float kParked = 1.0e6f;
std::mutex g_toggleMutex;
std::unordered_map<std::string, std::vector<Values>> g_layouts;
struct Template {
    BlenderGeneratorLayout* blender;
    std::string key;
};
std::vector<Template> g_templates;  // the patched template blenders (new clones copy them)
std::atomic_bool g_enabled{true};

// -- The walk clip's bone mask --------------------------------------------------------------------
// hkbBoneWeightArray (Havok 2010.2): hkbBindable 0x30 | boneWeights hkArray<hkReal> 0x30. Indexed by the character's
// animation skeleton bones; vanilla behaviors carry arrays of 99 (the vanilla skeleton) or fewer.
struct BoneWeightArrayLayout {
    std::uint8_t bindable[0x30];
    RE::hkArray<float> boneWeights;
};
static_assert(sizeof(BoneWeightArrayLayout) == 0x40);
// The walk buffer's weight on an upper-body bone. Not 0: Havok normalizes each bone's weights over the children, and
// at the walk end (the run clip's weight 0) the walk clip is the only child left; a bone with no weight at all
// would have nothing to take its pose from. The upper body's walk share is m(1-r) / (m(1-r) + r) for the run side's
// weight r: with 0.001 it flipped from run to walk inside the mix's last hundredth, one or two frames as a run eased
// into a walk (11-15, CapsLock while running: the hand moved 5.2 in a frame at the walk end, vanilla 0.8). With 0.1 the
// handover spreads over the mix's last third (walk share 9% at r 0.5, 47% at 0.1, 83% at 0.02). A stop holds the
// axis near the run (LocomotionDrive), so its cross-fade still keeps the run's upper body.
constexpr float kMaskedUpper = 0.1f;
struct UpperMask {
    BoneWeightArrayLayout* array;
    std::vector<int> upper;  // the upper-body bone indices (kMaskedUpper when on, 1 when off)
};
std::vector<UpperMask> g_masks;  // g_toggleMutex
// The mask's classification by bone (UpperBone of the skeleton it was made from), and the skeletons CheckMask saw.
std::vector<bool> g_maskUpper;                                  // g_toggleMutex
std::string g_maskSkeleton;                                     // g_toggleMutex
std::unordered_set<const RE::hkaSkeleton*> g_checkedSkeletons;  // g_toggleMutex

// The upper body as vanilla's 2HW locomotion splits it (1hm_locomotion #0158, 2HW_LocomotionArmBlend's mask): Spine2
// and everything above it, the hands' attachment nodes (weapon, shield, quiver, bow, magic nodes, AnimObjectL/R).
// Root to Spine1, the legs, the skirt bones and the hip sheaths (WeaponSword, WeaponAxe, ...) are the lower body.
// By name, so skeletons with added bones (XPMSE) are classified too.
bool UpperBone(const char* name) {
    if (!name) return false;
    static constexpr std::array exact{
        "Shield", "Weapon", "Quiver", "WeaponBack", "WeaponBow", "AnimObjectL", "AnimObjectR", "NPCEyeBone"};
    for (const char* e : exact)
        if (_stricmp(name, e) == 0) return true;
    static constexpr std::array parts{"spine2", "spine3", "clavicle", "upperarm", "forearm", "hand", "finger",
        "pauldron", "magicnode", "neck", "head", "item0"};
    const auto lower = Lower(name);
    for (const char* part : parts)
        if (lower.find(part) != std::string::npos) return true;
    return false;
}

// One mask for the character's skeleton: lower body 1, upper body kMaskedUpper. Null (one warning) when the skeleton
// is not there yet.
BoneWeightArrayLayout* MakeUpperMask(RE::hkbCharacter* character) {
    auto* setup = character ? character->setup.get() : nullptr;
    const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
    const int n = skeleton ? skeleton->bones._size : 0;
    if (!skeleton || n <= 0 || n > 1024 || !skeleton->bones._data) {
        logger::warn("[Locomotion] the character has no animation skeleton at the graph pass ({} bones); the walk "
                     "clips get no bone mask (a walk/run change shows the walk's upper body too)",
            n);
        return nullptr;
    }
    auto* mask = static_cast<BoneWeightArrayLayout*>(NodeFactory::AllocZero(sizeof(BoneWeightArrayLayout)));
    NodeFactory::InitRefObj(mask, RE::VTABLE_hkbBoneWeightArray);
    NodeFactory::EmptyArray(reinterpret_cast<RE::hkbBindable*>(mask)->cachedBindables);
    auto* weights = static_cast<float*>(NodeFactory::AllocZero(sizeof(float) * static_cast<std::size_t>(n)));
    UpperMask entry{mask, {}};
    std::string names;
    const float masked = g_enabled.load() ? kMaskedUpper : 1.0f;
    for (int i = 0; i < n; ++i) {
        const char* name = skeleton->bones._data[i].name.c_str();
        const bool upper = UpperBone(name);
        weights[i] = upper ? masked : 1.0f;
        if (upper) {
            entry.upper.push_back(i);
            names += fmt::format("{}{}", names.empty() ? "" : ", ", name ? name : "?");
        }
    }
    NodeFactory::OwnArray(mask->boneWeights, weights, n);
    const char* skeletonName = skeleton->name.c_str();
    logger::info("[Locomotion] upper body mask: skeleton '{}', {} bones, {} upper body",
        skeletonName ? skeletonName : "?", n, entry.upper.size());
    logger::debug("[Locomotion] upper bones: {}", names);
    {
        std::lock_guard lock(g_toggleMutex);
        g_maskUpper.assign(static_cast<std::size_t>(n), false);
        for (const int i : entry.upper) g_maskUpper[static_cast<std::size_t>(i)] = true;
        g_maskSkeleton = skeletonName ? skeletonName : "?";
        g_checkedSkeletons.insert(skeleton);
        g_masks.push_back(std::move(entry));
    }
    return mask;
}

void SetMasks(bool on) {
    for (auto& m : g_masks)
        for (const int i : m.upper)
            if (i < m.array->boneWeights._size) m.array->boneWeights._data[i] = on ? kMaskedUpper : 1.0f;
}

// Which foot sync points a trigger array has: 1 FootLeft, 2 FootRight (their events carry FLAG_SYNC_POINT).
int FootSyncs(RE::hkbBehaviorGraph* graph, const RE::hkbClipTriggerArray* triggers) {
    int bits = 0;
    if (!triggers) return 0;
    for (int i = 0; i < triggers->triggers._size; ++i) {
        const char* e =
            NodeFactory::EventNameOf(graph, static_cast<std::int32_t>(triggers->triggers._data[i].event.id));
        if (_stricmp(e, "FootLeft") == 0)
            bits |= 1;
        else if (_stricmp(e, "FootRight") == 0)
            bits |= 2;
    }
    return bits;
}

// <Stance>: a vanilla clip that plays a file, and the graph it is in (its trigger event ids are that graph's).
struct Played {
    RE::hkbBehaviorGraph* graph = nullptr;
    const RE::hkbClipGenerator* clip = nullptr;
};
using PlayedFiles = std::unordered_map<std::string, Played>;  // lowered animation path

struct Builder {
    std::vector<AnimData::ClipCopy>& copies;
    RE::hkbBehaviorGraph* graph = nullptr;  // the graph that owns the blenders (event ids are its own)
    RE::hkbCharacter* character = nullptr;  // <Stance> files are bound by its animation table
    const PlayedFiles* played = nullptr;    // <Stance>: the vanilla clips of the stance files
    int stanceClips = 0;
    int nodes = 0;
    const BlenderGeneratorLayout* blender = nullptr;  // the blender being patched (SyncedTriggers looks in it)
    std::unordered_set<std::string> completed;        // clips whose copies took another slot's triggers (logged once)
    int bound = 0, boundOwn = 0, unbound = 0;         // walk and run clips whose rate follows the speed

    // The clip's trigger array, or, when it has only one foot's sync point, the array of another slot of the blender
    // that plays the same animation with both. Havok aligns blended clips by sync point id: a one-foot walk mixed
    // straight with a two-foot run put the run half a cycle off, onto the other foot, in one frame (11-14, a 2HM left
    // stop once the strafe slot between them was out of the mix: the left foot moved 34 in a frame, vanilla 9 at most). Vanilla
    // 2HM_WalkLeft has FootRight only and 2HM_WalkBackwardLeft FootLeft only; their Slow slots play the same files with
    // both feet.
    RE::hkbClipTriggerArray* SyncedTriggers(const RE::hkbClipGenerator* src) {
        auto* own = src->triggers.get();
        const int bits = FootSyncs(graph, own);
        if ((bits != 1 && bits != 2) || !blender) return own;
        const char* file = src->animationName.c_str();
        for (int i = 0; i < blender->children._size; ++i) {
            const auto* child = blender->children._data[i];
            if (!child || !NodeFactory::SameVTable(child->generator, RE::VTABLE_hkbClipGenerator)) continue;
            const auto* clip = static_cast<const RE::hkbClipGenerator*>(child->generator);
            const char* other = clip->animationName.c_str();
            if (clip == src || !file || !other || _stricmp(file, other) != 0) continue;
            auto* triggers = clip->triggers.get();
            if (FootSyncs(graph, triggers) != 3) continue;
            if (completed.insert(Lower(NodeFactory::NodeName(src))).second)
                logger::info("[Ladder] {}: copies of {} take {}'s triggers (both foot sync points; it has {} only)",
                    NodeFactory::NodeName(reinterpret_cast<const RE::hkbNode*>(blender)), NodeFactory::NodeName(src),
                    NodeFactory::NodeName(clip), bits == 1 ? "FootLeft" : "FootRight");
            return triggers;
        }
        return own;
    }

    // A vanilla slot's clip made from its record: another mod took it out of the ladder. Vanilla clips are bound by
    // animation name when the project loads (binding index 0xFFFF until then, as in the vanilla files); their events
    // are the graph's own (InjectEvent finds the existing FootLeft and FootRight).
    RE::hkbClipGenerator* Fresh(const VanillaLadders::Slot& v, const RE::hkbNode* like) {
        std::vector<NodeFactory::TriggerDef> defs;
        for (const auto& t : v.triggers)
            if (const int id = NodeFactory::InjectEvent(graph, t.event); id >= 0)
                defs.push_back({t.time, id, t.relativeToEnd});
        auto* triggers = defs.empty() ? nullptr : NodeFactory::MakeTriggerArray(defs);
        auto* c = NodeFactory::MakeClip(
            v.clip, v.animation, 0xFFFF, triggers, like, (v.flags & kDontConvertAnnotations) == 0);
        c->playbackSpeed = v.playback;
        c->mode = static_cast<RE::hkbClipGenerator::PlaybackMode>(v.mode);
        c->flags = static_cast<std::uint8_t>(v.flags);
        ++nodes;
        return c;
    }

    // The walk or run clip's playback becomes Speed / the file's ground speed (LocomotionDrive::BindRate). The ground
    // speed is the source clip's animation data. An own tree's clip is not registered yet: its vanilla source's, and
    // when it plays a dedicated file (its ClipCopy carries no source annotations) the variable is that file's own, so
    // the file's animmotion annotations can replace the speed once the clip is first activated (core/ClipHooks).
    void BindRate(const Slot& from, RE::hkbClipGenerator* clip, const std::string& name) {
        float fileSpeed = AnimData::ClipSpeed(from.sourceName.c_str());
        const char* ownFile = "";
        for (const auto& made : copies)
            if (_stricmp(made.copy.c_str(), from.sourceName.c_str()) == 0) {
                if (fileSpeed < 1.0f) fileSpeed = AnimData::ClipSpeed(made.source.c_str());
                if (!made.annotations) ownFile = from.clip->animationName.c_str();
                break;
            }
        if (fileSpeed >= 1.0f && LocomotionDrive::BindRate(graph, clip, fileSpeed, ownFile ? ownFile : "")) {
            ++bound;
            if (ownFile && *ownFile) ++boundOwn;
        } else {
            ++unbound;
            logger::warn("[Locomotion] {} (from {}): no ground speed for its file ({:.1f}); it plays at its vanilla "
                         "speed and does not follow the actor's",
                name, from.sourceName, fileSpeed);
        }
    }

    // <Stance>: a clip of the stance file standing beside `own` (the walk or run copy) in its stance blender. A vanilla
    // clip that plays the file gives its triggers (vanilla HKX carry no foot annotations; their foot events are the
    // behavior's), their events renamed into this graph, its flags and playback speed, and its ground speed; without one
    // the file's own FootLeft / FootRight annotations become its sync points and its animmotion annotations its speed.
    // Its motion is `own`'s (registered as a copy of own's source clip).
    RE::hkbClipGenerator* Stance(const Slot& own, const std::string& file, char side) {
        const auto name = fmt::format("{}_AIS{}", NodeFactory::NodeName(own.clip), side);
        const Played* like = nullptr;
        if (played)
            if (const auto it = played->find(Lower(file)); it != played->end()) like = &it->second;
        RE::hkbClipTriggerArray* triggers = nullptr;
        if (like && like->clip->triggers.get()) {
            const auto* source = like->clip->triggers.get();
            const auto* data = like->graph ? like->graph->data.get() : nullptr;
            const auto* strings = data ? data->stringData.get() : nullptr;
            std::vector<NodeFactory::TriggerDef> defs;
            for (int i = 0; strings && i < source->triggers._size; ++i) {
                const auto& t = source->triggers._data[i];
                const int id = static_cast<int>(t.event.id);
                if (id < 0 || id >= strings->eventNames._size) continue;
                const char* event = strings->eventNames._data[id].c_str();
                if (const int mine = event ? NodeFactory::InjectEvent(graph, event) : -1; mine >= 0)
                    defs.push_back({t.localTime, mine, t.relativeToEndOfClip});
            }
            if (!defs.empty()) triggers = NodeFactory::MakeTriggerArray(defs);
        }
        const int binding = NodeFactory::EnsureAnimationName(character, file.c_str());
        auto* c = NodeFactory::MakeClip(name.c_str(), file.c_str(),
            static_cast<std::uint16_t>(binding >= 0 ? binding : 0xFFFF), triggers, own.clip,
            !like || (like->clip->flags & kDontConvertAnnotations) == 0);
        c->mode = own.clip->mode;
        if (like) {
            c->playbackSpeed = like->clip->playbackSpeed;
            c->flags = like->clip->flags;
        } else
            c->flags = 0;  // convert the file's annotations: its own foot sync points
        ++nodes;
        ++stanceClips;
        const float vanillaSpeed = like ? AnimData::ClipSpeed(NodeFactory::NodeName(like->clip)) : 0.0f;
        float fileSpeed = vanillaSpeed >= 1.0f ? vanillaSpeed : AnimData::ClipSpeed(own.sourceName.c_str());
        // An own tree's clip (H2H, 2HW) is not registered yet: its vanilla source's speed, as BindRate takes it. Without
        // a speed the rate was not bound and the stance file played at 1x (11-23: unarmed, the feet slid back 40/s).
        if (fileSpeed < 1.0f)
            for (const auto& made : copies)
                if (_stricmp(made.copy.c_str(), own.sourceName.c_str()) == 0) {
                    fileSpeed = AnimData::ClipSpeed(made.source.c_str());
                    break;
                }
        // A file with no vanilla clip reads its own speed from its animmotion annotations when it first plays.
        if (fileSpeed < 1.0f && !like) fileSpeed = 100.0f;
        logger::debug("[Stance] {} plays {}: vanilla clip '{}' speed {:.1f}, {}'s {:.1f} -> rate from {:.1f}", name,
            file, like ? NodeFactory::NodeName(like->clip) : "-", vanillaSpeed, own.sourceName,
            AnimData::ClipSpeed(own.sourceName.c_str()), fileSpeed);
        if (fileSpeed >= 1.0f && LocomotionDrive::BindRate(graph, c, fileSpeed, like ? "" : file.c_str()))
            ++bound;
        else {
            ++unbound;
            logger::warn(
                "[Stance] {} ({}): its rate could not be bound; it plays at 1x and the feet slide", name, file);
        }
        copies.push_back(AnimData::ClipCopy{own.sourceName, name, own.rate});
        return c;
    }

    // A new clip from `from` with playback × rate. It shares the source's triggers and gets its annotations: every
    // slot of a ladder then aligns on the same sync point ids (FootLeft, FootRight), as vanilla's own same-clip buffer
    // MT_WalkForwardFast does. Renamed sync events broke that: Havok matches sync points by id, so a buffer whose
    // ids differed from its neighbour's could not be phase-aligned with it and the pose jumped (11-14, stopping and
    // turning while running).
    Slot Copy(const Slot& from, float rate, char kind) {
        const auto* src = from.clip;
        Slot out;
        out.sourceName = from.sourceName;
        out.rate = from.rate * rate;
        const auto name = fmt::format("{}_AI{}{}", from.sourceName, kind, RateTag(out.rate));
        auto* triggers = SyncedTriggers(src);
        if (triggers) NodeFactory::AddRef(triggers);
        const bool convert = (src->flags & kDontConvertAnnotations) == 0;
        auto* c = NodeFactory::MakeClip(
            name.c_str(), src->animationName.c_str(), src->animationBindingIndex, triggers, src, convert);
        c->cropStartAmountLocalTime = src->cropStartAmountLocalTime;
        c->cropEndAmountLocalTime = src->cropEndAmountLocalTime;
        c->startTime = src->startTime;
        c->playbackSpeed = src->playbackSpeed * rate;
        c->enforcedDuration = src->enforcedDuration;
        c->userControlledTimeFraction = src->userControlledTimeFraction;
        c->mode = src->mode;
        c->flags = src->flags;
        ++nodes;
        out.clip = c;
        if (kind == 'w' || kind == 'r') BindRate(from, c, name);
        copies.push_back(AnimData::ClipCopy{from.sourceName, name, out.rate});
        return out;
    }
};

// A child object for a new clip, with the bone weights and world-from-model weight of the slot it stands beside, or
// `boneWeights` in place of the slot's. The objects it holds (the clip, the slot's binding set, the bone weights) get a
// reference each, as OwnLocomotion::CopyChild's do. (Objects NodeFactory makes are never freed: InitRefObj.)
BlenderGeneratorChildLayout* MakeChild(const BlenderGeneratorChildLayout* like, RE::hkbGenerator* clip, float weight,
    RE::hkReferencedObject* boneWeights = nullptr) {
    auto* c = static_cast<BlenderGeneratorChildLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorChildLayout)));
    std::memcpy(c, like, sizeof(BlenderGeneratorChildLayout));
    NodeFactory::InitRefObj(c, RE::VTABLE_hkbBlenderGeneratorChild);
    auto* bindable = reinterpret_cast<RE::hkbBindable*>(c);
    NodeFactory::EmptyArray(bindable->cachedBindables);
    bindable->areBindablesCached = false;
    if (auto* set = bindable->variableBindingSet.get()) NodeFactory::AddRef(set);
    c->generator = clip;
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(clip));  // Rebuild reuses the slot's own clip
    c->weight = weight;
    if (boneWeights) c->boneWeights = boneWeights;
    if (c->boneWeights) NodeFactory::AddRef(c->boneWeights);
    return c;
}

// A child object for a vanilla slot made from its record (vanilla direction slots have no bone weights).
BlenderGeneratorChildLayout* FreshChild(RE::hkbClipGenerator* clip, float weight) {
    auto* c = static_cast<BlenderGeneratorChildLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorChildLayout)));
    NodeFactory::InitRefObj(c, RE::VTABLE_hkbBlenderGeneratorChild);
    NodeFactory::EmptyArray(reinterpret_cast<RE::hkbBindable*>(c)->cachedBindables);
    c->generator = clip;
    c->weight = weight;
    c->worldFromModelWeight = 1.0f;
    return c;
}

// The current child (and its clip) whose clip has the vanilla slot's name, or nulls.
// The vanilla slot at `index`'s child: the child in that place when its clip has the slot's name, else the first
// unused child that has it. By place first: a vanilla ladder may name two slots alike (Magic_BackwardLeftBlend's Slow
// slot is "Magic_WalkBackwardLeft" at 1.0 like its walk slot, 11-14), and a name lookup alone took the first twice.
std::pair<BlenderGeneratorChildLayout*, RE::hkbClipGenerator*> FindSlot(const BlenderGeneratorLayout* b,
    const VanillaLadders::Slot& v, std::size_t index, std::unordered_set<const void*>& used) {
    const auto usable = [&](int i) -> RE::hkbClipGenerator* {
        auto* child = b->children._data[i];
        if (!child || used.contains(child) || !NodeFactory::SameVTable(child, RE::VTABLE_hkbBlenderGeneratorChild) ||
            !child->generator || !NodeFactory::SameVTable(child->generator, RE::VTABLE_hkbClipGenerator) ||
            _stricmp(NodeFactory::NodeName(child->generator), v.clip) != 0)
            return nullptr;
        return static_cast<RE::hkbClipGenerator*>(child->generator);
    };
    if (static_cast<int>(index) < b->children._size)
        if (auto* clip = usable(static_cast<int>(index))) {
            used.insert(b->children._data[index]);
            return {b->children._data[index], clip};
        }
    for (int i = 0; i < b->children._size; ++i)
        if (auto* clip = usable(i)) {
            used.insert(b->children._data[i]);
            return {b->children._data[i], clip};
        }
    return {nullptr, nullptr};
}

// How the blender differs from its vanilla record; empty when it is vanilla.
std::string Differences(const BlenderGeneratorLayout* b, const VanillaLadders::Ladder& v) {
    std::vector<std::string> out;
    std::unordered_set<const void*> used;
    if (b->children._size != static_cast<int>(v.slots.size()))
        out.push_back(fmt::format("slots {} (vanilla {})", b->children._size, v.slots.size()));
    for (std::size_t i = 0; i < v.slots.size(); ++i) {
        const auto& slot = v.slots[i];
        const auto [child, clip] = FindSlot(b, slot, i, used);
        if (!clip) {
            out.push_back(fmt::format("{} missing", slot.clip));
            continue;
        }
        if (std::fabs(child->weight - slot.weight) > 1e-3f)
            out.push_back(fmt::format("{} weight {} (vanilla {})", slot.clip, child->weight, slot.weight));
        if (std::fabs(clip->playbackSpeed - slot.playback) > 1e-4f)
            out.push_back(fmt::format("{} playback {} (vanilla {})", slot.clip, clip->playbackSpeed, slot.playback));
        if (static_cast<int>(i) >= b->children._size || b->children._data[i] != child)
            out.push_back(fmt::format("{} not in place {}", slot.clip, i));
    }
    std::string text;
    for (std::size_t i = 0; i < out.size() && i < 6; ++i) text += (i ? ", " : "") + out[i];
    if (out.size() > 6) text += fmt::format(", and {} more", out.size() - 6);
    return text;
}

// The children array as vanilla: each vanilla slot from the blender's own clip of that name (so another mod's edits
// to the clip itself, such as its triggers, stay), copied at the vanilla playback speed if that was changed, or made
// from the record if the clip is gone. Slots another mod added are left out.
void Rebuild(BlenderGeneratorLayout* b, const VanillaLadders::Ladder& v, Builder& build) {
    build.blender = b;
    const RE::hkbNode* like = nullptr;
    for (int i = 0; i < b->children._size && !like; ++i)
        if (auto* child = b->children._data[i];
            child && child->generator && NodeFactory::SameVTable(child->generator, RE::VTABLE_hkbClipGenerator))
            like = child->generator;
    std::vector<BlenderGeneratorChildLayout*> out;
    std::unordered_set<const void*> used;
    for (std::size_t i = 0; i < v.slots.size(); ++i) {
        const auto& slot = v.slots[i];
        auto [child, clip] = FindSlot(b, slot, i, used);
        if (!clip) {
            out.push_back(FreshChild(build.Fresh(slot, like), slot.weight));
            continue;
        }
        if (std::fabs(clip->playbackSpeed - slot.playback) > 1e-4f && clip->playbackSpeed > 0.0f) {
            const Slot from{clip, slot.clip, clip->playbackSpeed / slot.playback};
            clip = build.Copy(from, slot.playback / clip->playbackSpeed, 'v').clip;
        }
        out.push_back(clip == child->generator && std::fabs(child->weight - slot.weight) <= 1e-3f
                          ? child
                          : MakeChild(child, clip, slot.weight));
    }
    auto* data = static_cast<BlenderGeneratorChildLayout**>(
        NodeFactory::AllocZero(out.size() * sizeof(BlenderGeneratorChildLayout*)));
    std::copy(out.begin(), out.end(), data);
    NodeFactory::OwnArray(b->children, data, static_cast<std::int32_t>(out.size()));
}

// The vanilla record's walk and run slots: the lowest and the highest slot that plays its clip at 1x. Below the walk
// are the 0.1x Slow slots, above the run the 10x Fast ones; between them lie the 1.2x walk of the sheathed ladders
// and the strafe runs of the side ones. (Magic_BackwardLeftBlend's Slow slot plays the walk clip at 1x: the same file.)
struct Picked {
    int walk = -1;
    int run = -1;
};
Picked Pick(const VanillaLadders::Ladder& v) {
    Picked out;
    for (std::size_t i = 0; i < v.slots.size(); ++i)
        if (std::fabs(v.slots[i].playback - 1.0f) < 1e-3f) {
            if (out.walk < 0) out.walk = static_cast<int>(i);
            out.run = static_cast<int>(i);
        }
    return out;
}

// The self-check of the blender itself (its children are rebuilt from the vanilla record when they differ). Empty:
// the blender may be patched.
std::string Check(const BlenderGeneratorLayout* b, const VanillaLadders::Ladder& v, const Picked& picked) {
    if (!(b->flags & BlenderGeneratorLayout::kParametricBlend)) return "not a parametric blend";
    if (b->children._size < 1 || b->children._size > 32 || !b->children._data)
        return fmt::format("{} children", b->children._size);
    if (b->childrenInternalStates._size || b->sortedChildren._size) return "per-instance arrays are not empty";
    if (v.slots.size() < 2) return "the vanilla record has fewer than two slots";
    if (picked.walk < 0 || picked.run <= picked.walk) return "the vanilla record has no separate walk and run slot";
    if (std::string_view(v.slots[picked.walk].animation) == v.slots[picked.run].animation)
        return "walk and run are one file here";
    return {};
}

std::string LayoutKey(const char* graphName, const char* blenderName) {
    return Lower(graphName ? graphName : "") + "|" + Lower(blenderName ? blenderName : "");
}

// The blender's child weights for the state (g_toggleMutex held). Returns the children set.
int SetValues(BlenderGeneratorLayout* b, const std::vector<Values>& layout, bool on) {
    if (b->children._size != static_cast<int>(layout.size())) return 0;
    for (int i = 0; i < b->children._size; ++i)
        if (auto* child = b->children._data[i]) child->weight = on ? layout[i].on : layout[i].off;
    return b->children._size;
}

// <Stance> (runtime/LadderSpec.h): the walk or run copy `own` and the stance clips of this direction under a blender of
// the direction blender's kind on kStanceVariable, children [left -1, own 0, right 1] (those whose file exists), synced
// as the direction blender is (flags 0x11): the stance clips step with own. Null when no stance file plays here or the
// blender could not be bound (then `own` stands alone, as without a <Stance>).
RE::hkbGenerator* StanceBlend(
    const BlenderGeneratorLayout* outer, const Slot& own, LadderDirection direction, bool run, Builder& build) {
    const auto& locomotion = Actions::Locomotion();
    std::vector<std::pair<float, std::string>> sides;
    for (const auto side : {StanceSide::kLeft, StanceSide::kRight}) {
        const auto& s = locomotion.Stance(side);
        if (!s.declared) continue;
        if (auto file = StanceFile(run ? s.run : s.walk, direction); OwnLocomotion::Listed(file))
            sides.emplace_back(side == StanceSide::kLeft ? -1.0f : 1.0f, std::move(file));
    }
    if (sides.empty() || !build.character) return nullptr;
    std::vector<std::pair<float, RE::hkbClipGenerator*>> kids{{0.0f, own.clip}};
    for (const auto& [at, file] : sides) kids.emplace_back(at, build.Stance(own, file, at > 0.0f ? 'R' : 'L'));
    std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto* b = static_cast<BlenderGeneratorLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorLayout)));
    std::memcpy(b, outer, sizeof(BlenderGeneratorLayout));
    NodeFactory::InitRefObj(b, RE::VTABLE_hkbBlenderGenerator);
    auto* node = reinterpret_cast<RE::hkbNode*>(b);
    std::memset(&node->name, 0, sizeof(node->name));
    NodeFactory::MakeString(
        node->name, fmt::format("{}_AIStance{}", NodeFactory::NodeName(outer), run ? 'R' : 'W').c_str());
    NodeFactory::EmptyArray(node->cachedBindables);
    node->areBindablesCached = false;
    if (auto* set = node->variableBindingSet.get()) NodeFactory::AddRef(set);  // the direction blender's, rebound below
    NodeFactory::EmptyArray(b->childrenInternalStates);
    NodeFactory::EmptyArray(b->sortedChildren);
    b->blendParameter = 0.0f;
    b->minCyclicBlendParameter = 0.0f;
    b->maxCyclicBlendParameter = 0.0f;
    b->indexOfSyncMasterChild = -1;
    auto* data = static_cast<BlenderGeneratorChildLayout**>(
        NodeFactory::AllocZero(kids.size() * sizeof(BlenderGeneratorChildLayout*)));
    for (std::size_t i = 0; i < kids.size(); ++i) {
        auto* child =
            static_cast<BlenderGeneratorChildLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorChildLayout)));
        NodeFactory::InitRefObj(child, RE::VTABLE_hkbBlenderGeneratorChild);
        NodeFactory::EmptyArray(reinterpret_cast<RE::hkbBindable*>(child)->cachedBindables);
        child->generator = kids[i].second;
        NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(kids[i].second));
        child->weight = kids[i].first;
        child->worldFromModelWeight = 1.0f;
        data[i] = child;
    }
    NodeFactory::OwnArray(b->children, data, static_cast<int>(kids.size()));
    ++build.nodes;
    if (!LocomotionDrive::RebindStance(build.graph, node)) return nullptr;
    return reinterpret_cast<RE::hkbGenerator*>(b);
}

// The direction a ladder blender's name ends in ("1HM_ForwardLeftBlend": forwardLeft).
std::optional<LadderDirection> DirectionOf(std::string_view name) {
    for (std::size_t d = 0; d < LadderNames::kDirections.size(); ++d) {
        const auto tail = fmt::format("_{}Blend", LadderNames::kDirections[d].second);
        if (name.size() >= tail.size() &&
            _strnicmp(name.data() + name.size() - tail.size(), tail.c_str(), tail.size()) == 0)
            return static_cast<LadderDirection>(d);
    }
    return std::nullopt;
}

// Adds the walk and the run clip after the vanilla slots (a blender whose children are vanilla: Rebuild ran first
// when they were not). With a <Stance> file for the direction (`stance`), the walk or run stands in a stance blender.
// Returns the clips made.
int Patch(BlenderGeneratorLayout* b, const Picked& picked, Builder& build, RE::hkReferencedObject* walkMask,
    const std::string& key, std::optional<LadderDirection> stance) {
    const int n = b->children._size;
    build.blender = b;
    const int before = build.nodes;
    auto slotOf = [](const BlenderGeneratorChildLayout* child) {
        auto* clip = static_cast<RE::hkbClipGenerator*>(child->generator);
        return Slot{clip, NodeFactory::NodeName(clip), 1.0f};
    };
    auto* walkSlot = b->children._data[picked.walk];
    auto* runSlot = b->children._data[picked.run];
    const auto walk = build.Copy(slotOf(walkSlot), 1.0f, 'w');
    const auto run = build.Copy(slotOf(runSlot), 1.0f, 'r');
    std::vector<Values> layout;
    auto* data = static_cast<BlenderGeneratorChildLayout**>(
        NodeFactory::AllocZero(static_cast<std::size_t>(n + 2) * sizeof(BlenderGeneratorChildLayout*)));
    for (int i = 0; i < n; ++i) {
        data[i] = b->children._data[i];
        layout.push_back({-kParked + static_cast<float>(i), data[i]->weight});
    }
    // While the walk and the run mix, the walk gives the legs and the run the upper body.
    RE::hkbGenerator* walkGenerator = walk.clip;
    RE::hkbGenerator* runGenerator = run.clip;
    if (stance) {
        if (auto* s = StanceBlend(b, walk, *stance, false, build)) walkGenerator = s;
        if (auto* s = StanceBlend(b, run, *stance, true, build)) runGenerator = s;
    }
    data[n] = MakeChild(walkSlot, walkGenerator, LocomotionDrive::kWalkAxis, walkMask);
    data[n + 1] = MakeChild(runSlot, runGenerator, LocomotionDrive::kRunAxis);
    layout.push_back({LocomotionDrive::kWalkAxis, kParked});
    layout.push_back({LocomotionDrive::kRunAxis, kParked + 1.0f});
    NodeFactory::OwnArray(b->children, data, n + 2);
    {
        std::lock_guard lock(g_toggleMutex);
        SetValues(b, layout, g_enabled.load());
        g_layouts[key] = std::move(layout);
        g_templates.push_back({b, key});
    }
    return build.nodes - before;
}

// Every patched blender reachable from an actor's live graph, behavior references followed (g_toggleMutex held).
std::pair<int, int> SetLive(RE::hkbBehaviorGraph* graph, bool on, std::vector<Child>& buffer) {
    int blenders = 0, children = 0;
    std::unordered_set<RE::hkbNode*> seen;
    std::vector<std::pair<RE::hkbNode*, const char*>> stack;  // node, its graph's root generator name
    if (graph && graph->rootGenerator)
        stack.emplace_back(graph->rootGenerator.get(), NodeFactory::NodeName(graph->rootGenerator.get()));
    while (!stack.empty()) {
        const auto [node, graphName] = stack.back();
        stack.pop_back();
        if (!node || !seen.insert(node).second || seen.size() > 400000) continue;
        if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBlenderGenerator))
            if (const auto layout = g_layouts.find(LayoutKey(graphName, NodeFactory::NodeName(node)));
                layout != g_layouts.end()) {
                ++blenders;
                children += SetValues(reinterpret_cast<BlenderGeneratorLayout*>(node), layout->second, on);
            }
        if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) {
            // hkbBehaviorReferenceGenerator: hkbGenerator 0x48 | behaviorName 0x48 | behavior 0x50.
            auto* referenced = *reinterpret_cast<RE::hkbBehaviorGraph**>(reinterpret_cast<std::uintptr_t>(node) + 0x50);
            if (referenced && NodeFactory::SameVTable(referenced, RE::VTABLE_hkbBehaviorGraph) &&
                referenced->rootGenerator)
                stack.emplace_back(
                    referenced->rootGenerator.get(), NodeFactory::NodeName(referenced->rootGenerator.get()));
        }
        const int count = ListChildren(node, buffer);
        for (int i = count - 1; i >= 0; --i) stack.emplace_back(buffer[i].node, graphName);
    }
    return {blenders, children};
}

void Notify(const char* text) {
    using func_t = void(const char*, const char*, bool);
    static REL::Relocation<func_t> func{RELOCATION_ID(52050, 52933)};
    func(text, nullptr, true);
}

// <Tree weapon="2HW" use="2HM">: two-handed axes and hammers move with the greatsword's locomotion, as the 2HW Move
// Fix behavior patch did (1hm_locomotion #0839 2HW_iStateGen.pDefaultGenerator -> 2HM's, #0304
// SneakWeap_DirectionMSG.generators[6] -> [5]). Every node and value is checked first (names, iState 7, 13 sneak
// generators in the vanilla order); anything else leaves the graph as it was. Each place gets a switch to vanilla's
// node (OwnLocomotion::Switch), as the own trees' places do, so Toggle turns it back to vanilla too (review R6, 11-20).
void AxesAs2HM(const char* graphName, RE::hkbBehaviorGraph* graph, const std::function<bool(std::size_t)>& room,
    RE::hkbNode* tag2HW, RE::hkbNode* tag2HM, RE::hkbNode* sneak) {
    if (!tag2HW || !tag2HM || !NodeFactory::SameVTable(tag2HW, RE::VTABLE_BSiStateTaggingGenerator) ||
        !NodeFactory::SameVTable(tag2HM, RE::VTABLE_BSiStateTaggingGenerator)) {
        logger::warn("[Tree] {}: 2HW use=\"2HM\" — the 2HW/2HM iState generators were not found; axes move as before",
            graphName);
        return;
    }
    auto* hw = reinterpret_cast<StateTaggingLayout*>(tag2HW);
    auto* hm = reinterpret_cast<StateTaggingLayout*>(tag2HM);
    if (hw->iStateToSetAs != 7 || hm->iStateToSetAs != 7 || !NodeNamed(hm->pDefaultGenerator, nullptr) ||
        !NodeNamed(hw->pDefaultGenerator, nullptr)) {
        logger::warn(
            "[Tree] {}: 2HW use=\"2HM\" — the iState generators are not as expected (iState {}/{}); axes move as "
            "before",
            graphName, hw->iStateToSetAs, hm->iStateToSetAs);
        return;
    }
    const std::string before = NodeFactory::NodeName(hw->pDefaultGenerator);
    if (hw->pDefaultGenerator != hm->pDefaultGenerator) {
        auto* shared = OwnLocomotion::Switch(graph, reinterpret_cast<RE::hkbNode*>(hm->pDefaultGenerator),
            reinterpret_cast<RE::hkbNode*>(hw->pDefaultGenerator), "AI_Switch_2HWShared", room);
        hw->pDefaultGenerator = reinterpret_cast<RE::hkbGenerator*>(shared);
        NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(shared));
    }
    std::string sneakNote = "sneak selector not found";
    if (sneak && NodeFactory::SameVTable(sneak, RE::VTABLE_hkbManualSelectorGenerator)) {
        auto* msg = reinterpret_cast<ManualSelectorLayout*>(sneak);
        auto& g = msg->generators;
        if (g._size == 13 && Readable(g._data, 13 * sizeof(void*)) &&
            NodeNamed(g._data[5], "Sneak2HM_DirectionBlend") &&
            (NodeNamed(g._data[6], "Sneak2HW_DirectionBlend") || g._data[6] == g._data[5])) {
            if (g._data[6] != g._data[5]) {
                auto* shared = OwnLocomotion::Switch(graph, reinterpret_cast<RE::hkbNode*>(g._data[5]),
                    reinterpret_cast<RE::hkbNode*>(g._data[6]), "AI_Switch_Sneak2HWShared", room);
                g._data[6] = reinterpret_cast<RE::hkbGenerator*>(shared);
                NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(shared));
            }
            sneakNote = "sneak uses Sneak2HM_DirectionBlend";
        } else
            sneakNote = "sneak selector not as expected, left as it was";
    }
    logger::info("[Tree] {}: 2HW use=\"2HM\" — 2HW locomotion {} -> {}; {}", graphName, before,
        NodeFactory::NodeName(hm->pDefaultGenerator), sneakNote);
}

}  // namespace

Summary Apply(RE::hkbBehaviorGraph* master, const RE::BSScrapArray<RE::hkbBehaviorGraph*>& graphs,
    RE::hkbCharacter* character, const RoomFn& room) {
    Summary summary;
    const auto& locomotion = Actions::Locomotion();
    if (locomotion.Empty()) return summary;
    // The blender names of every set the declaration covers; the Block sets name the same blenders as the others,
    // in BlockBehavior only.
    std::unordered_map<std::string, LadderSet> rows, blockRows;
    for (std::size_t s = 0; s < kLadderSetCount; ++s) {
        const auto set = static_cast<LadderSet>(s);
        if (!locomotion.Applies(set)) continue;
        for (std::size_t d = 0; d < LadderNames::kDirections.size(); ++d)
            (LadderInBlock(set) ? blockRows : rows)
                .emplace(Lower(LadderBlenderName(set, static_cast<LadderDirection>(d))), set);
    }
    RE::hkReferencedObject* walkMask = nullptr;
    if (!rows.empty() || !blockRows.empty())
        walkMask = reinterpret_cast<RE::hkReferencedObject*>(MakeUpperMask(character));
    int gaits = 0;

    std::vector<RE::hkbBehaviorGraph*> all;
    if (master) all.push_back(master);
    for (auto* g : graphs)
        if (g && g != master) all.push_back(g);
    std::unordered_set<RE::hkbNode*> done;
    std::unordered_set<RE::hkbNode*> directionDone;
    std::unordered_set<std::string> matched;
    int heldDirections = 0;
    int bound = 0, boundOwn = 0, unbound = 0;
    bool treesDone = false;
    bool unarmedTree = false;  // the H2H tree was made (its files can play)
    // 1HM_Behavior's H2H_SprintPoseBehavior and 1HM_Readied_MSG (OwnLocomotion::FullBodyArms).
    RE::hkbNode* armPose = nullptr;
    RE::hkbNode* readied = nullptr;
    RE::hkbBehaviorGraph* armGraph = nullptr;  // the graph they are in
    const bool anyOwn = locomotion.AnyOwnTree();
    std::vector<AnimData::ClipCopy> copies;
    std::vector<Child> buffer;
    // <Stance>: the vanilla clips that play its files, in any of the character's graphs.
    PlayedFiles played;
    if (locomotion.AnyStance()) {
        std::unordered_map<std::string, std::vector<Played>> candidates;
        std::unordered_set<std::string> wanted;
        for (const auto& f : OwnLocomotion::Files()) wanted.insert(Lower(f));
        for (auto* graph : all) {
            std::unordered_set<RE::hkbNode*> seen;
            std::vector<RE::hkbNode*> stack;
            if (graph->rootGenerator) stack.push_back(graph->rootGenerator.get());
            while (!stack.empty()) {
                auto* node = stack.back();
                stack.pop_back();
                if (!node || !seen.insert(node).second || seen.size() > 200000) continue;
                if (NodeFactory::SameVTable(node, RE::VTABLE_hkbClipGenerator)) {
                    const auto* clip = static_cast<const RE::hkbClipGenerator*>(node);
                    if (const char* file = clip->animationName.c_str(); file && wanted.contains(Lower(file)))
                        candidates[Lower(file)].push_back(Played{graph, clip});
                }
                if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
                const int count = ListChildren(node, buffer);
                for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[i].node);
            }
        }
        // The clip that plays the file as a walk or run does: at 1x and with both foot sync points (a ladder's Slow slot
        // plays it at 0.1x, 11-23: its foot triggers and speed put the feet off the ground).
        for (auto& [file, list] : candidates) {
            const Played* best = nullptr;
            float bestScore = 1e9f;
            for (const auto& c : list) {
                const int feet = FootSyncs(c.graph, c.clip->triggers.get());
                const float score = std::fabs(c.clip->playbackSpeed - 1.0f) + (feet == 3 ? 0.0f : 10.0f);
                logger::debug("[Stance] {}: candidate '{}' in {} playback {:.2f} feet {} speed {:.1f}", file,
                    NodeFactory::NodeName(c.clip), NodeFactory::NodeName(c.graph->rootGenerator.get()),
                    c.clip->playbackSpeed, feet, AnimData::ClipSpeed(NodeFactory::NodeName(c.clip)));
                if (score < bestScore) {
                    bestScore = score;
                    best = &c;
                }
            }
            if (best) played.emplace(file, *best);
        }
        logger::info("[Stance] {} of its files are played by a vanilla clip (their foot triggers and speed are used)",
            played.size());
    }
    {
        // A vanilla clip that binds playbackSpeed alone, anywhere in the character's graphs: the sample the walk and
        // run clips' binding sets are copied from (LocomotionDrive::BindRate).
        RE::hkbNode* sample = nullptr;
        for (auto* graph : all) {
            std::unordered_set<RE::hkbNode*> seen;
            std::vector<RE::hkbNode*> stack;
            if (graph->rootGenerator) stack.push_back(graph->rootGenerator.get());
            while (!stack.empty() && !sample) {
                auto* node = stack.back();
                stack.pop_back();
                if (!node || !seen.insert(node).second || seen.size() > 200000) continue;
                if (NodeFactory::SameVTable(node, RE::VTABLE_hkbClipGenerator) && LocomotionDrive::IsRateBinding(node))
                    sample = node;
                if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
                const int count = ListChildren(node, buffer);
                for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[i].node);
            }
            if (sample) break;
        }
        if (sample)
            LocomotionDrive::SetRateSample(sample->variableBindingSet.get());
        else
            logger::warn("[Locomotion] no vanilla clip binds playbackSpeed in this project's graphs: the walk and run "
                         "clips play at their vanilla speed and do not follow the actor's");
    }
    for (auto* graph : all) {
        auto* root = graph->rootGenerator.get();
        if (!root) continue;
        const char* graphName = NodeFactory::NodeName(root);
        const bool block = _stricmp(graphName, kBlockGraph) == 0;
        const auto& graphRows = block ? blockRows : rows;
        if (graphRows.empty() && !locomotion.AnyTree()) continue;
        // The graph's declared blenders, found first: the walk must not see the children it is replacing.
        std::vector<RE::hkbNode*> found;
        OwnLocomotion::Found tree;  // the nodes the weapons' trees hang from (<Tree>)
        RE::hkbNode* sneakSelector = nullptr;
        RE::hkbNode* blockSelector = nullptr;  // BlockBehavior's Shd_Locomotion_MSG (use="own")
        std::unordered_set<RE::hkbNode*> seen;
        std::vector<RE::hkbNode*> stack{root};
        while (!stack.empty()) {
            auto* node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second || seen.size() > 200000) continue;
            if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBlenderGenerator) &&
                graphRows.contains(Lower(NodeFactory::NodeName(node))) && done.insert(node).second)
                found.push_back(node);
            if (block && anyOwn && _stricmp(NodeFactory::NodeName(node), "Shd_Locomotion_MSG") == 0)
                blockSelector = node;
            // A vanilla selector bound to an int variable: what the own trees' switches are copied from.
            if (locomotion.AnyTree() && NodeFactory::SameVTable(node, RE::VTABLE_hkbManualSelectorGenerator))
                LocomotionDrive::NoteSwitchSample(graph, node);
            if (locomotion.AnyTree() && !block) {
                const char* n = NodeFactory::NodeName(node);
                if (_stricmp(n, "2HW_iStateGen") == 0)
                    tree.tag2HW = node;
                else if (_stricmp(n, "2HM_iStateGen") == 0)
                    tree.tag2HM = node;
                else if (_stricmp(n, "1HM_iStateGen") == 0)
                    tree.tag1HM = node;
                else if (_stricmp(n, "Melee_Direction_Behavior") == 0)
                    tree.weapons = node;
                else if (_stricmp(n, "SneakWeap_DirectionMSG") == 0)
                    sneakSelector = node;
                else if (_stricmp(n, "Sneak2HW_DirectionBlend") == 0)
                    tree.sneak2HW = node;
                else if (_stricmp(n, "H2H_SprintPoseBehavior") == 0) {
                    armPose = node;
                    armGraph = graph;
                } else if (_stricmp(n, "1HM_Readied_MSG") == 0)
                    readied = node;
            }
            // Behavior references lead into other graphs; those graphs are walked on their own.
            if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
            const int count = ListChildren(node, buffer);
            for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[i].node);
        }
        // <Tree>: each weapon's own tree is made from its source kind's before any blender of that kind is patched,
        // and its direction blenders are then patched like the blenders the walk found.
        const auto take = [&](const OwnLocomotion::Built& built) {
            summary.nodes += built.nodes;
            for (auto* node : built.directionBlenders)
                if (graphRows.contains(Lower(NodeFactory::NodeName(node))) && done.insert(node).second)
                    found.push_back(node);
        };
        const auto fits = [&](std::size_t nodes) { return room(graph, nodes); };
        if (locomotion.AnyTree() && _stricmp(graphName, "Melee_Locomotion_Behavior") == 0) {
            for (std::size_t w = 0; w < kTreeKinds.size(); ++w) {
                const auto weapon = static_cast<TreeWeapon>(w);
                if (locomotion.Use(weapon) == TreeUse::kShared)
                    AxesAs2HM(graphName, graph, fits, tree.tag2HW, tree.tag2HM, sneakSelector);
                else if (locomotion.Use(weapon) == TreeUse::kOwn) {
                    const auto built = OwnLocomotion::Build(weapon, graph, character, tree, copies, fits);
                    if (weapon == TreeWeapon::kH2H) unarmedTree = !built.directionBlenders.empty();
                    take(built);
                }
            }
            treesDone = true;
        }
        if (block)
            for (std::size_t w = 0; w < kTreeKinds.size(); ++w)
                if (const auto weapon = static_cast<TreeWeapon>(w); locomotion.Use(weapon) == TreeUse::kOwn)
                    take(OwnLocomotion::BuildBlock(weapon, graph, character, blockSelector, copies, fits));
        if (found.empty()) continue;
        Builder build{copies, graph};
        build.character = character;
        build.played = &played;
        std::size_t need = 0;
        struct Ready {
            BlenderGeneratorLayout* blender;
            const VanillaLadders::Ladder* vanilla;
            Picked picked;
            std::string differences;
        };
        std::vector<Ready> ready;
        int other = 0;  // blenders of a declared name that are another ladder (aiming a bow, sneaking with a weapon)
        for (auto* node : found) {
            ++summary.blenders;
            auto* b = reinterpret_cast<BlenderGeneratorLayout*>(node);
            const char* name = NodeFactory::NodeName(node);
            matched.insert(Lower(name));
            const auto* vanilla = VanillaLadders::Find(graphName, name);
            if (!vanilla) {
                ++other;
                logger::debug(
                    "[Locomotion] {} / {}: left as it was — no vanilla record of this blender", graphName, name);
                continue;
            }
            const auto picked = Pick(*vanilla);
            if (const auto reason = Check(b, *vanilla, picked); !reason.empty()) {
                if (reason.starts_with("the vanilla record") || reason.starts_with("walk and run")) {
                    ++other;
                    logger::debug("[Locomotion] {} / {}: left as it was — {}", graphName, name, reason);
                } else
                    logger::warn("[Locomotion] {} / {}: left as it was — {}", graphName, name, reason);
                continue;
            }
            auto differences = Differences(b, *vanilla);
            need += 2 + (differences.empty() ? 0 : vanilla->slots.size()) + (locomotion.AnyStance() ? 6 : 0);
            ready.push_back({b, vanilla, picked, std::move(differences)});
        }
        if (ready.empty()) continue;
        if (!room(graph, need)) {
            logger::warn("[Locomotion] {}: {} blenders left as they were — {} more nodes do not fit the node id budget",
                graphName, ready.size(), need);
            continue;
        }
        for (const auto& [b, vanilla, picked, differences] : ready) {
            auto* node = reinterpret_cast<RE::hkbNode*>(b);
            const char* name = NodeFactory::NodeName(node);
            if (!differences.empty()) {
                Rebuild(b, *vanilla, build);
                ++summary.rebuilt;
                logger::info("[Locomotion] {} / {}: another mod changed this blender ({}); rebuilt as vanilla",
                    graphName, name, differences);
            }
            const int before = b->children._size;
            std::optional<LadderDirection> stance;
            if (const auto set = graphRows.find(Lower(name));
                locomotion.AnyStance() && set != graphRows.end() && LocomotionSpec::StanceSet(set->second))
                stance = DirectionOf(name);
            Patch(b, picked, build, walkMask, LayoutKey(graphName, name), stance);
            ++summary.patched;
            // Moving while blocking is always the walk: its blenders read an axis that stays at the walk clip.
            if (block ? LocomotionDrive::RebindWalk(graph, node) : LocomotionDrive::Rebind(graph, node)) ++gaits;
            logger::debug("[Locomotion] {} / {}: slots {} -> {}, walk {} run {}", graphName, name, before,
                b->children._size, vanilla->slots[picked.walk].clip, vanilla->slots[picked.run].clip);
        }
        summary.nodes += build.nodes;
        bound += build.bound;
        boundOwn += build.boundOwn;
        unbound += build.unbound;
        // The patched blenders' direction blends read the held direction (LocomotionDrive::kDirectionVariable). Every
        // vanilla direction blend is unbound; the BSCyclicBlendTransitionGenerator above it passes its fBlendParameter
        // (bound to Direction) down, so that binding is the one to point elsewhere (11-15).
        int directions = 0;
        {
            std::unordered_set<RE::hkbNode*> patched;
            for (const auto& r : ready) patched.insert(reinterpret_cast<RE::hkbNode*>(r.blender));
            std::unordered_set<RE::hkbNode*> visited;
            std::vector<RE::hkbNode*> pending{root};
            while (!pending.empty()) {
                auto* node = pending.back();
                pending.pop_back();
                if (!node || !visited.insert(node).second || visited.size() > 200000) continue;
                auto* blend = NodeFactory::SameVTable(node, RE::VTABLE_BSCyclicBlendTransitionGenerator)
                                  ? reinterpret_cast<RE::hkbNode*>(
                                        reinterpret_cast<RE::BSCyclicBlendTransitionGenerator*>(node)->generator)
                                  : nullptr;
                if (blend && NodeFactory::SameVTable(blend, RE::VTABLE_hkbBlenderGenerator)) {
                    const auto* b = reinterpret_cast<const BlenderGeneratorLayout*>(blend);
                    bool parent = false;
                    for (int i = 0; i < b->children._size && !parent; ++i)
                        parent = b->children._data[i] &&
                                 patched.contains(reinterpret_cast<RE::hkbNode*>(b->children._data[i]->generator));
                    if (parent && directionDone.insert(node).second && LocomotionDrive::RebindDirection(graph, node))
                        ++directions;
                }
                if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
                const int count = ListChildren(node, buffer);
                for (int i = count - 1; i >= 0; --i) pending.push_back(buffer[i].node);
            }
        }
        heldDirections += directions;
        logger::info("[Locomotion] {}: {} blenders split ({} of that name are other ladders), {} clips, {} direction "
                     "blends hold their direction at speed 0{}",
            graphName, ready.size(), other, build.nodes, directions,
            build.stanceClips ? fmt::format("; {} stance clips", build.stanceClips) : std::string());
    }
    int missing = 0;
    for (const auto* table : {&rows, &blockRows})
        for (const auto& [name, set] : *table)
            if (!matched.contains(name)) {
                ++missing;
                logger::debug(
                    "[Locomotion] {} ({}): no graph of the character has this blender", name, LadderSetName(set));
            }
    if (locomotion.AnyTree() && !treesDone)
        logger::warn("[Tree] <Tree> is declared, but this project has no Melee_Locomotion_Behavior graph");
    // Unarmed's own tree shows on the whole body: after the loop, as 1HM_Behavior may come before the locomotion graph.
    if (unarmedTree)
        OwnLocomotion::FullBodyArms(
            armGraph, armPose, readied, [&](std::size_t nodes) { return room(armGraph, nodes); });
    if (!copies.empty()) AnimData::QueueCopies(std::move(copies));
    logger::info("[Locomotion] {} of {} blenders split ({} rebuilt as vanilla first), {} clips; axis on {} (direction "
                 "held on {}); rates follow the speed on {} clips ({} of dedicated files), fixed on {}; {} declared "
                 "blender names not in this project",
        summary.patched, summary.blenders, summary.rebuilt, summary.nodes, gaits, heldDirections, bound, boundOwn,
        unbound, missing);
    return summary;
}

void CheckMask(RE::hkbCharacter* character) {
    auto* setup = character ? character->setup.get() : nullptr;
    const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
    if (!skeleton) return;
    std::lock_guard lock(g_toggleMutex);
    if (g_maskUpper.empty() || !g_checkedSkeletons.insert(skeleton).second) return;
    const int n = skeleton->bones._size;
    const char* name = skeleton->name.c_str();
    const int mask = static_cast<int>(g_maskUpper.size());
    int first = -1, differ = 0;
    if (n > 0 && n <= 1024 && skeleton->bones._data)
        for (int i = 0; i < std::min(n, mask); ++i)
            if (UpperBone(skeleton->bones._data[i].name.c_str()) != g_maskUpper[static_cast<std::size_t>(i)]) {
                if (first < 0) first = i;
                ++differ;
            }
    if (n == mask && !differ) {
        logger::info("[Locomotion] upper body mask fits skeleton '{}' ({} bones)", name ? name : "?", n);
        return;
    }
    const char* at = first >= 0 ? skeleton->bones._data[first].name.c_str() : nullptr;
    logger::warn("[Locomotion] skeleton '{}' ({} bones) does not match the upper body mask made from '{}' ({} bones): "
                 "{} bones classed otherwise{}{}; while walk and run mix, this character's upper body may take the "
                 "wrong clip",
        name ? name : "?", n, g_maskSkeleton, mask, differ, first >= 0 ? ", the first at " : "",
        first >= 0 ? fmt::format("{} '{}'", first, at ? at : "?") : std::string());
}

bool Enabled() { return g_enabled.load(); }

void Toggle() {
    const bool on = !g_enabled.load();
    std::lock_guard lock(g_toggleMutex);
    if (g_templates.empty()) {
        Notify("Animation Injector: no <Locomotion> is loaded");
        return;
    }
    g_enabled.store(on);
    for (const auto& t : g_templates)
        if (const auto layout = g_layouts.find(t.key); layout != g_layouts.end())
            SetValues(t.blender, layout->second, on);
    SetMasks(on);  // shared by the templates and their clones
    std::vector<Child> buffer;
    int actors = 0, blenders = 0, children = 0;
    const auto setActor = [&](RE::Actor* actor) {
        if (!actor || !actor->Is3DLoaded()) return;
        RE::BSAnimationGraphManagerPtr manager;
        if (!actor->GetAnimationGraphManager(manager) || !manager) return;
        bool any = false;
        for (auto& graph : manager->graphs) {
            if (!graph || !graph->behaviorGraph) continue;
            const auto [b, c] = SetLive(graph->behaviorGraph, on, buffer);
            blenders += b;
            children += c;
            any |= b > 0;
        }
        actors += any;
    };
    setActor(RE::PlayerCharacter::GetSingleton());
    if (auto* lists = RE::ProcessLists::GetSingleton())
        for (auto& handle : lists->highActorHandles)
            if (auto actor = handle.get()) setActor(actor.get());
    LocomotionDrive::SetEnabled(on);
    logger::info("[Locomotion] {}: {} actors, {} blenders, {} slots set (templates {})", on ? "on" : "off (vanilla)",
        actors, blenders, children, g_templates.size());
    Notify(on ? "Animation Injector: locomotion split ON" : "Animation Injector: locomotion split OFF (vanilla)");
}

}  // namespace AnimationInjector::LocomotionSplit

namespace AnimationInjector::LocomotionToggle {
void OnKeyDown(std::uint32_t code) {
    const auto key = FrameworkSettings::Get().ladderToggleKey;
    if (key && code == static_cast<std::uint32_t>(key)) LocomotionSplit::Toggle();
}
}  // namespace AnimationInjector::LocomotionToggle

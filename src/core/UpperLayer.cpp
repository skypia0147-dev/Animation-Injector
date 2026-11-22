#include "core/UpperLayer.h"
#include "core/AnimationLimit.h"
#include "core/BlenderLayout.h"
#include "core/NodeFactory.h"
#include "core/PoseTrackReader.h"
#include "core/UpperStabilize.h"
#include "runtime/ActionRegistry.h"
#include "runtime/GripHold.h"

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace AnimationInjector::UpperLayer {
namespace {
using Havok::BlenderGeneratorChildLayout;
using Havok::BlenderGeneratorLayout;
using Havok::ManualSelectorLayout;
using Binding = RE::hkbVariableBindingSet::Binding;
using Quat = UpperStabilize::Quat;

constexpr int kPoseTrack = 2;                 // hkbGeneratorOutput::TRACK_POSE (core/CarryPoseLock)
constexpr float kBlenderMarker = 1.2345e-7f;  // AI_Layer_Upper's referencePoseWeightThreshold
constexpr float kMixMarker = 2.3456e-7f;      // AI_Layer_Upper_Mix's
constexpr std::array<const char*, kBodyLayers> kLayerNames{"Upper", "RightArm", "LeftArm"};

struct RawArr {
    void* data;
    std::int32_t size;
    std::int32_t capFlags;
};
struct Child {
    RE::hkbNode* node;
    void* info;
};
// The node's generator children as the engine lists them (hkbNode virtuals 8 and 9), as LocomotionSplit's.
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

const char* BindingPath(const Binding& binding) {
    return reinterpret_cast<const RE::hkStringPtr*>(&binding.bindingPath)->c_str();
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ── Tables, written by the graph pass, read by the runner and the hooks afterwards ───────────────────────────────
std::atomic_bool g_ready{false};
std::array<bool, kBodyLayers> g_layerReady{};
std::vector<int> g_clipIndex;                       // by action
std::unique_ptr<std::atomic<float>[]> g_durations;  // by action
std::unordered_map<std::string, int> g_clipAction;  // lowered clip node name -> action
std::vector<int> g_spineChain, g_parentChain;       // bone indices, root first: Spine, and its parent
Quat g_parentRest{0, 0, 0, 1};                      // the spine's parent's model rotation in the reference pose
std::vector<int> g_lowerBones;  // standing's bones: the spine's parent (NPC COM) and all under it but the spine's
// Standing amount by character (SetStand): written on the main thread, read on the animation threads.
std::mutex g_standLock;
std::vector<std::pair<const void*, float>> g_stand;
float StandOf(const void* character) {
    std::scoped_lock guard(g_standLock);
    for (const auto& [c, s] : g_stand)
        if (c == character) return s;
    return 0.0f;
}
int g_spine = -1;
// facing="camera" (SetAim): the spine's three bones that share the aim's turn, and the first one's model rotation in
// the reference pose (headings are measured from it). -1: no aim (the skeleton has no Spine1 > Spine2 under Spine).
int g_spine1 = -1, g_spine2 = -1;
Quat g_spineRest{0, 0, 0, 1};
// The waist spread (UpperStabilize::SpreadWaist): the pelvis, and the reference model rotations of pelvis, Spine,
// Spine1, Spine2. -1: no spread.
int g_pelvis = -1;
std::array<Quat, 4> g_waistRests{};
struct AimValue {
    float yaw = 0.0f, pitch = 0.0f;
};
std::mutex g_aimLock;
std::vector<std::pair<const void*, AimValue>> g_aim;
// <Stance>: the upper layer clip's own chest turn from the front by character (StanceTurn), degrees, left positive.
std::mutex g_turnLock;
std::vector<std::pair<const void*, float>> g_turn;
void NoteTurn(const void* character, float degrees) {
    std::scoped_lock guard(g_turnLock);
    for (auto& [c, t] : g_turn)
        if (c == character) {
            t = degrees;
            return;
        }
    g_turn.emplace_back(character, degrees);
}
bool AimOf(const void* character, AimValue& out) {
    std::scoped_lock guard(g_aimLock);
    for (const auto& [c, a] : g_aim)
        if (c == character) {
            out = a;
            return true;
        }
    return false;
}

// ── Bone classes ───────────────────────────────────────────────────────────────────────────────────────────────
enum Mask : unsigned { kUpperBit = 1, kRightBit = 2, kLeftBit = 4 };
unsigned Classify(const char* name) {
    if (!name) return 0;
    const auto lower = Lower(name);
    static constexpr std::array armParts{
        "clavicle", "upperarm", "uparm", "forearm", "hand", "finger", "magicnode", "pauldron"};
    const auto arm = [&](std::string_view side) {
        if (!lower.starts_with(side)) return false;
        for (const char* part : armParts)
            if (lower.find(part) != std::string::npos) return true;
        return false;
    };
    unsigned bits = 0;
    if (arm("npc r ") || lower == "weapon" || lower == "animobjectr") bits |= kRightBit;
    if (arm("npc l ") || lower == "shield" || lower == "animobjectl") bits |= kLeftBit;
    if (bits) return bits | kUpperBit;
    static constexpr std::array upper{"spine", "neck", "head", "eyebone", "quiver", "weaponback", "weaponbow", "item0"};
    for (const char* part : upper)
        if (lower.find(part) != std::string::npos) return kUpperBit;
    return 0;
}

// hkbBoneWeightArray (Havok 2010.2): hkbBindable 0x30 | boneWeights hkArray<hkReal> 0x30 (as LocomotionSplit's).
struct BoneWeightArrayLayout {
    std::uint8_t bindable[0x30];
    RE::hkArray<float> boneWeights;
};
static_assert(sizeof(BoneWeightArrayLayout) == 0x40);
RE::hkReferencedObject* MakeMask(const RE::hkaSkeleton* skeleton, unsigned bit, int& count) {
    const int n = skeleton->bones._size;
    auto* mask = static_cast<BoneWeightArrayLayout*>(NodeFactory::AllocZero(sizeof(BoneWeightArrayLayout)));
    NodeFactory::InitRefObj(mask, RE::VTABLE_hkbBoneWeightArray);
    NodeFactory::EmptyArray(reinterpret_cast<RE::hkbBindable*>(mask)->cachedBindables);
    auto* weights = static_cast<float*>(NodeFactory::AllocZero(sizeof(float) * static_cast<std::size_t>(n)));
    count = 0;
    for (int i = 0; i < n; ++i)
        if (Classify(skeleton->bones._data[i].name.c_str()) & bit) {
            weights[i] = 1.0f;
            ++count;
        }
    NodeFactory::OwnArray(mask->boneWeights, weights, n);
    return reinterpret_cast<RE::hkReferencedObject*>(mask);
}

// The bone's chain from the root, by the skeleton's parent indices; empty when the name is not there.
std::vector<int> Chain(const RE::hkaSkeleton* skeleton, const char* name, int& index) {
    index = -1;
    for (int i = 0; i < skeleton->bones._size; ++i)
        if (const char* n = skeleton->bones._data[i].name.c_str(); n && _stricmp(n, name) == 0) index = i;
    std::vector<int> chain;
    if (index < 0 || skeleton->parentIndices._size != skeleton->bones._size) return chain;
    for (int b = index; b >= 0 && chain.size() < 64; b = skeleton->parentIndices._data[b])
        chain.insert(chain.begin(), b);
    return chain;
}

// ── Where the layers go ─────────────────────────────────────────────────────────────────────────────────────────
// Under the bone switches Master_Behavior picks by hand (docs/조사-왼손무기-Shield보정.md, 11-21 live graph): the dual
// wield Shield adjustment (a left-hand one-handed weapon's Shield bone is DW_ShieldAdjustment's whatever plays below),
// the staff and crossbow switches. Every one of them reaches RootBehaviorGraph, the state machine of every ordinary
// state, from four parents (1st3rdPerson_MSG, Crossbow1st3rdPerson_MSG, BSBoneSwitchGenerator00,
// WeaponAdjustment_OffsetAnimationGen). Over the switches, a part's Shield track replaced the adjustment and turned the
// left-hand weapon over.
struct Under {
    RE::hkbGenerator* target = nullptr;
    std::vector<std::pair<RE::hkbNode*, RE::hkbGenerator**>> refs;  // each parent's reference to the target
    std::string why;
};
Under BelowSwitches(RE::hkbNode* root) {
    Under out;
    std::vector<Child> buffer;
    std::vector<RE::hkbNode*> queue{root}, parents;
    std::unordered_set<RE::hkbNode*> seen{root};
    for (std::size_t q = 0; q < queue.size() && queue.size() < 200000; ++q) {
        auto* node = queue[q];
        if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
        const int n = ListChildren(node, buffer);
        for (int i = 0; i < n; ++i) {
            auto* c = buffer[static_cast<std::size_t>(i)].node;
            if (!c) continue;
            if (!out.target && NodeFactory::SameVTable(c, RE::VTABLE_hkbStateMachine) &&
                _stricmp(NodeFactory::NodeName(c), "RootBehaviorGraph") == 0)
                out.target = reinterpret_cast<RE::hkbGenerator*>(c);
            if (c == reinterpret_cast<RE::hkbNode*>(out.target) &&
                std::find(parents.begin(), parents.end(), node) == parents.end())
                parents.push_back(node);
            if (seen.insert(c).second) queue.push_back(c);
        }
    }
    if (!out.target) {
        out.why = "no RootBehaviorGraph state machine in Master_Behavior";
        return out;
    }
    for (auto* parent : parents) {
        const std::size_t before = out.refs.size();
        if (NodeFactory::SameVTable(parent, RE::VTABLE_hkbManualSelectorGenerator)) {
            auto& list = reinterpret_cast<ManualSelectorLayout*>(parent)->generators;
            for (int i = 0; i < list._size; ++i)
                if (list._data[i] == out.target) out.refs.emplace_back(parent, &list._data[i]);
        } else if (NodeFactory::SameVTable(parent, RE::VTABLE_BSBoneSwitchGenerator) ||
                   NodeFactory::SameVTable(parent, RE::VTABLE_BSOffsetAnimationGenerator)) {
            // Both: hkbGenerator 0x48 | 8 bytes | pDefaultGenerator 0x50, Bethesda generators' members starting at 0x50
            // (Havok::StateTaggingLayout). The live graph: RootBehaviorGraph is that default; anything else refuses.
            auto** ref = reinterpret_cast<RE::hkbGenerator**>(reinterpret_cast<std::uintptr_t>(parent) + 0x50);
            if (*ref == out.target) out.refs.emplace_back(parent, ref);
        }
        if (out.refs.size() == before) {
            out.why =
                fmt::format("its parent '{}' holds it where the layers cannot repoint", NodeFactory::NodeName(parent));
            out.refs.clear();
            return out;
        }
    }
    if (out.refs.empty()) out.why = "RootBehaviorGraph has no parent";
    return out;
}

// ── Bindings: copies of a vanilla binding with another variable ───────────────────────────────────────────────
struct Sample {
    const RE::hkbVariableBindingSet* set = nullptr;
    int at = -1;
};
// The first binding of `path` (mode 0, a variable of `type`) on a node of the vtable, anywhere in the graphs.
Sample FindSample(const std::vector<RE::hkbBehaviorGraph*>& graphs, const std::array<REL::VariantID, 1>& vtable,
    const char* path, RE::hkbVariableInfo::VariableType type) {
    std::vector<Child> buffer;
    for (auto* graph : graphs) {
        auto* data = graph ? graph->data.get() : nullptr;
        if (!data || !graph->rootGenerator) continue;
        std::unordered_set<RE::hkbNode*> seen;
        std::vector<RE::hkbNode*> stack{graph->rootGenerator.get()};
        while (!stack.empty()) {
            auto* node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second || seen.size() > 200000) continue;
            if (NodeFactory::SameVTable(node, vtable))
                if (const auto* set = node->variableBindingSet.get();
                    set && NodeFactory::SameVTable(set, RE::VTABLE_hkbVariableBindingSet) && set->bindings._data &&
                    set->bindings._size > 0 && set->bindings._size <= 8)
                    for (int i = 0; i < set->bindings._size; ++i) {
                        const auto& b = set->bindings._data[i];
                        const char* p = BindingPath(b);
                        if (p && std::strcmp(p, path) == 0 && static_cast<std::int8_t>(b.bindingMode) == 0 &&
                            b.variableIndex >= 0 && b.variableIndex < data->variableInfos._size &&
                            data->variableInfos._data[b.variableIndex].m_type == type)
                            return {set, i};
                    }
            if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
            const int count = ListChildren(node, buffer);
            for (int i = count - 1; i >= 0; --i) stack.push_back(buffer[static_cast<std::size_t>(i)].node);
        }
    }
    return {};
}
// A new binding set holding the sample's one binding pointed at `variable`.
RE::hkbVariableBindingSet* BindingTo(const Sample& sample, int variable) {
    auto* set = static_cast<RE::hkbVariableBindingSet*>(NodeFactory::AllocZero(sizeof(RE::hkbVariableBindingSet)));
    auto* item = static_cast<Binding*>(NodeFactory::AllocZero(sizeof(Binding)));
    std::memcpy(set, sample.set, sizeof(*set));
    NodeFactory::InitRefObj(set, RE::VTABLE_hkbVariableBindingSet);
    *item = sample.set->bindings._data[sample.at];
    item->variableIndex = variable;
    NodeFactory::OwnArray(set->bindings, item, 1);
    set->indexOfActivationBinding = -1;
    return set;
}
int AddVariable(RE::hkbBehaviorGraph* graph, const std::string& name, bool integer) {
    auto* data = graph ? graph->data.get() : nullptr;
    if (!data || !data->stringData || !data->variableInitialValues) return -1;
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return -1;
    if (const int existing = NodeFactory::FindName(names, name.c_str()); existing >= 0) return existing;
    const int count = names._size;
    const int index = integer ? data->AddIntVariable(RE::hkStringPtr(name.c_str()), 0)
                              : data->AddFloatVariable(RE::hkStringPtr(name.c_str()), 0.0f);
    return index == count && names._size == count + 1 ? index : -1;
}

void NewNodeBase(RE::hkbNode* node, const RE::hkbNode* like, const std::string& name) {
    std::memset(&node->variableBindingSet, 0, sizeof(node->variableBindingSet));
    NodeFactory::EmptyArray(node->cachedBindables);
    node->areBindablesCached = false;
    node->userData = 0;
    std::memset(&node->name, 0, sizeof(node->name));
    NodeFactory::MakeString(node->name, name.c_str());
    node->id = like->id;
    node->cloneState = like->cloneState;
}

BlenderGeneratorChildLayout* MakeChild(
    RE::hkbGenerator* generator, float weight, float worldFromModel, RE::hkReferencedObject* boneWeights) {
    auto* c = static_cast<BlenderGeneratorChildLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorChildLayout)));
    NodeFactory::InitRefObj(c, RE::VTABLE_hkbBlenderGeneratorChild);
    NodeFactory::EmptyArray(reinterpret_cast<RE::hkbBindable*>(c)->cachedBindables);
    c->generator = generator;
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(generator));
    c->weight = weight;
    c->worldFromModelWeight = worldFromModel;
    c->boneWeights = boneWeights;
    if (boneWeights) NodeFactory::AddRef(boneWeights);
    return c;
}

// ── Pose tracks ────────────────────────────────────────────────────────────────────────────────────────────────
bool ReadRotation(const PoseTrackReader::Output& output, int bone, Quat& out) {
    const auto r = PoseTrackReader::Read(output, kPoseTrack, bone);
    if (r.error) return false;
    out = {r.qs[4], r.qs[5], r.qs[6], r.qs[7]};
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && std::isfinite(out[3]);
}
bool ChainRotation(const PoseTrackReader::Output& output, const std::vector<int>& chain, Quat& out) {
    std::array<Quat, 64> locals{};
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (!ReadRotation(output, chain[i], locals[i])) return false;
    out = UpperStabilize::ChainRotation(std::span<const Quat>(locals.data(), chain.size()));
    return true;
}
// Blends one bone of the output toward `clip`'s by t: translation linearly, rotation on the shortest arc.
void BlendBone(PoseTrackReader::Output& output, const PoseTrackReader::Output& clip, int bone, float t) {
    const auto a = PoseTrackReader::Read(output, kPoseTrack, bone);
    const auto b = PoseTrackReader::Read(clip, kPoseTrack, bone);
    if (a.error || b.error) return;
    std::array<float, 8> qs{};
    for (int i = 0; i < 3; ++i) qs[i] = a.qs[i] + (b.qs[i] - a.qs[i]) * t;
    qs[3] = a.qs[3];
    const auto q = UpperStabilize::Slerp({a.qs[4], a.qs[5], a.qs[6], a.qs[7]}, {b.qs[4], b.qs[5], b.qs[6], b.qs[7]}, t);
    std::copy(q.begin(), q.end(), qs.begin() + 4);
    auto* base = const_cast<std::byte*>(reinterpret_cast<const std::byte*>(output.tracks));
    PoseTrackReader::Header header;
    std::memcpy(
        &header, base + sizeof(PoseTrackReader::Master) + kPoseTrack * sizeof(PoseTrackReader::Header), sizeof(header));
    std::memcpy(base + header.offset + bone * 48, qs.data(), sizeof(qs));
}
// Writes one bone's rotation (Havok x, y, z, w) over a dense track already validated by a read of that bone.
void WriteRotation(PoseTrackReader::Output& output, int bone, const Quat& q) {
    const auto read = PoseTrackReader::Read(output, kPoseTrack, bone);
    if (read.error) return;
    auto* base = const_cast<std::byte*>(reinterpret_cast<const std::byte*>(output.tracks));
    PoseTrackReader::Header header;
    std::memcpy(
        &header, base + sizeof(PoseTrackReader::Master) + kPoseTrack * sizeof(PoseTrackReader::Header), sizeof(header));
    std::memcpy(base + header.offset + bone * 48 + 4 * sizeof(float), q.data(), 4 * sizeof(float));
}

// The slots' model-space spine rotation, kept for the character the thread is generating (Havok generates one
// character's graph on one thread, children before their parent).
struct Kept {
    const void* character = nullptr;
    Quat spine{0, 0, 0, 1};
    Quat parent{0, 0, 0, 1};  // the slots' own spine parent (Root, COM): its heading is the clip's front
};
thread_local Kept t_kept;

struct BlenderGenerate {
    static void thunk(RE::hkbNode* node, const RE::hkbContext& context, const PoseTrackReader::Output** children,
        PoseTrackReader::Output& output, float offset) {
        original(node, context, children, output, offset);
        const auto* blender = reinterpret_cast<const BlenderGeneratorLayout*>(node);
        if (blender->referencePoseWeightThreshold == kMixMarker) {
            // The slots' pose, generated just before the upper layer that holds them.
            Quat spine, parent{0, 0, 0, 1};
            if (!g_spineChain.empty() && ChainRotation(output, g_spineChain, spine)) {
                ChainRotation(output, g_parentChain, parent);
                t_kept = {context.character, spine, parent};
                // The clip's chest heading from the spine's reference pose: how far the clip turns the upper body.
                const Quat twist = UpperStabilize::TwistZ(UpperStabilize::Multiply(
                    spine, {-g_spineRest[0], -g_spineRest[1], -g_spineRest[2], g_spineRest[3]}));
                NoteTurn(context.character, 2.0f * std::atan2(twist[2], twist[3]) * 57.29578f);
            }
            return;
        }
        if (blender->referencePoseWeightThreshold != kBlenderMarker) return;
        // A held grip over what the layer gave its bones (runtime/GripHold.h): the top layer's write is the last.
        GripHold::Apply(context, output, true);
        const auto kept = std::exchange(t_kept, Kept{});
        const float w = std::clamp(blender->blendParameter / (1.0f - kBelowFloor), 0.0f, 1.0f);
        if (!(w > 0.0f) || kept.character != context.character || g_spine < 0) return;
        // Standing: the clip's own legs under its upper body (children[1] is the layer's mix, the clip's whole pose).
        const auto* clip = children ? children[1] : nullptr;
        const float stand = clip ? std::clamp(StandOf(context.character), 0.0f, 1.0f) : 0.0f;
        if (stand > 0.0f)
            for (const int bone : g_lowerBones) BlendBone(output, *clip, bone, w * stand);
        Quat parent, local;
        if (!ChainRotation(output, g_parentChain, parent) || !ReadRotation(output, g_spine, local)) return;
        Quat target = UpperStabilize::SpineLocal(parent, g_parentRest, kept.parent, kept.spine, local, 1.0f);
        if (Quat own; stand > 0.0f && ReadRotation(*clip, g_spine, own))
            target = UpperStabilize::Slerp(target, own, stand);
        WriteRotation(output, g_spine, UpperStabilize::Slerp(local, target, w));
        // facing="camera": the upper body turned where the camera looks, the turn shared by the spine's three bones
        // (one bone alone bent the body over at a side step's 90 degrees, 11-21).
        if (AimValue aim; g_spine2 >= 0 && AimOf(context.character, aim)) {
            Quat l0, l1, l2;
            if (!ReadRotation(output, g_spine, l0) || !ReadRotation(output, g_spine1, l1) ||
                !ReadRotation(output, g_spine2, l2))
                return;
            constexpr Quat identity{0, 0, 0, 1};
            const Quat m0 = UpperStabilize::Multiply(parent, l0);
            const Quat turn = UpperStabilize::Slerp(
                identity, UpperStabilize::AimTurn(kept.spine, m0, g_spineRest, aim.yaw, aim.pitch), w);
            const Quat share = UpperStabilize::Slerp(identity, turn, 1.0f / 3.0f);
            const Quat m1 = UpperStabilize::Multiply(m0, l1);
            WriteRotation(output, g_spine, UpperStabilize::TurnLocal(parent, share, l0));
            WriteRotation(output, g_spine1, UpperStabilize::TurnLocal(m0, share, l1));
            WriteRotation(output, g_spine2, UpperStabilize::TurnLocal(m1, share, l2));
        }
        // The waist: the upper body's turn from the locomotion's pelvis spread over the spine's three joints, the
        // chest's heading kept (standing, the clip's own legs and spine are the clip's).
        if (const float spread = w * (1.0f - stand); g_pelvis >= 0 && spread > 0.0f) {
            std::array<Quat, 3> locals;
            Quat pelvis;
            if (ReadRotation(output, g_pelvis, pelvis) && ReadRotation(output, g_spine, locals[0]) &&
                ReadRotation(output, g_spine1, locals[1]) && ReadRotation(output, g_spine2, locals[2])) {
                const auto spread3 = UpperStabilize::SpreadWaist(parent, pelvis, locals, g_waistRests, spread);
                WriteRotation(output, g_spine, spread3[0]);
                WriteRotation(output, g_spine1, spread3[1]);
                WriteRotation(output, g_spine2, spread3[2]);
            }
        }
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}  // namespace

Summary Apply(RE::hkbBehaviorGraph* master, const RE::BSScrapArray<RE::hkbBehaviorGraph*>& graphs,
    RE::hkbCharacter* character, const RoomFn& room) {
    Summary summary;
    const int count = Actions::Count();
    std::array<std::vector<int>, kBodyLayers> byLayer;
    for (int a = 0; a < count; ++a)
        if (const auto& spec = Actions::Spec(a); spec.Part() && !spec.clips.empty())
            byLayer[static_cast<std::size_t>(LayerOf(spec.body))].push_back(a);
    if (std::all_of(byLayer.begin(), byLayer.end(), [](const auto& l) { return l.empty(); })) return summary;
    const auto refuse = [](const std::string& why) {
        logger::warn("[UpperLayer] no part layers: {}; body= actions do not start", why);
        return Summary{};
    };
    if (g_ready.load()) return summary;  // one template: the character projects share the behavior graphs

    // Where: around RootBehaviorGraph, under the hand bone switches (BelowSwitches). A graph without that shape keeps
    // the first place, RootModifierGenerator's generator, over the switches.
    auto* root = master ? master->rootGenerator.get() : nullptr;
    if (!root) return refuse("no master graph");
    std::vector<Child> buffer;
    RE::hkbNode* modifierGenerator = nullptr;
    const int kids = ListChildren(root, buffer);
    for (int i = 0; i < kids && !modifierGenerator; ++i)
        if (auto* n = buffer[static_cast<std::size_t>(i)].node;
            n && NodeFactory::SameVTable(n, RE::VTABLE_hkbModifierGenerator) &&
            _stricmp(NodeFactory::NodeName(n), "RootModifierGenerator") == 0)
            modifierGenerator = n;
    if (!modifierGenerator) return refuse("Master_Behavior has no RootModifierGenerator");
    // hkbModifierGenerator: hkbGenerator 0x48 | modifier 0x48 | generator 0x50 (11-20 probe).
    auto& slot = *reinterpret_cast<RE::hkbGenerator**>(reinterpret_cast<std::uintptr_t>(modifierGenerator) + 0x50);
    if (!Havok::NodeNamed(slot, nullptr)) return refuse("RootModifierGenerator has no generator");
    const auto under = BelowSwitches(root);
    if (under.refs.empty())
        logger::warn(
            "[UpperLayer] {}: the part layers go over the hand bone switches, where a left-hand one-handed weapon "
            "turns over under a part action",
            under.why);
    RE::hkbGenerator* const target = under.refs.empty() ? slot : under.target;
    const std::string belowName = NodeFactory::NodeName(target);

    auto* setup = character ? character->setup.get() : nullptr;
    const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
    if (!skeleton || skeleton->bones._size <= 0 || skeleton->bones._size > 1024 || !skeleton->bones._data)
        return refuse("the character has no animation skeleton");
    // The spine hangs from NPC COM in the game's skeleton, beside the pelvis (11-20: the first build assumed the
    // pelvis and found no stabilizer): whatever its parent, the spine is turned against that parent's model rotation.
    g_spineChain = Chain(skeleton, "NPC Spine [Spn0]", g_spine);
    g_parentChain.assign(g_spineChain.begin(), g_spineChain.empty() ? g_spineChain.begin() : g_spineChain.end() - 1);
    if (g_spineChain.size() < 2) {
        logger::warn("[UpperLayer] no NPC Spine [Spn0] under the root in '{}': no stabilizer",
            skeleton->name.c_str() ? skeleton->name.c_str() : "?");
        g_spineChain.clear();
        g_parentChain.clear();
        g_spine = -1;
    } else {
        std::vector<Quat> rest;
        for (const int b : g_parentChain) {
            Quat q{0, 0, 0, 1};
            if (b >= 0 && b < static_cast<int>(skeleton->referencePose.size()))
                std::memcpy(q.data(), &skeleton->referencePose[b].rotation.vec, sizeof(q));
            rest.push_back(q);
        }
        g_parentRest = UpperStabilize::ChainRotation(rest);
        {
            Quat own{0, 0, 0, 1};
            if (g_spine < static_cast<int>(skeleton->referencePose.size()))
                std::memcpy(own.data(), &skeleton->referencePose[g_spine].rotation.vec, sizeof(own));
            const std::array<Quat, 2> spineRest{g_parentRest, own};
            g_spineRest = UpperStabilize::ChainRotation(spineRest);
            int top = -1;
            const auto upper = Chain(skeleton, "NPC Spine2 [Spn2]", top);
            if (upper.size() == g_spineChain.size() + 2 && upper[g_spineChain.size() - 1] == g_spine) {
                g_spine1 = upper[g_spineChain.size()];
                g_spine2 = top;
                // The pelvis beside the spine, under the same parent (NPC COM): the waist spread.
                int pelvis = -1;
                const auto hips = Chain(skeleton, "NPC Pelvis [Pelv]", pelvis);
                const auto rest = [&](int b) {
                    Quat q{0, 0, 0, 1};
                    if (b >= 0 && b < static_cast<int>(skeleton->referencePose.size()))
                        std::memcpy(q.data(), &skeleton->referencePose[b].rotation.vec, sizeof(q));
                    return q;
                };
                if (pelvis >= 0 && hips.size() == g_spineChain.size() &&
                    hips[hips.size() - 2] == g_parentChain.back()) {
                    g_pelvis = pelvis;
                    const Quat spine1 = UpperStabilize::Multiply(g_spineRest, rest(g_spine1));
                    g_waistRests = {UpperStabilize::Multiply(g_parentRest, rest(pelvis)), g_spineRest, spine1,
                        UpperStabilize::Multiply(spine1, rest(g_spine2))};
                } else
                    logger::warn(
                        "[UpperLayer] NPC Pelvis [Pelv] is not beside NPC Spine: the waist turn is not spread");
            } else
                logger::warn(
                    "[UpperLayer] no NPC Spine1 > NPC Spine2 under NPC Spine: facing=\"camera\" keeps the legs' heading");
        }
        // Standing's bones: under the spine's parent, outside the spine's branch.
        g_lowerBones.clear();
        const int com = g_parentChain.back();
        const auto parentOf = [&](int b) {
            return b >= 0 && b < static_cast<int>(skeleton->parentIndices.size())
                       ? static_cast<int>(skeleton->parentIndices[b])
                       : -1;
        };
        for (int b = 0; b < skeleton->bones._size; ++b) {
            bool under = false, spine = false;
            for (int a = b, steps = 0; a >= 0 && steps < 128; a = parentOf(a), ++steps) {
                if (a == g_spine) spine = true;
                if (a == com) {
                    under = true;
                    break;
                }
            }
            if (under && !spine) g_lowerBones.push_back(b);
        }
        logger::info("[UpperLayer] standing gives the clip {} bones under {}", g_lowerBones.size(),
            skeleton->bones._data[com].name.c_str());
        std::string chain;
        for (const int b : g_spineChain)
            chain += fmt::format("{}{}", chain.empty() ? "" : " > ", skeleton->bones._data[b].name.c_str());
        logger::info("[UpperLayer] stabilizer chain: {}", chain);
    }

    std::vector<RE::hkbBehaviorGraph*> all{master};
    for (auto* g : graphs)
        if (g && g != master) all.push_back(g);
    const auto parameter =
        FindSample(all, RE::VTABLE_hkbBlenderGenerator, "blendParameter", RE::hkbVariableInfo::VARIABLE_TYPE_REAL);
    const auto selector = FindSample(
        all, RE::VTABLE_hkbManualSelectorGenerator, "selectedGeneratorIndex", RE::hkbVariableInfo::VARIABLE_TYPE_INT32);
    if (!parameter.set || !selector.set) return refuse("no vanilla blendParameter or selector binding to copy");
    // The node templates: a vanilla parametric blender and the selector that gave the binding.
    const RE::hkbNode* blenderLike = nullptr;
    const RE::hkbNode* selectorLike = nullptr;
    const RE::hkbNode* clipLike = nullptr;
    {
        std::unordered_set<RE::hkbNode*> seen;
        std::vector<RE::hkbNode*> stack{root};
        while (!stack.empty() && !(blenderLike && selectorLike && clipLike)) {
            auto* node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second || seen.size() > 200000) continue;
            if (!blenderLike && NodeFactory::SameVTable(node, RE::VTABLE_hkbBlenderGenerator)) blenderLike = node;
            if (!selectorLike && NodeFactory::SameVTable(node, RE::VTABLE_hkbManualSelectorGenerator))
                selectorLike = node;
            if (!clipLike && NodeFactory::SameVTable(node, RE::VTABLE_hkbClipGenerator)) clipLike = node;
            if (NodeFactory::SameVTable(node, RE::VTABLE_hkbBehaviorReferenceGenerator)) continue;
            const int n = ListChildren(node, buffer);
            for (int i = n - 1; i >= 0; --i) stack.push_back(buffer[static_cast<std::size_t>(i)].node);
        }
    }
    if (!blenderLike || !selectorLike || !clipLike) return refuse("no blender, selector or clip in Master_Behavior");
    std::size_t need = 0;
    for (const auto& l : byLayer)
        if (!l.empty()) need += 4 + 2 * l.size();
    if (!room(master, need)) return refuse(fmt::format("{} more nodes do not fit the node id budget", need));

    g_clipIndex.assign(static_cast<std::size_t>(count), -1);
    g_durations = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(count));
    RE::hkbGenerator* below = target;
    for (std::size_t layer = 0; layer < kBodyLayers; ++layer) {
        const auto& actions = byLayer[layer];
        if (actions.empty()) continue;
        const int w = AddVariable(master, kWeightVariable + std::to_string(layer), false);
        const int mixVar = AddVariable(master, kMixVariable + std::to_string(layer), false);
        const int clipVar = AddVariable(master, kClipVariable + std::to_string(layer), true);
        const int clipBVar = AddVariable(master, kClipBVariable + std::to_string(layer), true);
        if (w < 0 || mixVar < 0 || clipVar < 0 || clipBVar < 0) {
            logger::warn(
                "[UpperLayer] {}: its variables could not be added; its actions do not start", kLayerNames[layer]);
            continue;
        }
        int masked = 0;
        auto* mask = MakeMask(skeleton, layer == 0 ? kUpperBit : layer == 1 ? kRightBit : kLeftBit, masked);
        // The clips, in the selectors' order: one copy for each slot.
        std::array<std::vector<RE::hkbGenerator*>, 2> clips;
        for (const int a : actions) {
            const auto& spec = Actions::Spec(a);
            const auto& file = spec.clips.front().file;
            const int binding = NodeFactory::EnsureAnimationName(character, file.c_str());
            if (binding < 0 || binding > AnimationLimit::MaxBindingIndex()) {
                logger::warn("[UpperLayer] {}: no animation index for '{}'; it does not start", spec.id, file);
                continue;
            }
            g_clipIndex[static_cast<std::size_t>(a)] = static_cast<int>(clips[0].size());
            for (std::size_t slot = 0; slot < 2; ++slot) {
                const auto name = "AI_" + spec.id + (slot ? "_PartB" : "_Part");
                auto* clip = NodeFactory::MakeClip(
                    name.c_str(), file.c_str(), static_cast<std::uint16_t>(binding), nullptr, clipLike, true);
                clip->mode =
                    spec.loop.enabled ? RE::hkbClipGenerator::kModeLooping : RE::hkbClipGenerator::kModeSinglePlay;
                g_clipAction[Lower(name)] = a;
                clips[slot].push_back(clip);
            }
            ++summary.clips;
        }
        if (clips[0].empty()) continue;
        // A slot: a selector over its copies.
        const auto makeSelector = [&](std::size_t slot, int variable) {
            auto* select = static_cast<RE::hkbNode*>(NodeFactory::AllocZero(sizeof(ManualSelectorLayout)));
            std::memcpy(select, selectorLike, sizeof(ManualSelectorLayout));
            NodeFactory::InitRefObj(select, RE::VTABLE_hkbManualSelectorGenerator);
            NewNodeBase(select, selectorLike, std::string("AI_Layer_") + kLayerNames[layer] + (slot ? "_B" : "_A"));
            const auto& list = clips[slot];
            auto* items =
                static_cast<RE::hkbGenerator**>(NodeFactory::AllocZero(list.size() * sizeof(RE::hkbGenerator*)));
            std::copy(list.begin(), list.end(), items);
            auto* sel = reinterpret_cast<ManualSelectorLayout*>(select);
            NodeFactory::OwnArray(sel->generators, items, static_cast<std::int32_t>(list.size()));
            sel->selectedGeneratorIndex = 0;
            sel->currentGeneratorIndex = 0;
            select->variableBindingSet.reset(BindingTo(selector, variable));
            NodeFactory::NoteCreatedNode();
            return reinterpret_cast<RE::hkbGenerator*>(select);
        };
        // A parametric blend of two children at weights 0 and 1 (no sync: a sync master rescales the other child's
        // time, Draw Fix 2026-09; dense pose forced).
        const auto makeBlender = [&](const std::string& name, float marker, int variable, RE::hkbGenerator* first,
                                     float firstWorld, RE::hkbGenerator* second, float secondWorld,
                                     RE::hkReferencedObject* secondMask) {
            auto* node = static_cast<RE::hkbNode*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorLayout)));
            std::memcpy(node, blenderLike, sizeof(BlenderGeneratorLayout));
            NodeFactory::InitRefObj(node, RE::VTABLE_hkbBlenderGenerator);
            NewNodeBase(node, blenderLike, name);
            auto* b = reinterpret_cast<BlenderGeneratorLayout*>(node);
            b->referencePoseWeightThreshold = marker;
            b->blendParameter = 0.0f;
            b->minCyclicBlendParameter = 0.0f;
            b->maxCyclicBlendParameter = 0.0f;
            b->indexOfSyncMasterChild = -1;
            b->flags = BlenderGeneratorLayout::kParametricBlend | BlenderGeneratorLayout::kForceDensePose;
            b->subtractLastChild = false;
            auto* children = static_cast<BlenderGeneratorChildLayout**>(
                NodeFactory::AllocZero(2 * sizeof(BlenderGeneratorChildLayout*)));
            children[0] = MakeChild(first, 0.0f, firstWorld, nullptr);
            children[1] = MakeChild(second, 1.0f, secondWorld, secondMask);
            NodeFactory::OwnArray(b->children, children, 2);
            NodeFactory::EmptyArray(b->childrenInternalStates);
            NodeFactory::EmptyArray(b->sortedChildren);
            b->endIntervalWeight = 0.0f;
            b->numActiveChildren = 0;
            b->beginIntervalIndex = b->endIntervalIndex = 0;
            b->initSync = b->doSubtractiveBlend = false;
            node->variableBindingSet.reset(BindingTo(parameter, variable));
            NodeFactory::NoteCreatedNode();
            return reinterpret_cast<RE::hkbGenerator*>(node);
        };
        const bool stabilized = layer == 0 && g_spine >= 0;
        auto* mix = makeBlender(std::string("AI_Layer_") + kLayerNames[layer] + "_Mix", stabilized ? kMixMarker : 0.0f,
            mixVar, makeSelector(0, clipVar), 0.0f, makeSelector(1, clipBVar), 0.0f, nullptr);
        // The layer: what is below at 0, the slots at 1.
        below = makeBlender(std::string("AI_Layer_") + kLayerNames[layer], stabilized ? kBlenderMarker : 0.0f, w, below,
            1.0f, mix, 0.0f, mask);
        g_layerReady[layer] = true;
        ++summary.layers;
        logger::info("[UpperLayer] {}: {} clips in two slots, {} of {} bones masked{}", kLayerNames[layer],
            clips[0].size(), masked, skeleton->bones._size, stabilized ? ", stabilized at NPC Spine [Spn0]" : "");
    }
    if (below != target) {
        if (under.refs.empty()) {
            NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(below));
            slot = below;
            modifierGenerator->areBindablesCached = false;
        } else {
            for (const auto& [parent, ref] : under.refs) {
                NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(below));
                *ref = below;
                parent->areBindablesCached = false;
            }
        }
    }
    g_ready.store(true);
    logger::info("[UpperLayer] {} layers over '{}' ({} references from {}); {} part clips", summary.layers, belowName,
        under.refs.empty() ? 1 : under.refs.size(),
        under.refs.empty() ? "RootModifierGenerator" : "under the hand switches", summary.clips);
    return summary;
}

int ClipIndex(int action) {
    if (!g_ready.load() || action < 0 || action >= static_cast<int>(g_clipIndex.size())) return -1;
    return g_clipIndex[static_cast<std::size_t>(action)];
}
void SetStand(const RE::hkbCharacter* character, float stand) {
    std::scoped_lock guard(g_standLock);
    const auto it = std::find_if(g_stand.begin(), g_stand.end(), [&](const auto& e) { return e.first == character; });
    if (!(stand > 0.0f)) {
        if (it != g_stand.end()) g_stand.erase(it);
    } else if (it != g_stand.end())
        it->second = stand;
    else
        g_stand.emplace_back(character, stand);
}

void ClearStand() {
    {
        std::scoped_lock guard(g_standLock);
        g_stand.clear();
    }
    {
        std::scoped_lock guard(g_aimLock);
        g_aim.clear();
    }
    std::scoped_lock guard(g_turnLock);
    g_turn.clear();
}

bool StanceTurn(const RE::hkbCharacter* character, float& degrees) {
    std::scoped_lock guard(g_turnLock);
    for (const auto& [c, t] : g_turn)
        if (c == character) {
            degrees = t > 180.0f ? t - 360.0f : t < -180.0f ? t + 360.0f : t;
            return true;
        }
    return false;
}

void SetAim(const RE::hkbCharacter* character, bool on, float yaw, float pitch) {
    std::scoped_lock guard(g_aimLock);
    const auto it = std::find_if(g_aim.begin(), g_aim.end(), [&](const auto& e) { return e.first == character; });
    if (!on) {
        if (it != g_aim.end()) g_aim.erase(it);
    } else if (it != g_aim.end())
        it->second = {yaw, pitch};
    else
        g_aim.emplace_back(character, AimValue{yaw, pitch});
}

bool LayerReady(int layer) {
    return g_ready.load() && layer >= 0 && layer < static_cast<int>(kBodyLayers) &&
           g_layerReady[static_cast<std::size_t>(layer)];
}
float ClipDuration(int action) {
    if (!g_ready.load() || action < 0 || action >= static_cast<int>(g_clipIndex.size())) return 0.0f;
    return g_durations[static_cast<std::size_t>(action)].load(std::memory_order_relaxed);
}
void OnClipActivated(const RE::hkbClipGenerator* clip, float duration) {
    if (!g_ready.load() || !clip || !(duration > 0.0f)) return;
    const char* name = clip->name.c_str();
    const std::size_t n = name ? std::strlen(name) : 0;
    if (n < 8 || _strnicmp(name, "AI_", 3) != 0 ||
        (_stricmp(name + n - 5, "_Part") != 0 && _stricmp(name + n - 6, "_PartB") != 0))
        return;
    if (const auto it = g_clipAction.find(Lower(name)); it != g_clipAction.end())
        g_durations[static_cast<std::size_t>(it->second)].store(duration, std::memory_order_relaxed);
}

void Install() {
    REL::Relocation<std::uintptr_t> blender{RE::VTABLE_hkbBlenderGenerator[0]};
    BlenderGenerate::original = blender.write_vfunc(0x17, BlenderGenerate::thunk);
    logger::info("[UpperLayer] generate hooks installed (stabilizer)");
}

}  // namespace AnimationInjector::UpperLayer

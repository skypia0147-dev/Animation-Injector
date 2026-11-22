#include "PCH.h"
#include "runtime/GripHold.h"
#include "RE/B/BShkbAnimationGraph.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace AnimationInjector::GripHold {
namespace {

using Qs = std::array<float, 12>;  // translation 0..2, rotation 4..7 (x, y, z, w), scale 8..10
constexpr int kPoseTrack = 2;
constexpr float kCaptureTimeout = 2.0f;  // seconds a capture may wait for its graph to generate

enum class Phase : std::uint8_t { kCapture, kTransfer, kHold };

struct Slot {
    const void* graph = nullptr;  // identity only
    RE::FormID actor = 0;
    int hand = 0;  // 0 right (Weapon), 1 left (Shield)
    Phase phase = Phase::kCapture;
    bool target = true;            // a capture of the shown view that only feeds the others is dropped once it has
    bool captured = false;         // kCapture: Apply took the value
    const void* source = nullptr;  // kTransfer: the graph that captures
    bool sourceReady = false;
    Qs value{}, ref{}, sourceValue{}, sourceRef{};
    float age = 0.0f;
    RE::FormID item = 0;    // the hand's equipped object at the hold
    bool notified = false;  // a moved left-hand hold told that graph Adjustment_Additive (see Notify)
};

std::mutex g_lock;
std::vector<Slot> g_slots;
// Left Hand Equipment Overhaul turns its additive mode on per graph, from the graph whose clip sent the annotation.
// A left-hand hold moved to a view that did not play the clip tells that graph itself: Adjustment_Additive when it
// starts, Adjustment_Off when it ends. Sent on the main thread after g_lock is let go (the sink runs other plugins).
struct Notice {
    RE::FormID actor;
    const void* graph;
    bool on;
};
std::vector<Notice> g_notices;
std::atomic<std::size_t> g_count{0};

void Publish() { g_count.store(g_slots.size(), std::memory_order_release); }

// ---- engine access

static_assert(offsetof(RE::BShkbAnimationGraph, characterInstance) == 0xC0);
RE::BShkbAnimationGraph* GraphOf(const RE::hkbContext& a_ctx) {
    if (!a_ctx.character) return nullptr;
    return reinterpret_cast<RE::BShkbAnimationGraph*>(
        reinterpret_cast<std::byte*>(a_ctx.character) - offsetof(RE::BShkbAnimationGraph, characterInstance));
}

bool FirstPerson(const RE::BShkbAnimationGraph* a_graph) {
    const char* name = a_graph->projectName.c_str();
    if (!name) return false;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower.find("1stperson") != std::string::npos || lower.find("firstperson") != std::string::npos;
}

// Putting the weapon away, or away. Not drawing: a weapon equipped while drawn is drawn again (kWantToDraw,
// kDrawing), and that hand's equipment check, not this, releases it (in game 11-21: the other hand was let go too).
bool Sheathing(RE::WEAPON_STATE a_state) {
    return a_state == RE::WEAPON_STATE::kSheathed || a_state == RE::WEAPON_STATE::kWantToSheathe ||
           a_state == RE::WEAPON_STATE::kSheathing;
}

RE::FormID ItemOf(RE::Actor* a_actor, int a_hand) {
    const auto* form = a_actor->GetEquippedObject(a_hand == 1);
    return form ? form->GetFormID() : 0;
}

// Skeleton -> Weapon and Shield bone indices and reference transforms; kept with the bone array's address and size
// and checked on every use (a skeleton freed and another allocated at its address is read again).
struct HandBones {
    std::array<int, 2> index{-1, -1};
    std::array<Qs, 2> ref{};
    const void* bones = nullptr;
    int count = 0;
};
std::shared_mutex g_skeletonLock;
std::unordered_map<const void*, HandBones> g_skeletons;

HandBones BonesOf(const RE::hkbContext& a_ctx) {
    const auto* setup = a_ctx.character ? a_ctx.character->setup.get() : nullptr;
    const auto* skeleton = setup ? setup->animationSkeleton.get() : nullptr;
    if (!skeleton) return {};
    {
        std::shared_lock guard(g_skeletonLock);
        if (const auto it = g_skeletons.find(skeleton); it != g_skeletons.end() &&
                                                        it->second.bones == skeleton->bones._data &&
                                                        it->second.count == skeleton->bones._size)
            return it->second;
    }
    HandBones out;
    out.bones = skeleton->bones._data;
    out.count = skeleton->bones._size;
    for (int i = 0; i < skeleton->bones._size && i < skeleton->referencePose._size; ++i) {
        const char* name = skeleton->bones._data[i].name.c_str();
        if (!name) continue;
        const int hand = _stricmp(name, "Weapon") == 0 ? 0 : _stricmp(name, "Shield") == 0 ? 1 : -1;
        if (hand < 0) continue;
        out.index[hand] = i;
        std::memcpy(out.ref[hand].data(), &skeleton->referencePose._data[i], sizeof(Qs));
    }
    std::unique_lock guard(g_skeletonLock);
    g_skeletons.insert_or_assign(skeleton, out);
    return out;
}

// The bone's transform in a dense pose track (type 1, no flags, stride 48), or nullptr.
float* Bone(PoseTrackReader::Output& a_out, int a_bone) {
    auto* master = const_cast<PoseTrackReader::Master*>(a_out.tracks);
    if (!master || master->bytes < 16 || master->bytes > 1024 * 1024 || master->tracks <= kPoseTrack ||
        master->tracks > 128 || a_bone < 0)
        return nullptr;
    auto* base = reinterpret_cast<std::byte*>(master);
    const auto end = sizeof(PoseTrackReader::Master) + static_cast<std::size_t>(master->tracks) * 16;
    if (end > static_cast<std::size_t>(master->bytes)) return nullptr;
    PoseTrackReader::Header h;
    std::memcpy(&h, base + sizeof(PoseTrackReader::Master) + kPoseTrack * sizeof(h), sizeof(h));
    if (h.type != 1 || h.flags != 0 || h.stride != 48 || a_bone >= h.count || h.offset < 0 ||
        static_cast<std::size_t>(h.offset) + static_cast<std::size_t>(h.capacity) * 48 >
            static_cast<std::size_t>(master->bytes))
        return nullptr;
    return reinterpret_cast<float*>(base + h.offset + a_bone * 48);
}

// ---- math (quaternions x, y, z, w)

std::array<float, 4> Mul(const float* a, const float* b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0], a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
std::array<float, 4> Conj(const float* q) { return {-q[0], -q[1], -q[2], q[3]}; }
bool Normalize(float* q) {
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len > 1e-6f) || !std::isfinite(len)) return false;
    for (int i = 0; i < 4; ++i) q[i] /= len;
    return true;
}
// The value moved from one skeleton's reference to another's, in the parent frame.
Qs Transfer(const Qs& a_value, const Qs& a_from, const Qs& a_to) {
    Qs out = a_value;
    for (int i = 0; i < 3; ++i) out[i] = a_to[i] + (a_value[i] - a_from[i]);
    const auto delta = Mul(a_value.data() + 4, Conj(a_from.data() + 4).data());
    auto q = Mul(delta.data(), a_to.data() + 4);
    if (Normalize(q.data()))
        for (int i = 0; i < 4; ++i) out[4 + i] = q[i];
    return out;
}
// ---- RootBehaviorGraph generate hook

// Node pointer -> is it RootBehaviorGraph; the name's string pointer is kept with it (see BonesOf).
struct NodeInfo {
    const char* name;
    bool root;
};
std::shared_mutex g_nodeLock;
std::unordered_map<const void*, NodeInfo> g_nodes;

bool IsRoot(const RE::hkbNode* a_node) {
    const char* name = a_node->name.c_str();
    {
        std::shared_lock guard(g_nodeLock);
        if (const auto it = g_nodes.find(a_node); it != g_nodes.end() && it->second.name == name)
            return it->second.root;
    }
    const bool root = name && std::strcmp(name, "RootBehaviorGraph") == 0;
    std::unique_lock guard(g_nodeLock);
    g_nodes.insert_or_assign(a_node, NodeInfo{name, root});
    return root;
}

struct MachineGenerate {
    static void thunk(RE::hkbNode* a_node, const RE::hkbContext& a_ctx, const PoseTrackReader::Output** a_children,
        PoseTrackReader::Output& a_out, float a_offset) {
        original(a_node, a_ctx, a_children, a_out, a_offset);
        if (g_count.load(std::memory_order_acquire) && IsRoot(a_node)) Apply(a_ctx, a_out);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};

void Erase(const std::function<bool(const Slot&)>& a_pred, const char* a_why) {
    const auto before = g_slots.size();
    std::vector<Notice> offs;
    for (const auto& s : g_slots)
        if (s.notified && a_pred(s)) offs.push_back({s.actor, s.graph, false});
    std::erase_if(g_slots, a_pred);
    // Left Hand Equipment Overhaul's Adjustment_Off turns every graph of the actor back: only once no left-hand hold
    // of that actor is left (a first-person release would else end a third-person one's additive mode too).
    for (const auto& n : offs)
        if (std::none_of(
                g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.actor == n.actor && s.hand == 1; }))
            g_notices.push_back(n);
    if (g_slots.size() != before) {
        logger::info("[GripHold] {} hold(s) released: {}", before - g_slots.size(), a_why);
        Publish();
    }
}

// Main thread, g_lock not held.
void Notify() {
    std::vector<Notice> notices;
    {
        std::lock_guard guard(g_lock);
        notices.swap(g_notices);
    }
    static const RE::BSFixedString on("Adjustment_Additive"), off("Adjustment_Off");
    for (const auto& n : notices) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(n.actor);
        RE::BSAnimationGraphManagerPtr manager;
        if (!actor || !actor->GetAnimationGraphManager(manager) || !manager) continue;
        for (auto& g : manager->graphs)
            if (g.get() == n.graph) {
                RE::BSAnimationGraphEvent event{n.on ? on : off, actor, RE::BSFixedString{}};
                static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(actor)->ProcessEvent(
                    &event, static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get()));
            }
    }
}

}  // namespace

void Apply(const RE::hkbContext& a_context, PoseTrackReader::Output& a_output, bool) {
    if (!g_count.load(std::memory_order_acquire)) return;
    auto* graph = GraphOf(a_context);
    auto* actor = graph ? graph->holder : nullptr;
    if (!actor) return;
    const auto id = actor->GetFormID();
    std::optional<HandBones> bones;
    std::lock_guard guard(g_lock);
    for (auto& s : g_slots) {
        if (s.graph != graph || s.actor != id) continue;
        if (!bones) bones = BonesOf(a_context);
        float* bone = Bone(a_output, bones->index[s.hand]);
        if (!bone) continue;
        switch (s.phase) {
        case Phase::kCapture:
            // Every write point in this frame takes it again; the last (the top) is the one kept.
            std::memcpy(s.value.data(), bone, sizeof(Qs));
            s.ref = bones->ref[s.hand];
            s.captured = true;
            break;
        case Phase::kTransfer:
            if (!s.sourceReady) break;
            s.value = Transfer(s.sourceValue, s.sourceRef, bones->ref[s.hand]);
            s.phase = Phase::kHold;
            [[fallthrough]];
        case Phase::kHold: std::memcpy(bone, s.value.data(), sizeof(Qs)); break;
        }
    }
}

void Hold(RE::Actor* a_actor, GripHand a_hand, GripView a_view) {
    if (!a_actor) return;
    RE::BSAnimationGraphManagerPtr manager;
    if (!a_actor->GetAnimationGraphManager(manager) || !manager) return;
    const auto active = manager->GetRuntimeData().activeGraph;
    if (active >= manager->graphs.size() || !manager->graphs[active]) return;
    const void* shown = manager->graphs[active].get();
    const auto id = a_actor->GetFormID();
    std::unique_lock guard(g_lock);
    for (int hand = 0; hand < 2; ++hand) {
        if (!Has(a_hand, hand == 0 ? GripHand::kRight : GripHand::kLeft)) continue;
        const auto item = ItemOf(a_actor, hand);
        std::string views;
        for (std::uint32_t i = 0; i < manager->graphs.size(); ++i) {
            auto* g = manager->graphs[i].get();
            if (!g) continue;
            const bool first = FirstPerson(g);
            const bool wanted = Has(a_view, first ? GripView::kFirst : GripView::kThird);
            const bool isShown = g == shown;
            if (!wanted && !isShown) continue;
            std::erase_if(g_slots, [&](const Slot& s) { return s.graph == g && s.actor == id && s.hand == hand; });
            Slot slot;
            slot.graph = g;
            slot.actor = id;
            slot.hand = hand;
            slot.item = item;
            slot.target = wanted;
            slot.phase = isShown ? Phase::kCapture : Phase::kTransfer;
            slot.source = isShown ? nullptr : shown;
            if (hand == 1 && !isShown) {
                slot.notified = true;
                g_notices.push_back({id, g, true});
            }
            g_slots.push_back(slot);
            views += fmt::format("{}{}'{}'{}", views.empty() ? "" : ", ", first ? "first " : "third ",
                g->projectName.c_str() ? g->projectName.c_str() : "", isShown ? " (shown)" : "");
        }
        logger::info("[GripHold] {:08X} {} hand held: {}", id, hand == 0 ? "right" : "left", views);
    }
    Publish();
    guard.unlock();
    Notify();
}

void Release(RE::Actor* a_actor, GripHand a_hand, GripView a_view) {
    if (!a_actor) return;
    RE::BSAnimationGraphManagerPtr manager;
    if (!a_actor->GetAnimationGraphManager(manager) || !manager) return;
    const auto id = a_actor->GetFormID();
    // Every graph and hand at once: Erase decides about Adjustment_Off from what is left after it.
    std::vector<const void*> graphs;
    for (auto& p : manager->graphs)
        if (const auto* g = p.get(); g && Has(a_view, FirstPerson(g) ? GripView::kFirst : GripView::kThird))
            graphs.push_back(g);
    std::unique_lock guard(g_lock);
    Erase(
        [&](const Slot& s) {
            return s.actor == id && Has(a_hand, s.hand == 0 ? GripHand::kRight : GripHand::kLeft) &&
                   std::find(graphs.begin(), graphs.end(), s.graph) != graphs.end();
        },
        "gripRelease");
    Publish();
    guard.unlock();
    Notify();
}

void Update(float a_delta) {
    if (!g_count.load(std::memory_order_acquire)) return;
    std::unique_lock guard(g_lock);
    // Captures taken this frame feed the other views and start holding.
    for (auto& s : g_slots) {
        if (s.phase != Phase::kCapture) continue;
        s.age += a_delta;
        if (!s.captured) continue;
        logger::debug("[GripHold] {:08X} {} captured T({:.2f},{:.2f},{:.2f}) R({:.4f},{:.4f},{:.4f},{:.4f}) ref "
                      "R({:.4f},{:.4f},{:.4f},{:.4f})",
            s.actor, s.hand == 0 ? "Weapon" : "Shield", s.value[0], s.value[1], s.value[2], s.value[4], s.value[5],
            s.value[6], s.value[7], s.ref[4], s.ref[5], s.ref[6], s.ref[7]);
        for (auto& t : g_slots)
            if (t.phase == Phase::kTransfer && t.source == s.graph && t.actor == s.actor && t.hand == s.hand) {
                t.sourceValue = s.value;
                t.sourceRef = s.ref;
                t.sourceReady = true;
            }
        s.phase = Phase::kHold;
    }
    Erase([](const Slot& s) { return s.phase == Phase::kHold && !s.target; }, "the shown view only fed the others");
    Erase([](const Slot& s) { return s.phase == Phase::kCapture && s.age > kCaptureTimeout; },
        "its graph did not generate (no capture)");
    // Automatic releases, per actor and hand.
    std::vector<std::pair<RE::FormID, int>> seen;
    for (const auto& s : g_slots)
        if (std::find(seen.begin(), seen.end(), std::pair{s.actor, s.hand}) == seen.end())
            seen.emplace_back(s.actor, s.hand);
    for (const auto& [id, hand] : seen) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        const char* why = nullptr;
        if (!actor || !actor->Is3DLoaded())
            why = "actor unloaded";
        else if (actor->IsDead())
            why = "death";
        else if (actor->IsInRagdollState())
            why = "ragdoll";
        else if (const auto* state = actor->AsActorState(); state && Sheathing(state->GetWeaponState()))
            why = "weapon sheathed";
        else {
            const auto item = ItemOf(actor, hand);
            for (const auto& s : g_slots)
                if (s.actor == id && s.hand == hand && s.item != item) why = "equipment changed";
        }
        if (why) Erase([&](const Slot& s) { return s.actor == id && s.hand == hand; }, why);
    }
    Publish();
    guard.unlock();
    Notify();
}

void ReleaseAll() {
    std::lock_guard guard(g_lock);
    g_slots.clear();
    g_notices.clear();  // the plugin that listens resets its own state on load
    Publish();
}

void Install() {
    REL::Relocation<std::uintptr_t> machine{RE::VTABLE_hkbStateMachine[0]};
    MachineGenerate::original = machine.write_vfunc(0x17, MachineGenerate::thunk);
    logger::info("[GripHold] RootBehaviorGraph generate hook installed");
}

}  // namespace AnimationInjector::GripHold

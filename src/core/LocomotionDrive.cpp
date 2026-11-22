#include "core/LocomotionDrive.h"
#include "core/NodeFactory.h"
#include "runtime/LadderSpec.h"
#include "runtime/TdmMovement.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

namespace AnimationInjector::LocomotionDrive {
namespace {
std::atomic_bool g_enabled{true};

// Opposite turns (11-21, user report: running left then right, or forward then back, pops; vanilla too). The engine
// turns Direction half a circle in one frame (measured ~300 ms after the key change, at the slowest point, Speed
// 13.6, never 0), so the cyclic blend jumps from one side's clips to the other's: 42 units in one frame for the left
// foot from forward to back (vanilla 30, running 4~6). Every vanilla locomotion tree's BSCyclicBlendTransitionGenerator
// has a cross blend for this (CyclicFreeze clones the tree at its blend value, CyclicCrossBlend blends from the clone
// to the live tree, 0.2 s smooth) that no behavior sends (the engine's code does in a full-speed flip: see the engine's
// own cross blend below). On such a turn: CyclicFreeze this frame with the old direction kept, the new direction and
// CyclicCrossBlend the next, so the clone surely holds the old side.
constexpr float kOppositeTurn = 0.375f;  // of a circle (135 degrees) or more
// A reversal the player makes by hand sweeps instead (11-21, user's reproduction, reversal probe): the engine slows
// down keeping the old direction, then at its slowest (Speed 7~12) turns Direction over 2~3 frames through the front or
// the back (0.798 -> 0.890 -> 0.169 -> 0.216), every step under kOppositeTurn, and the blend swept past the front
// run's clips (feet 44~108 in a frame). So below kSlowTurnSpeed the turn is measured from the anchor, the direction as
// the slow part began (it follows only steps under kAnchorStep, a camera's slow turn); kSlowTurn or more from it is
// a turn.
constexpr float kSlowTurnSpeed = 60.0f;
constexpr float kSlowTurn = 0.25f;    // 90 degrees
constexpr float kAnchorStep = 0.01f;  // per frame
// The sweep's first step still showed (the second session, 11-21: caught, but the frame before the freeze had moved
// 0.09 of a circle, feet 20~50 at the cross blend's start). A step of kSweepStep or more while slow is not shown for
// up to kMaxHold: a turn found meanwhile freezes the pose from before the sweep; otherwise the direction then follows.
// Replaying both recorded sessions: no slow step of kSweepStep or more left outside a cross blend.
constexpr float kSweepStep = 0.04f;  // per frame (the slowing drift: 0.008~0.04)
constexpr float kMaxHold = 0.06f;    // seconds
struct Turn {
    const void* graph;
    float direction;
};
std::mutex g_turnLock;
std::vector<Turn> g_turns;  // graphs frozen last frame, waiting for their new direction
std::atomic_int g_turnCount{0};

float Apart(float a, float b) {
    const float d = std::fmod(std::fabs(a - b), 1.0f);
    return std::min(d, 1.0f - d);
}
bool Opposite(float a, float b) { return Apart(a, b) >= kOppositeTurn; }

// The engine's own cross blend (11-21, forward/back with the weapon sheathed after a draw and sheathe: the engine kept
// the speed at 340 and sent CyclicFreeze, then CyclicCrossBlend 20 ms later, as it turned Direction; the turn below
// then saw the flip and sent its own CyclicFreeze 10 ms after, cutting that blend: feet 29~34 in a frame, 5~11 with
// it left alone). A turn within kEngineTurnWindow of the engine's CyclicFreeze or CyclicCrossBlend is not ours to do.
constexpr std::uint64_t kEngineTurnWindow = 150;  // ms
thread_local bool t_ourSend = false;              // while the turns below notify the graph
std::mutex g_engineLock;
std::vector<std::pair<RE::FormID, std::uint64_t>> g_engineTurns;  // actor, tick of the engine's last cyclic event
bool EngineTurnedLately(RE::FormID actor) {
    std::scoped_lock guard(g_engineLock);
    const auto now = GetTickCount64();
    std::erase_if(g_engineTurns, [&](const auto& e) { return now - e.second > kEngineTurnWindow; });
    return std::any_of(g_engineTurns.begin(), g_engineTurns.end(), [&](const auto& e) { return e.first == actor; });
}
bool Notify(RE::Actor* actor, const RE::BSFixedString& event) {
    t_ourSend = true;
    const bool accepted = actor->NotifyAnimationGraph(event);
    t_ourSend = false;
    return accepted;
}

// Turn events (kTurnRightToLeftEvent and the others): the player only, from the movement input. A way is the input
// pointing right, left, forward or back (past 0.7 that way, under 0.4 across). The event goes when the opposite way is
// pressed within kPivotGap of the last frame the first one was held, if on that frame the player ran (run or sprint,
// not sneaking) at kPivotSpeed or more (kPivotBackSpeed running back) and its Direction was within 45 degrees of the way: right 0.25, left 0.75, back
// 0.5 (a body TDM had turned to face the way it went reads forward), forward 0 only where back is a backward run (not
// TDM's directional setting, in which back turns the body around and reads forward too).
constexpr float kPivotSpeed = 250.0f;      // units/s: running (a test setup runs at 339, walks under about 130)
constexpr float kPivotBackSpeed = 160.0f;  // units/s: running back (measured 200 running back, 80 walking back)
constexpr float kPivotGap = 0.15f;         // seconds between letting go of one side and pressing the other
constexpr float kPivotCooldown = 0.4f;     // seconds
enum Way : int { kNoWay, kRight, kLeft, kForward, kBack };
constexpr Way OppositeWay(Way w) {
    return w == kRight ? kLeft : w == kLeft ? kRight : w == kForward ? kBack : w == kBack ? kForward : kNoWay;
}
struct Pivot {
    Way way = kNoWay;    // the last way held
    float since = 1e9f;  // seconds since it was last held
    bool ready = false;  // on that frame: running, fast, moving that way without facing it
    float cooldown = 0.0f;
};
Pivot g_pivot;  // the player's, on its update
void PivotStep(RE::Actor* player, float delta) {
    static const RE::BSFixedString speedName{"Speed"}, directionName{"Direction"};
    // The event for leaving each way (index: Way), as the name it is logged by.
    static const char* const names[] = {
        nullptr, kTurnRightToLeftEvent, kTurnLeftToRightEvent, kTurnForwardToBackEvent, kTurnBackToForwardEvent};
    static const RE::BSFixedString events[] = {RE::BSFixedString{}, RE::BSFixedString{names[1]},
        RE::BSFixedString{names[2]}, RE::BSFixedString{names[3]}, RE::BSFixedString{names[4]}};
    static constexpr float facing[] = {0.0f, 0.25f, 0.75f, 0.0f, 0.5f};  // the Direction of a run that way
    auto* controls = RE::PlayerControls::GetSingleton();
    if (!controls) return;
    const float x = controls->data.moveInputVec.x, y = controls->data.moveInputVec.y;
    const Way way = std::fabs(y) < 0.4f   ? (x > 0.7f       ? kRight
                                                : x < -0.7f ? kLeft
                                                            : kNoWay)
                    : std::fabs(x) < 0.4f ? (y > 0.7f       ? kForward
                                                : y < -0.7f ? kBack
                                                            : kNoWay)
                                          : kNoWay;
    auto& p = g_pivot;
    p.cooldown = std::max(0.0f, p.cooldown - delta);
    if (way == kNoWay) {
        p.since += delta;
        return;
    }
    if (way == OppositeWay(p.way) && p.since <= kPivotGap && p.ready && p.cooldown <= 0.0f) {
        const bool accepted = player->NotifyAnimationGraph(events[p.way]);
        logger::info("[Locomotion] turn event {} (graph accepted {})", names[p.way], accepted);
        p.cooldown = kPivotCooldown;
    }
    float speed = 0.0f, direction = -1.0f;
    player->GetGraphVariableFloat(speedName, speed);
    player->GetGraphVariableFloat(directionName, direction);
    const auto* state = player->AsActorState();
    const bool running = state && (state->actorState1.running || state->IsSprinting()) && !player->IsSneaking();
    // Running forward reads forward in TDM's directional setting too, where back turns the body around.
    const bool backRuns =
        way != kForward || TdmMovement::Current(state && state->IsWeaponDrawn()) != TdmMovement::Mode::kDirectional;
    p.ready = running && backRuns && speed >= (way == kBack ? kPivotBackSpeed : kPivotSpeed) && direction >= 0.0f &&
              Apart(direction, facing[way]) <= 0.125f;
    p.way = way;
    p.since = 0.0f;
}
// The direction a graph takes this frame on its way from `held` to `next`. Only the actor's active graph: the events
// reach that one (NotifyAnimationGraph).
// `anchor` and `hold` (the graph's kTurnAnchorVariable and kTurnHoldVariable, null without them) are read and moved
// here.
float TurnStep(RE::Actor* actor, const void* graph, bool active, float held, float next, float speed, float delta,
    float* anchor, float* hold) {
    static const RE::BSFixedString freeze{"CyclicFreeze"};
    static const RE::BSFixedString crossBlend{"CyclicCrossBlend"};
    if (g_turnCount.load(std::memory_order_relaxed) > 0) {
        std::unique_lock guard(g_turnLock);
        const auto it = std::find_if(g_turns.begin(), g_turns.end(), [&](const Turn& t) { return t.graph == graph; });
        if (it != g_turns.end()) {
            g_turns.erase(it);
            g_turnCount.store(static_cast<int>(g_turns.size()), std::memory_order_relaxed);
            guard.unlock();
            if (active) Notify(actor, crossBlend);
            return next;
        }
    }
    bool turn = next != held && Opposite(held, next);
    float shown = next;
    if (anchor && hold) {
        if (speed >= kSlowTurnSpeed) {
            *anchor = next;
            *hold = 0.0f;
        } else {
            const float step = Apart(next, held);
            if (*hold <= 0.0f && step < kAnchorStep) *anchor = next;
            if (Apart(*anchor, next) >= kSlowTurn)
                turn = true;
            else if (step >= kSweepStep && *hold < kMaxHold && active) {
                *hold += delta;
                shown = held;
            } else
                *hold = 0.0f;
        }
    }
    if (turn && EngineTurnedLately(actor->GetFormID())) {
        turn = false;
        shown = next;
    }
    if (!active || !turn) return shown;
    if (anchor) *anchor = next;
    if (hold) *hold = 0.0f;
    // Its answer is not whether the cyclic generator heard it: at full speed the engine answers false (11-21, user's
    // reproduction: 7 reversals at Speed 339 flipped Direction in one frame and the turn was dropped on that answer).
    Notify(actor, freeze);
    std::scoped_lock guard(g_turnLock);
    g_turns.push_back({graph, next});
    g_turnCount.store(static_cast<int>(g_turns.size()), std::memory_order_relaxed);
    return held;
}

using Binding = RE::hkbVariableBindingSet::Binding;
// hkbVariableBindingSet::Binding starts with the member path (an hkStringPtr), as GraphInjector reads it.
const char* BindingPath(const Binding& binding) {
    return reinterpret_cast<const RE::hkStringPtr*>(&binding.bindingPath)->c_str();
}

int VariableIndex(RE::hkbBehaviorGraph* graph, const char* name) {
    auto* data = graph ? graph->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    return strings ? NodeFactory::FindName(strings->variableNames, name) : -1;
}

// `name` in `graph`: its index, added as a float once. -1 when the graph data is not as expected.
int EnsureVariable(RE::hkbBehaviorGraph* graph, const char* name, float initial = 0.0f) {
    auto* data = graph ? graph->data.get() : nullptr;
    if (!data || !data->stringData || !data->variableInitialValues) return -1;
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return -1;
    if (const int existing = NodeFactory::FindName(names, name); existing >= 0)
        return data->variableInfos._data[existing].m_type == RE::hkbVariableInfo::VARIABLE_TYPE_REAL ? existing : -1;
    const int count = names._size;
    const int index = data->AddFloatVariable(RE::hkStringPtr(name), initial);
    if (index != count || names._size != count + 1 || data->variableInfos._size != count + 1) return -1;
    return index;
}

// The node's `member` binding to `from` pointed at `to` (added to the graph once), on a copy of its binding set; other
// users of the original set keep `from`. False (one warning) when the binding is not the expected one.
bool RebindParameter(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender, const char* member, const char* from,
    const char* to, bool warn, float initial = 0.0f) {
    const char* name = NodeFactory::NodeName(blender);
    const int source = VariableIndex(graph, from);
    auto* old = blender ? blender->variableBindingSet.get() : nullptr;
    if (source < 0 || !old || !NodeFactory::SameVTable(old, RE::VTABLE_hkbVariableBindingSet) || !old->bindings._data ||
        old->bindings._size < 1 || old->bindings._size > 8) {
        if (warn) logger::warn("[Locomotion] {}: no binding set as expected; the blender keeps {}", name, from);
        return false;
    }
    int at = -1;
    for (int i = 0; i < old->bindings._size; ++i) {
        const auto& b = old->bindings._data[i];
        const char* path = BindingPath(b);
        if (path && std::strcmp(path, member) == 0 && static_cast<std::int8_t>(b.bindingMode) == 0 &&
            b.variableIndex == source)
            at = i;
    }
    const int variable = at >= 0 ? EnsureVariable(graph, to, initial) : -1;
    if (variable < 0) {
        if (warn)
            logger::warn("[Locomotion] {}: {} is not bound to {} here (or {} could not be added); the blender "
                         "keeps {}",
                name, member, from, to, from);
        return false;
    }
    // A copy of the set with that one binding pointed at the variable (GraphInjector's CopyActivityFlag does the same).
    auto* set = static_cast<RE::hkbVariableBindingSet*>(NodeFactory::AllocZero(sizeof(RE::hkbVariableBindingSet)));
    auto* items = static_cast<Binding*>(NodeFactory::AllocZero(sizeof(Binding) * old->bindings._size));
    std::memcpy(set, old, sizeof(*set));
    NodeFactory::InitRefObj(set, RE::VTABLE_hkbVariableBindingSet);
    for (int i = 0; i < old->bindings._size; ++i) items[i] = old->bindings._data[i];
    items[at].variableIndex = variable;
    NodeFactory::OwnArray(set->bindings, items, old->bindings._size);
    blender->variableBindingSet.reset(set);
    blender->areBindablesCached = false;
    return true;
}

// A rate variable: every clip bound to it plays at Speed / fileSpeed.
struct RateVar {
    RE::BSFixedString name;
    float fileSpeed;
    std::string ownFile;  // lowered animation path of a dedicated file, or empty (shared by a file speed)
    bool known;           // ownFile: its own speed was read (FileSpeedKnown)
};
std::mutex g_rateMutex;
std::vector<RateVar> g_rates;  // g_rateMutex
std::atomic_int g_waiting{0};  // dedicated files whose own speed is not known yet
// What the per-frame and clip-activation readers see: copies published under g_rateMutex whenever g_rates changes
// (graph pass, a dedicated file's speed read), read without the lock. Before 11-20 the actor step held g_rateMutex
// while writing every rate variable through the engine, and every clip activation of every actor took it (and lowered
// the clip's path into a new string) for as long as one dedicated file had not played yet, which with rarely played
// files (blocking, sneaking) was the whole session.
struct RateView {
    RE::BSFixedString name;
    float fileSpeed;
};
std::atomic<std::shared_ptr<const std::vector<RateView>>> g_rateView;
std::atomic<std::shared_ptr<const std::vector<std::string>>> g_pendingFiles;  // dedicated files not known yet
void PublishLocked() {
    auto view = std::make_shared<std::vector<RateView>>();
    auto pending = std::make_shared<std::vector<std::string>>();
    view->reserve(g_rates.size());
    for (const auto& r : g_rates) {
        view->push_back({r.name, r.fileSpeed});
        if (!r.ownFile.empty() && !r.known) pending->push_back(r.ownFile);
    }
    g_rateView.store(std::move(view));
    g_pendingFiles.store(std::move(pending));
}
std::atomic_int g_generation{1};  // steps when a file speed changes: every actor writes again
std::atomic<RE::hkbVariableBindingSet*> g_rateSample{nullptr};
std::atomic<RE::hkbNode*> g_switchSample{nullptr};  // a vanilla selector bound to an int variable
std::atomic_int g_switchBinding{-1};                // that binding's place in its set

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A split blender was bound (Rebind / RebindWalk): until then no actor has a graph to step.
std::atomic_bool g_anySplit{false};

// The graph's rate variables, written when the actor's Speed (or a file speed) changed since they were last written.
// The stamp is the Speed written plus 10000 per file speed generation. A Speed within kRateSlack of the one written
// keeps the rates: an NPC following a path wavers by fractions of a unit from frame to frame, and each write is one
// engine call per rate variable (about 60). The feet are off by at most that share.
void Rates(RE::BShkbAnimationGraph* graph, float speed) {
    static const RE::BSFixedString stampName{kStampVariable};
    constexpr float kRateSlack = 0.005f, kRateSlackMin = 0.5f;
    float old = 0.0f;
    if (!graph->GetGraphVariableFloat(stampName, old)) return;  // no bound clip in this graph
    const float base = 10000.0f * static_cast<float>(g_generation.load(std::memory_order_relaxed));
    if (old >= base && old < base + 10000.0f &&
        std::fabs(speed - (old - base)) <= std::max(kRateSlackMin, kRateSlack * speed))
        return;
    const auto view = g_rateView.load();
    if (!view) return;
    for (const auto& r : *view)
        graph->SetGraphVariableFloat(r.name, std::clamp(speed / r.fileSpeed, kMinRate, kMaxRate));
    graph->SetGraphVariableFloat(stampName, speed + base);
}

// The values step once a frame for each actor, before the engine updates it.
//   NPCs: Actor::Update (vfunc 0xAD) of Character, with the frame's length read from the engine: the engine passes
//   0 seconds to an NPC's update. Until 11-16 the step was skipped on 0 seconds, so no NPC's values ever changed and
//   every NPC played the walk clip at its file's own speed whatever it did (found with the guards of Whiterun:
//   AI_RateStamp stayed 0 beside the player).
//   The player: PlayerCharacter's UpdateAnimation slot (0x7D), which is called.
struct UpdateAnimation {
    static void thunk(RE::Actor* actor, float delta) {
        // Character::Update is called with 0 for its seconds (measured 11-16: 114,000 calls, every one 0), so the
        // frame's own length is read; it is 0 while the game is paused.
        const float seconds = delta > 0.0f ? delta : RE::GetSecondsSinceLastFrame();
        if (actor && seconds > 0.0f) Step(actor, seconds);
        func(actor, delta);
    }
    // Each of the actor's graphs that has the split's variables, the inactive ones too. The actor's own Get/Set reach
    // its active graph only: in first person the player's is the first-person graph, which has none of them, and the
    // third-person graph kept its last values for as long as the view lasted (11-20, measured: AI_Gait read 0 in first
    // person and stayed 1.001 through a first-person run; a set from first person did not reach it). The third-person
    // body still shows there (the shadow, first-person body mods).
    // Speed, Direction and SpeedSampled are the actor's: read from its active graph (the first-person graph has Speed:
    // 365 while running, 11-20), from the stepped graph's own when the active one lacks them.
    static void Step(RE::Actor* actor, float delta) {
        static const RE::BSFixedString gait{kGaitVariable};
        static const RE::BSFixedString speedName{"Speed"};
        static const RE::BSFixedString sampled{"SpeedSampled"};
        static const RE::BSFixedString direction{"Direction"};
        static const RE::BSFixedString heldDirection{kDirectionVariable};
        static const RE::BSFixedString walkOnly{kWalkOnlyVariable};
        static const RE::BSFixedString offName{kOffVariable};
        if (!g_anySplit.load(std::memory_order_relaxed)) return;
        RE::BSAnimationGraphManagerPtr manager;
        if (!actor->GetAnimationGraphManager(manager) || !manager) return;
        const bool enabled = g_enabled.load(std::memory_order_relaxed);
        // The actor's values, read once at the first graph that needs them.
        const auto activeIndex = manager->GetRuntimeData().activeGraph;
        std::uint32_t index = 0;
        bool read = false, hasSpeed = false, hasDirection = false, hasSampled = false;
        float speed = 0.0f, now = 0.0f, actorSampled = 0.0f;
        for (auto& graph : manager->graphs) {
            const bool active = index++ == activeIndex;
            if (!graph) continue;
            float value = 0.0f;
            if (!graph->GetGraphVariableFloat(gait, value)) continue;  // not a graph the split patched
            if (!read) {
                read = true;
                hasSpeed = actor->GetGraphVariableFloat(speedName, speed);
                hasDirection = actor->GetGraphVariableFloat(direction, now);
                hasSampled = actor->GetGraphVariableFloat(sampled, actorSampled);
            }
            float graphSpeed = speed;
            if (!hasSpeed && !graph->GetGraphVariableFloat(speedName, graphSpeed)) continue;
            float axis = actorSampled;  // vanilla's axis, for the split turned off
            if (!hasSampled) graph->GetGraphVariableFloat(sampled, axis);
            float next = value;
            if (enabled) {
                Rates(graph.get(), graphSpeed);
                const auto* state = actor->AsActorState();
                const bool running = state->actorState1.running || state->IsSprinting();
                const float share = (std::clamp(value, kMinAxis, kRunAxis) - kMinAxis) / (kRunAxis - kMinAxis);
                next = kMinAxis + (kRunAxis - kMinAxis) *
                                      Gait::NextShare(share, running, graphSpeed, actor->GetWalkSpeed(), delta);
            } else
                next = axis;
            if (next != value) graph->SetGraphVariableFloat(gait, next);
            // Moving while blocking: the walk clip, or vanilla's axis with the split off.
            float walkValue = 0.0f;
            if (graph->GetGraphVariableFloat(walkOnly, walkValue)) {
                const float walkNext = enabled ? kMinAxis : axis;
                if (walkNext != walkValue) graph->SetGraphVariableFloat(walkOnly, walkNext);
            }
            // The own trees' switches: vanilla's nodes while the split is off.
            int off = 0;
            if (graph->GetGraphVariableInt(offName, off) && off != (enabled ? 0 : 1))
                graph->SetGraphVariableInt(offName, enabled ? 0 : 1);
            // Direction: held while the speed is 0. As a diagonal stop ends the engine turns Direction to the
            // straight side in one frame (front-left 0.875 -> left 0.75, 11-15), switching the direction blend from
            // one ladder to another while the stop's cross-fade still shows it. Moving, it follows Direction at once.
            float held = 0.0f, graphDirection = now;
            if (graph->GetGraphVariableFloat(heldDirection, held) &&
                (hasDirection || graph->GetGraphVariableFloat(direction, graphDirection))) {
                float nextDirection = enabled && graphSpeed <= 0.0f ? held : graphDirection;
                if (enabled) {
                    static const RE::BSFixedString anchorName{kTurnAnchorVariable};
                    static const RE::BSFixedString holdName{kTurnHoldVariable};
                    float anchor = 0.0f, hold = 0.0f;
                    const bool tracked = graph->GetGraphVariableFloat(anchorName, anchor) &&
                                         graph->GetGraphVariableFloat(holdName, hold);
                    const float anchorBefore = anchor, holdBefore = hold;
                    nextDirection = TurnStep(actor, graph.get(), active, held, nextDirection, graphSpeed, delta,
                        tracked ? &anchor : nullptr, tracked ? &hold : nullptr);
                    if (tracked && anchor != anchorBefore) graph->SetGraphVariableFloat(anchorName, anchor);
                    if (tracked && hold != holdBefore) graph->SetGraphVariableFloat(holdName, hold);
                }
                if (nextDirection != held) graph->SetGraphVariableFloat(heldDirection, nextDirection);
            }
        }
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
// The player's own UpdateAnimation slot: the same step, its own original.
struct PlayerUpdateAnimation {
    static void thunk(RE::Actor* actor, float delta) {
        if (actor && delta > 0.0f) {
            if (g_enabled.load(std::memory_order_relaxed)) PivotStep(actor, delta);
            UpdateAnimation::Step(actor, delta);
        }
        func(actor, delta);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
}  // namespace

void SetEnabled(bool on) { g_enabled.store(on); }

void ResetOnLoad() {
    {
        std::scoped_lock guard(g_turnLock);
        g_turns.clear();
        g_turnCount.store(0, std::memory_order_relaxed);
    }
    {
        std::scoped_lock guard(g_engineLock);
        g_engineTurns.clear();
    }
    g_pivot = Pivot{};
}

void NoteCyclicEvent(RE::Actor* actor) {
    if (t_ourSend || !actor) return;
    const auto id = actor->GetFormID();
    std::scoped_lock guard(g_engineLock);
    const auto now = GetTickCount64();
    for (auto& e : g_engineTurns)
        if (e.first == id) {
            e.second = now;
            return;
        }
    g_engineTurns.emplace_back(id, now);
}

bool Rebind(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender) {
    const bool done = RebindParameter(graph, blender, "blendParameter", "SpeedSampled", kGaitVariable, true, kMinAxis);
    if (done) g_anySplit.store(true);
    return done;
}
bool RebindWalk(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender) {
    const bool done =
        RebindParameter(graph, blender, "blendParameter", "SpeedSampled", kWalkOnlyVariable, true, kMinAxis);
    if (done) g_anySplit.store(true);
    return done;
}
bool RebindStance(RE::hkbBehaviorGraph* graph, RE::hkbNode* blender) {
    return RebindParameter(graph, blender, "blendParameter", "SpeedSampled", kStanceVariable, true, 0.0f);
}
bool RebindDirection(RE::hkbBehaviorGraph* graph, RE::hkbNode* cyclic) {
    const bool done = RebindParameter(graph, cyclic, "fBlendParameter", "Direction", kDirectionVariable, false);
    if (done) {
        EnsureVariable(graph, kTurnAnchorVariable, 0.0f);
        EnsureVariable(graph, kTurnHoldVariable, 0.0f);
    }
    return done;
}

bool IsRateBinding(const RE::hkbNode* clip) {
    const auto* set = clip ? clip->variableBindingSet.get() : nullptr;
    if (!set || !NodeFactory::SameVTable(set, RE::VTABLE_hkbVariableBindingSet) || set->bindings._size != 1 ||
        !set->bindings._data || set->indexOfActivationBinding >= 0)
        return false;
    const auto& b = set->bindings._data[0];
    const char* path = BindingPath(b);
    return path && std::strcmp(path, "playbackSpeed") == 0 && static_cast<std::int8_t>(b.bindingMode) == 0;
}
void SetRateSample(RE::hkbVariableBindingSet* sample) { g_rateSample.store(sample); }

void NoteSwitchSample(RE::hkbBehaviorGraph* graph, RE::hkbNode* selector) {
    if (g_switchSample.load() || !selector) return;
    auto* data = graph ? graph->data.get() : nullptr;
    const auto* set = selector->variableBindingSet.get();
    if (!data || !set || !NodeFactory::SameVTable(set, RE::VTABLE_hkbVariableBindingSet) || !set->bindings._data ||
        set->bindings._size < 1 || set->bindings._size > 8)
        return;
    for (int i = 0; i < set->bindings._size; ++i) {
        const auto& b = set->bindings._data[i];
        const char* path = BindingPath(b);
        if (!path || std::strcmp(path, "selectedGeneratorIndex") != 0 || static_cast<std::int8_t>(b.bindingMode) != 0 ||
            b.variableIndex < 0 || b.variableIndex >= data->variableInfos._size ||
            data->variableInfos._data[b.variableIndex].m_type != RE::hkbVariableInfo::VARIABLE_TYPE_INT32)
            continue;
        g_switchBinding.store(i);
        g_switchSample.store(selector);
        return;
    }
}
const RE::hkbNode* SwitchSample() { return g_switchSample.load(); }

bool BindSwitch(RE::hkbBehaviorGraph* graph, RE::hkbNode* selector) {
    const auto* sample = g_switchSample.load();
    const int at = g_switchBinding.load();
    auto* data = graph ? graph->data.get() : nullptr;
    const auto* old = sample ? sample->variableBindingSet.get() : nullptr;
    if (!selector || !old || at < 0 || at >= old->bindings._size || !data || !data->stringData ||
        !data->variableInitialValues)
        return false;
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return false;
    int variable = NodeFactory::FindName(names, kOffVariable);
    if (variable < 0) {
        const int count = names._size;
        variable = data->AddIntVariable(RE::hkStringPtr(kOffVariable), g_enabled.load() ? 0 : 1);
        if (variable != count || names._size != count + 1 || data->variableInfos._size != count + 1) return false;
    } else if (data->variableInfos._data[variable].m_type != RE::hkbVariableInfo::VARIABLE_TYPE_INT32)
        return false;
    auto* set = static_cast<RE::hkbVariableBindingSet*>(NodeFactory::AllocZero(sizeof(RE::hkbVariableBindingSet)));
    auto* item = static_cast<Binding*>(NodeFactory::AllocZero(sizeof(Binding)));
    std::memcpy(set, old, sizeof(*set));
    NodeFactory::InitRefObj(set, RE::VTABLE_hkbVariableBindingSet);
    *item = old->bindings._data[at];
    item->variableIndex = variable;
    NodeFactory::OwnArray(set->bindings, item, 1);
    set->indexOfActivationBinding = -1;
    selector->variableBindingSet.reset(set);
    selector->areBindablesCached = false;
    return true;
}

bool BindRate(RE::hkbBehaviorGraph* graph, RE::hkbNode* clip, float fileSpeed, std::string_view ownFile) {
    auto* sample = g_rateSample.load();
    auto* data = graph ? graph->data.get() : nullptr;
    if (!sample || !clip || fileSpeed < 1.0f || !data || !data->stringData || !data->variableInitialValues)
        return false;
    const auto file = Lower(ownFile);
    std::string name;
    {
        // The variable's name: by file speed, or one per dedicated file (its number in the table).
        std::lock_guard lock(g_rateMutex);
        if (file.empty())
            name = fmt::format("AI_Rate{}", std::lround(fileSpeed * 10.0f));
        else {
            std::size_t index = g_rates.size();
            for (std::size_t i = 0; i < g_rates.size(); ++i)
                if (g_rates[i].ownFile == file) index = i;
            name =
                index < g_rates.size() ? std::string(g_rates[index].name.c_str()) : fmt::format("AI_RateFile{}", index);
        }
    }
    auto& names = data->stringData->variableNames;
    if (names._size != data->variableInfos._size) return false;
    int variable = NodeFactory::FindName(names, name.c_str());
    if (variable < 0) {
        const int count = names._size;
        variable = data->AddFloatVariable(RE::hkStringPtr(name.c_str()), 1.0f);
        if (variable != count || names._size != count + 1) return false;
    }
    if (EnsureVariable(graph, kStampVariable) < 0) return false;
    // A copy of the sample set with its one binding pointed at the variable.
    auto* set = static_cast<RE::hkbVariableBindingSet*>(NodeFactory::AllocZero(sizeof(RE::hkbVariableBindingSet)));
    auto* item = static_cast<Binding*>(NodeFactory::AllocZero(sizeof(Binding)));
    std::memcpy(set, sample, sizeof(*set));
    NodeFactory::InitRefObj(set, RE::VTABLE_hkbVariableBindingSet);
    *item = sample->bindings._data[0];
    item->variableIndex = variable;
    NodeFactory::OwnArray(set->bindings, item, 1);
    clip->variableBindingSet.reset(set);
    clip->areBindablesCached = false;
    std::lock_guard lock(g_rateMutex);
    for (const auto& r : g_rates)
        if (_stricmp(r.name.c_str(), name.c_str()) == 0) return true;
    g_rates.push_back({RE::BSFixedString{name.c_str()},
        file.empty() ? static_cast<float>(std::lround(fileSpeed * 10.0f)) / 10.0f : fileSpeed, file, false});
    if (!file.empty()) ++g_waiting;
    PublishLocked();
    return true;
}

bool WantsFileSpeed(const char* animationPath) {
    if (g_waiting.load(std::memory_order_relaxed) <= 0 || !animationPath) return false;
    const auto pending = g_pendingFiles.load();
    if (!pending) return false;
    for (const auto& file : *pending)
        if (_stricmp(file.c_str(), animationPath) == 0) return true;
    return false;
}

void FileSpeedKnown(const char* animationPath, float speed) {
    if (!animationPath) return;
    const auto file = Lower(animationPath);
    std::lock_guard lock(g_rateMutex);
    bool changed = false;
    for (auto& r : g_rates) {
        if (r.known || r.ownFile != file) continue;
        r.known = true;
        --g_waiting;
        if (speed >= 1.0f) {
            logger::info("[Locomotion] {}: its own ground speed {:.1f} (animmotion; was the vanilla file's {:.1f})",
                animationPath, speed, r.fileSpeed);
            r.fileSpeed = speed;
            changed = true;
        } else
            logger::info("[Locomotion] {}: no animmotion annotation, it keeps the vanilla file's ground speed {:.1f}",
                animationPath, r.fileSpeed);
    }
    // The new speed is published before the generation steps: an actor that sees the new generation writes it.
    PublishLocked();
    if (changed) ++g_generation;
}

void Install() {
    REL::Relocation<std::uintptr_t> character{RE::VTABLE_Character[0]};
    REL::Relocation<std::uintptr_t> player{RE::VTABLE_PlayerCharacter[0]};
    UpdateAnimation::func = character.write_vfunc(0xAD, UpdateAnimation::thunk);
    PlayerUpdateAnimation::func = player.write_vfunc(0x7D, PlayerUpdateAnimation::thunk);
    logger::info("[Locomotion] per-actor hook installed (Character::Update, PlayerCharacter::UpdateAnimation)");
}

}  // namespace AnimationInjector::LocomotionDrive

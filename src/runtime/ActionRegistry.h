#pragma once
#include "runtime/ConditionValues.h"
#include "PCH.h"
#include "runtime/ActionSpec.h"
#include "runtime/TdmMovement.h"

// 로드된 액션 선언과 실행기의 목록. 1층(삽입·훅)과 3층(실행기)이 여기서 서로를 찾는다.
namespace AnimationInjector {

class ActionRunner;

namespace Actions {
// Starts loading the declarations in the background (plugin load). Every function below that reads
// them waits for that load on its first call.
void Load();
// The game data is in (kDataLoaded): keyword checks named by editor ID find their keywords, now or when the
// declarations are published.
void OnDataLoaded();
int Count();
bool HasChance(int action);
int SelectStart(
    RE::Actor* actor, int source, RE::ActorHandle partner = {}, ConditionValues::Request* request = nullptr);
int ResolveFlow(RE::Actor* actor, int action, RE::ActorHandle partner = {}, bool requireAvailable = false,
    ConditionValues::Request* request = nullptr);
// The player's movement direction for a request's conditions, once (as the character moves: runtime/TdmMovement.h).
void CaptureMovement(ConditionValues::Request& request);
// That direction now (the keys, or forward while TDM's directional mode turns the player to them); -1 unloaded.
int MoveDirection();
// TDM's setting for the player now (runtime/TdmMovement.h; the weapon state seen outside our actions).
TdmMovement::Mode TdmMode();
// The held time of the key that started the player's action, once per request (runtime/HeldKeys.h).
void CaptureHeld(ConditionValues::Request& request);
// A pair started by its key or event. When its flow picks the destination, the pair's own conditions
// (Require, view) gate every branch first, as SelectStart does for an action (0.12.239); the destination
// pair checks its own when it is requested.
int SelectPairedStart(
    RE::Actor* actor, int source, RE::ActorHandle partner = {}, ConditionValues::Request* request = nullptr);
RE::ActorHandle CaptureTarget(RE::Actor* actor, int action, RE::ActorHandle partner = {});
ActionSpec& Spec(int a_idx);
// Exact action id -> index, or -1.
int IndexOf(std::string_view id);
// Actions whose start key (keyboard scan code, mouse = 256 + button) or gamepad button is the given
// one, in ascending order.
const std::vector<int>& ActionsForKey(std::uint32_t code);
const std::vector<int>& ActionsForGamepad(std::uint32_t button);
// Actions started by a game control (0.12.292), and whether any start or transition is on one: their input then
// goes through the ordered dispatch hook, which keeps a press from the game when it starts one of ours.
const std::vector<int>& ActionsForControl(GameControl control);
bool UsesGameControls();
// (source action, index into its links) for every link whose destination is this action, in source
// and link order.
const std::vector<std::pair<int, int>>& IncomingLinks(int action);
// Runners exist per actor once their action is used (runtime/RunnerSet.h); a missing runner is an
// idle one that never ran.
ActionRunner& Runner(int a_idx);  // The player's runner, created on first use.
// The actor's runner. create: made if missing, otherwise nullptr when the action never ran for it.
// Main thread only for NPCs: idle NPC entries are removed there, which ends the raw pointer's lifetime.
ActionRunner* RunnerFor(RE::Actor* a_actor, int a_idx, bool a_create);
// Any thread: keeps the runner alive while the caller reads it. Never creates a runner.
std::shared_ptr<ActionRunner> RunnerRef(RE::Actor* a_actor, int a_idx);
// The actor's existing runners in ascending action order: the first after `after` (-1 = from the
// start), or -1. Same thread rule as RunnerFor.
int NextRunner(RE::Actor* a_actor, int a_after, ActionRunner** a_runner);
// The first existing runner of the actor for which f(action, runner) is true, or nullptr.
template<class F>
ActionRunner* FindRunner(RE::Actor* a_actor, F&& f) {
    ActionRunner* runner = nullptr;
    for (int a = NextRunner(a_actor, -1, &runner); a >= 0; a = NextRunner(a_actor, a, &runner))
        if (runner && f(a, *runner)) return runner;
    return nullptr;
}
// Clip hooks report here. On the main thread the runner is updated immediately (including the
// synchronous path inside NotifyAnimationGraph); from a worker thread it is queued for the next update.
// a_moves: the clip's HKX carries AMR motion ("animmotion" annotations).
void ClipActivated(RE::Actor* a_actor, int a_action, int a_variant, float a_duration, bool a_moves);
void ClipDeactivated(RE::Actor* a_actor, int a_action, int a_variant);
void OnAnimEventFrom(RE::Actor* a_actor, const RE::BSFixedString& a_tag);  // 전역 애니 이벤트 훅이 부른다
// Event triggers in two steps around the graph's own handling of an inbound event: Capture
// snapshots what graph handlers may change (the combat target of an event-bound partner, the
// sprint flag the start flow's 'sprinting' test reads), Commit queues the requests for the next
// update. QueueEventTrigger does both at once.
struct CapturedTrigger {
    RE::ActorHandle actor;
    int action = -1;
    RE::ActorHandle target;
    bool sprinting = false;
};
std::vector<CapturedTrigger> CaptureEventTrigger(RE::Actor* actor, const RE::BSFixedString& event);
bool CommitEventTrigger(const std::vector<CapturedTrigger>& captured);
bool QueueEventTrigger(RE::Actor* actor, const RE::BSFixedString& event);
void ResetEventTriggers();
bool CaptureNativeCancel(RE::Actor* actor, const RE::BSFixedString& event);
void QueueNativeReplay(RE::ActorHandle actor, const RE::BSFixedString& event);
// 클립 이름 → (액션, 변형). 우리 클립이 아니면 false
bool FindClip(const char* a_clipName, int& a_actionIdx, int& a_variant);
// 모든 액션의 클립 파일 경로 (LoadClips 훅용)
const std::vector<std::string>& AllClipFiles();
const std::vector<std::string>& AllClipFiles(PairedRig::Kind rig);
// Clip files of the actions that play in first person (view first or both): the first-person project.
const std::vector<std::string>& FirstPersonClipFiles();
// Every graph event name the actions use (entries, slots, windows ...), each once ignoring case.
const std::vector<std::string>& AllEventNames();
// The event registry's names (and "name|value" spellings) alone: every graph of an action project gets
// them, not only the graphs an action is injected into.
const std::vector<std::string>& RegistryEventNames();
bool AnyActive();  // 플레이어 실행기 중 Idle 이 아닌 것
// Main thread (ViewGuard): ends every running player action and pending link through its exit blend.
void CancelPlayerRun(const char* reason);
bool HyperArmorFor(RE::Actor* a_actor);  // 그 액터의 실행기 중 스태거 무시 구간 (IFrame 포함)

void InstallHooks();  // PlayerCharacter::Update, 애니 이벤트 싱크
void BeforeGameLoad();
void OnGameLoad();  // kPostLoadGame / kNewGame
}

}  // namespace AnimationInjector

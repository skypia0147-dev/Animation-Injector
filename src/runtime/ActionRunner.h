#pragma once
#include "PCH.h"
#include "runtime/ActionSpec.h"
#include "runtime/DirectLinkRetirement.h"
#include <atomic>
#include <mutex>

// 액션 실행기 — 액션 하나의 생명주기 (3층).
//   입력 → Request(변형) → 조건 → 진입 이벤트 송신 → 그래프가 우리 상태로 전이 → 클립 재생
//   → 슬롯 이벤트(WinOpen/WinClose/Exit)는 HKX 주석 또는 우리 트리거가 발화 → 끝나면 시작 상태 복귀
//   → 어떤 경로로 나가든 클립 비활성 훅에서 Finish 로 닫는다.
// 외부 상태 정리 (전역 규칙 6-2): bAnimationDriven, 보낸 진입 이벤트(Exit 로 닫음), 쿨다운.
//   캔슬 아웃(공격 등)으로 나가면 bAnimationDriven 은 다음 상태가 쓰므로 즉시 끄지 않고, 그 행동이 끝났을 때(attackState 0) 아직 켜져 있으면 끈다.
namespace AnimationInjector {

class ActionRunner {
public:
    enum class State : std::uint8_t { kIdle, kActive, kExiting, kBlendingOut };

    ActionRunner(int a_actionIdx, RE::ActorHandle a_owner, bool a_isPlayer)
        : _idx(a_actionIdx), _owner(a_owner), _isPlayer(a_isPlayer) {}

    RE::Actor* Owner() const {
        auto p = _owner.get();
        return p ? p.get() : nullptr;
    }  // 규칙 4-3 원칙 2: 핸들로 보관, 쓸 때마다 검증
    bool CaptureNativeCancel(const RE::BSFixedString& event);
    bool Busy() const noexcept {
        return _state != State::kIdle || _cooldown > 0.0f || _postCancelTimer > 0.0f;
    }  // NPC 실행기 보관 여부
    // 외부 진입: 다른 모드가 진입 이벤트(AI_<id>_Enter<tag>)를 직접 보내 우리 클립이 활성됐을 때 실행기가 그 실행을 떠맡는다 (NPC 발화 경로)
    void Adopt(int a_variant);

    bool Request(int origin = -1);  // 입력 싱크가 부른다
    bool RequestPaired(
        RE::ActorHandle boundTarget = {});  // An explicit event target is never replaced by a nearby actor.
    RE::ActorHandle CapturePairedTarget(RE::ActorHandle existing = {}) const;
    // The nearest target in `action`'s pair range and cone of `actor`, without a runner (a target
    // search must not create one per candidate action).
    static RE::ActorHandle PairedTargetFor(RE::Actor* actor, int action);
    // The nearest living, loaded actor of any rig within `range` and the `angle` cone ahead of `actor` (0.12.299: the
    // target of a start choosing among non-pair actions).
    static RE::ActorHandle FrontTargetFor(RE::Actor* actor, float range, float angle);
    bool ValidPairedTarget(RE::ActorHandle target) const;
    bool CanRequestPairLink(RE::ActorHandle target) const;
    bool ExitForLink();
    // Direct ordinary handoff; source slots transfer only on destination activation.
    bool RequestLinked(ActionRunner& source, const RE::BSFixedString& groundEvent, const RE::BSFixedString& airEvent);

    // Validate destination conditions before interrupting the source action.
    bool CanRequestLink() const { return Conditions(Owner(), true); }
    // Recheck both actors after pair cleanup, before either new action starts.
    bool CanRequest() const { return Conditions(Owner()); }
    bool CanStart() const;
    // Main thread (ViewGuard): ends a running action at once through its exit blend; a requested link
    // destination that has not played yet is dropped. true when it was running.
    bool Cancel(const char* why);
    int RequestOrigin() const { return _requestOrigin; }
    void Update(float a_delta);  // 매 프레임 (플레이어)
    void ResetOnLoad();
    void OnAnimEvent(const RE::BSFixedString& a_tag);
    // a_moves: the clip's HKX carries AMR motion ("animmotion" annotations).
    void OnClipActivated(int a_variant, float a_duration, bool a_moves);
    void OnClipDeactivated(int a_variant);
    // 창 입력 (액션 실행 중 창 키가 눌림): step:next 창이면 Next 이벤트를 보낸다 (그래프가 창 밖이면 보관/폐기)
    // 이동 입력 변화 (InputSink): kind 0 = 이동키/스틱, 1 = 스프린트, 2 = 점프. Recovery 가 열려 있으면 이동으로 나간다
    void OnMoveInput(int a_kind);
    void TraceMotionOutput(float time, const RE::NiPoint3& translation);

    bool IFrameActive() const noexcept { return _iframeOn; }
    bool HyperArmorActive() const noexcept { return _hyperOn; }
    State GetState() const noexcept { return _state; }
    bool IsActive() const noexcept { return _state == State::kActive; }
    int Variant() const noexcept { return _variant; }
    float Elapsed() const noexcept { return _elapsed; }
    // True once this run reached its CarryRelease time or event (reset when a run starts). The carry
    // polls it; the runner never touches the carried target.
    bool CarryReleaseDue() const noexcept { return _carryReleaseDue; }
    std::uint64_t PlaybackSerial() const { return _playbackSerial.load(std::memory_order_relaxed); }
    bool BeginNormalExitMovement(std::uint64_t serial);
    const ActionSpec& Spec() const;

private:
    bool RequestImpl(
        const RE::BSFixedString* ground = nullptr, const RE::BSFixedString* air = nullptr, int origin = -1);
    int _requestOrigin = -1;
    void SetupPlayback();
    std::atomic_uint64_t _playbackSerial{0};
    bool _normalExitMovementAccepted = false;
    void UpdateNativeCancel();
    void ClearNativeCapture();
    // CaptureNativeCancel runs on the thread that notifies the graph; this slot is shared with
    // the main thread and guarded by _nativeLock (never held across an engine call).
    std::mutex _nativeLock;
    RE::BSFixedString _nativeReplay;
    int _nativeWindow = -1;
    bool _nativeExiting = false;
    void AbortLinkedRequest();
    int _linkSource = -1, _linkTarget = -1;
    DirectLinkRetirement _linkRetirement;
    float _linkAge = 0.0f;
    bool Conditions(RE::Actor* a_actor, bool a_linkPreflight = false) const;
    void Finish();
    bool InterruptIfUnavailable();
    void ReleaseRotation();
    void PrepareExit(float blend);
    void MaintainRecoveryMovement();
    bool _interrupted = false;
    float _exitTimeout = 6.0f;
    RE::FormID _immunity = 0;
    void SetAnimationDriven(bool a_on, bool a_finalRelease = false);

public:
    // An NPC's move cancel (runtime/NpcMoveCancel.h): a graph event the NPC got, on any thread; true when it is the
    // AI's request that ends the action.
    bool NpcHandoffEvent(const RE::BSFixedString& a_event);

private:
    float SafetyTime() const noexcept;
    void PostCancelWatch(float a_delta);
    enum class AirPhase : std::uint8_t { kBeforeEntry, kAfterEntry, kAfterSetup, kFrame, kFinish, kAdopt };
    void BeginAirPhysics(bool a_inAir);
    void LogAirPhysics(AirPhase a_phase) const;

    int _idx;
    RE::ActorHandle _owner;
    bool _isPlayer = true;
    float _motionTraceNext = 0;
    unsigned _motionTraceCount = 0;
    State _state = State::kIdle;
    int _variant = 0;
    float _elapsed = 0.0f;
    // An NPC's move cancel (runtime/NpcMoveCancel.h): movement handed back to the AI at the move-cancel point, and a
    // request it sent since (set on any thread, acted on in Update).
    bool _npcHandoff = false;
    std::atomic_bool _npcHandoffExit{false};
    float _cooldown = 0.0f;
    bool _idleSettled = false;  // an idle update ran since the runner last left idle (Update skips then)
    bool _exitSent = false;
    float _clipDuration = 0.0f;
    float _exitingTime = 0.0f;    // kExiting 경과 (클립 비활성이 안 오면 강제 Finish)
    bool _animDrivenSet = false;  // Retain ownership through the exit blend for the final release.
    bool _startedInAir = false;
    bool _preserveAirPhysics = false;
    float _airPhysicsAge = 0.0f;
    float _airPhysicsNextSample = 0.0f;
    int _airPhysicsSamples = 0;
    float _postCancelTimer = 0.0f;
    float _postCancelTick = 0.0f;
    // 대체값 추적 (재생마다 초기화): 주석 이벤트를 받았는지. 못 받았는데 대체 시각이 지나면 플러그인이 보낸다
    std::vector<bool> _openSeen, _closeSeen;
    bool _exitSeen = false;
    bool _recoveryOpen = false;  // Recovery 이벤트(또는 대체 시각) 이후
    bool _recoveryMovement = false;
    bool _recoveryAttackCleared = false;
    // 슬롯 상태 (재생마다 초기화, 종료 경로 전부에서 복원)
    bool _rotationLockOwned = false, _rotationBeforeLock = false;
    bool _rotateOn = false, _costDone = false;
    std::vector<bool> _timedFired;  // per clip-time effect (Spec().pairFeatures.timed), 0.12.274
    bool _carryReleaseDue = false;  // per run, kept after Finish for the carry to read
    void ReachCarryRelease(const char* why);
    // Read by the notify hook on other threads (HyperArmorFor).
    std::atomic_bool _iframeOn{false}, _hyperOn{false};
    struct SlotState {
        bool onSeen = false, offSeen = false;
    };
    SlotState _slot[3];  // rotate, iframe, hyperArmor
    void ResetPlaybackFlags();
    void RecoveryClock();
    void SlotUpdate();  // 시각 기반 슬롯 적용
    void SlotEvent(const RE::BSFixedString& a_tag);
    void ApplyRotate(bool a_on);
    void ApplyIFrame(bool a_on);  // 액터 베이스 kIsGhost|kInvulnerable (공격이 통과·피해 없음) + 피해 훅
    void ApplyCost();
    void RunTimedEffect(const EventFunctionSpec& function);
    void ApplyAttackData();    // 공격 의미: 이 액션의 종족 공격 데이터를 attackData 에 넣는다
    void RestoreAttackData();  // 우리가 넣은 것이 아직 있으면 이전 값으로
    RE::NiPointer<RE::BGSAttackData> _attackSet, _attackPrev;  // 우리가 넣은 것 / 넣기 전 값
    void RestoreSlots();                                       // 종료: 전부 원상복구
    void OpenRecovery(const char* a_why);
    void Recover(const char* a_why);
    std::unordered_set<std::string> _seenTags;  // 디버그: 이번 실행 중 받은 이벤트 이름
    // The event names every animation event is compared with, interned once per runner (main thread): the
    // comparison used to hash each name again for every event (code review 2026-09-30 R-19).
    struct EventTags {
        std::array<RE::BSFixedString, 3> slotOn, slotOff;  // rotate, iframe, hyperArmor
        RE::BSFixedString cost, carryRelease, exit;
        std::vector<RE::BSFixedString> open, close;  // by window
        std::vector<RE::BSFixedString> recovery;     // by variant
    };
    std::unique_ptr<EventTags> _tags;
    const EventTags& Tags();
};

}  // namespace AnimationInjector

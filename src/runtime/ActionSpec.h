#pragma once
#include "PCH.h"
#include "runtime/QteSpec.h"
#include "runtime/GameControls.h"
#include "runtime/PressKinds.h"
#include <optional>
#include "runtime/PairedRig.h"
#include "runtime/CarrySpec.h"
#include "runtime/PairFeatureSpec.h"
#include "runtime/ViewSpec.h"
#include "declarations/ConditionFlowXml.h"

// 액션 선언 — 2층(선언 컴파일러)의 산출물이자 1층(그래프 기본기)·3층(액션 실행기)의 입력.
// 지금은 폴더 INI(DirectDodge.ini)에서 만들고, M1 에서 XML 이 같은 구조를 만든다. (계획서 §4)
//
// 이름 규칙 (충돌 방지): 클립 "AI_<id>_Clip<tag>", 진입 이벤트 "AI_<id>_Enter<tag>", 그 외 슬롯 "AI_<id>_<슬롯>".
// 슬롯 이벤트 이름은 XML <Events> 로 덮어쓸 수 있다 — HKX 주석에 그 이름을 넣으면 그 시각에 발화한다.
namespace AnimationInjector {

// The player's movement input direction (InputSink::CurrentMoveDir): recovery and exit blends read it. Actions
// no longer pick clips by it (0.12.231): a clip moves only by its HKX's AMR annotations.
enum class MoveDir : std::uint8_t { kForward = 0, kBack = 1, kLeft = 2, kRight = 3, kNone = 4 };
constexpr int kMoveDirCount = 4;

// 변형 = 상태 하나. 일반 액션은 클립 1개, 페어는 같은 HKX 의 두 역할(Lead·Support). 콤보(Step 사슬)는 형식 4에서
// 없어졌다: 이어지는 동작은 액션마다 하나씩 두고 Connect 로 잇는다.
struct RecoverySpec {
    bool enabled = false;
    std::string event;                               // HKX 주석 이벤트 이름 (기본 AI_<id>_Recovery)
    float at = -1.0f;                                // 클립 시작 기준 초. <0 = 이벤트를 기다림
    bool move = true, sprint = false, jump = false;  // 어떤 입력을 이동 입력으로 볼지
    float blend = -1.0f;                             // 이동으로 나갈 때 블렌드 (<0 = Exit 효과)
    std::string recoverEvent;                        // 내부 전이 이벤트 AI_<id>_Recover (Finalize)
};

struct ClipVariant {
    std::string tag;   // "" (일반 액션) / "Lead"·"Support" (페어) — 이름 접미
    std::string file;  // meshes\actors\character\ 기준 HKX 경로 ("Animations\…")
    int syncRole = 0;  // 페어: 0 = 일반 클립, 1 = 시작 액터(lead, prefix "2_"), 2 = 대상 액터(support, prefix "")
    // 아래는 Finalize 가 채운다
    std::string clipName;    // "AI_Dodge_Clip"
    std::string enterEvent;  // "AI_Dodge_Enter"
    // Entry in the master state machine while ground behavior is inactive.
    std::string airEnterEvent;
};

// 바닐라 동작으로의 전환 창 (<Cancel to="vanilla:…">): 열림/닫힘 이벤트 동안 시작 상태의 전이를 상속한다(캔슬 아웃).
// <Window>를 생략하면 클립 전체(mode 0).
struct CancelSpec {
    std::string nativeCancelEvent;
    std::string id;
    std::string openEvent, closeEvent;        // 이벤트 이름 (Finalize 가 기본값 채움). 시각 지정이면 트리거도 만든다
    float openAt = 0.0f;                      // <0 = HKX 주석이 공급
    float closeAt = 0.0f;                     // 클립 끝에서 뺀 초. <0 = HKX 주석이 공급
    int mode = 2;                             // 0 항상 / 1 창 안에서만 / 2 창 + 선입력
    std::vector<std::string> vanillaMatches;  // 상속할 시작 상태 전이의 이벤트 이름 부분 문자열
    float blend = -1.0f;                      // 이 창으로 나갈 때의 블렌드 길이(초). <0 = 기존 효과 재사용
};

struct SlotEvents {    // 기능 슬롯 ↔ 이벤트 이름 (기본값은 Finalize 에서 채움)
    std::string exit;  // 시작 상태 복귀
};

// 이름 있는 입력: <Inputs><Input id="attack" key="G" gamepad="0x1000"/></Inputs> → <Trigger input="attack"/>, <Window input="attack"/>
struct InputDef {
    std::string id;
    std::uint32_t key = 0;                     // DXScanCode (마우스는 +256). 0 = 없음
    std::uint32_t gamepad = 0;                 // 패드 버튼 마스크. 0 = 없음
    GameControl control = GameControl::kNone;  // a game control instead of a key (0.12.292)
};

struct TriggerSpec {                                     // 액션 시작 입력
    std::string inputId;                                 // <Inputs> 참조 (있으면 Finalize 가 key/gamepad 를 채운다)
    std::uint32_t key = 0x38;                            // DXScanCode (마우스는 +256)
    std::uint32_t gamepad = 0;                           // 패드 버튼 마스크 (0 = 없음)
    GameControl control = GameControl::kNone;            // a game control instead of a key (0.12.292)
    PressKinds::Press press = PressKinds::Press::kDown;  // how the key or control is pressed (10-30)
    // 이벤트 발화 (NPC/플레이어 공통): 액터의 그래프가 이 이벤트를 쏘면 시작. actor = "player" | "npc" | "both"
    std::string onEvent;
    bool onEventPlayer = false, onEventNpc = true;
    // The target its conditions read (0.12.299): the sender's combat target (an event start's is captured when the
    // event fires), or for a start choosing among non-pair actions the nearest actor ahead within range and angle.
    bool combatTarget = false;
    bool frontTarget = false;
    float targetRange = 200.0f, targetAngle = 90.0f;
};

// Recovery 슬롯 (계획서 §4-3): 이 이벤트(또는 시각) 이후에는 이동 입력만으로 액션을 끊고 이동으로 나간다.
//   이미 누르고 있던 이동키도 인정(MCO 와 다른 점). 다른 창은 그대로 살아 있다.

// 켜기/끄기 슬롯 창 (Rotate·IFrame·HyperArmor): on/off 각각 이벤트 또는 "@시각". 실행기가 처리(그래프 변경 없음).
//   시각(@)은 실행기 타이머로 적용, 이벤트는 HKX 주석이 쏠 때만(0.12.278: 대체값 없음). off 는 "@end-초" 도 가능.
struct SlotWindow {
    bool enabled = false;
    std::string onEvent, offEvent;  // 기본 AI_<id>_<Slot>On/Off (Finalize)
    float onAt = -1.0f;             // 클립 시작 기준 초 (<0 = 이벤트)
    float offAt = -1.0f;            // offRel=false: 시작 기준 초 / offRel=true: 끝에서 뺀 초
    bool offRel = false;
};

// 공격 의미 (V8): 종족(BGSAttackDataMap)의 공격 데이터를 우리 상태에 붙인다.
//   엔진(피해 배율·스태거·attackType)과 Precision(무기 노드 충돌 정의)이 attackData 로 "어떤 공격인지" 판단한다.
//   data = 종족 공격 데이터의 이벤트 이름 (attackStart, attackPowerStartForward, bashStart …).
struct AttackSpec {
    bool enabled = false;
    std::string data;  // 기본 attackStart
};

// A combined paired clip is injected as native lead/support synchronized states.
// SetupSpecialIdle starts the pair using a runtime idle and validated actor handles.
struct PairedSpec {
    PairedRig::Kind targetRig = PairedRig::Kind::kCharacter;
    bool enabled = false;
    // Opt-in survivor exit blend on partner death/ragdoll; normal exits are unchanged.
    float lossBlend = -1.0f;
    std::string lossExitEvent;
    float range = 150.0f;  // 대상 탐색 거리
    float angle = 60.0f;   // 전방 원뿔 전체 각(도)
    float getToMarkTime = 0.0f, markErrorThreshold = 0.1f;
    bool reorientSupport = true, applyMotionFromRoot = false;
};

// The putdown Action's moment to release the carried target (seconds or annotation event).
struct CarryReleaseSpec {
    bool enabled = false;
    std::string event;  // annotation event name; default AI_<id>_CarryRelease
    float at = -1.0f;   // seconds from the clip start (the runner's timer)
};

struct CostSpec {  // 자원 소모. at 생략 = 시작할 때
    bool enabled = false;
    std::string event;  // 기본 AI_<id>_Cost
    float at = -1.0f;   // "@초" 면 실행기 타이머
    float stamina = 0.0f, magicka = 0.0f;
};

// 시작 제한 (0.12.276, docs/mockup-start-limits): the action's own nature, checked on every way into it (a key,
// an event, a transition, a QTE result). State checks (weapon drawn, attacking, stamina) are a condition branch's
// tests (Flow <Test kind="weaponDrawn|attackState|stamina">): one place asks each question.
struct ConditionSpec {
    int inAir = 0;          // 공중(점프/낙하) 중 시작: 0 = 거부(기본), 1 = 허용, 2 = 공중에서만
    float cooldown = 0.0f;  // 끝난 뒤 다시 시작할 수 있을 때까지(초)
};

// Charge (10-30): <Action loop="true" maxHold="3">. The clip plays over and over (hkbClipGenerator looping) and the
// action has no end: it leaves by a transition, a move cancel or a vanilla transition. At maxHold seconds the key
// counts as released (an NPC has none and holds until then); without a release transition the action exits.
// maxHold counts from the press of the key that started the charge while it is still down (a moving charge goes
// from loop to loop by on="move" and keeps one clock), else from this action's own start.
struct LoopSpec {
    bool enabled = false;
    float maxHold = 3.0f;
};

struct SuppressSpec {
    enum class When { kStartable, kActive, kBoth };
    When when = When::kStartable;
    std::vector<std::string> events;
};

struct ActionSpec {
    bool rotationLocked = false;
    SuppressSpec suppress;
    std::string id;  // "Dodge"
    ViewSpec view;   // the camera view it plays in (runtime/ViewSpec.h); pairs and carries: third
    ConditionFlowXml::Program flow;
    std::vector<std::string> flowActions;
    // Ordinary actions join the root's FullBodyPlayerIdles machine in both views; pairs the root (0.12.232).
    std::vector<ClipVariant> clips;  // 일반 1개, 페어 2개(Lead·Support)
    SlotEvents events;
    std::vector<CancelSpec> cancels;
    std::vector<InputDef> inputs;
    RecoverySpec recovery;
    bool HasRecovery() const { return recovery.enabled; }
    SlotWindow rotate;      // bAllowRotation (기본: 액션 중 false)
    SlotWindow iframe;      // 체력 피해 무시 + 스태거 무시
    SlotWindow hyperArmor;  // 스태거 무시
    CostSpec cost;
    CarryReleaseSpec carryRelease;
    std::vector<QteSpec> qtes;
    AttackSpec attack;  // 공격 의미 (attackData)
    PairedSpec paired;  // 페어 (V10)
    CarrySpec carry;
    PairFeatureSpec pairFeatures;
    std::vector<EventValuesSpec> eventValues;  // <EventValues>: numbers for registered events (0.12.281)
    std::vector<ActionLinkSpec> links;
    TriggerSpec trigger;
    ConditionSpec condition;
    LoopSpec loop;
    float safetyDuration = 3.0f;         // 클립 길이를 모를 때의 보험 타이머 (페어 기본은 파서가 30)
    float exitTriggerBeforeEnd = 0.05f;  // 클립 끝 몇 초 전에 exit 발화. <0 = 트리거를 만들지 않음(HKX 주석이 공급)
    // Charge (10-30): an on="end" transition takes the Exit point (seconds before the clip's end) from the exit
    // trigger, which is then not made: the runner calls ActionLinks::ReachedEnd there. <0 = none.
    float endLinkAt = -1.0f;
    float enterBlend = -1.0f;  // 진입 블렌드(초). <0 = 기존 효과 재사용
    float exitBlend = -1.0f;   // 퇴장 블렌드(초). <0 = 기존 효과 재사용
    std::string source;        // 선언 출처 (파일 경로)

    // 이름 기본값 채우기 (선언 로드 직후 한 번)
    void Finalize() {
        for (std::size_t i = 0; i < links.size(); ++i) {
            links[i].enterEvent = "AI_" + id + "_Link" + std::to_string(i);
            links[i].airEnterEvent = links[i].enterEvent + "Air";
        }
        if (paired.enabled && paired.lossBlend > 0.0f) paired.lossExitEvent = "AI_" + id + "_PartnerLossExit";
        // 파일 하나 → 변형 2개. Skyrim.esm 실측: IDLE 'pa_HugA' 의 ENAM = "pa_HugA"(시작 액터가 받음, 상태 lead=false prefix ""),
        //   대상은 "pa_" 를 뗀 "HugA"(lead=true prefix "2_") 를 받는다. 킬무브도 같음(공격자 ENAM pa_KillMove…, 피해자 KillMove…). 즉 lead = 대상.
        if (paired.enabled && !clips.empty()) {
            ClipVariant lead = clips[0], sup = clips[0];
            lead.tag = "Lead";
            lead.syncRole = 1;  // 대상 액터: prefix "2_", 이벤트 AI_<id>_EnterLead
            sup.tag = "Support";
            sup.syncRole = 2;  // 시작 액터: prefix "", 이벤트 pa_AI_<id>_EnterLead
            clips = {lead, sup};
        }
        for (auto& c : clips) {
            c.clipName = "AI_" + id + "_Clip" + c.tag;
            c.enterEvent = "AI_" + id + "_Enter" + c.tag;
            c.airEnterEvent = "AI_" + id + "_AirEnter" + c.tag;
        }
        if (paired.enabled && clips.size() == 2) clips[1].enterEvent = "pa_" + clips[0].enterEvent;
        if (events.exit.empty()) events.exit = "AI_" + id + "_Exit";
        if (recovery.event.empty()) recovery.event = "AI_" + id + "_Recovery";
        recovery.recoverEvent = "AI_" + id + "_Recover";
        auto slotNames = [&](SlotWindow& w, const char* a_slot) {
            if (w.onEvent.empty()) w.onEvent = "AI_" + id + "_" + a_slot + "On";
            if (w.offEvent.empty()) w.offEvent = "AI_" + id + "_" + a_slot + "Off";
        };
        slotNames(rotate, "Rotate");
        slotNames(iframe, "IFrame");
        slotNames(hyperArmor, "HyperArmor");
        if (cost.event.empty()) cost.event = "AI_" + id + "_Cost";
        if (carryRelease.enabled && carryRelease.at < 0 && carryRelease.event.empty())
            carryRelease.event = "AI_" + id + "_CarryRelease";
        auto findInput = [&](const std::string& a_id) -> const InputDef* {
            for (const auto& in : inputs)
                if (_stricmp(in.id.c_str(), a_id.c_str()) == 0) return &in;
            return nullptr;
        };
        if (!trigger.inputId.empty())
            if (const auto* in = findInput(trigger.inputId)) {
                trigger.key = in->key;
                trigger.gamepad = in->gamepad;
                trigger.control = in->control;
            }
        int k = 0;
        for (auto& w : cancels) {
            if (w.id.empty()) w.id = "cancel" + std::to_string(k + 1);
            w.nativeCancelEvent = "AI_" + id + "_" + w.id + "NativeCancel";
            if (w.openEvent.empty()) w.openEvent = "AI_" + id + "_" + w.id + "Open";
            if (w.closeEvent.empty()) w.closeEvent = "AI_" + id + "_" + w.id + "Close";
            ++k;
        }
    }
    int VariantByClipName(const char* a_name) const {
        if (!a_name) return -1;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i)
            if (_stricmp(clips[i].clipName.c_str(), a_name) == 0) return i;
        return -1;
    }
    // A selecting action (0.12.244): no clip, its Flow sends every route to another action. It is never injected;
    // its start gates and Flow are all it has, and the action the Flow picks is checked when it starts.
    bool SelectsOnly() const { return clips.empty() && !flow.nodes.empty(); }
};

}  // namespace AnimationInjector

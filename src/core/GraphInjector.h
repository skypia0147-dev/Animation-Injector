#pragma once
#include "PCH.h"
#include "core/HavokLayout.h"
#include "runtime/ActionSpec.h"

// 런타임 그래프 삽입 (1층) — Nemesis/Pandora 가 오프라인에서 하는 일을 로드 시점에 한다.
//
// 훅 1: CreateSymbolIdMap (REL 62640/63585 +0x22A/0x237). 인자로 프로젝트의 모든 hkbBehaviorGraph 가 온다.
//        eventIDMap 이 아직 null 인 첫 호출에서 (1) 모든 그래프에 이벤트 등록 (2) 대상 루트 SM 마다 액션 상태 삽입.
// 훅 2: LoadClips (REL 63028/63846 +0x8D/0x101, OAR 과 같은 자리). 캐릭터 파일(defaultmale/female/1인칭)마다 오므로
//        **원본 전에** 애니 이름을 그 캐릭터의 animationNames 에 넣는다 — 한 곳에만 넣으면 다른 성별은 T포즈.
//
// 액션 하나가 그래프 하나에 넣는 것:
//   StateInfo ×변형수  ├ generator = hkbClipGenerator (트리거: Exit@끝−δ, WinOpen@t, WinClose@끝)
//                      └ transitions = [Exit → 시작 상태] + (캔슬 아웃) 시작 상태 전이 상속분 (이벤트 기반 구간, 우선순위 = 진입과 동일)
//   와일드카드 배열에 [진입 이벤트 → 상태] ×변형수 (LOCAL_WILDCARD|DISABLE_CONDITION, priority 5000 → 공격 중첩 상태에서도 채택)
//
// 자가검증 (하나라도 실패하면 그 그래프는 건드리지 않고 로그만):
//   (a) 루트가 hkbStateMachine, startStateId·states 수가 Atlas 값  (b) 시작 상태 이름 = Atlas (StateInfo 배치)
//   (c) 와일드카드[0] 의 flags 에 LOCAL_WILDCARD|DISABLE_CONDITION 비트, 이벤트 이름 = Atlas (TransitionInfo 배치·enum 값)
namespace AnimationInjector::GraphInjector {

struct Result {
    int actionIdx = -1;
    std::string graph;  // "RootBehaviorGraph", "FullBodyPlayerIdles_Behavior"
    bool injected = false;
    bool airRoute = false;
    PairedRig::Kind pairedRig = PairedRig::Kind::kUnsupported;
    std::string reason;
    RE::hkbBehaviorGraph* graphPtr = nullptr;  // 템플릿 그래프 (이벤트 id → 이름)
    std::vector<std::int32_t> enterIds;        // 변형별 진입 이벤트 (그래프 로컬, 진입 불가 변형은 -1)
    std::int32_t exitId = -1;
    std::int32_t recoverId = -1;         // Recovery: 이동으로 나가는 전이 이벤트 (없으면 -1)
    std::vector<std::int32_t> stateIds;  // 삽입한 상태 id
    int cancelOutCount = 0;
};

// Paired template metadata for diagnostics. Binding creation is engine-owned.
// rawIndex is retained for source compatibility; PairB never uses it to patch a live node.
struct SyncNode {
    RE::BSSynchronizedClipGenerator* gen;
    RE::hkbClipGenerator* clip;
    std::string file;
    int actionIdx;
    int variant;
    int rawIndex = -1;
    bool firstPerson = false;
    PairedRig::Kind rig = PairedRig::Kind::kCharacter;
};
// Loader threads keep appending while the game runs; readers get a copy or a query answer,
// never a reference into the live tables.
std::vector<SyncNode> SyncNodes();

void Install();
// True when stateId is one of this action's injected states in the named machine. A pair in a shared
// container (core/PairEntry.h) has its role's container there, which every pair of that role shares.
bool IsActionState(int action, const char* graph, std::int32_t stateId);
bool AnyInjected(int a_actionIdx);
// Both project routes must exist before a cross-species request can start.
bool HasPairedRoutes(int action);
bool IsNativeTransitionEvent(const char* event);
bool HasFullBodyRoute(int action);
// True only after an airborne master-graph entry was successfully injected.
bool HasAirRoute(int a_actionIdx);
bool IsOurStateId(const char* a_smName, std::int32_t a_stateId);  // 프로브용
const char* EventName(const char* a_smName, std::int32_t a_id);   // 프로브용. 모르면 nullptr

}  // namespace AnimationInjector::GraphInjector

#pragma once
#include "PCH.h"

// hkbClipGenerator Activate(vfunc 4) / Deactivate(vfunc 7) 훅 (1층).
//   Activate 직전: 우리 클립이면 animationBindingIndex 를 이 캐릭터의 animationNames 에서 다시 찾는다
//                  (캐릭터 파일마다 배열이 다르고 OAR 이 뒤에 항목을 넣는다 — 인덱스는 활성 직전에만 믿을 수 있다)
//   Activate 뒤:  실행기에 (변형, 애니 길이) 통지. ★이 시점엔 바인딩이 아직 없을 수 있다(비동기 로드) → 길이 0 이면 모름
//   Deactivate 뒤: 실행기에 통지 — 어떤 경로로 상태를 떠났든 여기서 닫힌다
namespace AnimationInjector::ClipHooks {
RE::Actor* ActorForContext(const RE::hkbContext& context);
void Install();
}

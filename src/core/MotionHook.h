#pragma once
#include "PCH.h"

// 루트 모션 (1층). Character::ProcessMotionData (REL 31949/32703) 안의 ProcessTranslationData/RotationData 호출부 4곳에 체인.
//   엔진은 animdata 레지스트리에 등록된 클립(AnimDataRegistry)에 대해서만 이 경로를 부른다. 이름 = 클립 이름.
//   페어 클립이면 페어 모션(PairFeatures)을 돌려주고, 아니면 이전 대상(AMR → 바닐라)으로 넘긴다: 일반 액션의 클립은
//   HKX 의 AMR 주석(animmotion/animrotation)으로만 움직인다(0.12.231).
//   AMR 은 처리 후 원본 함수를 직접 불러 체인을 타지 않으므로 우리는 반드시 AMR 보다 바깥쪽(kDataLoaded 설치)이어야 한다.
namespace AnimationInjector::MotionHook {
void Install();
}

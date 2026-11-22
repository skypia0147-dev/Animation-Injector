#pragma once
#include "PCH.h"

// 런타임 animationdatasinglefile 등록 (1층).
// 엔진은 루트 모션을 AnimationClipDataSingleton(프로젝트 → 클립 이름 → ClipData → bound_datas[모션 블록 0x28])에서 찾고,
// **등록되지 않은 클립은 ProcessMotionData 경로 자체를 부르지 않는다.** Nemesis/Pandora 가 animationdatasinglefile.txt 에
// 클립 블록+모션 블록 쌍을 쓰는 일을 로드된 구조체에 직접 한다. 배치는 실측(원소 0x28, 앞 0x20 = AMR MotionDataContainer, 0x20 = duration).
// 등록만으로 엔진이 캐릭터 컨트롤러로 이동시킨다(벽 충돌 유지). MotionHook 은 그 위에서 곡선을 실시간으로 덮어쓴다.
namespace AnimationInjector::AnimData {

// 모든 액션의 클립을 DefaultMale/DefaultFemale 에 등록. 멱등. 성공(또는 이미 됨) true
bool TryRegister(const char* a_reason);
bool Registered();
// 클립 활성 때 실제 애니 길이를 모션 블록 duration 에 기록
void SetClipDuration(const char* a_clipName, float a_seconds);

// A copy of a vanilla clip under a new name (core/LocomotionSplit): in every character project the copy gets the source's
// motion block (the same bound data index, so the same root motion), its playback field × rate, and its annotations.
// A source the project does not have gives no copy there (logged once).
struct ClipCopy {
    std::string source;
    std::string copy;
    float rate = 1.0f;
    // false: the copy gets the motion but none of the source's annotations (a clip whose own file carries them:
    // core/OwnLocomotion's clips).
    bool annotations = true;
};
// Queues the copies (one per copy name; a later request for a name already queued is ignored) and registers them in
// the character projects loaded now; TryRegister registers them in projects that load later. Any thread.
void QueueCopies(std::vector<ClipCopy> a_copies);
// The clip's ground speed in the first character project that has it: its motion block's last translation key over
// its duration (the file's own root travel). 0 when the clip is not registered or has no motion block.
float ClipSpeed(const char* a_clip);

}  // namespace AnimationInjector::AnimData

#pragma once
#include "PCH.h"

// NotifyAnimationGraph(이름) 은 BShkbAnimationGraph::projectDBData 의 이름→id 맵(unkA0)에서 id 를 찾는다.
// The project DB predates injected graph symbols. Event names are extended here;
// the opt-in chicken profile also registers its two new activity variables after
// validating the native variable map against unchanged graph variable indices.
// 키는 BSFixedString 의 인턴 포인터 (포인터 해시). unkD0(id → 이름) 도 채운다.
namespace AnimationInjector::EventMaps {

// a_names 를 액터의 활성 그래프 프로젝트 DB 에 등록 (프로젝트 DB 마다 한 번 — DefaultMale/DefaultFemale 은 서로 다른 DB).
// A DB handled before is checked again on every call: an address reused by another project DB registers anew.
void Ensure(RE::Actor* a_actor, const std::vector<std::string>& a_names);
// 주기 호출: 로드된 모든 액터의 프로젝트 DB 를 훑어 등록 (NPC 가 외부 진입 이벤트를 받기 전에 이름이 있어야 한다)
void EnsureAllLoaded(const std::vector<std::string>& a_names);

}  // namespace AnimationInjector::EventMaps

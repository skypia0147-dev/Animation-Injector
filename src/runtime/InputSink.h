#pragma once
#include "PCH.h"
#include "runtime/ActionSpec.h"
#include <array>
#include <vector>

// 입력 → 액션 트리거 (3층). 이동키(userEvent 이름)·왼쪽 스틱을 추적해 방향 변형을 고른다.
namespace AnimationInjector {

class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
public:
    static InputSink* Get();
    static void Register();  // kDataLoaded
    RE::BSEventNotifyControl ProcessEvent(
        RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override;
    // 현재 이동 입력 방향 (없으면 a_default). Recovery 슬롯 등 "지금 이동키가 눌려 있나" 판정에도 쓴다
    MoveDir CurrentMoveDir(MoveDir a_default) const;
    bool AnyMoveInput() const;
    // The button events of the last batch to keep from the game (0.12.292): a press on a game control that started
    // one of our actions or that a transition took, and later that press's release. The ordered dispatch hook
    // (InputDispatchHook) drops them before PlayerControls, so the jump does not also happen.
    bool Taken(const RE::InputEvent* event) const;
    bool AnyTaken() const { return !_taken.empty(); }
    void Reset() {
        _batchMove = false;
        _fwd = _back = _left = _right = false;
        _stickX = _stickY = 0.0f;
        _swallowRelease = {};
    }

private:
    bool _batchMove = false;
    float _batchX = 0, _batchY = 0;
    bool _fwd = false, _back = false, _left = false, _right = false;
    float _stickX = 0.0f, _stickY = 0.0f;
    std::vector<const RE::InputEvent*> _taken;
    std::array<bool, kGameControlCount> _swallowRelease{};  // per control: its press was kept from the game
};

}  // namespace AnimationInjector

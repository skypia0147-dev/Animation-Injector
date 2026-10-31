#pragma once
#include "PCH.h"
#include "runtime/ActionSpec.h"
#include "runtime/ConditionValues.h"
#include "runtime/PressKinds.h"
#include "runtime/HeldKeys.h"
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
    // The movement input as a vector (x right, y forward; keys or the left stick, as CurrentMoveDir reads them).
    // False when there is none.
    bool MoveVector(float& x, float& y) const;
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
        _pressed.clear();
        _now = 0.0f;
        HeldKeys::Get().Reset();
    }
    // The sink's game clock (seconds of the player's updates): press kinds and held times read it.
    float Now() const { return _now; }
    // Press kinds of starts (runtime/PressKinds.h): game time passes (the player's update; not while paused), a wait
    // may end, and a press found to be the game's goes out.
    void Tick(float delta);
    void SetPressTiming(PressKinds::Timing timing) { _timing = timing; }

private:
    using Source = HeldKeys::Source;  // key: scan code (mouse +256); control: GameControl
    // One input that waits to tell how it is pressed.
    struct Pressed {
        Source source = Source::kKey;
        std::uint32_t code = 0;
        PressKinds::Classifier classifier;
        ConditionValues::Request request;  // the conditions of its first press (movement direction)
        RE::INPUT_DEVICE device = RE::INPUT_DEVICE::kKeyboard;
        std::uint32_t idCode = 0;
        RE::BSFixedString userEvent;
        bool down = false;                   // the key now
        float downAt = 0.0f;                 // when its first press went down (the held time counts from it)
        bool send = false, sendTap = false;  // a late press for the game, sent on the next tick
    };
    static const std::vector<int>& ActionsOf(Source source, std::uint32_t code);
    // Starts the actions of the list pressed this way that can start (dry: only whether one could). True when one
    // started (could).
    static bool Start(
        const std::vector<int>& list, PressKinds::Press press, ConditionValues::Request* request, bool dry);
    Pressed* Find(Source source, std::uint32_t code);
    // A button of this input went down or up. True: keep this press from the game.
    bool OnPress(const RE::ButtonEvent& event, Source source, std::uint32_t code);
    void Settle(Pressed& pressed);
    void Act(Pressed& pressed, PressKinds::Outcome outcome);
    void ToGame(Pressed& pressed, bool tap);
    void DropWaits();

    bool _batchMove = false;
    float _batchX = 0, _batchY = 0;
    bool _fwd = false, _back = false, _left = false, _right = false;
    float _stickX = 0.0f, _stickY = 0.0f;
    std::vector<const RE::InputEvent*> _taken;
    std::array<bool, kGameControlCount> _swallowRelease{};  // per control: its press was kept from the game
    std::vector<Pressed> _pressed;
    PressKinds::Timing _timing;
    float _now = 0.0f;
};

}  // namespace AnimationInjector

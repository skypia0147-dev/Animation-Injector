#pragma once
#include "PCH.h"
#include <memory>

namespace AnimationInjector::NativePairAlignment {
// Borrow the scene only across the native Start call. Persistent state contains
// actor handles, NiPointers and copied values, never a borrowed Havok pointer.
class Start {
public:
    explicit Start(RE::BGSSynchronizedAnimationInstance* scene);
    ~Start();
    void Complete(bool accepted);
    // Observe the engine's existing GetMark result during this Start call only.
    void RecordNativeMark(RE::BSSynchronizedClipGenerator* generator, RE::hkbCharacter* character, bool accepted,
        const RE::hkQsTransform& mark);
    // Every start keeps the victim (the scene's lead) where it stands and only turns it; the other role
    // (the attacker, player or not) takes the whole move. Asked at the engine's native placement
    // (TryMoveTo), which moves the attacker to the victim's current facing:
    //   kRedirected = `position` now holds the attacker's goal; the engine places and checks it there.
    //   kSkipped    = the engine would move the victim; skip that move.
    //   kBlocked    = reject the start (the attacker's path to its goal is obstructed).
    //   kNative     = let the engine place as it would (no usable plan yet).
    enum class Move { kNative, kRedirected, kSkipped, kBlocked };
    Move NativeMove(std::uint64_t request, const RE::bhkCharacterController* moving, RE::hkVector4& position);
    Start(const Start&) = delete;
    Start& operator=(const Start&) = delete;

private:
    struct State;
    std::unique_ptr<State> state;
};
void Observe(const RE::BSSynchronizedClipGenerator& node, const RE::hkbContext& context, bool exiting);
void BeforeUpdate();
void Update(float delta);
void Reset(bool restore);
// The native activation barrier consumes cached start transforms again.
void InstallActivationHook();
}

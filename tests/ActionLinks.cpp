#include "LinkHarness.h"
#include "runtime/DirectLinkRetirement.h"
#include "core/PairRootRuntime.h"
#include "runtime/HeldKeys.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace AnimationInjector;

void Setup(std::string partner = "B", bool sourcePaired = true) {
    Actions::requestSeeds.clear();
    Actions::flowSelection = -2;
    PairRootRuntime::busy = false;
    ActionLinks::ResetOnLoad();
    specs = {};
    runners = {};
    PairedSession::status = {};
    PairedSession::cancellations = 0;
    PairedSession::handoffs = PairedSession::completions = 0;
    PairedSession::allowHandoff = true;
    PairedSession::actorStarted = PairedSession::partnerStarted = false;
    for (unsigned actor = 0; actor < RE::actors.size(); ++actor) {
        RE::actors[actor] = {};
        RE::actors[actor].id = actor;
        for (int action = 0; action < 4; ++action) runners[actor][action].index = action;
    }
    specs[0].id = "Pair";
    specs[0].paired.enabled = sourcePaired;
    specs[1].id = "A";
    specs[2].id = "B";
    specs[3].id = "OtherPair";
    specs[3].paired.enabled = true;
    ActionLinkSpec link;
    link.action = "A";
    link.partnerAction = partner;
    link.key = 9;
    specs[0].links = {link};
}
void Press() {
    ActionLinks::Begin({1}, 0, specs[0].paired.enabled, true);
    ActionLinks::Input(9, false);
    ActionLinks::Update(0.01f);
}
void Release() {
    PairedSession::status.phase = PairedSession::Phase::kIdle;
    ActionLinks::End({1}, 0);
    ActionLinks::Update(0.21f);
}
int main() {
    Setup("", false);
    specs[0].links[0].action = "OtherPair";
    Press();
    assert(runners[1][3].starts == 1 && runners[1][3].pairedTarget == RE::ActorHandle{2});
    Setup("", false);
    specs[0].links[0].action = "OtherPair";
    runners[1][3].pairTargetValid = false;
    Press();
    assert(runners[1][0].exits == 0 && runners[1][3].starts == 0);
    Setup("", false);
    Actions::flowSelection = -1;
    Press();
    assert(runners[1][0].exits == 0 && runners[1][1].starts == 0 && PairedSession::cancellations == 0);
    Setup("", false);
    Actions::flowSelection = 2;
    Press();
    assert(runners[1][1].starts == 0 && runners[1][2].starts == 1);
    Setup();
    specs[0].links[0].event = "PartnerBranch";
    ActionLinks::Begin({1}, 0, true, true, {2});
    ActionLinks::ObserveEvent({3}, RE::BSFixedString("PartnerBranch"));
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 0);
    ActionLinks::ObserveEvent({2}, RE::BSFixedString("PartnerBranch"));
    ActionLinks::ObserveEvent({1}, RE::BSFixedString("PartnerBranch"));
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 1 && runners[2][2].starts == 1);
    ActionLinks::ObserveEvent({2}, RE::BSFixedString("PartnerBranch"));
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 1);
    DirectLinkRetirement early;
    assert(!early.Deactivate());  // Old clip ended: wait before releasing source slots.
    assert(early.Commit());       // New clip activated: source may now retire.
    DirectLinkRetirement late;
    assert(!late.Commit());     // New clip activated: do not reuse the old runner yet.
    assert(late.Deactivate());  // Late callback retires the old runner, not the destination.
    early = {};
    late = {};
    assert(!early.committed && !early.deactivated && !late.committed && !late.deactivated);
    Setup();
    Press();
    assert(PairedSession::cancellations == 1 && runners[1][1].starts == 1 && runners[2][2].starts == 1);
    assert(PairedSession::status.phase == PairedSession::Phase::kStopping);
    assert(PairedSession::handoffs == 1 && PairedSession::completions == 1);
    assert(!ActionLinks::HasPendingTransition({1}));  // Immediate handoff has no pending gap.
    assert(PairedSession::actorStarted && PairedSession::partnerStarted);
    Release();
    assert(runners[1][1].starts == 1 && runners[2][2].starts == 1 && runners[3][2].starts == 0);

    Setup();
    runners[2][2].preflight = false;
    Press();
    assert(PairedSession::cancellations == 0);  // Invalid partner leaves the pair running.
    runners[2][2].preflight = true;
    ActionLinks::Input(9, false);
    ActionLinks::Update(0.01f);
    Release();
    assert(runners[2][2].starts == 1);  // The window can accept a later valid input.

    Setup();
    runners[2][2].condition = false;
    Press();
    assert(runners[1][1].starts == 0 && runners[2][2].starts == 0);
    assert(PairedSession::completions == 1 && !PairedSession::actorStarted && !PairedSession::partnerStarted);
    Setup();
    RE::actors[2].exists = false;
    Press();
    assert(runners[1][1].starts == 0 && runners[3][2].starts == 0);  // Never retarget a nearby actor.
    Setup();
    RE::actors[2].ragdoll = true;
    Press();
    assert(runners[1][1].starts == 0);
    Setup();
    PairedSession::allowHandoff = false;
    Press();
    assert(runners[1][1].starts == 0);
    Setup();
    ActionLinks::Begin({1}, 0, true, true);
    ActionLinks::Input(9, false);
    ActionLinks::ResetOnLoad();
    Release();
    assert(runners[1][1].starts == 0);

    Setup();
    runners[2][2].accept = false;
    Press();
    Release();
    assert(runners[1][1].starts == 0);
    Setup();
    runners[1][1].accept = false;
    Press();
    Release();
    assert(runners[2][2].starts == 1 && runners[2][2].exits == 1);
    assert(PairedSession::completions == 1 && !PairedSession::actorStarted && PairedSession::partnerStarted);
    Setup("A");
    Press();
    Release();
    assert(runners[1][1].starts == 1 && runners[2][1].starts == 1);
    Setup("");
    Press();
    Release();
    assert(runners[1][1].starts == 1 && runners[2][2].starts == 0);
    Setup("Missing");
    Press();
    assert(PairedSession::cancellations == 0);
    Setup("OtherPair");
    Press();
    assert(PairedSession::cancellations == 0);
    Setup("B", false);
    Press();
    assert(runners[1][0].exits == 0);
    Setup("", false);
    Press();
    assert(runners[1][0].exits == 1 && runners[1][1].starts == 1);
    Setup("", false);
    specs[0].links[0].action = "OtherPair";
    Press();
    assert(runners[1][3].starts == 1);
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    Release();
    assert(runners[1][3].starts == 1);
    // Paired destinations retain their existing cleanup wait and cancellation guards.
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    assert(ActionLinks::HasPendingTransition({1}) && ActionLinks::HasPendingTransition({2}));
    assert(!ActionLinks::HasPendingTransition({3}));
    ActionLinks::Update(8.1f);
    assert(!ActionLinks::HasPendingTransition({1}) && !ActionLinks::HasPendingTransition({2}));
    assert(runners[1][3].starts == 0);
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    ++PairedSession::status.ticket.request;
    Release();
    assert(runners[1][3].starts == 0);
    assert(!ActionLinks::HasPendingTransition({1}));
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    ActionLinks::ResetOnLoad();
    Release();
    assert(runners[1][3].starts == 0);
    assert(!ActionLinks::HasPendingTransition({1}));
    Setup();
    PairedSession::status.initiator.actor = {2};
    PairedSession::status.target.actor = {1};
    specs[0].links[0].event = "Branch";
    ActionLinks::Begin({2}, 0, true, false);
    ActionLinks::ObserveEvent({2}, RE::BSFixedString("Branch"));
    ActionLinks::Update(0.01f);
    PairedSession::status.phase = PairedSession::Phase::kIdle;
    ActionLinks::End({2}, 0);
    ActionLinks::Update(0.21f);
    assert(runners[2][1].starts == 1 && runners[1][2].starts == 1);  // Roles are independent of player identity.
    // 10-29: an event a running action has a transition on is that transition's, as a key is (an event start on it
    // is ignored), for the actor playing it and its pair partner, until the action ends; a key transition claims none.
    Setup("", false);
    specs[0].links[0].key = 0;
    specs[0].links[0].event = "blockHitStart";
    assert(!ActionLinks::ClaimsEvent({1}, RE::BSFixedString("blockHitStart")));
    ActionLinks::Begin({1}, 0, false, true);
    assert(ActionLinks::ClaimsEvent({1}, RE::BSFixedString("blockHitStart")));
    assert(!ActionLinks::ClaimsEvent({1}, RE::BSFixedString("other")));
    assert(!ActionLinks::ClaimsEvent({2}, RE::BSFixedString("blockHitStart")));
    assert(!ActionLinks::ClaimsEvent({1}, RE::BSFixedString("")));
    ActionLinks::End({1}, 0);
    assert(!ActionLinks::ClaimsEvent({1}, RE::BSFixedString("blockHitStart")));
    Setup();
    specs[0].links[0].event = "PartnerBranch";
    ActionLinks::Begin({1}, 0, true, true, {2});
    assert(ActionLinks::ClaimsEvent({2}, RE::BSFixedString("PartnerBranch")));
    Setup("", false);
    ActionLinks::Begin({1}, 0, false, true);
    assert(!ActionLinks::ClaimsEvent({1}, RE::BSFixedString("blockHitStart")));
    // The actual dispatcher bypasses ExitForLink for eligible ordinary nodes.
    Setup("", false);
    specs[0].clips = {0};
    specs[1].clips = {0};
    Press();
    assert(runners[1][0].exits == 0 && runners[1][1].starts == 1);
    assert(!ActionLinks::HasPendingTransition({1}));
    Setup("", false);
    specs[0].clips = {0};
    specs[1].clips = {0};
    runners[1][1].preflight = false;
    Press();
    assert(runners[1][0].exits == 0 && runners[1][1].starts == 0);
    Setup("", false);
    specs[0].clips = {0};
    specs[1].clips = {0};
    auto& timed = specs[0].links[0];
    timed.openAt = .5f;
    timed.closeAt = .1f;
    timed.closeRelativeToEnd = true;
    ActionLinks::Begin({1}, 0, false, true);
    ActionLinks::Input(9, false);
    ActionLinks::ObserveTime({1}, 0, .3f, 2);
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 0);
    ActionLinks::ObserveTime({1}, 0, .6f, 2);
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 1);
    // Rapid G presses remain owned by stage 2 after stage 1 retires.
    Setup("", false);
    for (int i = 0; i < 3; ++i) {
        specs[i].clips = {0};
        ActionLinkSpec link;
        link.action = specs[(i + 1) % 3].id;
        link.key = 9;
        link.openAt = .45f;
        specs[i].links = {link};
    }
    ActionLinks::Begin({1}, 0, false, true);
    for (int cycle = 0; cycle < 12; ++cycle) {
        const int stage = cycle % 3, next = (stage + 1) % 3;
        const int before = runners[1][next].starts;
        for (int press = 0; press < 20; ++press) {
            assert(ActionLinks::Input(9, false).claimed);
            assert(ActionLinks::Input(9, false, GameControl::kNone, false).claimed);
            ActionLinks::ObserveTime({1}, stage, .01f * press, 2);
            ActionLinks::Update(.01f);
            assert(runners[1][next].starts == before);
        }
        ActionLinks::ObserveTime({1}, stage, .5f, 2);
        ActionLinks::Update(.01f);
        assert(runners[1][next].starts == before + 1);
        // Model the production activation/retirement callbacks.
        ActionLinks::Begin({1}, next, false, true);
        ActionLinks::End({1}, stage);
        assert(ActionLinks::Input(9, false, GameControl::kNone, false).claimed);
        assert(!ActionLinks::Input(10, false).claimed);  // Unrelated dodge key remains available.
        assert(!ActionLinks::Input(9, true).claimed);    // Device namespaces remain distinct.
    }
    ActionLinks::End({1}, 0);
    assert(!ActionLinks::Input(9, false).claimed);

    // Non-buffered windows still reserve the key outside their acceptance range.
    Setup("", false);
    specs[0].clips = {0};
    specs[1].clips = {0};
    specs[0].links[0].openAt = .5f;
    specs[0].links[0].closeAt = 1;
    specs[0].links[0].buffered = false;
    ActionLinks::Begin({1}, 0, false, true);
    assert(ActionLinks::Input(9, false).claimed);
    ActionLinks::ObserveTime({1}, 0, .6f, 2);
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 0);  // Early press was claimed but not buffered.
    assert(ActionLinks::Input(9, false, GameControl::kNone, false).claimed);
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 0);  // Key-up does not trigger the link.
    ActionLinks::ObserveTime({1}, 0, 1.1f, 2);
    ActionLinks::Update(.01f);
    assert(ActionLinks::Input(9, false).claimed);
    ActionLinks::Update(.01f);
    assert(runners[1][1].starts == 0);
    ActionLinks::ResetOnLoad();
    assert(!ActionLinks::Input(9, false).claimed);

    // Pending pair cleanup retains ownership even after the source entry ends.
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    ActionLinks::End({1}, 0);
    assert(ActionLinks::Input(9, false).claimed);
    assert(ActionLinks::Input(9, false, GameControl::kNone, false).claimed);
    assert(!ActionLinks::Input(10, false).claimed);
    ActionLinks::Update(8.1f);
    assert(!ActionLinks::Input(9, false).claimed);
    Setup("", false);
    ActionLinks::Begin({2}, 0, false, false);
    assert(!ActionLinks::Input(9, false).claimed);  // NPC links do not own player input.
    Setup("", false);
    specs[0].links[0].gamepad = 8;
    ActionLinks::Begin({1}, 0, false, true);
    assert(ActionLinks::Input(8, true, GameControl::kNone, false).claimed);
    assert(!ActionLinks::Input(8, false).claimed);
    assert(!ActionLinks::Input(0, false).claimed);
    // A game control (0.12.292): any device's press of that control, and only that control, is the transition's.
    Setup("", false);
    specs[0].links[0].key = 0;
    specs[0].links[0].control = GameControl::kJump;
    ActionLinks::Begin({1}, 0, false, true);
    {
        const auto keyboard = ActionLinks::Input(57, false, GameControl::kJump);
        assert(keyboard.claimed && keyboard.taken);
    }
    assert(ActionLinks::Input(32768, true, GameControl::kJump).taken);
    assert(!ActionLinks::Input(57, false).claimed);                        // The same key without the control.
    assert(!ActionLinks::Input(42, false, GameControl::kSprint).claimed);  // Another control.
    Setup("");
    specs[0].links[0].action = "OtherPair";
    PairRootRuntime::busy = true;
    Press();
    Release();
    assert(runners[1][3].starts == 0 && ActionLinks::HasPendingTransition({1}));
    PairRootRuntime::busy = false;
    ActionLinks::Update(.01f);
    assert(runners[1][3].starts == 1 && !ActionLinks::HasPendingTransition({1}));
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    Release();
    assert(Actions::requestSeeds.size() == 2 && Actions::requestSeeds[0] != 0 &&
           Actions::requestSeeds[0] == Actions::requestSeeds[1]);
    const auto previousSeed = Actions::requestSeeds[0];
    Setup("");
    specs[0].links[0].action = "OtherPair";
    Press();
    Release();
    assert(Actions::requestSeeds.size() == 2 && Actions::requestSeeds[0] != previousSeed &&
           Actions::requestSeeds[0] == Actions::requestSeeds[1]);
    // An event QTE's result in a pair (0.12.264): the pair hands both over from either role.
    Setup();
    ActionLinks::Begin({1}, 0, true, true);
    assert(ActionLinks::RequestHandoff({1}, 1, 2));
    ActionLinks::Update(0.01f);
    assert(runners[1][1].starts == 1 && runners[2][2].starts == 1 && PairedSession::handoffs == 1);
    Setup();
    ActionLinks::Begin({1}, 0, true, false);  // the NPC leads, the player is the victim
    assert(ActionLinks::RequestHandoff({2}, 1, 2));
    ActionLinks::Update(0.01f);
    assert(runners[2][1].starts == 1 && runners[1][2].starts == 1);
    Setup();
    ActionLinks::Begin({1}, 0, true, false);
    assert(ActionLinks::RequestHandoff({2}, -1, 1));  // only the partner named: it leaves, the player is released
    ActionLinks::Update(0.01f);
    assert(runners[1][1].starts == 1 && runners[2][1].starts == 0);
    Setup();
    ActionLinks::Begin({1}, 0, true, true);
    PairedSession::status.phase = PairedSession::Phase::kIdle;
    assert(!ActionLinks::RequestHandoff({1}, 1, 2));  // no pair: the caller starts the player's action itself
    Setup();
    assert(!ActionLinks::RequestHandoff({3}, 1, 2));  // not in this pair
    Setup();  // a pair without connections (no link entry): the session names its action (0.12.265)
    specs[0].links.clear();
    ActionLinks::Begin({1}, 0, true, false);
    assert(ActionLinks::RequestHandoff({2}, 1, 2));
    ActionLinks::Update(0.01f);
    assert(runners[2][1].starts == 1 && runners[1][2].starts == 1);
    Setup();
    ActionLinks::Begin({1}, 0, true, true);
    assert(ActionLinks::RequestHandoff({1}, 1, 2));
    ActionLinks::ResetOnLoad();
    ActionLinks::Update(0.01f);
    assert(runners[1][1].starts == 0);  // a load drops a request
    // Charge (10-30). on="end" goes at the end; on="release" once its key is up, a state: let go before the window
    // opens, it goes as it opens. An NPC holds until a forced release (a loop's maxHold). An end or forced release
    // whose destination cannot start ends its source; a player's release that cannot start leaves it playing.
    const auto charge = [](ActionLinkSpec link) {
        Setup("", false);
        HeldKeys::Get().Reset();
        specs[0].clips = {0};
        specs[1].clips = {0};
        link.action = "A";
        specs[0].links = {link};
    };
    using HeldSource = HeldKeys::Source;
    const HeldKeys::Key g{HeldSource::kKey, 34};
    {
        ActionLinkSpec end;
        end.onEnd = true;
        charge(end);
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0);
        assert(ActionLinks::ReachedEnd({1}, 0));
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 1 && runners[1][0].exits == 0);
        Setup("", false);
        ActionLinks::Begin({1}, 0, false, true);
        assert(!ActionLinks::ReachedEnd({1}, 0) && !ActionLinks::ForceRelease({1}, 0));  // a key transition only
    }
    {
        ActionLinkSpec release;
        release.onRelease = true;
        release.openAt = 1.0f;
        charge(release);
        HeldKeys::Get().Press(g, 0);
        HeldKeys::Get().SetChain(g, 0);
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::ObserveTime({1}, 0, 0.5f);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0);  // held
        HeldKeys::Get().Release(g, 0.5f);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0);  // let go before the window opens
        ActionLinks::ObserveTime({1}, 0, 1.0f);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 1);  // it goes as the window opens
    }
    {
        ActionLinkSpec release;
        release.onRelease = true;
        release.key = 34;  // its own key, not the one that started the action
        charge(release);
        HeldKeys::Get().Press(g, 0);
        ActionLinks::Begin({1}, 0, false, true);
        assert(!ActionLinks::Input(34, false).claimed);  // a release transition takes no press
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0);
        HeldKeys::Get().Release(g, 0.3f);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 1);
    }
    {
        ActionLinkSpec release;
        release.onRelease = true;
        charge(release);
        ActionLinks::Begin({1}, 0, false, false);  // an NPC: no key
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0);
        assert(ActionLinks::ForceRelease({1}, 0));
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 1);
    }
    {
        ActionLinkSpec end;
        end.onEnd = true;
        charge(end);
        runners[1][1].preflight = false;
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::ReachedEnd({1}, 0);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0 && runners[1][0].exits == 1);  // the source ends as an exit would
        ActionLinkSpec release;
        release.onRelease = true;
        charge(release);  // no key holds: the player's release goes at once
        runners[1][1].preflight = false;
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::Update(0.01f);
        assert(runners[1][1].starts == 0 && runners[1][0].exits == 0);  // a release that cannot go: plays on
        charge(release);
        runners[1][1].preflight = false;
        ActionLinks::Begin({1}, 0, false, false);
        ActionLinks::ForceRelease({1}, 0);
        ActionLinks::Update(0.01f);
        assert(runners[1][0].exits == 1);  // a forced one (a loop's maxHold): the source ends
    }
    // on="move" (10-31): a movement key's press as a key transition takes its key's, buffered before the window; a key
    // held as the window opens counts as buffered. The card reads the direction held, else the one pressed. An NPC's
    // never goes.
    {
        constexpr int kNone = 4;
        ActionLinkSpec move;
        move.onMove = true;
        move.openAt = 0.5f;
        // In a charge: the key that started the action (g) is down.
        const auto begin = [&](ActionLinkSpec link, int direction) {
            charge(link);
            HeldKeys::Get().Press(g, 0.0f);
            HeldKeys::Get().SetChain(g, 0.0f);
            HeldKeys::Get().SetMoveDirection(direction);
            ActionLinks::Begin({1}, 0, false, true);
            ActionLinks::ObserveTime({1}, 0, 0.1f, 1.0f);
            ActionLinks::Update(0.01f);
        };
        const auto at = [](float time, int direction) {
            HeldKeys::Get().SetMoveDirection(direction);
            ActionLinks::ObserveTime({1}, 0, time, 1.0f);
            ActionLinks::Update(0.01f);
        };
        // The short press that started it is up before the window: no second one.
        begin(move, kNone);
        assert(!ActionLinks::Input(9, false).claimed);  // no key of its own
        at(0.6f, kNone);
        assert(runners[1][1].starts == 0);
        // A tap before the window (buffered) goes as it opens, with the tapped direction for the card.
        begin(move, kNone);
        at(0.2f, 2);
        at(0.3f, kNone);
        at(0.4f, kNone);
        assert(runners[1][1].starts == 0);  // not open yet
        at(0.6f, kNone);
        assert(runners[1][1].starts == 1 && Actions::resolvedMovement == 2);
        // Held through the window's opening (the key that started it, still down): goes, reading it.
        begin(move, 3);
        at(0.4f, 3);
        assert(runners[1][1].starts == 0);
        at(0.6f, 3);
        assert(runners[1][1].starts == 1 && Actions::resolvedMovement == 3);
        // A press inside the open window goes at once, reading the direction held.
        begin(move, kNone);
        at(0.6f, kNone);
        assert(runners[1][1].starts == 0);
        at(0.7f, 0);
        assert(runners[1][1].starts == 1 && Actions::resolvedMovement == 0);
        // Without the buffer: a key held as the window opens does not go, a fresh press in it does.
        auto unbuffered = move;
        unbuffered.buffered = false;
        begin(unbuffered, 1);
        at(0.6f, 1);
        at(0.7f, 1);
        assert(runners[1][1].starts == 0);
        at(0.75f, kNone);
        at(0.8f, 1);
        assert(runners[1][1].starts == 1);
        // A tap before an unbuffered window is not kept.
        begin(unbuffered, kNone);
        at(0.2f, 2);
        at(0.3f, kNone);
        at(0.6f, kNone);
        assert(runners[1][1].starts == 0);
        // A closed window takes nothing.
        auto closing = move;
        closing.closeAt = 0.8f;
        begin(closing, kNone);
        at(0.85f, 2);
        assert(runners[1][1].starts == 0);
        // An NPC's never goes.
        charge(move);
        HeldKeys::Get().SetMoveDirection(0);
        ActionLinks::Begin({1}, 0, false, false);
        at(0.6f, 0);
        assert(runners[1][1].starts == 0);
        // Outside a charge (its key up, as a dodge's tapped key): no movement key goes, and a buffered one is dropped.
        begin(move, 3);
        HeldKeys::Get().Release(g, 0.2f);
        at(0.6f, 3);
        at(0.7f, kNone);
        at(0.8f, 2);
        assert(runners[1][1].starts == 0);
        begin(move, kNone);
        at(0.2f, 2);
        at(0.3f, kNone);
        HeldKeys::Get().Release(g, 0.35f);
        at(0.4f, kNone);
        HeldKeys::Get().Press(g, 0.45f);
        at(0.6f, kNone);
        assert(runners[1][1].starts == 0);
        // A loop's window opens again every cycle: a press in the cycle goes then.
        begin(move, kNone);
        at(0.6f, kNone);
        at(0.95f, kNone);
        at(0.05f, kNone);  // cycle 2
        at(0.3f, 1);
        assert(runners[1][1].starts == 0);
        at(0.6f, 1);
        assert(runners[1][1].starts == 1 && Actions::resolvedMovement == 1);
        HeldKeys::Get().SetMoveDirection(-1);
    }
    // The same action again (10-31): its runner restarts the clip; a loop moving the same way plays on.
    {
        ActionLinkSpec self;
        self.onMove = true;
        self.action = "Pair";
        charge(self);
        specs[0].links[0].action = "Pair";
        HeldKeys::Get().Press(g, 0.0f);
        HeldKeys::Get().SetChain(g, 0.0f);
        HeldKeys::Get().SetMoveDirection(2);
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::Update(0.01f);
        assert(runners[1][0].restarts == 1 && runners[1][0].starts == 0 && runners[1][0].exits == 0);
        charge(self);
        specs[0].links[0].action = "Pair";
        specs[0].loop.enabled = true;
        HeldKeys::Get().Press(g, 0.0f);
        HeldKeys::Get().SetChain(g, 0.0f);
        HeldKeys::Get().SetMoveDirection(2);
        ActionLinks::Begin({1}, 0, false, true);
        ActionLinks::Update(0.01f);
        assert(runners[1][0].restarts == 0 && runners[1][0].exits == 0);
        // A key transition into a loop's own action restarts it.
        ActionLinkSpec key;
        key.key = 9;
        key.action = "Pair";
        charge(key);
        specs[0].links[0].action = "Pair";
        specs[0].loop.enabled = true;
        ActionLinks::Begin({1}, 0, false, true);
        assert(ActionLinks::Input(9, false).taken);
        ActionLinks::Update(0.01f);
        assert(runners[1][0].restarts == 1);
        HeldKeys::Get().SetMoveDirection(-1);
    }
    std::cout << "Production ActionLinks scenarios passed\n";
}

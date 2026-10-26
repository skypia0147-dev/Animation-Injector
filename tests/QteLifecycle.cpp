#include "runtime/Qte.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionLinks.h"
#include "runtime/QteServices.h"
#include "runtime/QteScreenFx.h"
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector;
std::array<std::shared_ptr<RE::Actor>, 4> RE::actors;
std::vector<Actions::TestSpec> Actions::specs;
std::vector<std::pair<unsigned, std::string>> sends;
std::function<void()> callback;
std::vector<unsigned> soundSends;
std::vector<std::pair<unsigned, std::string>> started;  // an event QTE result's actions (0.12.249)
std::vector<std::string> partnerStarted;                // and the pair partner's (0.12.264)
namespace AnimationInjector::QteServices {
bool ResolveControl(QteStyle& style, GameControl) {
    style.key = 33;
    style.alternateKey = 256;
    style.gamepad = 10;
    return true;
}
std::uint32_t ResolveSound(const QteResultSpec& r) { return r.soundForm.empty() ? 0 : 1; }
void PlaySound(std::uint32_t id) { soundSends.push_back(id); }
void StartAction(RE::ActorHandle actor, const std::string& action, const std::string& partnerAction) {
    started.emplace_back(actor.id, action);
    partnerStarted.push_back(partnerAction);
}
}
std::vector<int> screenResults;
namespace AnimationInjector::QteScreenFx {
void Result(QteOutcome outcome, bool) { screenResults.push_back(static_cast<int>(outcome)); }
void Update(float) {}
void ResetOnLoad() {}
}
void RE::Actor::NotifyAnimationGraph(const RE::BSFixedString& event) {
    sends.emplace_back(id, event);
    if (callback) callback();
}
bool Actions::QueueEventTrigger(RE::Actor*, const RE::BSFixedString&) { return false; }
void Actions::OnAnimEventFrom(RE::Actor* actor, const RE::BSFixedString& event) {
    Qte::ObserveEvent(actor->GetHandle(), event);
}
void AnimationInjector::ActionLinks::ObserveEvent(RE::ActorHandle, const RE::BSFixedString&) {}
unsigned checks = 0;
void Check(bool x, const char* s) {
    ++checks;
    if (!x) throw std::runtime_error(s);
}
void Reset() {
    Qte::ResetOnLoad();
    sends.clear();
    soundSends.clear();
    started.clear();
    callback = {};
    for (unsigned i = 1; i < 4; i++) RE::actors[i] = std::make_shared<RE::Actor>(RE::Actor{i});
    QteSpec q;
    q.id = "Q";
    q.at = .5;
    q.results[0] = {"Won", "self"};
    q.results[1] = {"Lost", "self"};
    Actions::specs = {{"Test", {q}}};
}
void Start() {
    Qte::Begin({1}, 0);
    Qte::ObserveTime({1}, 0, .5);
    Qte::Update(0);
}
int main() {
    try {
        // PairFeatures arms before playback. Idle ActionRunners update every frame,
        // including the runner with the same actor/action as the live pair.
        for (bool playerVictim : {false, true}) {
            Reset();
            auto& q = Actions::specs[0].qtes[0];
            q.at = -1;
            q.event = "Grapple_Stage_Start_01";
            q.inputOwner = q.eventSource = playerVictim ? "partner" : "self";
            q.delay = .3f;
            const RE::ActorHandle initiator{playerVictim ? 2u : 1u}, partner{playerVictim ? 1u : 2u};
            Qte::Begin(initiator, 0, partner);
            for (int frame = 0; frame < 5; ++frame) {
                Qte::End(initiator, 0);
                Qte::Update(.01f);
            }
            Qte::ObserveTime(initiator, 0, 2.56f);
            Qte::ObserveEvent({1}, q.event);
            Qte::End(initiator, 0);
            Qte::Update(0);
            Check(
                Qte::ReadView().visible, "idle ordinary cleanup must preserve armed paired QTE for either player role");
            Qte::End(initiator, 0, {3});
            Qte::Update(0);
            Check(Qte::ReadView().visible, "unrelated partner cannot retire paired QTE");
            Qte::Input({}, true);
            Qte::Update(.1f);
            Qte::End(initiator, 0);
            Qte::Update(.2f);
            Check(sends == std::vector<std::pair<unsigned, std::string>>{{initiator.id, "Lost"}},
                "idle cleanup preserves delayed pair result");
            Qte::End(initiator, 0, partner);
            Qte::Update(2);
            Check(!Qte::ReadView().visible && sends.size() == 1,
                "owning pair can retire completed QTE without duplicate result");
            Qte::Begin(initiator, 0, partner);
            Qte::ObserveTime(initiator, 0, 0);
            Qte::ObserveEvent({1}, q.event);
            Qte::Update(0);
            Qte::Input({{}, 18, 2}, true);
            Qte::Input({}, true);
            Qte::End(initiator, 0, partner);
            Qte::Update(1);
            Check(!Qte::ReadView().visible && sends.size() == 1, "pair interruption cancels its own pending verdict");
        }
        Reset();
        Start();
        Qte::End({1}, 0, {2});
        Check(Qte::ReadView().visible, "pair cleanup cannot retire ordinary QTE");
        Qte::End({1}, 0);
        Check(!Qte::ReadView().visible, "ordinary cleanup still cancels ordinary QTE");
        Reset();
        Start();
        Check(Qte::ReadView().visible, "timed prompt starts after clip observation");
        Qte::Update(QteClock::target * 2.4f);
        Check(Qte::Input({}, true), "designated fresh down claimed");
        Qte::Update(0);
        Check(sends == std::vector<std::pair<unsigned, std::string>>{{1, "Won"}}, "perfect defaults to success");
        Qte::Update(.01f);
        Check(sends.size() == 1, "no duplicate result");
        Qte::End({1}, 0);
        Check(Qte::ReadView().visible, "result visual survives dispatched transition");
        Check(Qte::Input({{}, 18, 2}, true), "matching release consumed after source retired");
        Qte::Update(1.35f);
        Check(!Qte::ReadView().visible, "result tail expires");
        // An HKX may send the same event several times (0.12.262): the rule asks each time, once its last prompt
        // is gone; a second event while it is still on screen does not restart it.
        Reset();
        {
            auto& q = Actions::specs[0].qtes[0];
            q.at = -1;
            q.event = "Again";
            Qte::Begin({1}, 0);
            Qte::ObserveTime({1}, 0, 0);
            Qte::ObserveEvent({1}, "Again");
            Qte::Update(0);
            const auto first = Qte::ReadView().session;
            Check(Qte::ReadView().visible, "first event asks");
            Qte::ObserveEvent({1}, "Again");
            Qte::Update(0);
            Check(Qte::ReadView().session == first, "an event while the prompt is open does not restart it");
            Qte::Update(2.5f);
            Qte::Update(1.4f);
            Check(!Qte::ReadView().visible && sends.size() == 1, "first prompt timed out and is gone");
            Qte::ObserveEvent({1}, "Again");
            Qte::Update(0);
            Check(Qte::ReadView().visible && Qte::ReadView().session != first, "the same event later asks again");
        }
        Reset();
        Qte::Input({}, true);
        Start();
        Qte::Update(1);
        Check(!Qte::Input({}, true), "pre-held key cannot resolve");
        Check(!Qte::Input({{}, 18, 2}, true), "pre-held native key-up is not swallowed");
        Check(Qte::Input({}, true), "release and re-press resolves");
        Reset();
        Start();
        Check(!Qte::Input({{}, 19, 0}, true), "unrelated key passes through");
        Qte::Update(2.4f);
        Qte::Update(0);
        Check(sends[0].second == "Lost", "timeout failure");
        Reset();
        Start();
        Qte::End({1}, 0);
        Qte::Update(10);
        Check(sends.empty() && !Qte::ReadView().visible, "source cancellation has no failure action");
        Reset();
        Start();
        RE::actors[1]->dead = true;
        Qte::Update(.1f);
        Check(sends.empty() && !Qte::ReadView().visible, "death cancels without result");
        Reset();
        Start();
        Qte::ResetOnLoad();
        Qte::Update(10);
        Check(sends.empty() && !Qte::ReadView().visible, "load invalidates old session");
        Reset();
        auto& q = Actions::specs[0].qtes[0];
        q.at = -1;
        q.event = "Start";
        q.inputOwner = "partner";
        q.eventSource = "partner";
        q.results[1] = {"TargetFailed", "both"};
        Qte::Begin({2}, 0, {1});
        Qte::ObserveEvent({2}, "Start");
        Qte::Update(0);
        Check(!Qte::ReadView().visible, "event sender role honored");
        Qte::ObserveEvent({1}, "Start");
        Qte::Update(0);
        Check(!Qte::ReadView().visible, "pair must actually enter playback before QTE opens");
        Qte::ObserveTime({2}, 0, 0);
        Qte::Update(0);
        Check(Qte::ReadView().visible, "player victim accepts partner event");
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends == std::vector<std::pair<unsigned, std::string>>{{2, "TargetFailed"}, {1, "TargetFailed"}},
            "both result recipients preserved");
        Reset();
        Actions::specs[0].qtes[0].delay = .3f;
        Start();
        Qte::Input({}, true);
        Qte::Update(.1f);
        Check(sends.empty(), "configured result delay");
        Qte::End({1}, 0);
        Qte::Update(.3f);
        Check(sends.empty(), "ending during result delay cancels pending action");
        Reset();
        Start();
        callback = [] { Qte::End({1}, 0); };
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.size() == 1, "reentrant end callback has no lock or duplicate");
        Reset();
        Actions::specs[0].qtes.push_back(Actions::specs[0].qtes[0]);
        Actions::specs[0].qtes[1].id = "Q2";
        Start();
        Qte::Input({}, true);
        Qte::Update(0);
        Qte::Update(2);
        Check(sends.size() == 1, "overlapping challenges skipped without stale queued result");
        Reset();
        Actions::specs[0].qtes[0].results[1].recipient = "both";
        Qte::Begin({1}, 0, {2});
        Qte::ObserveTime({1}, 0, .5f);
        Qte::Update(0);
        Qte::Input({}, true);
        callback = [] { Qte::ResetOnLoad(); };
        Qte::Update(0);
        Check(sends.size() == 1, "load during callback invalidates remaining result recipients");
        Reset();
        Qte::Begin({1}, 0, {}, 2);
        Qte::ObserveTime({1}, 0, 2.1f);
        Qte::Update(0);
        Check(!Qte::ReadView().visible, "pre-activation wait is excluded from QTE start time");
        Qte::ObserveTime({1}, 0, 2.5f);
        Qte::Update(0);
        Check(Qte::ReadView().visible, "QTE time is relative to actual activation");
        Reset();
        Actions::specs[0].qtes[0].type = QteType::HoldRelease;
        Start();
        Qte::Input({}, true);
        Qte::Update(QteClock::target * 2.4f);
        Check(Qte::ReadView().clock.outcome == QteOutcome::None, "holding is not a press verdict");
        Qte::Input({{}, 18, 2}, true);
        Qte::Update(0);
        Check(sends[0].second == "Won", "real release dispatches hold result");
        Reset();
        Actions::specs[0].qtes[0].type = QteType::HoldRelease;
        Start();
        Qte::Input({}, true);
        Qte::End({1}, 0);
        Check(Qte::Input({{}, 18, 2}, true), "cancelled hold still consumes matching release");
        Qte::Update(0);
        Check(sends.empty(), "cancelled hold never dispatches");
        Reset();
        Actions::specs[0].qtes[0].type = QteType::HoldRelease;
        Start();
        Qte::Input({}, true);
        Qte::Input({{}, 18, 2}, false);
        Check(!Qte::ReadView().visible, "release while paused cancels instead of sticking");
        Reset();
        Actions::specs[0].qtes[0].type = QteType::HoldRelease;
        Start();
        Qte::Input({}, true);
        Qte::DisconnectInput();
        Qte::Update(3);
        Check(sends.empty() && !Qte::ReadView().visible, "device loss cancels hold");
        Reset();
        Actions::specs[0].qtes[0].type = QteType::RapidPress;
        Actions::specs[0].qtes[0].pressCount = 3;
        Start();
        Qte::Input({}, true);
        Qte::Input({}, true);
        Check(Qte::ReadView().clock.completed == 1, "held/repeat cannot count twice");
        for (int i = 0; i < 2; ++i) {
            Qte::Input({{}, 18, 2}, true);
            Qte::Input({}, true);
        }
        Qte::Update(0);
        Check(sends[0].second == "Won", "released fresh rapid presses count");
        Reset();
        auto& seq = Actions::specs[0].qtes[0];
        seq.type = QteType::KeySequence;
        seq.sequenceCount = 3;
        seq.sequence[0] = {18, 4096};
        seq.sequence[1] = {16, 8192};
        seq.sequence[2] = {18, 4096};
        Start();
        Qte::Input({}, true);
        Qte::Input({{}, 18, 2}, true);
        Qte::Input({{}, 16, 0}, true);
        Qte::Input({{}, 16, 2}, true);
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends[0].second == "Won", "key sequence distinct and repeated keys");
        Reset();
        auto& wrong = Actions::specs[0].qtes[0];
        wrong.type = QteType::KeySequence;
        wrong.sequenceCount = 2;
        wrong.sequence[0] = {18, 0};
        wrong.sequence[1] = {16, 0};
        Start();
        Qte::Input({{}, 16, 0}, true);
        Qte::Update(0);
        Check(sends[0].second == "Lost", "out of order configured key fails immediately");
        Reset();
        auto& pad = Actions::specs[0].qtes[0];
        pad.type = QteType::KeySequence;
        pad.sequenceCount = 2;
        pad.sequence[0] = {0, 4096};
        pad.sequence[1] = {0, 8192};
        Start();
        Check(Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 4096, 0}, true), "gamepad sequence down claimed");
        Check(Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 4096, 2}, true), "gamepad sequence up claimed");
        Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 8192, 0}, true);
        Qte::Update(0);
        Check(sends[0].second == "Won", "gamepad sequence completes");
        Reset();
        auto& mouse = Actions::specs[0].qtes[0];
        mouse.type = QteType::HoldRelease;
        mouse.key = 256;
        Start();
        Qte::Input({{RE::INPUT_DEVICE::kMouse}, 0, 0}, true);
        Qte::Update(QteClock::target * 2.4f);
        Qte::Input({{RE::INPUT_DEVICE::kMouse}, 0, 2}, true);
        Qte::Update(0);
        Check(sends[0].second == "Won", "mouse hold release uses offset key codes");
        Reset();
        auto& dual = Actions::specs[0].qtes[0];
        dual.type = QteType::HoldRelease;
        dual.gamepad = 4096;
        Start();
        Qte::Input({}, true);
        Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 4096, 0}, true);
        Qte::Update(.5f);
        Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 4096, 2}, true);
        Check(Qte::ReadView().clock.outcome == QteOutcome::None, "other device release cannot release active hold");
        Qte::Update(QteClock::target * 2.4f - .5f);
        Qte::Input({{}, 18, 2}, true);
        Qte::Update(0);
        Check(sends[0].second == "Won", "only original device releases hold");
        Reset();
        auto& advanced = Actions::specs[0].qtes[0];
        advanced.control = "rightAttack";
        advanced.stopEvent = "Stop";
        advanced.delay = .3f;
        advanced.results[1].soundForm = "824";
        Start();
        Check(Qte::Input({{RE::INPUT_DEVICE::kMouse}, 0, 0}, true), "remapped mouse alternate input accepted");
        Qte::Update(0);
        Check(soundSends.size() == 1 && sends.empty(), "sound at verdict before delayed route");
        Qte::ObserveEvent({1}, "Stop");
        Qte::Update(1);
        Check(!Qte::ReadView().visible && sends.empty() && soundSends.size() == 1,
            "stop cancels delayed route without replaying sound");
        Check(Qte::Input({{RE::INPUT_DEVICE::kMouse}, 0, 2}, true), "stop still releases claimed input");
        Reset();
        Actions::specs[0].qtes[0].control = "rightAttack";
        Start();
        Check(Qte::Input({{RE::INPUT_DEVICE::kGamepad}, 10, 0}, true), "gamepad right trigger accepted");
        Reset();
        auto second = Actions::specs[0].qtes[0];
        second.id = "Second";
        second.speed = 4;
        second.event = "Next";
        second.at = -1;
        second.replaceActive = true;
        Actions::specs[0].qtes.push_back(second);
        Start();
        Qte::ObserveEvent({1}, "Next");
        Qte::Update(0);
        Check(Qte::ReadView().spec.speed == 4 && Qte::ReadView().clock.elapsed == 0,
            "same-session active stage replacement");
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.size() == 1, "replacement has one result");
        Reset();
        Actions::specs[0].qtes[0].delay = .3f;
        Actions::specs[0].qtes.push_back(second);
        Start();
        Qte::Input({}, true);
        Qte::ObserveEvent({1}, "Next");
        Qte::Update(.1f);
        Qte::Update(.2f);
        Check(sends.size() == 1 && sends[0].second == "Lost", "pending route cannot be overwritten by next stage");
        Reset();
        Actions::specs[0].qtes[0].results[1].event.clear();
        Actions::specs[0].qtes[0].delay = .3f;
        second.results[1].event = "SecondFailed";
        Actions::specs[0].qtes.push_back(second);
        Start();
        Qte::Input({}, true);
        Qte::ObserveEvent({1}, "Next");
        Qte::Update(.1f);
        Qte::Input({{}, 18, 2}, true);
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.size() == 1 && sends[0].second == "SecondFailed",
            "continue result can yield to next stage during feedback");
        // Channels: a challenge on another channel opens beside the first and takes only its own key.
        Reset();
        {
            auto& first = Actions::specs[0].qtes[0];
            first.key = 256;  // LMB
            auto carry = first;
            carry.id = "Carry";
            carry.channel = 1;
            carry.type = QteType::RapidPress;
            carry.pressCount = 1;
            carry.key = 18;  // E
            carry.results[0] = {"CarryWon", "self"};
            carry.results[1] = {};
            Actions::specs[0].qtes.push_back(carry);
        }
        Start();
        const auto both = Qte::ReadViews();
        Check(both[0].visible && both[1].visible && both[0].session != both[1].session, "two channels open together");
        Check(!Qte::Input({{RE::INPUT_DEVICE::kKeyboard}, 30, 0}, true), "a key of neither channel is not claimed");
        Check(Qte::Input({{}, 18, 0}, true), "E belongs to the second channel");
        Qte::Update(0);
        Check(sends.size() == 1 && sends[0].second == "CarryWon", "second channel resolves alone");
        Check(Qte::ReadView(0).visible && Qte::ReadView(0).clock.outcome == QteOutcome::None,
            "first channel keeps running");
        Check(Qte::Input({{RE::INPUT_DEVICE::kMouse}, 0, 0}, true), "LMB belongs to the first channel");
        Qte::Update(0);
        Check(sends.size() == 2 && sends[1].second == "Lost", "first channel resolves with its own route");
        Qte::End({1}, 0);
        Qte::Update(2);
        Check(!Qte::ReadView(0).visible && !Qte::ReadView(1).visible, "both result tails expire after the source ends");
        Reset();
        Actions::specs[0].qtes[0].speed = 4;
        Start();
        Qte::Update(.6f);
        Check(Qte::ReadView().clock.outcome == QteOutcome::Failure, "A speed four times out at .6 seconds");
        Reset();
        Actions::specs[0].qtes[0].speed = 4;
        Actions::specs[0].qtes[0].results[0].soundForm = "822";
        Start();
        Qte::Update(QteClock::target * .6f);
        Qte::Input({}, true);
        Qte::Update(0);
        Qte::Update(.01f);
        Check(sends.size() == 1 && sends[0].second == "Won" && soundSends.size() == 1,
            "A speed four perfect uses success route and sound exactly once");
        Reset();
        Actions::specs[0].qtes[0].results[1].soundForm = "824";
        Start();
        Qte::Input({}, true);
        Qte::ResetOnLoad();
        Qte::Update(1);
        Check(sends.empty() && soundSends.empty(), "load before verdict dispatch suppresses sound and result");
        // An event's QTEs (0.12.248): the player only, asked at once, kept through a ragdoll and through any
        // action's begin and end; the event again while they are open is the same start; done after the tail.
        Reset();
        QteSpec ev;
        ev.id = "GetUp";
        ev.at = 0;
        ev.results[0] = {"Ragdoll_Recovery", "self"};
        const std::vector<QteSpec> evs{ev};
        Check(!Qte::BeginEvent({2}, evs, "Ragdoll"), "an event QTE is the player's only");
        Check(Qte::BeginEvent({1}, evs, "Ragdoll"), "an event QTE is asked of the player");
        Qte::Update(0);
        Check(Qte::ReadView().visible, "an event QTE shows at once");
        Check(Qte::BeginEvent({1}, evs, "Ragdoll"), "the event again while it is open is the same start");
        RE::actors[1]->ragdoll = true;
        Qte::Begin({1}, 0);
        Qte::End({1}, 0);
        Qte::Update(QteClock::target * 2.4f);
        Check(Qte::ReadView().visible && Qte::ReadView().clock.outcome == QteOutcome::None,
            "an event QTE outlasts a ragdoll and an action's begin and end");
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.size() == 1 && sends[0].second == "Ragdoll_Recovery", "its success sends its event once");
        Qte::Update(2);
        Check(!Qte::ReadView().visible, "gone after its result tail");
        Qte::BeginEvent({1}, evs, "Ragdoll");
        Qte::Update(0);
        Check(Qte::ReadView().visible && Qte::ReadView().clock.outcome == QteOutcome::None,
            "asked afresh by the next event");
        // A result naming an action (0.12.249) starts it for the player, with no event; a failure without one
        // starts nothing.
        Reset();
        QteSpec go = ev;
        go.results[0] = {};
        go.results[0].to = "GetUpBack";
        const std::vector<QteSpec> gos{go};
        Qte::BeginEvent({1}, gos, "GetUpBack");
        Qte::Update(0);
        Qte::Update(QteClock::target * 2.4f);
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.empty() && started.size() == 1 && started[0].first == 1 && started[0].second == "GetUpBack",
            "a success naming an action starts it");
        Reset();
        Qte::BeginEvent({1}, gos, "GetUpBack");
        Qte::Update(0);
        Qte::Update(3);
        Qte::Update(2);
        Check(started.empty(), "a missed QTE starts nothing");
        // Asked during a pair (0.12.264): the result names the partner's action too.
        Reset();
        partnerStarted.clear();
        QteSpec paired = go;
        paired.results[0].to = "Stage10Attacker";
        paired.results[0].partnerTo = "Stage10Target";
        const std::vector<QteSpec> pairs{paired};
        Qte::BeginEvent({1}, pairs, "Grapple_Stage_Start_10");
        Qte::Update(0);
        Qte::Update(QteClock::target * 2.4f);
        Qte::Input({}, true);
        Qte::Update(0);
        Check(started.size() == 1 && started[0].second == "Stage10Attacker" && partnerStarted.size() == 1 &&
                  partnerStarted[0] == "Stage10Target",
            "a success hands the player and the partner their actions");
        // A hidden QTE (0.12.266): asked and judged like any other, its channel free once its result is sent.
        Reset();
        QteSpec quiet = ev;
        quiet.hidden = true;
        const std::vector<QteSpec> quiets{quiet};
        Qte::BeginEvent({1}, quiets, "Quiet");
        Qte::Update(0);
        Check(Qte::ReadView().visible && Qte::ReadView().spec.hidden, "a hidden QTE is asked (the menu draws nothing)");
        Qte::Update(QteClock::target * 2.4f);
        Qte::Input({}, true);
        Qte::Update(0);
        Check(sends.size() == 1 && !Qte::ReadView().visible, "its result is sent with no result time to show");
        std::cout << checks << " QTE lifecycle/input checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

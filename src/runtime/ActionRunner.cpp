#include "PCH.h"
#include "runtime/PairTargetMath.h"
#include "runtime/ActionRunner.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionHistory.h"
#include "runtime/NpcMoveCancel.h"
#include "core/GraphInjector.h"
#include "core/AirControl.h"
#include "core/AnimDataRegistry.h"
#include "core/MotionHook.h"
#include "Settings.h"
#include "runtime/InputSink.h"
#include "runtime/TdmTurn.h"
#include "core/EventMaps.h"
#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "runtime/ActionLinks.h"
#include "runtime/PairFeatures.h"
#include "runtime/Qte.h"
#include "runtime/DrivenFlagOwnership.h"
#include <string_view>
#include <shared_mutex>
#include "runtime/ActorImmunity.h"
#include "runtime/EventEffects.h"
#include "runtime/LifecyclePolicy.h"
#include "runtime/NativeCancelPolicy.h"
#include "runtime/ViewGuard.h"

namespace AnimationInjector {
namespace {
// The player's steer for root motion (runtime/TdmTurn.h): the body's heading minus the engine's motion heading.
std::atomic<float> g_playerSteer{0.0f};
}

namespace {
// 매 프레임 문자열 생성 금지 — 이벤트 이름은 액션·변형마다 한 번만 해싱
// Also reached from animation threads (e.g. Adopt → attack data), so inserts are locked.
// unordered_map keeps element references valid when it rehashes.
const RE::BSFixedString& Fixed(const std::string& a_name) {
    static std::shared_mutex lock;
    static std::unordered_map<std::string, RE::BSFixedString> cache;
    {
        std::shared_lock read(lock);
        if (const auto it = cache.find(a_name); it != cache.end()) return it->second;
    }
    std::unique_lock write(lock);
    return cache.try_emplace(a_name, a_name.c_str()).first->second;
}
const RE::BSFixedString& VarAnimDriven() {
    static RE::BSFixedString s{"bAnimationDriven"};
    return s;
}
const RE::BSFixedString& VarAllowRotation() {
    static RE::BSFixedString s{"bAllowRotation"};
    return s;
}
const RE::BSFixedString& VarInJump() {
    static RE::BSFixedString s{"bInJumpState"};
    return s;
}
}

const ActionSpec& ActionRunner::Spec() const { return Actions::Spec(_idx); }

bool ActionRunner::Conditions(RE::Actor* p, bool a_linkPreflight) const {
    const auto& c = Spec().condition;
    auto deny = [&](const char* a_why) {
        logger::debug("[{}] 거부: {}", Spec().id, a_why);
        return false;
    };
    if (!p || !p->Is3DLoaded() || p->IsDead()) return deny("액터 없음/사망");
    if (Carry::BlocksAction(p, _idx)) return deny("carry owns actor");
    PairedSession::Ticket pairTicket;
    if (!a_linkPreflight && PairedSession::TryGetTicket(p->GetHandle(), pairTicket) &&
        !PairedSession::CanEnterHandoff(p->GetHandle(), _idx))
        return deny("페어 진행 중");
    if (_state != State::kIdle) return deny("이미 실행 중");
    if (_cooldown > 0.0f) return deny("쿨다운");
    // A selecting action is never injected (0.12.244): only the action its Flow picks needs a graph (CanStart). Until
    // 0.12.269 this refused every single selecting action's start (a pair skipped the check).
    if (!Spec().paired.enabled && !Spec().SelectsOnly() && !GraphInjector::AnyInjected(_idx))
        return deny("삽입된 그래프 없음");
    auto* as = p->AsActorState();
    if (!as) return false;
    if (as->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return deny("넉다운/스태거");
    if (as->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return deny("앉음/수면");
    if (as->IsSwimming() || p->IsOnMount() || p->IsInKillMove() || p->IsInRagdollState())
        return deny("수영/탑승/킬무브/래그돌");
    {
        auto* cc = p->GetCharController();
        const bool air = cc && cc->context.currentState == RE::hkpCharacterStateType::kInAir;
        logger::debug("[{}] AirCheck policy={} controller={} state={} inAir={}", Spec().id, c.inAir, cc != nullptr,
            cc ? static_cast<int>(cc->context.currentState) : -1, air);
        if (air && c.inAir == 0) return deny("공중");
        if (!air && c.inAir == 2) return deny("공중 아님 (inAir=only)");
    }
    if (_isPlayer)
        if (!ViewGuard::AllowStart(_idx)) return deny("시점");
    if (!_isPlayer && Spec().view.view == View::kFirst) return deny("1인칭 전용");
    return true;
}

bool ActionRunner::Request(int origin) { return RequestImpl(nullptr, nullptr, origin); }
bool ActionRunner::CanStart() const {
    if (!Conditions(Owner()) || Spec().clips.empty()) return false;
    auto* p = Owner();
    auto* cc = p ? p->GetCharController() : nullptr;
    if (!cc) return false;
    return cc->context.currentState != RE::hkpCharacterStateType::kInAir || GraphInjector::HasAirRoute(_idx);
}

bool ActionRunner::RequestLinked(ActionRunner& source, const RE::BSFixedString& ground, const RE::BSFixedString& air) {
    if (&source == this || source._owner != _owner || !source.IsActive() || source.Spec().paired.enabled ||
        Spec().paired.enabled || source.Spec().clips.size() != 1 || Spec().clips.size() != 1 || !CanRequestLink())
        return false;
    _linkSource = source._idx;
    _linkAge = 0;
    source._linkTarget = _idx;
    source._linkRetirement = {};
    source._state = State::kBlendingOut;
    Qte::End(source._owner, source._idx);
    const bool accepted = RequestImpl(&ground, &air);
    if (!accepted) AbortLinkedRequest();
    return accepted;
}

bool ActionRunner::RestartLinked(const RE::BSFixedString& ground, const RE::BSFixedString& air) {
    auto* p = Owner();
    if (_state != State::kActive || _exitSent || _linkSource >= 0 || _linkTarget >= 0 || Spec().paired.enabled ||
        Spec().clips.size() != 1 || !p || !p->Is3DLoaded() || p->IsDead())
        return false;
    // The engine restarts the clip without deactivating or activating it (in-game 10-31): the run starts over here.
    if (!p->NotifyAnimationGraph(_startedInAir ? air : ground)) {
        logger::debug("[{}] SelfLink: notify refused; plays on", Spec().id);
        return false;
    }
    RestoreSlots();
    _elapsed = 0.0f;
    _carryReleaseDue = false;
    _endReached = _releaseForced = false;
    _seenTags.clear();
    ResetPlaybackFlags();
    _clipTime = -1.0f;
    _clipSpeed.store(0.0f);
    ActionLinks::Begin(_owner, _idx, false, _isPlayer);
    SetupPlayback();
    logger::debug("[{}] SelfLink: restarted event='{}'", Spec().id, (_startedInAir ? air : ground).c_str());
    return true;
}

void ActionRunner::AbortLinkedRequest() {
    if (_linkSource < 0) return;
    if (auto* source = Actions::RunnerFor(Owner(), _linkSource, false); source && source->_linkTarget == _idx) {
        source->_linkTarget = -1;
        if (source->_linkRetirement.deactivated) {
            source->_exitSent = true;
            source->Finish();
        } else
            source->_state = State::kActive;
    }
    _linkSource = -1;
    _state = State::kIdle;
    BeginAirPhysics(false);
}

bool ActionRunner::CaptureNativeCancel(const RE::BSFixedString& event) {
    if (_state != State::kActive || _startedInAir || !event.c_str() || !GraphInjector::HasFullBodyRoute(_idx) ||
        !GraphInjector::IsNativeTransitionEvent(event.c_str()))
        return false;
    const auto& spec = Spec();
    for (std::size_t i = 0; i < spec.cancels.size(); ++i) {
        const auto& w = spec.cancels[i];
        if (w.vanillaMatches.empty()) continue;
        bool match = false;
        for (const auto& name : w.vanillaMatches) {
            const char* text = event.c_str();
            for (; *text; ++text)
                if (_strnicmp(text, name.c_str(), name.size()) == 0) {
                    match = true;
                    break;
                }
            if (match) break;
        }
        if (!match || !AcceptNativeCancelInput(w.mode, _openSeen[i], _closeSeen[i])) continue;
        std::lock_guard lock(_nativeLock);
        _nativeReplay = event;
        _nativeWindow = static_cast<int>(i);
        return true;
    }
    return false;
}
void ActionRunner::ClearNativeCapture() {
    std::lock_guard lock(_nativeLock);
    _nativeWindow = -1;
    _nativeReplay = RE::BSFixedString();
}
void ActionRunner::UpdateNativeCancel() {
    int window = -1;
    {
        std::lock_guard lock(_nativeLock);
        window = _nativeWindow;
    }
    if (window < 0 || _state != State::kActive) return;
    const auto i = static_cast<std::size_t>(window);
    const auto& w = Spec().cancels[i];
    if (w.mode != 0 && _closeSeen[i]) {
        ClearNativeCapture();
        return;
    }
    if (!ReadyNativeCancel(w.mode, _openSeen[i], _closeSeen[i])) return;
    auto* actor = Owner();
    if (!actor || !actor->Is3DLoaded()) return;
    _nativeExiting = true;
    PrepareExit(w.blend >= 0 ? w.blend : Spec().exitBlend);
    const bool ok = actor->NotifyAnimationGraph(Fixed(w.nativeCancelEvent));
    if (!ok && _state == State::kExiting) {
        _state = State::kActive;
        _exitSent = false;
        _nativeExiting = false;
        ClearNativeCapture();
    }
    logger::debug("[NativeCancel] exit actor={:08X} action='{}' accepted={}", actor->GetFormID(), Spec().id, ok);
}

void ActionRunner::SetupPlayback() {
    ++_playbackSerial;
    _normalExitMovementAccepted = false;
    SetAnimationDriven(true);
    ApplyAttackData();
    ApplyRotate(false);
    SlotUpdate();
    LogAirPhysics(AirPhase::kAfterSetup);
}

bool ActionRunner::RequestImpl(const RE::BSFixedString* ground, const RE::BSFixedString* air, int origin) {
    auto* p = Owner();
    if (ground ? !Conditions(p, true) : !CanStart()) return false;
    if (!_isPlayer)
        EventMaps::Ensure(p,
            Actions::AllEventNames());  // NPC 의 프로젝트 DB 에 우리 이름이 있어야 NotifyAnimationGraph 가 id 를 찾는다
    const auto& spec = Spec();
    if (spec.clips.empty()) return false;
    const int variant = 0;  // an action's one clip; a pair's lead role
    auto* controller = p->GetCharController();
    const bool airRoute = spec.condition.inAir != 0 && controller &&
                          controller->context.currentState == RE::hkpCharacterStateType::kInAir;
    if (airRoute && !GraphInjector::HasAirRoute(_idx)) {
        logger::warn("[{}] 공중 진입 경로가 삽입되지 않아 요청 거부", spec.id);
        return false;
    }
    const auto& enterEvent = airRoute ? spec.clips[variant].airEnterEvent : spec.clips[variant].enterEvent;
    if (!AnimData::Registered()) AnimData::TryRegister("Request");
    _interrupted = false;
    _requestOrigin = origin >= 0 ? origin : _idx;
    _variant = variant;
    _elapsed = 0.0f;
    _carryReleaseDue = false;
    _exitSent = false;
    _endReached = _releaseForced = false;
    _clipDuration = 0.0f;
    _exitingTime = 0.0f;
    _seenTags.clear();
    ResetPlaybackFlags();
    _postCancelTimer = 0.0f;  // 이전 캔슬 아웃 관찰이 새 실행의 bAnimationDriven 을 끄지 않도록
    const int atk = p->AsActorState() ? static_cast<int>(p->AsActorState()->GetAttackState()) : -1;
    _state = State::
        kActive;  // 동기 전이: notify 안에서 클립 활성 훅이 온다 — 그 전에 Active 여야 외부 진입 채택(Adopt)으로 오인하지 않는다
    BeginAirPhysics(airRoute);
    LogAirPhysics(AirPhase::kBeforeEntry);
    logger::debug("[{}] EntryRoute={} event='{}'", spec.id, airRoute ? "air" : "ground", enterEvent);
    const bool ok = p->NotifyAnimationGraph(ground ? (airRoute ? *air : *ground) : Fixed(enterEvent));
    LogAirPhysics(AirPhase::kAfterEntry);
    logger::debug("[{}] 요청 변형 {}('{}') notify={} attackState={} 액터 {:08X}", spec.id, variant,
        spec.clips[variant].tag, ok, atk, p->GetFormID());
    if (!ok || _state == State::kIdle) {
        _state = State::kIdle;
        _requestOrigin = -1;
        BeginAirPhysics(false);
        return false;
    }
    if (!ground) SetupPlayback();
    return true;
}

// 페어 시작 (V10 스파이크). 대상: 로드된 캐릭터(DefaultMale/Female 프로젝트) 중 전방 원뿔 안 가장 가까운 액터.
//   엔진 AIProcess::PlayIdle 이 BGSSynchronizedAnimationInstance 를 만들고 두 액터에 이벤트를 보낸다 — 어떤 이름을 보내는지는 Notify 훅 로그로 확인.
namespace {
bool IsPairedRig(RE::Actor* a, PairedRig::Kind expected) {
    auto* race = a ? a->GetRace() : nullptr;
    if (!race) return false;
    const char* m = race->behaviorGraphs[0].GetModel();
    return m && PairedRig::ProjectPath(m) == expected;
}
// A pair target stands on its own: not ragdolled, and not down or getting up. When its ragdoll has
// just come to rest the ragdoll flag is already off while the knock state still reads down or
// get-up, and its bodies may still simulate: a pair (and a carry after it) started then fought the
// engine's physics (0.12.216: the carried target vanished at the putdown, then lay on the ground
// and followed the carrier).
bool TargetUpright(RE::Actor* a) {
    auto* state = a ? a->AsActorState() : nullptr;
    return state && !a->IsInRagdollState() && state->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal;
}
RE::Actor* FindPairedTarget(RE::Actor* a_self, float a_range, float a_angleDeg, PairedRig::Kind rig) {
    auto* pl = RE::ProcessLists::GetSingleton();
    if (!pl || !a_self) return nullptr;
    RE::Actor* best = nullptr;
    float bestD = a_range;
    std::string rejected;
    for (auto& h : pl->highActorHandles) {
        auto p = h.get();
        RE::Actor* a = p ? p.get() : nullptr;
        if (!a || a == a_self || !a->Is3DLoaded() || a->IsDead() || a->IsChild()) continue;
        if (!TargetUpright(a)) {
            if (FrameworkSettings::Get().debug) rejected += fmt::format(" [{} not upright]", a->GetDisplayFullName());
            continue;
        }
        const float d = a_self->GetPosition().GetDistance(a->GetPosition());
        if (d > a_range) continue;
        if (!IsPairedRig(a, rig)) {
            if (FrameworkSettings::Get().debug)
                rejected += fmt::format(" [{} {:.0f}u expected={} project='{}']", a->GetDisplayFullName(), d,
                    PairedRig::Name(rig), a->GetRace() ? a->GetRace()->behaviorGraphs[0].GetModel() : "?");
            continue;
        }
        const float ang = PairTargetMath::AbsoluteHeading(a_self->GetHeadingAngle(a->GetPosition(), false));
        if (ang > a_angleDeg * 0.5f) {
            if (FrameworkSettings::Get().debug)
                rejected += fmt::format(" [{} {:.0f}u 각 {:.0f}°]", a->GetDisplayFullName(), d, ang);
            continue;
        }
        if (d < bestD) {
            best = a;
            bestD = d;
        }
    }
    // The player is not guaranteed to be in the high-process NPC list.
    // NPC-initiated pairs must consider the player with the same eligibility rules.
    if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player != a_self && player->Is3DLoaded() &&
                                                            !player->IsDead() && !player->IsChild() &&
                                                            IsPairedRig(player, rig)) {
        const float distance = a_self->GetPosition().GetDistance(player->GetPosition());
        const float heading = PairTargetMath::AbsoluteHeading(a_self->GetHeadingAngle(player->GetPosition(), false));
        if (distance < bestD && heading <= a_angleDeg * 0.5f) {
            best = player;
            bestD = distance;
        }
    }
    if (!best) logger::debug("페어 대상 없음. 거른 것:{}", rejected);
    return best;
}
}

bool ActionRunner::Cancel(const char* why) {
    if (_state != State::kActive) {
        if (_state == State::kIdle && _linkSource >= 0) AbortLinkedRequest();
        return false;
    }
    logger::info("[{}] cancelled ({}) at {:.2f}s", Spec().id, why, _elapsed);
    if (InterruptIfUnavailable()) return true;
    return ExitForLink();
}

bool ActionRunner::ExitForLink() {
    auto* actor = Owner();
    if (!actor || !actor->Is3DLoaded() || _state != State::kActive) return false;
    PrepareExit(Spec().exitBlend);
    const bool accepted = actor->NotifyAnimationGraph(Fixed(Spec().events.exit));
    if (!accepted && _state == State::kExiting) {
        _exitSent = false;
        _state = State::kActive;
    }
    return accepted;
}

RE::ActorHandle ActionRunner::FrontTargetFor(RE::Actor* actor, float range, float angle) {
    auto* lists = RE::ProcessLists::GetSingleton();
    if (!lists || !actor || !actor->Is3DLoaded()) return {};
    RE::Actor* best = nullptr;
    float bestDistance = range;
    const auto consider = [&](RE::Actor* a) {
        if (!a || a == actor || !a->Is3DLoaded() || a->IsDead()) return;
        const float d = actor->GetPosition().GetDistance(a->GetPosition());
        if (d > bestDistance) return;
        if (PairTargetMath::AbsoluteHeading(actor->GetHeadingAngle(a->GetPosition(), false)) > angle * 0.5f) return;
        best = a;
        bestDistance = d;
    };
    for (auto& h : lists->highActorHandles) consider(h.get().get());
    // The player is not guaranteed to be in the high-process list (as FindPairedTarget).
    consider(RE::PlayerCharacter::GetSingleton());
    return best ? best->GetHandle() : RE::ActorHandle{};
}

RE::ActorHandle ActionRunner::CapturePairedTarget(RE::ActorHandle existing) const {
    if (existing.native_handle() != 0) return existing;
    return PairedTargetFor(Owner(), _idx);
}
RE::ActorHandle ActionRunner::PairedTargetFor(RE::Actor* actor, int action) {
    if (!actor || !actor->Is3DLoaded() || actor->IsDead() || action < 0 || action >= Actions::Count()) return {};
    const auto& paired = Actions::Spec(action).paired;
    auto* target = FindPairedTarget(actor, paired.range, paired.angle, paired.targetRig);
    return target ? target->GetHandle() : RE::ActorHandle{};
}
bool ActionRunner::ValidPairedTarget(RE::ActorHandle handle) const {
    auto* actor = Owner();
    auto target = handle.get();
    return actor && actor->Is3DLoaded() && target && target.get() != actor && target->Is3DLoaded() &&
           !target->IsDead() && TargetUpright(target.get()) && !target->IsChild() &&
           IsPairedRig(target.get(), Spec().paired.targetRig) &&
           PairTargetMath::WithinRangeAndCone(actor->GetPosition().GetDistance(target->GetPosition()),
               Spec().paired.range, actor->GetHeadingAngle(target->GetPosition(), false), Spec().paired.angle);
}
bool ActionRunner::CanRequestPairLink(RE::ActorHandle target) const {
    if (!CanRequestLink() || !ValidPairedTarget(target) || !GraphInjector::HasPairedRoutes(_idx)) return false;
    // A pair -> carry link cancels its source pair before the carry is requested, so the carry's own
    // conditions (player carrier, weapon rule, not airborne) are checked here, while the source plays on.
    if (!Spec().carry.enabled) return true;
    auto partner = target.get();
    return Carry::CanBegin(_idx, Owner(), partner.get());
}
bool ActionRunner::RequestPaired(RE::ActorHandle boundTarget) {
    auto* actor = Owner();
    if (!Conditions(actor)) return false;
    if (!IsPairedRig(actor, PairedRig::Kind::kCharacter)) {
        logger::warn("[PairRig] '{}' requires a human initiator", Spec().id);
        return false;
    }
    auto retained = boundTarget.get();
    const bool explicitTarget = boundTarget.native_handle() != 0;
    auto* target = explicitTarget
                       ? retained.get()
                       : FindPairedTarget(actor, Spec().paired.range, Spec().paired.angle, Spec().paired.targetRig);
    if (explicitTarget && !ValidPairedTarget(boundTarget)) {
        // Name the failed check. Distance and heading are only meaningful for a loaded target.
        const bool loaded = target && target->Is3DLoaded();
        const auto position = loaded ? target->GetPosition() : RE::NiPoint3{};
        logger::warn(
            "[PairB] captured event target unavailable/outside range or cone: handle={:08X} loaded={} dead={} ragdoll={} child={} rig={} distance={:.1f}/{:.0f} heading={:.1f}/{:.0f}; request cancelled",
            boundTarget.native_handle(), loaded, loaded && target->IsDead(), loaded && target->IsInRagdollState(),
            loaded && target->IsChild(), loaded && IsPairedRig(target, Spec().paired.targetRig),
            loaded ? actor->GetPosition().GetDistance(position) : -1.0f, Spec().paired.range,
            loaded ? actor->GetHeadingAngle(position, false) : 0.0f, Spec().paired.angle * .5f);
        return false;
    }
    auto* data = RE::TESDataHandler::GetSingleton();
    auto* idle = data ? data->LookupForm<RE::TESIdleForm>(0xBDE2, "Dawnguard.esm") : nullptr;
    if (!target) {
        logger::warn("[PairB] 대상 없음: 캐릭터 몸 기준 전방 각도 ±{:.0f}도, 거리 {:.0f} 이내 필요",
            Spec().paired.angle * 0.5f, Spec().paired.range);
        return false;
    }
    if (Carry::BlocksAction(target)) return false;
    if (Spec().carry.enabled && !Carry::CanBegin(_idx, actor, target)) return false;
    if (!idle) {
        logger::warn("[PairB] Dawnguard 전방 페어 IDLE 없음");
        return false;
    }
    if (!GraphInjector::HasPairedRoutes(_idx)) {
        bool support = false, lead = false;
        for (const auto& node : GraphInjector::SyncNodes())
            if (node.actionIdx == _idx) {
                support |= node.variant == 1;
                lead |= node.variant == 0;
            }
        logger::warn(
            "[PairB] '{}' paired routes missing: actor={:08X} target={:08X} targetRig={} support={} lead={}; native start not attempted",
            Spec().id, actor->GetFormID(), target->GetFormID(), PairedRig::Name(Spec().paired.targetRig), support,
            lead);
        return false;
    }
    // The engine owns forms. Keep only the runtime form ID between requests.
    static std::unordered_map<int, RE::FormID> idleForms;
    auto* injectedIdle =
        idleForms.contains(_idx) ? RE::TESForm::LookupByID<RE::TESIdleForm>(idleForms.at(_idx)) : nullptr;
    if (!injectedIdle) {
        injectedIdle = RE::IFormFactory::Create<RE::TESIdleForm>();
        if (!injectedIdle) return false;
        injectedIdle->data = idle->data;
        injectedIdle->animFileName = idle->animFileName;
        injectedIdle->animEventName = Fixed(Spec().clips[1].enterEvent);
        // CanUseIdle validates the root ancestor. Use the established vanilla
        // ancestry without editing the shared idle list or the original form.
        injectedIdle->parentIdle = idle;
        idleForms[_idx] = injectedIdle->GetFormID();
        logger::debug("[PairB] runtime idle={:08X} parent={:08X} event='{}' flags={} group={}",
            injectedIdle->GetFormID(), idle->GetFormID(), injectedIdle->animEventName.c_str(),
            static_cast<int>(injectedIdle->data.flags.get()), injectedIdle->data.animationGroupSelection);
    }
    EventMaps::Ensure(actor, Actions::AllEventNames());
    EventMaps::Ensure(target, Actions::AllEventNames());
    PairedSession::Request request;
    request.actionIndex = _idx;
    request.initiator = actor->GetHandle();
    request.target = target->GetHandle();
    request.idleForm = injectedIdle->GetFormID();
    request.playbackTimeout = std::max(15.0f, Spec().safetyDuration);
    PairedSession::Ticket ticket;
    const bool accepted = PairedSession::Submit(request, ticket);
    if (accepted && Spec().carry.enabled) Carry::PairStarted(_idx, PairedSession::ReadStatus());
    return accepted;
}

void ActionRunner::Adopt(int a_variant) {
    _interrupted = false;
    auto* p = Owner();
    if (!p || !p->Is3DLoaded() || p->IsDead() || p->IsInRagdollState() || a_variant < 0 ||
        a_variant >= static_cast<int>(Spec().clips.size()))
        return;
    ++_playbackSerial;
    _normalExitMovementAccepted = false;
    if (!AnimData::Registered()) AnimData::TryRegister("Adopt");
    _variant = a_variant;
    _elapsed = 0.0f;
    _carryReleaseDue = false;
    _exitSent = false;
    _endReached = _releaseForced = false;
    _clipDuration = 0.0f;
    _exitingTime = 0.0f;
    _seenTags.clear();
    ResetPlaybackFlags();
    _postCancelTimer = 0.0f;
    _state = State::kActive;
    auto* controller = p->GetCharController();
    BeginAirPhysics(Spec().condition.inAir != 0 && controller &&
                    controller->context.currentState == RE::hkpCharacterStateType::kInAir);
    LogAirPhysics(AirPhase::kAdopt);
    SetAnimationDriven(true);
    ApplyAttackData();
    ApplyRotate(false);
    SlotUpdate();
    logger::debug("[{}] 외부 진입 채택: 액터 {:08X} 변형 {}('{}')", Spec().id, p->GetFormID(), a_variant,
        Spec().clips[a_variant].tag);
}

// ── 슬롯 ─────────────────────────────────────────────────────────────────────
void ActionRunner::ApplyRotate(bool a_on) {
    if (Spec().rotationLocked) a_on = false;
    if (auto* actor = Owner(); actor && actor->Is3DLoaded()) {
        if (!_rotationLockOwned)
            _rotationLockOwned = actor->GetGraphVariableBool(VarAllowRotation(), _rotationBeforeLock);
        actor->SetGraphVariableBool(VarAllowRotation(), a_on);
    }
    if (_rotateOn != a_on) logger::debug("[{}] Rotate {} at {:.2f}s", Spec().id, a_on ? "ON" : "OFF", _elapsed);
    _rotateOn = a_on;
}

// IFrame: 이 액터만 유령 상태로 만든다 (Papyrus SetGhost 와 같은 엔진 상태 — 무기·주문이 통과, 자기는 공격 가능).
//   세이브에 남는 상태라 모든 종료 경로에서 되돌리고 (RestoreSlots / ResetOnLoad), 코세이브가 로드 뒤 되돌린다 (runtime/ActorImmunity.h).
void ActionRunner::ApplyIFrame(bool a_on) {
    if (_iframeOn == a_on) return;
    if (a_on)
        _immunity = ActorImmunity::Acquire(Owner());
    else
        ActorImmunity::Release(_immunity);
    _iframeOn = a_on && _immunity != 0;
}

void ActionRunner::ApplyCost() {
    if (_costDone) return;
    _costDone = true;
    const auto& c = Spec().cost;
    auto* p = Owner();
    if (!p || !p->AsActorValueOwner()) return;
    if (c.stamina > 0.0f)
        p->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -c.stamina);
    if (c.magicka > 0.0f)
        p->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -c.magicka);
    logger::debug("[{}] Cost stamina {:.0f} magicka {:.0f} at {:.2f}s", Spec().id, c.stamina, c.magicka, _elapsed);
}

// 공격 의미: 종족 BGSAttackDataMap 에서 이름으로 찾아 HighProcessData::attackData 에 넣는다.
//   엔진은 attackStart 류 이벤트를 받을 때 같은 일을 하지만 우리 진입 이벤트는 그 경로를 안 탄다 (실측: attackData 항상 비어 있음).
//   Precision 은 attackData 가 없으면 종족 정의 대신 DEFAULT(작은 캡슐)로 충돌을 만든다.
void ActionRunner::ApplyAttackData() {
    const auto& spec = Spec();
    if (!spec.attack.enabled) return;
    const std::string& name = spec.attack.data;
    auto* p = Owner();
    if (!p || !p->Is3DLoaded()) return;
    auto* proc = p->GetActorRuntimeData().currentProcess;
    auto* high = proc ? proc->high : nullptr;
    auto* race = p->GetRace();
    if (!high || !race || !race->attackDataMap) {
        logger::warn("[{}] Attack: high/race/attackDataMap 없음", spec.id);
        return;
    }
    auto& map = race->attackDataMap->attackDataMap;
    auto it = map.find(Fixed(name));
    if (it == map.end() || !it->second) {
        std::string keys;
        for (const auto& e : map) {
            if (!keys.empty()) keys += ", ";
            keys += e.first.c_str() ? e.first.c_str() : "?";
        }
        logger::warn("[{}] Attack: 종족 '{}' 에 공격 데이터 '{}' 없음. 있는 이름: {}", spec.id, race->GetFormEditorID(),
            name, keys);
        return;
    }
    if (high->attackData != _attackSet || !_attackSet)
        _attackPrev = high->attackData;  // 앞 재생이 넣은 우리 것이 남아 있으면 그 전 값 유지
    _attackSet = it->second;
    high->attackData = _attackSet;
    const auto& d = _attackSet->data;
    logger::debug(
        "[{}] Attack: attackData ← '{}' (dmgMult {:.2f} flags 0x{:X} angle {:.0f}/{:.0f} stagger {:.2f} type {})",
        spec.id, name, d.damageMult, d.flags.underlying(), d.attackAngle, d.strikeAngle, d.staggerOffset,
        d.attackType ? d.attackType->GetFormEditorID() : "-");
}

void ActionRunner::RestoreAttackData() {
    if (!_attackSet) return;
    auto* p = Owner();
    auto* proc = p ? p->GetActorRuntimeData().currentProcess : nullptr;
    auto* high = proc ? proc->high : nullptr;
    if (high && high->attackData == _attackSet) {  // 아직 우리 것이면 되돌린다 (캔슬 아웃으로 엔진이 바꿨으면 그대로)
        high->attackData = _attackPrev;
        logger::debug(
            "[{}] Attack: attackData 복원 → '{}'", Spec().id, _attackPrev ? _attackPrev->event.c_str() : "(없음)");
    }
    _attackSet.reset();
    _attackPrev.reset();
}

void ActionRunner::ReleaseRotation() {
    if (_rotationLockOwned) {
        if (auto* actor = Owner(); actor && actor->Is3DLoaded()) {
            bool transferred = false;
            if (auto *next = Actions::FindRunner(actor, [&](int action, ActionRunner& r) {
                    return action != _idx && r._state != State::kIdle && r._state != State::kBlendingOut;
                })) {
                next->_rotationBeforeLock = _rotationBeforeLock;
                next->_rotationLockOwned = true;
                transferred = true;
            }
            PairedSession::Ticket ticket;
            // The destination state owns the mode from here; the start value is kept only for the log.
            if (!transferred && !PairedSession::TryGetTicket(_owner, ticket)) {
                HandBackRotation(
                    _rotationLockOwned, _rotateOn,
                    [&](bool& value) { return actor->GetGraphVariableBool(VarAllowRotation(), value); },
                    [&](bool value) { actor->SetGraphVariableBool(VarAllowRotation(), value); });
            }
            bool after = false;
            const bool readable = actor->GetGraphVariableBool(VarAllowRotation(), after);
            logger::debug("[{}] RotationRelease original={} last={} transferred={} readable={} after={}", Spec().id,
                _rotationBeforeLock, _rotateOn, transferred, readable, after);
        }
        _rotationLockOwned = false;
    }
    _rotateOn = false;
}

void ActionRunner::PrepareExit(float blend) {
    Qte::End(_owner, _idx);
    _recoveryMovement = false;
    _exitSent = true;
    _state = State::kExiting;
    _exitingTime = 0;
    _exitTimeout = ExitWatchdog(blend);
}

bool ActionRunner::InterruptIfUnavailable() {
    auto* actor = Owner();
    const bool unavailable = !actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState();
    if (!unavailable) return false;
    _postCancelTimer = 0;
    if (_state == State::kIdle) return true;
    logger::debug("[{}] Interrupted actor={:08X} dead={} ragdoll={}", Spec().id, actor ? actor->GetFormID() : 0,
        actor && actor->IsDead(), actor && actor->IsInRagdollState());
    _interrupted = true;
    _recoveryOpen = false;
    _npcHandoff = false;
    _npcHandoffExit = false;
    _exitSent = false;
    // Physics owns the graph now. Do not send Idle/JumpFall or clear its driven flag.
    _animDrivenSet = false;
    Finish();
    return true;
}

void ActionRunner::RestoreSlots() {
    RestoreAttackData();
    ReleaseRotation();
    if (_iframeOn) ApplyIFrame(false);
    if (_hyperOn) {
        _hyperOn = false;
        logger::debug("[{}] HyperArmor OFF (종료)", Spec().id);
    }
}

// 시각 기반 슬롯: on/off 가 @시각인 것
void ActionRunner::SlotUpdate() {
    if (_state != State::kActive) return;
    const auto& spec = Spec();
    // A loop has no end: "until end" stays on (charge, 10-30).
    const float dur = spec.loop.enabled ? 1.0e9f : _clipDuration > 0.05f ? _clipDuration : spec.safetyDuration;
    const SlotWindow* wins[3] = {&spec.rotate, &spec.iframe, &spec.hyperArmor};
    for (int k = 0; k < 3; ++k) {
        const auto& w = *wins[k];
        auto& st = _slot[k];
        if (!w.enabled) continue;
        bool turnOn = false, turnOff = false;
        if (!st.onSeen && w.onAt >= 0.0f && _elapsed >= w.onAt) turnOn = true;
        if (_state != State::kActive || InterruptIfUnavailable()) return;
        if (turnOn) st.onSeen = true;
        if (st.onSeen && !st.offSeen) {
            const float offT = w.offRel ? dur - w.offAt : w.offAt;
            if (w.offAt >= 0.0f && _elapsed >= offT) turnOff = true;
        }
        if (_state != State::kActive || InterruptIfUnavailable()) return;
        if (turnOff) st.offSeen = true;
        const bool on = st.onSeen && !st.offSeen;
        switch (k) {
        case 0:
            if (on != _rotateOn) ApplyRotate(on);
            break;
        case 1:
            if (on != _iframeOn) ApplyIFrame(on);
            break;
        case 2:
            if (on != _hyperOn) {
                _hyperOn = on;
                logger::debug("[{}] HyperArmor {} at {:.2f}s", spec.id, on ? "ON" : "OFF", _elapsed);
            }
            break;
        }
    }
    if (spec.cost.enabled && !_costDone && spec.cost.at >= 0.0f && _elapsed >= spec.cost.at) ApplyCost();
    // An ordinary action's clip-time effects (0.12.274); a pair's run in PairFeatures.
    if (const auto& timed = spec.pairFeatures.timed; !spec.paired.enabled && !timed.empty()) {
        if (_timedFired.size() != timed.size()) _timedFired.assign(timed.size(), false);
        for (std::size_t i = 0; i < timed.size(); ++i)
            if (!_timedFired[i] && _elapsed >= timed[i].at) {
                _timedFired[i] = true;
                RunTimedEffect(timed[i].function);
                if (_state != State::kActive) return;
            }
    }
    if (spec.carryRelease.enabled && !_carryReleaseDue && spec.carryRelease.at >= 0.0f &&
        _elapsed >= spec.carryRelease.at)
        ReachCarryRelease("time");
}

void ActionRunner::RunTimedEffect(const EventFunctionSpec& function) {
    auto* actor = Owner();
    if (!actor) return;
    const auto context = EventEffects::ContextFor(actor, "action clip time");
    EventEffects::RunFor(function, context, {actor->GetHandle()});
    logger::debug("[{}] clip-time effect op={} at {:.2f}s", Spec().id, static_cast<int>(function.op), _elapsed);
}

void ActionRunner::ReachCarryRelease(const char* why) {
    _carryReleaseDue = true;
    logger::info("[{}] CarryRelease reached by {} at {:.2f}s", Spec().id, why, _elapsed);
}

const ActionRunner::EventTags& ActionRunner::Tags() {
    if (!_tags) {
        const auto& spec = Spec();
        auto tags = std::make_unique<EventTags>();
        const SlotWindow* wins[3] = {&spec.rotate, &spec.iframe, &spec.hyperArmor};
        for (std::size_t k = 0; k < 3; ++k) {
            tags->slotOn[k] = RE::BSFixedString(wins[k]->onEvent.c_str());
            tags->slotOff[k] = RE::BSFixedString(wins[k]->offEvent.c_str());
        }
        tags->cost = RE::BSFixedString(spec.cost.event.c_str());
        tags->carryRelease = RE::BSFixedString(spec.carryRelease.event.c_str());
        tags->exit = RE::BSFixedString(spec.events.exit.c_str());
        for (const auto& w : spec.cancels) {
            tags->open.emplace_back(w.openEvent.c_str());
            tags->close.emplace_back(w.closeEvent.c_str());
        }
        for (std::size_t v = 0; v < spec.clips.size(); ++v) tags->recovery.emplace_back(spec.recovery.event.c_str());
        _tags = std::move(tags);
    }
    return *_tags;
}

// 이벤트 기반 슬롯 (HKX 주석)
void ActionRunner::SlotEvent(const RE::BSFixedString& a_tag) {
    if (_state != State::kActive) return;
    const auto& spec = Spec();
    const auto& tags = Tags();
    const SlotWindow* wins[3] = {&spec.rotate, &spec.iframe, &spec.hyperArmor};
    for (int k = 0; k < 3; ++k) {
        const auto& w = *wins[k];
        auto& st = _slot[k];
        if (!w.enabled) continue;
        if (!st.onSeen && a_tag == tags.slotOn[k])
            st.onSeen = true;
        else if (st.onSeen && !st.offSeen && a_tag == tags.slotOff[k])
            st.offSeen = true;
        else
            continue;
        const bool on = st.onSeen && !st.offSeen;
        switch (k) {
        case 0: ApplyRotate(on); break;
        case 1: ApplyIFrame(on); break;
        case 2:
            _hyperOn = on;
            logger::debug("[{}] HyperArmor {} (주석) at {:.2f}s", spec.id, on ? "ON" : "OFF", _elapsed);
            break;
        }
    }
    if (spec.cost.enabled && !_costDone && a_tag == tags.cost) ApplyCost();
    if (spec.carryRelease.enabled && !_carryReleaseDue && !spec.carryRelease.event.empty() &&
        a_tag == tags.carryRelease)
        ReachCarryRelease("event");
}

namespace {
// Clips whose HKX was seen without AMR motion (main thread, clip activation): a clip does not move unless its
// HKX carries "animmotion" annotations.
std::unordered_set<std::string> g_stillClips;

// A weapon draw or sheathe asked while an action played (the R key, an NPC's AI) is sent to the graph as WeapEquip or
// Unequip; our state has no transition for it, so the engine is left wanting to draw or sheathe with no animation and
// draws again on its own seconds later (user report 10-26: R during a sprint stop). Back in the vanilla graph the
// request still waiting is sent again, so it plays now, as after a vanilla attack.
void ReplayWeaponRequest(RE::Actor* actor, std::string_view action) {
    auto* state = actor && actor->Is3DLoaded() && !actor->IsDead() ? actor->AsActorState() : nullptr;
    if (!state) return;
    static const RE::BSFixedString unequip("Unequip"), equip("WeapEquip");
    const auto weapon = state->GetWeaponState();
    const RE::BSFixedString* event = weapon == RE::WEAPON_STATE::kWantToSheathe ? &unequip
                                     : weapon == RE::WEAPON_STATE::kWantToDraw  ? &equip
                                                                                : nullptr;
    if (!event) return;
    const bool sent = actor->NotifyAnimationGraph(*event);
    logger::info("[WeaponRequest] action='{}' actor={:08X} weaponState={} -> '{}' sent={}", action, actor->GetFormID(),
        static_cast<int>(weapon), event->c_str(), sent);
}
}

void ActionRunner::BeginAirPhysics(bool a_inAir) {
    _startedInAir = a_inAir;
    // A clip that does not move keeps the jump's own movement in the air. Known from an earlier activation;
    // the first one corrects it in OnClipActivated.
    _preserveAirPhysics = a_inAir && g_stillClips.contains(Spec().clips[_variant].clipName);
    _airPhysicsAge = 0.0f;
    _airPhysicsNextSample = 0.0f;
    _airPhysicsSamples = 0;
}

void ActionRunner::LogAirPhysics(AirPhase a_phase) const {
    if (!FrameworkSettings::Get().debug) return;
    if (a_phase != AirPhase::kFrame) {
        auto* actor = Owner();
        auto* controller = actor && actor->Is3DLoaded() ? actor->GetCharController() : nullptr;
        if (controller) {
            bool driven = false;
            const bool readable = actor->GetGraphVariableBool(VarAnimDriven(), driven);
            logger::debug(
                "[CollisionState] action='{}' actor={:08X} phase={} flags={:08X} noCharacters={} drivenRead={} driven={} radius={} height={}",
                Spec().id, actor->GetFormID(), static_cast<int>(a_phase),
                static_cast<std::uint32_t>(controller->flags.get()),
                controller->flags.any(RE::CHARACTER_FLAGS::kNoCharacterCollisions), readable, driven,
                controller->radius, controller->height);
        }
    }
    if (!_startedInAir) return;
    auto* actor = Owner();
    if (!actor || !actor->Is3DLoaded()) return;
    auto* controller = actor->GetCharController();
    if (!controller) return;
    RE::hkVector4 velocity;
    controller->GetLinearVelocityImpl(velocity);
    std::array<float, 4> components{};
    _mm_storeu_ps(components.data(), velocity.quad);
    bool driven = false, jumping = false;
    const bool drivenRead = actor->GetGraphVariableBool(VarAnimDriven(), driven);
    const bool jumpRead = actor->GetGraphVariableBool(VarInJump(), jumping);
    static constexpr std::array<std::string_view, 6> phases{
        "beforeEntry", "afterEntry", "afterSetup", "frame", "finish", "adopt"};
    // Velocity is reported in controller units, without converting to world units.
    logger::debug(
        "[AirPhysics] '{}' actor={:08X} phase={} t={:.3f} preserve={} state={} z={:.3f} velocity=({:.4f},{:.4f},{:.4f}) gravity={:.4f} jumpRead={} inJump={} drivenRead={} driven={}",
        Spec().id, actor->GetFormID(), phases[static_cast<std::size_t>(a_phase)], _airPhysicsAge, _preserveAirPhysics,
        static_cast<int>(controller->context.currentState), actor->GetPosition().z, components[0], components[1],
        components[2], controller->gravity, jumpRead, jumping, drivenRead, driven);
}

void ActionRunner::SetAnimationDriven(bool a_on, bool a_finalRelease) {
    // Pose-only air actions must not take ownership of native jump movement.
    if (a_on && _preserveAirPhysics) return;
    auto* p = Owner();
    if (!p || !p->Is3DLoaded()) {
        _animDrivenSet = false;
        return;
    }
    const bool owned = _animDrivenSet;
    bool before = false;
    const bool read = !a_on && a_finalRelease && owned && p->GetGraphVariableBool(VarAnimDriven(), before);
    const bool ok = WriteOwnedDrivenFlag(_animDrivenSet, a_on, a_finalRelease,
        [&](bool value) { return p->SetGraphVariableBool(VarAnimDriven(), value); });
    if (!a_on && a_finalRelease && owned)
        logger::debug("[{}] DrivenRelease actor={:08X} final=true readable={} before={} accepted={}", Spec().id,
            p->GetFormID(), read, before, ok);
}

void ActionRunner::OnClipActivated(int a_variant, float a_duration, bool a_moves) {
    if (a_variant >= 0 && a_variant < static_cast<int>(Spec().clips.size())) {
        const auto& name = Spec().clips[a_variant].clipName;
        if (a_moves)
            g_stillClips.erase(name);
        else
            g_stillClips.insert(name);
    }
    if (_state != State::kActive) return;
    // An in-air start of a clip seen still for the first time: hand the movement back to the jump.
    if (_startedInAir && !a_moves && !_preserveAirPhysics && a_variant == _variant) {
        SetAnimationDriven(false, true);
        _preserveAirPhysics = true;
    }
    _motionTraceNext = 0;
    _motionTraceCount = 0;
    _clipTime = -1.0f;
    _clipSpeed.store(0.0f);
    ActionLinks::Begin(_owner, _idx, false, _isPlayer);
    if (a_duration > 0.05f) {
        _clipDuration = a_duration;
        AnimData::SetClipDuration(Spec().clips[_variant].clipName.c_str(), a_duration);
    }
    if (_linkSource >= 0) {
        const int from = _linkSource;
        auto* source = Actions::RunnerFor(Owner(), from, false);
        if (source && source->_linkTarget == _idx) {
            source->RestoreSlots();
            if (_preserveAirPhysics) source->SetAnimationDriven(false, true);
            source->_animDrivenSet = false;
            source->_postCancelTimer = 0;
            source->_exitSent = false;
            const bool retired = source->_linkRetirement.Commit();
            ActionLinks::End(_owner, from);
            if (retired) source->Finish();
        }
        _linkSource = -1;
        SetupPlayback();
        logger::debug("[DirectLink] actor={:08X} from={} to={} activated=true idleWait=0",
            Owner() ? Owner()->GetFormID() : 0, from, _idx);
    }
    _idleSettled = false;  // the idle safety net ends this QTE again once the runner is idle
    Qte::Begin(_owner, _idx, {}, _elapsed);
    if (_isPlayer) PairFeatures::BeginActionCamera(_owner, _idx);
    logger::debug("[{}] 클립 활성 변형 {} 길이 {:.3f}s → 보험 {:.2f}s", Spec().id, _variant, a_duration, SafetyTime());
}

float ActionRunner::SafetyTime() const noexcept {
    // A loop ends by its maxHold (charge, 10-30); the safety exit is only a backstop past it.
    if (Spec().loop.enabled) return Spec().loop.maxHold + 1.0f;
    return _clipDuration > 0.05f ? _clipDuration + 0.15f : Spec().safetyDuration;
}

void ActionRunner::OnClipDeactivated(int a_variant) {
    if (InterruptIfUnavailable()) return;
    if (_state == State::kBlendingOut) {
        if (a_variant != _variant) return;
        if (_linkRetirement.Deactivate()) Finish();
        return;
    }
    if (_state == State::kIdle) return;
    if (a_variant != _variant) {  // another role's clip (a pair): this runner's own clip goes on
        logger::debug("[{}] 다른 변형의 클립 비활성 (변형 {}, 현재 {})", Spec().id, a_variant, _variant);
        return;
    }
    logger::debug("[{}] 클립 비활성 at {:.2f}s — {} 경로", Spec().id, _elapsed,
        _exitSent ? "Exit" : "다른 전이(캔슬 아웃/스태거)");
    if (!_exitSent && _animDrivenSet) {
        _postCancelTimer = 5.0f;
        _postCancelTick = 0.0f;
    }
    if (_nativeExiting && _exitSent) {
        RE::BSFixedString replay;
        {
            std::lock_guard lock(_nativeLock);
            replay = _nativeReplay;
        }
        if (replay.c_str() && *replay.c_str()) Actions::QueueNativeReplay(_owner, replay);
    }
    Finish();
}

// 캔슬 아웃 뒤: 다음 행동(공격)이 끝났는데 bAnimationDriven 이 켜져 있으면 끈다 (BFCO 공격은 우리 상태에서 진입하면 끝에 안 내림 — 실측)
void ActionRunner::PostCancelWatch(float a_delta) {
    if (_postCancelTimer <= 0.0f) return;
    if (_state != State::kIdle) {
        _postCancelTimer = 0.0f;
        return;
    }
    _postCancelTimer -= a_delta;
    _postCancelTick += a_delta;
    auto* p = Owner();
    if (!p || !p->Is3DLoaded() || p->IsDead() || p->IsInRagdollState() ||
        (p->AsActorState() && p->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)) {
        _postCancelTimer = 0.0f;
        return;
    }
    // This watcher belongs to a previous ordinary action. A new action or
    // paired session owns its own cleanup; never write its driven flag.
    PairedSession::Ticket ticket;
    const bool transferred =
        PairedSession::TryGetTicket(_owner, ticket) || Actions::FindRunner(p, [&](int action, ActionRunner& r) {
            return action != _idx && r.GetState() != State::kIdle;
        });
    if (transferred) {
        _postCancelTimer = 0.0f;
        _postCancelTick = 0.0f;
        logger::debug("[{}] cancel watcher retired: actor={:08X} control transferred; driven flag preserved", Spec().id,
            p->GetFormID());
        return;
    }
    bool anim = false;
    p->GetGraphVariableBool(VarAnimDriven(), anim);
    const int attackState = p->AsActorState() ? static_cast<int>(p->AsActorState()->GetAttackState()) : -1;
    if (attackState == 0 && anim) {
        p->SetGraphVariableBool(VarAnimDriven(), false);
        logger::debug("[{}] 캔슬아웃 후 복구: 공격 종료 시 bAnimationDriven 이 켜져 있어 끔", Spec().id);
        _postCancelTimer = 0.0f;
    }
}

void ActionRunner::ResetPlaybackFlags() {
    ClearNativeCapture();
    _nativeExiting = false;
    _openSeen.assign(Spec().cancels.size(), false);
    _closeSeen.assign(Spec().cancels.size(), false);
    _exitSeen = false;
    _recoveryOpen = false;
    _npcHandoff = false;
    _npcHandoffExit = false;
    for (auto& st : _slot) st = SlotState{};
    _costDone = false;
    _timedFired.assign(Spec().pairFeatures.timed.size(), false);
    // 새 재생이 시작되면 켜져 있던 슬롯은 새 재생의 창으로 다시 판정한다
    if (_rotateOn) ApplyRotate(false);
    if (_iframeOn) ApplyIFrame(false);
    _hyperOn = false;
}

// Recovery 가 열림: 이미 이동키를 누르고 있으면 즉시 나간다 (MCO 는 새로 눌러야 하지만 우리는 누른 상태도 인정)
void ActionRunner::OpenRecovery(const char* a_why) {
    if (_recoveryOpen || _state != State::kActive) return;
    if (!_isPlayer) {
        // An NPC in combat: movement back to its AI from here, the clip playing on (runtime/NpcMoveCancel.h).
        auto* actor = Owner();
        if (!Spec().recovery.move || !actor || !actor->IsInCombat()) return;
        _recoveryOpen = true;
        _npcHandoff = true;
        _npcHandoffExit = false;
        actor->SetGraphVariableBool(VarAnimDriven(), false);
        logger::debug("[NpcMoveCancel] {} actor={:08X}: movement to the AI at {:.2f}s ({})", Spec().id,
            actor->GetFormID(), _elapsed, a_why);
        return;
    }
    _recoveryOpen = true;
    const auto& r = Spec().recovery;
    const bool held = r.move && InputSink::Get()->AnyMoveInput();
    logger::debug("[{}] Recovery 열림 ({}) at {:.2f}s — 이동키 {}", Spec().id, a_why, _elapsed,
        held ? "누르고 있음 → 즉시" : "대기");
    if (held) Recover("이미 누른 이동키");
}

void ActionRunner::Recover(const char* a_why) {
    if (_state != State::kActive || _exitSent) return;
    auto* p = Owner();
    if (!p) return;
    if (InterruptIfUnavailable()) return;
    const bool ok =
        RequestPreparedExit([&] { PrepareExit(Spec().recovery.blend >= 0 ? Spec().recovery.blend : Spec().exitBlend); },
            [&] { return p->NotifyAnimationGraph(Fixed(Spec().recovery.recoverEvent)); },
            [&] { return _state == State::kExiting; },
            [&] {
                _recoveryMovement = true;
                _recoveryAttackCleared = false;
                // Hand facing back to locomotion while the source pose blends out.
                ReleaseRotation();
                MaintainRecoveryMovement();
            },
            [&] {
                _state = State::kActive;
                _exitSent = false;
            });
    logger::debug("[{}] Recovery → 이동 ({}) at {:.2f}s notify={}", Spec().id, a_why, _elapsed, ok);
}

bool ActionRunner::BeginNormalExitMovement(std::uint64_t serial) {
    if (serial != PlaybackSerial() || !_isPlayer || Spec().paired.enabled || _state != State::kExiting ||
        _nativeExiting || _interrupted || _linkSource >= 0 || _linkTarget >= 0 || _normalExitMovementAccepted)
        return false;
    _normalExitMovementAccepted = true;
    auto* actor = Owner();
    auto* state = actor ? actor->AsActorState() : nullptr;
    PairedSession::Ticket pair;
    if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() || !state ||
        state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || state->IsStaggered() || state->IsBleedingOut() ||
        PairedSession::TryGetTicket(_owner, pair))
        return false;
    if (Actions::FindRunner(actor, [&](int action, ActionRunner& r) {
            return action != _idx && r.GetState() != State::kIdle && r.GetState() != State::kBlendingOut;
        }))
        return false;
    _recoveryMovement = true;
    _recoveryAttackCleared = false;
    if (InterruptIfUnavailable()) return false;
    ReleaseRotation();
    MaintainRecoveryMovement();
    logger::debug(
        "[ExitBlendMove] action='{}' movement={} preserveBlend={:.3f}", Spec().id, _recoveryMovement, Spec().exitBlend);
    return _recoveryMovement;
}

void ActionRunner::MaintainRecoveryMovement() {
    if (!_recoveryMovement || _state != State::kExiting) return;
    auto* actor = Owner();
    auto* actorState = actor ? actor->AsActorState() : nullptr;
    const bool attacking = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
    PairedSession::Ticket ticket;
    bool transferred = !actor || !actor->Is3DLoaded() || !actorState || actor->IsDead() || actor->IsInRagdollState() ||
                       actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || actorState->IsStaggered() ||
                       actorState->IsBleedingOut() || (_recoveryAttackCleared && attacking) ||
                       PairedSession::TryGetTicket(_owner, ticket);
    // The source attack may still report its old attack state on the acceptance frame.
    if (!attacking) _recoveryAttackCleared = true;
    transferred = transferred || Actions::FindRunner(actor, [&](int action, ActionRunner& r) {
        return action != _idx && r.GetState() != State::kIdle && r.GetState() != State::kBlendingOut;
    });
    MaintainRecoveredDrivenFlag(_recoveryMovement, _animDrivenSet, !transferred,
        [&](bool value) { return actor->SetGraphVariableBool(VarAnimDriven(), value); });
    // Both animation mode flags must be off for native motion-driven movement.
    // A true bAllowRotation would keep the engine in StartAllowRotation mode.
    MaintainRecoveredRotation(
        _recoveryMovement, [&](bool& value) { return actor->GetGraphVariableBool(VarAllowRotation(), value); },
        [&](bool value) { return actor->SetGraphVariableBool(VarAllowRotation(), value); });
}

bool ActionRunner::NpcHandoffEvent(const RE::BSFixedString& a_event) {
    if (!_npcHandoff || _state != State::kActive || !a_event.c_str()) return false;
    const bool request = NpcMoveCancel::IsRequest(a_event.c_str());
    if (request) _npcHandoffExit = true;
    return request;
}

void ActionRunner::OnMoveInput(int a_kind) {
    if (!_isPlayer || _state != State::kActive || !_recoveryOpen) return;
    const auto& r = Spec().recovery;
    if ((a_kind == 0 && r.move) || (a_kind == 1 && r.sprint) || (a_kind == 2 && r.jump))
        Recover(a_kind == 0 ? "이동 입력" : a_kind == 1 ? "스프린트" : "점프");
}

// Recovery declared at a clip time opens on the runner's timer (an event one waits for its annotation only).
void ActionRunner::RecoveryClock() {
    if (_state != State::kActive) return;
    const auto& r = Spec().recovery;
    if (r.enabled && r.at >= 0.0f && !_recoveryOpen && _elapsed >= r.at) OpenRecovery("시각");
}

void ActionRunner::Update(float a_delta) {
    if (!std::isfinite(a_delta) || a_delta < 0) return;
    // An idle runner with nothing counting down has no work once one idle update ran its cleanup. The
    // per-frame walk visits every runner ever created, and input and target probes create one per
    // candidate action: with 5000 declarations the idle ones cost 100-150 us a frame (0.12.214).
    if (_state == State::kIdle && _idleSettled && _cooldown <= 0.0f && _linkSource < 0 && _postCancelTimer <= 0.0f)
        return;
    _idleSettled = _state == State::kIdle;
    if (_cooldown > 0.0f) _cooldown = std::max(0.0f, _cooldown - a_delta);
    if (InterruptIfUnavailable()) return;

    MaintainRecoveryMovement();
    // An NPC's move cancel (runtime/NpcMoveCancel.h): movement stays with the AI (the clip's state may set the flag
    // back), and an AI request heard since ends the action.
    if (_npcHandoff && _state == State::kActive) {
        if (auto* actor = Owner()) actor->SetGraphVariableBool(VarAnimDriven(), false);
        if (_npcHandoffExit.exchange(false)) Recover("AI 요청");
    }
    if (_state != State::kActive && !Spec().qtes.empty()) Qte::End(_owner, _idx);
    // The destination graph owns rotation once exit blending starts.
    if (Spec().rotationLocked && _state == State::kActive) {
        if (auto* actor = Owner(); actor && actor->Is3DLoaded()) {
            bool allowed = false;
            if (actor->GetGraphVariableBool(VarAllowRotation(), allowed) && allowed) ApplyRotate(false);
        }
    }
    if (_state == State::kBlendingOut) {
        _elapsed += a_delta;
        return;
    }
    if (_linkSource >= 0) {
        _linkAge += a_delta;
        if (_linkAge > 1.5f) {
            logger::warn("[DirectLink] destination={} activation timeout; source restored if still playing", _idx);
            AbortLinkedRequest();
        }
        return;
    }
    PostCancelWatch(a_delta);
    if (_state == State::kIdle) return;
    _elapsed += a_delta;
    if (_state == State::kActive) {
        // A loop's transition windows count each cycle's own time (moving charge, 10-30).
        const bool cycles = Spec().loop.enabled && _clipDuration > 0.05f;
        ActionLinks::ObserveTime(_owner, _idx, cycles ? std::fmod(_elapsed, _clipDuration) : _elapsed, _clipDuration);
        Qte::ObserveTime(_owner, _idx, _elapsed);
    }
    if (_startedInAir && FrameworkSettings::Get().debug) {
        _airPhysicsAge += a_delta;
        if (_airPhysicsSamples < 12 && (_airPhysicsSamples < 3 || _airPhysicsAge >= _airPhysicsNextSample)) {
            LogAirPhysics(AirPhase::kFrame);
            ++_airPhysicsSamples;
            _airPhysicsNextSample = _airPhysicsAge + 0.1f;
        }
    }
    RecoveryClock();
    UpdateNativeCancel();
    SlotUpdate();
    if (_isPlayer && _state == State::kActive) {
        // A turning frame would mix two steers: measure only while the turn holds still.
        const bool turned = _rotateOn && TurnLikeTdm(a_delta);
        if (!turned && Actions::TdmMode() != TdmMovement::Mode::kAbsent) MeasureSteer();
    }
    if (_state == State::kExiting) {
        _exitingTime += a_delta;
        if (_exitingTime > _exitTimeout) {
            logger::warn("[{}] Exit watchdog: 클립 비활성이 없음 — 강제 종료", Spec().id);
            Finish();
            return;
        }
    }
    // Charge (10-30): an on="end" transition goes at the Exit point instead of the exit; a loop's maxHold counts as
    // its key's release. Neither going (no such transition): the action exits as it would have.
    if (_state == State::kActive && !_exitSent) {
        const auto& spec = Spec();
        if (spec.endLinkAt >= 0.0f && !_endReached && _clipDuration > 0.05f &&
            _elapsed >= _clipDuration - spec.endLinkAt) {
            _endReached = true;
            if (!ActionLinks::ReachedEnd(_owner, _idx)) ExitForLink();
        }
        // maxHold counts from the press of the key that started the charge while it is down (a moving charge keeps
        // one clock from loop to loop), else from this action's start.
        const auto& held = HeldKeys::Get();
        const float holding = _isPlayer && held.ChainHeld() ? held.ChainHeldTime(InputSink::Get()->Now()) : _elapsed;
        if (spec.loop.enabled && !_releaseForced && holding >= spec.loop.maxHold) {
            _releaseForced = true;
            logger::debug("[{}] loop maxHold {:.2f}s: released", spec.id, spec.loop.maxHold);
            if (!ActionLinks::ForceRelease(_owner, _idx)) ExitForLink();
        }
    }
    if (_state == State::kActive && !_exitSent && _elapsed >= SafetyTime()) {
        PrepareExit(Spec().exitBlend);
        if (auto* p = Owner()) p->NotifyAnimationGraph(Fixed(Spec().events.exit));
        if (_state == State::kExiting) SetAnimationDriven(false);
        logger::debug("[{}] 보험 Exit at {:.2f}s", Spec().id, _elapsed);
    }
}

// TDM turns nobody while our action drives the player (runtime/TdmTurn.h): in the action's Rotate window the player
// turns as TDM's setting would, while a movement key is held; the camera keeps looking where it did, as TDM keeps it.
bool ActionRunner::TurnLikeTdm(float a_delta) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (!player || !player->Is3DLoaded() || !camera || !camera->currentState ||
        camera->currentState->id != RE::CameraStates::kThirdPerson)
        return false;
    // TDM's free camera: without it the camera is the body's, and the game turns the body by the mouse.
    auto* third = static_cast<RE::ThirdPersonState*>(camera->currentState.get());
    if (!third->freeRotationEnabled || !std::isfinite(third->freeRotation.x)) return false;
    float x = 0.0f, y = 0.0f;
    if (!InputSink::Get()->MoveVector(x, y)) return false;
    const float body = player->GetAngleZ(), cameraYaw = body + third->freeRotation.x;
    const float target = TdmTurn::Target(Actions::TdmMode(), cameraYaw, x, y);
    if (!std::isfinite(target)) return false;
    const float turn = TdmTurn::Step(TdmTurn::Wrap(target - body), a_delta);
    if (std::fabs(turn) < 1e-5f) return false;
    // As TDM turns the player (DirectionalMovementHandler::UpdateRotation): the heading, then the free rotation by
    // the same amount.
    player->SetHeading(body + turn);
    // The engine's motion heading stays: the steer turns as the body does.
    g_playerSteer.store(TdmTurn::Wrap(g_playerSteer.load() + turn));
    third->freeRotation.x = TdmTurn::Wrap(third->freeRotation.x - turn);
    return true;
}

void ActionRunner::OnAnimEvent(const RE::BSFixedString& a_tag) {
    if (InterruptIfUnavailable()) return;
    if (_state == State::kExiting || _state == State::kIdle || _state == State::kBlendingOut || _linkSource >= 0 ||
        !a_tag.c_str())
        return;
    const auto& spec = Spec();
    if (FrameworkSettings::Get().debug && _seenTags.size() < 40 && !_seenTags.count(a_tag.c_str())) {
        _seenTags.insert(a_tag.c_str());
        logger::debug("[{}] 이벤트 '{}' at {:.2f}s", spec.id, a_tag.c_str(), _elapsed);
    }
    const auto& tags = Tags();
    for (std::size_t w = 0; w < spec.cancels.size(); ++w) {
        if (w < _openSeen.size() && a_tag == tags.open[w]) _openSeen[w] = true;
        if (w < _closeSeen.size() && a_tag == tags.close[w]) _closeSeen[w] = true;
    }
    if (spec.recovery.enabled && _variant >= 0 && static_cast<std::size_t>(_variant) < tags.recovery.size() &&
        a_tag == tags.recovery[_variant])
        OpenRecovery("주석 이벤트");
    SlotEvent(a_tag);
    if (a_tag == tags.exit) {
        _exitSeen = true;
        if (_state == State::kActive) PrepareExit(spec.exitBlend);
        SetAnimationDriven(false);
    }
}

void ActionRunner::SteerMotion(float a_time, RE::NiPoint3& a_translation) {
    if (!_isPlayer) return;
    const float steer = g_playerSteer.load();
    // The clip's way since the last later sample (the engine may ask a time twice; a loop wraps back).
    if (std::fabs(a_time - _clipTime) > 0.004f) {
        const float dt = a_time - _clipTime;
        if (_clipTime >= 0.0f && dt > 0.0f && dt < 0.2f) {
            float x = a_translation.x - _clipX, y = a_translation.y - _clipY;
            const float speed = std::hypot(x, y) / dt;
            TdmTurn::Steer(steer, x, y);
            _clipYaw.store(std::atan2(x, y));
            _clipSpeed.store(speed);
            _clipSamples.fetch_add(1);
        }
        _clipTime = a_time;
        _clipX = a_translation.x;
        _clipY = a_translation.y;
    }
    if (steer != 0.0f) TdmTurn::Steer(steer, a_translation.x, a_translation.y);
}

// The engine's motion heading from what it moved for the clip's last sample.
void ActionRunner::MeasureSteer() {
    const unsigned samples = _clipSamples.load();
    if (samples == _clipSamplesMeasured) return;
    _clipSamplesMeasured = samples;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cc = player ? player->GetCharController() : nullptr;
    if (!cc) return;
    RE::hkVector4 v;
    cc->GetLinearVelocityImpl(v);
    const auto& q = v.quad.m128_f32;
    const float inverse = RE::bhkWorld::GetWorldScaleInverse();
    const float speed = std::hypot(q[0], q[1]) * inverse;
    if (!TdmTurn::Comparable(speed, _clipSpeed.load())) return;
    const float steer = TdmTurn::SteerFor(player->GetAngleZ(), std::atan2(q[0], q[1]), _clipYaw.load());
    if (std::fabs(TdmTurn::Wrap(steer - g_playerSteer.load())) > 0.05f)
        logger::debug("[TdmTurn] steer {:.3f} -> {:.3f} (heading {:.3f}, moving {:.3f})", g_playerSteer.load(), steer,
            player->GetAngleZ(), std::atan2(q[0], q[1]));
    g_playerSteer.store(steer);
}

void ActionRunner::ResetPlayerSteer() { g_playerSteer.store(0.0f); }

void ActionRunner::TraceMotionOutput(float time, const RE::NiPoint3& translation) {
    if (!FrameworkSettings::Get().debug || _state != State::kActive || _motionTraceCount >= 32 ||
        _elapsed < _motionTraceNext)
        return;
    auto* actor = Owner();
    if (!actor || !actor->Is3DLoaded()) return;
    auto* cc = actor->GetCharController();
    if (!cc) return;
    ++_motionTraceCount;
    _motionTraceNext = _elapsed + 0.08f;
    const auto pos = actor->GetPosition();
    RE::hkVector4 velocity;
    cc->GetLinearVelocityImpl(velocity);
    const auto& v = velocity.quad.m128_f32;
    const auto& initial = cc->initialVelocity.quad.m128_f32;
    bool driven = false;
    const bool readable = actor->GetGraphVariableBool(VarAnimDriven(), driven);
    logger::debug(
        "[ActionMotion] actor={:08X} player={} action='{}' elapsed={:.3f} sampleTime={:.3f} duration={:.3f} cumulative=({:.3f},{:.3f},{:.3f}) pos=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f}) initialVelocity=({:.3f},{:.3f},{:.3f}) speedPct={:.3f} scale={:.3f} driven={}/{} flags={:08X}",
        actor->GetFormID(), actor->IsPlayerRef(), Spec().id, _elapsed, time, _clipDuration, translation.x,
        translation.y, translation.z, pos.x, pos.y, pos.z, v[0], v[1], v[2], initial[0], initial[1], initial[2],
        cc->speedPct, cc->scale, readable, driven, static_cast<unsigned>(cc->flags.get()));
}

void ActionRunner::Finish() {
    ++_playbackSerial;
    // Deactivation can reassert the source mode after the last runner update.
    // Revalidate ownership before the final handoff, including synchronous exits.
    MaintainRecoveryMovement();
    _recoveryMovement = false;
    const int requestOrigin = _requestOrigin;
    _requestOrigin = -1;
    ClearNativeCapture();
    _nativeExiting = false;
    _recoveryOpen = false;
    _npcHandoff = false;
    _npcHandoffExit = false;
    _linkSource = _linkTarget = -1;
    _linkRetirement = {};
    ActionLinks::End(_owner, _idx);
    Qte::End(_owner, _idx);
    PairFeatures::EndActionCamera(_owner, _idx, _exitSent);
    LogAirPhysics(AirPhase::kFinish);
    RestoreSlots();
    if (_exitSent && Spec().condition.inAir !=
                         0) {  // 공중에서 끝났으면 낙하 상태로 돌려보낸다 (아니면 착지 처리 없이 Idle 로 서 있게 됨)
        auto* p = Owner();
        auto* cc = p ? p->GetCharController() : nullptr;
        if (cc && cc->context.currentState == RE::hkpCharacterStateType::kInAir) {
            const bool ok = AirControl::ResumeFalling(p);
            logger::debug("[{}] 공중에서 종료 → JumpFall 재개 notify={}", Spec().id, ok);
        }
    }
    if (_exitSent) {
        SetAnimationDriven(false, true);
        ReplayWeaponRequest(Owner(), Spec().id);
    } else
        _animDrivenSet = false;  // 다음 상태(공격)가 쓰는 중 — PostCancelWatch 가 끝에 정리
    if (FrameworkSettings::Get().debug) {
        auto* actor = Owner();
        const bool actorReady = actor && actor->Is3DLoaded();
        auto* controller = actorReady ? actor->GetCharController() : nullptr;
        bool driven = false;
        const bool graphReadable = actorReady && actor->GetGraphVariableBool(VarAnimDriven(), driven);
        auto* actorState = actorReady ? actor->AsActorState() : nullptr;
        logger::debug(
            "[{}] FinishState normalExit={} controller={} state={} inAir={} graphReadable={} driven={} weaponState={}",
            Spec().id, _exitSent, controller != nullptr,
            controller ? static_cast<int>(controller->context.currentState) : -1,
            controller && controller->context.currentState == RE::hkpCharacterStateType::kInAir, graphReadable, driven,
            actorState ? static_cast<int>(actorState->GetWeaponState()) : -1);
    }
    // The previousAction condition's record: completed = played to its own exit (only while a check needs it).
    ActionHistory::Record(_owner, _idx, _exitSent);
    _state = State::kIdle;
    _elapsed = 0.0f;
    _exitSent = false;
    _clipDuration = 0.0f;
    _exitingTime = 0.0f;
    _cooldown = Spec().condition.cooldown;
    // A conditional trigger is shared by its playback destinations. Preserve
    // its cooldown even when a different directional leaf runs next time.
    // Resolve by index for this actor; never retain another runner or actor.
    if (requestOrigin >= 0 && requestOrigin != _idx) {
        if (auto* owner = Owner(); owner && owner->Is3DLoaded()) {
            if (auto* origin = Actions::RunnerFor(owner, requestOrigin, false);
                origin && !origin->Spec().flow.nodes.empty()) {
                origin->_cooldown = std::max(origin->_cooldown, origin->Spec().condition.cooldown);
            }
        }
    }

    BeginAirPhysics(false);
}

void ActionRunner::ResetOnLoad() {
    ++_playbackSerial;
    _normalExitMovementAccepted = false;
    _recoveryMovement = false;
    _requestOrigin = -1;
    ClearNativeCapture();
    _nativeExiting = false;
    _linkSource = _linkTarget = -1;
    _linkRetirement = {};
    _linkAge = 0;
    RestoreSlots();
    if (_state != State::kIdle) {
        if (auto* p = Owner()) p->NotifyAnimationGraph(Fixed(Spec().events.exit));  // 보낸 진입은 반드시 닫는다
    }
    SetAnimationDriven(false, true);
    _state = State::kIdle;
    _elapsed = 0.0f;
    _carryReleaseDue = false;
    _exitSent = false;
    _cooldown = 0.0f;
    _clipDuration = 0.0f;
    _exitingTime = 0.0f;
    _postCancelTimer = 0.0f;
    BeginAirPhysics(false);
}

}  // namespace AnimationInjector

#include "runtime/EventEffects.h"
#include "runtime/ActionLinks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/ActorImmunity.h"
#include "runtime/FireArrow.h"
#include "runtime/PairEventRoles.h"
#include "runtime/PairFeatures.h"
#include "runtime/PairedSession.h"
#include "runtime/Strike.h"
#include "runtime/WeaponCull.h"
#include <charconv>

namespace AnimationInjector::EventEffects {
namespace {
// A chained action may start a moment after the one before it ended (a link's transition, a pair's
// handoff): a scope stays open this long without a running action (the weapon hold this replaced
// measured the same gap as its start grace).
constexpr float kChainGrace = 1.0f;
constexpr std::uint32_t kSaveType = 'AIES';        // event effect state spells
constexpr std::uint32_t kGlobalSaveType = 'AIEG';  // globals set with restore="end", held when saved (0.12.296)

enum class Kind : std::uint8_t { kHide, kState, kTime, kIFrame, kArmor, kRotate, kGlobal };
struct Hold {
    RE::ActorHandle actor;
    Kind kind;
    Scope scope = 0;
    RE::FormID spell = 0;        // kState
    WeaponCull::Owner cull = 0;  // kHide
    RE::FormID immunity = 0;     // kIFrame: the ActorImmunity lease
    RE::FormID global = 0;       // kGlobal: the global, its value before the first hold and the one set
    float prior = 0.0f, applied = 0.0f;
};
struct OpenScope {
    RE::ActorHandle actor;
    Scope id;
    float idle = 0.0f;
};
// A state spell an event function added: removed when its last hold ends. A spell the actor had before
// is never removed.
struct AddedSpell {
    RE::ActorHandle actor;
    RE::FormID spell;
};
struct Rotation {
    RE::ActorHandle actor;
    bool initial = false;
};

std::vector<Hold> holds;
std::vector<OpenScope> scopes;
std::vector<AddedSpell> addedSpells;
std::vector<Rotation> rotations;
std::vector<std::pair<RE::FormID, RE::FormID>> savedSpells;  // (actor, spell) read from the save
struct SavedGlobal {
    RE::FormID global = 0;
    float prior = 0.0f, applied = 0.0f;
};
std::vector<SavedGlobal> savedGlobals;  // globals held when saved: given back after the load if still as set
Scope nextScope = 1;
struct {
    bool owned = false;
    float prior = 1.0f, applied = 1.0f;
} timeScale;
std::array<std::atomic<std::uint32_t>, 16> armorSlots{};
std::unordered_map<std::string, RE::FormID> formCache;  // "plugin|form" -> form id (0 = missing)

const RE::BSFixedString& RotationVariable() {
    static RE::BSFixedString name("bAllowRotation");
    return name;
}
bool Usable(RE::Actor* actor) { return actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState(); }
bool Running(RE::Actor* actor) {
    if (!actor) return false;
    const auto handle = actor->GetHandle();
    if (Actions::FindRunner(actor, [](int, ActionRunner& r) { return r.GetState() != ActionRunner::State::kIdle; }))
        return true;
    const auto status = PairedSession::ReadStatus();
    if (status.phase != PairedSession::Phase::kIdle &&
        (status.initiator.actor == handle || status.target.actor == handle))
        return true;
    return ActionLinks::HasPendingTransition(handle);
}
// A function's plugin and local form as the form of type T: looked up once, then by id (a spell, a sound
// descriptor, a global).
template<class T>
T* FormOf(const EventFunctionSpec& f) {
    if (f.plugin.empty() || f.form.empty()) return nullptr;
    const auto key = f.plugin + "|" + f.form;
    auto found = formCache.find(key);
    if (found == formCache.end()) {
        RE::FormID local = 0, id = 0;
        const auto [end, ec] = std::from_chars(f.form.data(), f.form.data() + f.form.size(), local, 16);
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (ec == std::errc{} && end == f.form.data() + f.form.size() && handler)
            if (auto* form = handler->LookupForm(local, f.plugin)) id = form->GetFormID();
        if (!id) logger::warn("[EventEffects] missing form {}:{}", f.plugin, f.form);
        found = formCache.emplace(key, id).first;
    }
    return found->second ? RE::TESForm::LookupByID<T>(found->second) : nullptr;
}
RE::SpellItem* SpellOf(const EventFunctionSpec& f) { return FormOf<RE::SpellItem>(f); }
// A sound's bone (0.12.296): the skeleton node it follows; root (or a missing node) is the actor's 3D.
RE::NiAVObject* BoneOf(RE::Actor* actor, const std::string& bone) {
    static const RE::BSFixedString spine("NPC Spine2 [Spn2]"), head("NPC Head [Head]"), right("NPC R Hand [RHnd]"),
        left("NPC L Hand [LHnd]");
    const RE::BSFixedString* name = bone == "head"        ? &head
                                    : bone == "rightHand" ? &right
                                    : bone == "leftHand"  ? &left
                                    : bone == "root"      ? nullptr
                                                          : &spine;
    auto* node = name ? actor->GetNodeByName(*name) : nullptr;
    return node ? node : actor->Get3D();
}
// A sound file under Data (BSResource::ID from its path) or a registered sound descriptor, at a bone.
void PlaySound(const EventFunctionSpec& f, RE::Actor* actor) {
    auto* audio = RE::BSAudioManager::GetSingleton();
    auto* node = BoneOf(actor, f.bone);
    if (!audio || !node) return;
    RE::BSSoundHandle handle;
    if (f.file.empty()) {
        auto* descriptor = FormOf<RE::BGSSoundDescriptorForm>(f);
        if (!descriptor || !audio->GetSoundHandle(handle, descriptor)) return;
    } else {
        RE::BSResource::ID id;
        id.GenerateFromPath(f.file.c_str());
        audio->GetSoundHandleByFile(handle, id, 0x1A, 0);
    }
    if (!handle.IsValid()) {
        logger::warn("[EventEffects] sound '{}' {}:{} could not be played", f.file, f.plugin, f.form);
        return;
    }
    handle.SetVolume(std::clamp(f.volume, 0.0f, 1.0f));
    handle.SetPosition(node->world.translate);
    handle.SetObjectToFollow(node);
    handle.Play();
}
void PublishArmor() {
    std::size_t slot = 0;
    for (const auto& h : holds)
        if (h.kind == Kind::kArmor && slot < armorSlots.size()) {
            const auto id = h.actor.native_handle();
            bool seen = false;
            for (std::size_t i = 0; i < slot; ++i) seen |= armorSlots[i].load() == id;
            if (!seen) armorSlots[slot++].store(id);
        }
    for (; slot < armorSlots.size(); ++slot) armorSlots[slot].store(0);
}
bool Holds(RE::ActorHandle actor, Kind kind) {
    return std::any_of(holds.begin(), holds.end(), [&](const Hold& h) { return h.actor == actor && h.kind == kind; });
}
void RestoreTime() {
    if (timeScale.owned && std::fabs(RE::BSTimer::QGlobalTimeMultiplierTarget() - timeScale.applied) < 0.0001f)
        if (auto* timer = RE::BSTimer::GetSingleton()) timer->SetGlobalTimeMultiplier(timeScale.prior, false);
    timeScale.owned = false;
}
// Ends one hold, already taken out of `holds`: the state it changed returns when no hold of its kind
// is left there.
void End(const Hold& h) {
    auto actor = h.actor.get();
    switch (h.kind) {
    case Kind::kHide: WeaponCull::Show(h.cull); break;
    case Kind::kIFrame: {
        auto lease = h.immunity;
        ActorImmunity::Release(lease);
        break;
    }
    case Kind::kState:
        if (std::none_of(holds.begin(), holds.end(),
                [&](const Hold& o) { return o.actor == h.actor && o.kind == Kind::kState && o.spell == h.spell; })) {
            const auto added = std::find_if(addedSpells.begin(), addedSpells.end(),
                [&](const AddedSpell& a) { return a.actor == h.actor && a.spell == h.spell; });
            if (added != addedSpells.end()) {
                if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(h.spell); spell && actor)
                    actor->RemoveSpell(spell);
                addedSpells.erase(added);
            }
        }
        break;
    case Kind::kGlobal:
        if (std::none_of(holds.begin(), holds.end(),
                [&](const Hold& o) { return o.kind == Kind::kGlobal && o.global == h.global; }))
            if (auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(h.global);
                global && std::fabs(global->value - h.applied) < 0.0001f)
                global->value = h.prior;
        break;
    case Kind::kRotate:
        if (std::none_of(holds.begin(), holds.end(),
                [&](const Hold& o) { return o.actor == h.actor && o.kind == Kind::kRotate; })) {
            const auto found =
                std::find_if(rotations.begin(), rotations.end(), [&](const Rotation& r) { return r.actor == h.actor; });
            if (found != rotations.end()) {
                if (actor && actor->Is3DLoaded()) actor->SetGraphVariableBool(RotationVariable(), found->initial);
                rotations.erase(found);
            }
        }
        break;
    default: break;
    }
}
template<class Pred>
void Release(Pred&& pred, const char* reason) {
    std::vector<Hold> ended;
    for (const auto& h : holds)
        if (pred(h)) ended.push_back(h);
    if (ended.empty()) return;
    std::erase_if(holds, pred);
    for (const auto& h : ended) {
        End(h);
        logger::debug("[EventEffects] hold ended actor={:08X} kind={} scope={} reason={}", h.actor.native_handle(),
            static_cast<int>(h.kind), h.scope, reason);
    }
    if (!std::any_of(holds.begin(), holds.end(), [](const Hold& h) { return h.kind == Kind::kTime; })) RestoreTime();
    PublishArmor();
}
Hold* Find(RE::ActorHandle actor, Kind kind, Scope scope, RE::FormID spell = 0) {
    const auto it = std::find_if(holds.begin(), holds.end(),
        [&](const Hold& h) { return h.actor == actor && h.kind == kind && h.scope == scope && h.spell == spell; });
    return it == holds.end() ? nullptr : &*it;
}
void RunOn(const EventFunctionSpec& f, const Context& context, RE::Actor* actor, Scope scope) {
    const auto handle = actor->GetHandle();
    switch (f.op) {
    case EventOp::kHide:
        if (auto* h = Find(handle, Kind::kHide, scope))
            WeaponCull::Hide(h->cull, actor);
        else {
            Hold hold{handle, Kind::kHide, scope};
            hold.cull = WeaponCull::NewOwner();
            WeaponCull::Hide(hold.cull, actor);
            holds.push_back(hold);
        }
        break;
    case EventOp::kShow:
        Release([&](const Hold& h) { return h.actor == handle && h.kind == Kind::kHide; }, "show");
        break;
    case EventOp::kSpell:
        if (auto* spell = SpellOf(f))
            if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                caster->CastSpellImmediate(spell, false, actor, 1.0f, false, 0.0f, actor);
            }
        break;
    case EventOp::kStateOn:
        if (auto* spell = SpellOf(f); spell && !Find(handle, Kind::kState, scope, spell->GetFormID())) {
            const bool had = actor->HasSpell(spell);
            const bool ours = std::any_of(addedSpells.begin(), addedSpells.end(),
                [&](const AddedSpell& a) { return a.actor == handle && a.spell == spell->GetFormID(); });
            if (!had && actor->AddSpell(spell)) addedSpells.push_back({handle, spell->GetFormID()});
            if (!had || ours) holds.push_back({handle, Kind::kState, scope, spell->GetFormID()});
        }
        break;
    case EventOp::kStateOff:
        if (auto* spell = SpellOf(f)) {
            const auto id = spell->GetFormID();
            Release([&](const Hold& h) { return h.actor == handle && h.kind == Kind::kState && h.spell == id; },
                "stateOff");
        }
        break;
    case EventOp::kCost:
        if (auto* values = actor->AsActorValueOwner()) {
            values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -f.stamina);
            values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -f.magicka);
        }
        break;
    case EventOp::kShake:
        if (actor->IsPlayerRef())
            RE::ShakeCamera(
                std::clamp(f.strength, 0.0f, 10.0f), actor->GetPosition(), std::clamp(f.duration, 0.0f, 30.0f));
        break;
    case EventOp::kStopVelocity:
        if (auto* controller = actor->GetCharController()) controller->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
        break;
    case EventOp::kTimeOn:
        if (actor->IsPlayerRef())
            if (auto* timer = RE::BSTimer::GetSingleton()) {
                if (!timeScale.owned) timeScale.prior = RE::BSTimer::QGlobalTimeMultiplierTarget();
                timeScale.owned = true;
                timeScale.applied = f.factor;
                timer->SetGlobalTimeMultiplier(f.factor, false);
                if (!Find(handle, Kind::kTime, scope)) holds.push_back({handle, Kind::kTime, scope});
            }
        break;
    case EventOp::kTimeOff:
        if (actor->IsPlayerRef()) Release([](const Hold& h) { return h.kind == Kind::kTime; }, "timeOff");
        break;
    case EventOp::kIFrameOn:
        if (!Find(handle, Kind::kIFrame, scope)) {
            Hold hold{handle, Kind::kIFrame, scope};
            hold.immunity = ActorImmunity::Acquire(actor);
            if (hold.immunity) holds.push_back(hold);
        }
        break;
    case EventOp::kIFrameOff:
        Release([&](const Hold& h) { return h.actor == handle && h.kind == Kind::kIFrame; }, "iframeOff");
        break;
    case EventOp::kArmorOn:
        if (!Find(handle, Kind::kArmor, scope)) holds.push_back({handle, Kind::kArmor, scope});
        PublishArmor();
        break;
    case EventOp::kArmorOff:
        Release([&](const Hold& h) { return h.actor == handle && h.kind == Kind::kArmor; }, "armorOff");
        break;
    case EventOp::kRotateOn:
        if (std::none_of(rotations.begin(), rotations.end(), [&](const Rotation& r) { return r.actor == handle; })) {
            bool initial = false;
            if (!actor->GetGraphVariableBool(RotationVariable(), initial)) break;
            rotations.push_back({handle, initial});
        }
        actor->SetGraphVariableBool(RotationVariable(), true);
        if (!Find(handle, Kind::kRotate, scope)) holds.push_back({handle, Kind::kRotate, scope});
        break;
    case EventOp::kRotateOff:
        Release([&](const Hold& h) { return h.actor == handle && h.kind == Kind::kRotate; }, "rotateOff");
        break;
    case EventOp::kCancel: PairFeatures::RequestCancel(handle); break;
    case EventOp::kFireArrow:
        if (actor->IsPlayerRef()) FireArrow::Direct(actor, f.arrowDamage, context.origin, 0.0f);
        break;
    case EventOp::kRagdoll:
        if (actor->IsDead() || actor->IsOnMount() || !actor->Is3DLoaded()) break;
        if (auto* process = actor->GetActorRuntimeData().currentProcess) {
            // A pair holds its actors out of physics: this role leaves it first, and the pair's end then
            // reconciles its flags and watches its get-up (no-op outside a pair).
            PairFeatures::ReleasePhysics(handle);
            const auto at = actor->GetPosition();
            const auto point = RagdollPush::PushPoint(f.direction, at.x, at.y, at.z, actor->GetAngleZ());
            process->KnockExplosion(
                actor, RE::NiPoint3(point.x, point.y, point.z), RagdollPush::ForceOf(f.direction, f.force));
        }
        break;
    case EventOp::kSound: PlaySound(f, actor); break;
    case EventOp::kGlobalSet:
        if (auto* global = FormOf<RE::TESGlobal>(f)) {
            // restore="end": the value before the first such hold comes back when the run ends (or at a load, see
            // PostLoad); a global set without it stays as set.
            if (f.restore) {
                const auto held = std::find_if(holds.begin(), holds.end(),
                    [&](const Hold& h) { return h.kind == Kind::kGlobal && h.global == global->GetFormID(); });
                Hold hold{handle, Kind::kGlobal, scope};
                hold.global = global->GetFormID();
                hold.prior = held != holds.end() ? held->prior : global->value;
                hold.applied = f.value;
                for (auto& h : holds)
                    if (h.kind == Kind::kGlobal && h.global == hold.global) h.applied = f.value;
                holds.push_back(hold);
            }
            global->value = f.value;
        }
        break;
    case EventOp::kModEvent:
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            SKSE::ModCallbackEvent event{RE::BSFixedString(f.name), RE::BSFixedString(f.text), f.number, actor};
            source->SendEvent(std::addressof(event));
        }
        break;
    case EventOp::kStrike: {
        // The changed actor attacks; in a pair, its partner is the other role.
        const auto partner = handle == context.sender    ? context.partner
                             : handle == context.partner ? context.sender
                                                         : RE::ActorHandle{};
        Strike::Run(actor, partner, f.strike);
        break;
    }
    }
    logger::debug("[EventEffects] '{}' op={} actor={:08X} scope={}", context.origin, static_cast<int>(f.op),
        actor->GetFormID(), scope);
}
std::vector<RE::ActorHandle> TargetsOf(const EventFunctionSpec& f, const Context& c) {
    std::vector<RE::ActorHandle> out;
    auto add = [&](RE::ActorHandle h) {
        if (h && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    };
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto playerHandle = player ? player->GetHandle() : RE::ActorHandle{};
    if (c.senderRole == 0 || c.senderRole == 1) {
        const std::array<RE::ActorHandle, 2> roles =
            c.senderRole == 0 ? std::array{c.sender, c.partner} : std::array{c.partner, c.sender};
        const int playerRole = roles[0] == playerHandle ? 0 : roles[1] == playerHandle ? 1 : -1;
        const auto mask = PairEventRoles::Targets(f.target, c.senderRole, playerRole);
        for (int i = 0; i < 2; ++i)
            if (mask & (1u << i)) add(roles[i]);
        if (f.target == EventTarget::kPlayer && playerRole < 0) add(playerHandle);
    } else if (f.target == EventTarget::kSender || f.target == EventTarget::kBoth)
        add(c.sender);
    else if (f.target == EventTarget::kPlayer)
        add(playerHandle);
    return out;
}
}

Scope ScopeOf(RE::Actor* actor) {
    if (!actor) return 0;
    const auto handle = actor->GetHandle();
    for (const auto& s : scopes)
        if (s.actor == handle) return s.id;
    if (!Running(actor)) return 0;
    scopes.push_back({handle, nextScope});
    logger::debug("[EventEffects] scope {} opened actor={:08X}", nextScope, actor->GetFormID());
    return nextScope++;
}
Context ContextFor(RE::Actor* sender, const char* origin) {
    Context c;
    if (!sender) return c;
    c.sender = sender->GetHandle();
    c.senderIsPlayer = sender->IsPlayerRef();
    c.origin = origin ? origin : "";
    const auto status = PairedSession::ReadStatus();
    if (status.phase != PairedSession::Phase::kIdle) {
        if (status.initiator.actor == c.sender) {
            c.senderRole = 0;
            c.partner = status.target.actor;
        } else if (status.target.actor == c.sender) {
            c.senderRole = 1;
            c.partner = status.initiator.actor;
        }
    }
    return c;
}
void RunFor(const EventFunctionSpec& function, const Context& context, const std::vector<RE::ActorHandle>& targets) {
    auto sender = context.sender.get();
    const Scope senderScope = ScopeOf(sender.get());
    for (const auto& handle : targets) {
        auto actor = handle.get();
        if (!Usable(actor.get())) continue;
        const Scope own = ScopeOf(actor.get());
        RunOn(function, context, actor.get(), own ? own : senderScope);
    }
}
void Run(const EventFunctionSpec& function, const Context& context) {
    RunFor(function, context, TargetsOf(function, context));
}

void Update(float delta) {
    if (!std::isfinite(delta) || delta < 0 || delta > 1.0f) delta = 0.0f;
    Strike::Update(delta);
    for (auto it = scopes.begin(); it != scopes.end();) {
        auto actor = it->actor.get();
        if (actor && actor->Is3DLoaded() && !actor->IsDead() && Running(actor.get()))
            it->idle = 0.0f;
        else
            it->idle += delta;
        if (actor && actor->Is3DLoaded() && !actor->IsDead() && it->idle <= kChainGrace) {
            ++it;
            continue;
        }
        const auto id = it->id;
        it = scopes.erase(it);
        Release([&](const Hold& h) { return h.scope == id; }, "scope-ended");
        logger::debug("[EventEffects] scope {} closed", id);
    }
    std::vector<RE::ActorHandle> gone;
    for (const auto& h : holds) {
        auto actor = h.actor.get();
        if (!Usable(actor.get())) {
            if (std::find(gone.begin(), gone.end(), h.actor) == gone.end()) gone.push_back(h.actor);
        } else if (h.kind == Kind::kHide)
            WeaponCull::Hide(h.cull, actor.get());
    }
    for (const auto& handle : gone) Release([&](const Hold& h) { return h.actor == handle; }, "actor-unavailable");
}
bool HyperArmorFor(RE::ActorHandle actor) {
    const auto id = actor.native_handle();
    if (!id) return false;
    for (const auto& slot : armorSlots)
        if (slot.load() == id) return true;
    return false;
}
void ResetOnLoad() {
    // Weapon nodes and graph variables belong to the old 3D: forget them. The ghost state is saved with
    // the actor, and time is global: give it back.
    for (const auto& h : holds)
        if (h.kind == Kind::kIFrame) {
            auto lease = h.immunity;
            ActorImmunity::Release(lease);
        }
    holds.clear();
    scopes.clear();
    addedSpells.clear();
    Strike::ResetOnLoad();
    rotations.clear();
    RestoreTime();
    PublishArmor();
}
void SaveGlobals(SKSE::SerializationInterface* s) {
    std::vector<SavedGlobal> globals;
    for (const auto& h : holds)
        if (h.kind == Kind::kGlobal &&
            std::none_of(globals.begin(), globals.end(), [&](const SavedGlobal& g) { return g.global == h.global; }))
            globals.push_back({h.global, h.prior, h.applied});
    if (globals.empty()) return;
    if (!s->OpenRecord(kGlobalSaveType, 1)) {
        logger::error("[EventEffects] failed to open the global record");
        return;
    }
    const auto count = static_cast<std::uint32_t>(globals.size());
    s->WriteRecordData(count);
    for (const auto& g : globals) {
        s->WriteRecordData(g.global);
        s->WriteRecordData(g.prior);
        s->WriteRecordData(g.applied);
    }
}
void Save(SKSE::SerializationInterface* s) {
    SaveGlobals(s);
    std::vector<std::pair<RE::FormID, RE::FormID>> spells;
    for (const auto& added : addedSpells)
        if (auto actor = added.actor.get()) spells.emplace_back(actor->GetFormID(), added.spell);
    if (spells.empty()) return;
    if (!s->OpenRecord(kSaveType, 1)) {
        logger::error("[EventEffects] failed to open the state spell record");
        return;
    }
    const auto count = static_cast<std::uint32_t>(spells.size());
    s->WriteRecordData(count);
    for (const auto& [actor, spell] : spells) {
        s->WriteRecordData(actor);
        s->WriteRecordData(spell);
    }
}
bool LoadRecord(SKSE::SerializationInterface* s, std::uint32_t type, std::uint32_t version, std::uint32_t) {
    if (type == kGlobalSaveType) {
        savedGlobals.clear();
        std::uint32_t count = 0;
        if (version != 1 || s->ReadRecordData(count) != sizeof(count)) return true;
        for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
            SavedGlobal g;
            RE::FormID resolved = 0;
            if (s->ReadRecordData(g.global) != sizeof(g.global) || s->ReadRecordData(g.prior) != sizeof(g.prior) ||
                s->ReadRecordData(g.applied) != sizeof(g.applied))
                break;
            if (s->ResolveFormID(g.global, resolved)) savedGlobals.push_back({resolved, g.prior, g.applied});
        }
        return true;
    }
    if (type != kSaveType) return false;
    savedSpells.clear();
    std::uint32_t count = 0;
    if (version != 1 || s->ReadRecordData(count) != sizeof(count)) return true;
    for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
        RE::FormID actor = 0, spell = 0, resolvedActor = 0, resolvedSpell = 0;
        if (s->ReadRecordData(actor) != sizeof(actor) || s->ReadRecordData(spell) != sizeof(spell)) break;
        if (s->ResolveFormID(actor, resolvedActor) && s->ResolveFormID(spell, resolvedSpell))
            savedSpells.emplace_back(resolvedActor, resolvedSpell);
    }
    return true;
}
void PostLoad() {
    // A global set with restore="end" that was still held when saved: its value before, if no one set it since.
    for (const auto& g : savedGlobals)
        if (auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(g.global);
            global && std::fabs(global->value - g.applied) < 0.0001f) {
            global->value = g.prior;
            logger::info("[EventEffects] restored global {:08X} held when saved", g.global);
        }
    savedGlobals.clear();
    for (const auto& [actorId, spellId] : savedSpells) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(spellId);
        if (actor && spell && actor->HasSpell(spell)) {
            actor->RemoveSpell(spell);
            logger::info("[EventEffects] removed state spell {:08X} held when saved from {:08X}", spellId, actorId);
        }
    }
    savedSpells.clear();
}
}

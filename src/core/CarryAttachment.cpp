#include "PCH.h"
#include "core/CarryAttachment.h"
#include "core/CarrySyncResolver.h"
#include "core/CarrySyncStep.h"

namespace AnimationInjector::CarryAttachment {
namespace {
using RegisterSync = bool (*)(RE::TESObjectREFR*, RE::TESObjectREFR*, RE::NiNode*, const RE::NiTransform&, bool);
std::uintptr_t UnwindOwner(DWORD64 base, RUNTIME_FUNCTION entry) {
    const auto& data = REL::Module::get().segment(REL::Segment::rdata);
    for (unsigned depth = 0; depth < 16; ++depth) {
        const auto at = base + entry.UnwindData;
        if (at < data.address() || at + 4 > data.address() + data.size()) return 0;
        const auto* header = reinterpret_cast<const unsigned char*>(at);
        const unsigned version = header[0] & 7, flags = header[0] >> 3;
        if (version != 1 && version != 2) return 0;
        if (!(flags & UNW_FLAG_CHAININFO)) return base + entry.BeginAddress;
        // Windows x64 UNWIND_INFO: four-byte header, aligned two-byte code
        // array, then a chained RUNTIME_FUNCTION. These are PE metadata fields,
        // not guessed offsets into a Skyrim object.
        const auto chain = at + 4 + 2 * ((static_cast<unsigned>(header[2]) + 1) & ~1u);
        if (chain + sizeof(entry) > data.address() + data.size()) return 0;
        std::memcpy(&entry, reinterpret_cast<const void*>(chain), sizeof(entry));
    }
    return 0;
}
bool CodeRange(std::uintptr_t address, std::uintptr_t& end) {
    const auto& code = REL::Module::get().segment(REL::Segment::textx);
    if (address < code.address() || address >= code.address() + code.size()) return false;
    DWORD64 base = 0;
    auto* entry = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!entry || base + entry->BeginAddress != address) return false;
    end = base + entry->EndAddress;
    if (UnwindOwner(base, *entry) != address) return false;
    // The registrar's shrink-wrapped prologue produces seven adjacent unwind
    // entries. Merge only entries chained back to this exact function.
    for (unsigned part = 0; part < 32 && end - address < 0x1000; ++part) {
        DWORD64 nextBase = 0;
        auto* next = RtlLookupFunctionEntry(end, &nextBase, nullptr);
        if (!next || nextBase != base || nextBase + next->BeginAddress != end ||
            UnwindOwner(nextBase, *next) != address)
            break;
        const auto nextEnd = nextBase + next->EndAddress;
        if (nextEnd <= end || nextEnd > code.address() + code.size()) return false;
        end = nextEnd;
    }
    return end > address && end <= code.address() + code.size();
}
RegisterSync Resolve() {
    // Both vtables come from the runtime's Address Library. Do not guess the SE
    // registration ID from the AE ID or invoke a route whose ABI did not match.
    const REL::Relocation<std::uintptr_t*> mount{RE::VTABLE_MountInteraction[0]};
    const REL::Relocation<std::uintptr_t> sync{RE::VTABLE_TESObjectREFRSync__REFRSyncController[0]};
    const auto address = CarrySyncResolver::Resolve(mount.get()[1], sync.address(), CodeRange);
    logger::info("[CarryAttachment] native mount sync resolved={} runtime={}", address != 0,
        REL::Module::get().version().string());
    return reinterpret_cast<RegisterSync>(address);
}
RE::NiTimeController* Existing(RE::NiAVObject* scene) {
    static const REL::Relocation<const RE::NiRTTI*> type{RE::NiRTTI_REFRSyncController};
    return scene ? scene->GetController(type.get()) : nullptr;
}
bool Linked(const Lease& lease) { return CarrySyncStep::Linked(lease.scene.get(), lease.sync.get()); }
bool IdentityParent(RE::NiAVObject* scene) {
    if (!scene->parent) return true;
    const auto& world = scene->parent->world;
    const RE::NiTransform identity;
    if (!std::isfinite(world.scale) || std::abs(world.scale - 1) > 0.0001f || !std::isfinite(world.translate.x) ||
        !std::isfinite(world.translate.y) || !std::isfinite(world.translate.z) || world.translate.SqrLength() > 0.0001f)
        return false;
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (!std::isfinite(world.rotate.entry[r][c]) ||
                std::abs(world.rotate.entry[r][c] - identity.rotate.entry[r][c]) > 0.0001f)
                return false;
    return true;
}
}
bool Begin(Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor, const RE::NiTransform& offset) {
    if (lease.sync || !carrier || !carried || carrier == carried || !anchor || !carrier->Is3DLoaded() ||
        !carried->Is3DLoaded())
        return false;
    auto* scene = carried->Get3D(false);
    auto* firstPerson = carried->Get3D(true);
    auto* carrierScene = carrier->Get3D(false);
    // This creature spike has one scene. The native registrar also visits a
    // separate first-person scene, so reject that case instead of leaking it.
    if (!scene || !carrierScene || (firstPerson && firstPerson != scene) || Existing(scene) || !IdentityParent(scene)) {
        logger::warn(
            "[CarryAttachment] scene unavailable, already synchronized, or unsupported parent/first-person scene");
        return false;
    }
    static const auto registerSync = Resolve();
    if (!registerSync) return false;
    lease.scene.reset(scene);
    lease.carrierScene.reset(carrierScene);
    lease.anchor.reset(anchor);
    const bool registered = registerSync(carried, carrier, anchor, offset, false);
    lease.sync.reset(Existing(scene));
    if (!registered || !Linked(lease) || !lease.sync->flags.any(RE::NiTimeController::Flag::kActive)) {
        logger::warn("[CarryAttachment] native registration incomplete; rolling back owned controller");
        End(lease);
        return false;
    }
    logger::info("[CarryAttachment] registered target={:08X} controller={} offset=({:.3f},{:.3f},{:.3f})",
        carried->GetFormID(), fmt::ptr(lease.sync.get()), offset.translate.x, offset.translate.y, offset.translate.z);
    return true;
}
bool Matches(const Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor) {
    return carrier && carried && carrier->Is3DLoaded() && carried->Is3DLoaded() &&
           carrier->Get3D(false) == lease.carrierScene.get() && carried->Get3D(false) == lease.scene.get() && anchor &&
           anchor == lease.anchor.get() && Linked(lease) && lease.sync->flags.any(RE::NiTimeController::Flag::kActive);
}
bool Pulse(Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor) {
    if (!Matches(lease, carrier, carried, anchor)) return false;
    if (carried->IsAIEnabled() || !carried->QCanUpdateSync()) {
        logger::warn("[CarryAttachment] pulse unavailable target={:08X} aiEnabled={} canSync={}", carried->GetFormID(),
            carried->IsAIEnabled(), carried->QCanUpdateSync());
        return false;
    }
    const bool updated = CarrySyncStep::Advance<RE::NiUpdateData>(
        lease.scene.get(), lease.sync.get(), [](RE::NiTimeController* controller, RE::NiUpdateData& data) {
            // The inspected native NiAVObject/NiNode callers pass NiUpdateData*
            // in RDX at slot 0x27. The pinned CommonLib declares Update(float),
            // which is not that ABI. Use its virtual-call wrapper with the
            // verified signature, only for the native controller we created.
            REL::RelocateVirtual<void(RE::NiTimeController*, RE::NiUpdateData&)>(0x27, 0x27, controller, data);
        });
    if (updated) ++lease.ticks;
    return updated;
}
void End(Lease& lease) {
    if (lease.sync) {
        // Keep the reference until SetTarget has released the scene's reference.
        // Never use the native remove-by-type helper: a replacement is not ours.
        lease.sync->flags.reset(RE::NiTimeController::Flag::kActive);
        // Detach this exact instance even if it was retargeted elsewhere. A
        // replacement controller on the old scene is never touched.
        if (lease.sync->target) lease.sync->SetTarget(nullptr);
        logger::info(
            "[CarryAttachment] detached owned controller={} explicitTicks={}", fmt::ptr(lease.sync.get()), lease.ticks);
    }
    lease.sync.reset();
    lease.scene.reset();
    lease.carrierScene.reset();
    lease.anchor.reset();
    lease.ticks = 0;
}
}

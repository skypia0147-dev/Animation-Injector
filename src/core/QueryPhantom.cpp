#include "PCH.h"
#include "core/QueryPhantom.h"
#include <hde64.h>

namespace AnimationInjector {
namespace {
using Construct = RE::hkpCachingShapePhantom* (*)(void*, const RE::hkpShape*, const RE::hkTransform*, std::uint32_t);

bool CodeRange(std::uintptr_t address, std::uintptr_t& end) {
    const auto& code = REL::Module::get().segment(REL::Segment::textx);
    if (address < code.address() || address >= code.address() + code.size()) return false;
    DWORD64 base = 0;
    auto* entry = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!entry || base + entry->BeginAddress != address) return false;
    end = base + entry->EndAddress;
    return end > address && end <= code.address() + code.size() && end - address <= 0x1000;
}

Construct Resolve() {
    // Resolve from the Address Library vtable, then validate instruction boundaries.
    // No constructor ID is guessed for another runtime.
    const REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkpCachingShapePhantom[0]};
    const auto clone = reinterpret_cast<const std::uintptr_t*>(table.address())[0x0C];
    std::uintptr_t end = 0;
    if (!CodeRange(clone, end)) return nullptr;
    std::uintptr_t candidate = 0;
    unsigned matches = 0;
    for (auto at = clone; at < end;) {
        hde64s ins{};
        auto size = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
        if (!size || (ins.flags & F_ERROR) || at + size > end) return nullptr;
        // Verified native Clone passes filter, shape, and transform to its constructor.
        constexpr unsigned char arguments[]{0x44, 0x8B, 0x4F, 0x4C, 0x48, 0x8B, 0x57, 0x20};
        if (size == 5 && ins.opcode == 0xE8 && at >= clone + sizeof(arguments) &&
            std::memcmp(reinterpret_cast<const void*>(at - sizeof(arguments)), arguments, sizeof(arguments)) == 0) {
            candidate = at + size + static_cast<std::int32_t>(ins.imm.imm32);
            ++matches;
        }
        at += size;
    }
    if (matches != 1 || !CodeRange(candidate, end)) return nullptr;
    bool correctTable = false;
    for (auto at = candidate; at < end;) {
        hde64s ins{};
        auto size = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
        if (!size || (ins.flags & F_ERROR) || at + size > end) return nullptr;
        // Verify that this constructor installs the expected concrete vtable.
        constexpr unsigned char store[]{0x48, 0x89, 0x03};
        if (size == 7 && ins.rex_w == 1 && ins.rex_r == 0 && ins.rex_x == 0 && ins.rex_b == 0 && ins.opcode == 0x8D &&
            ins.modrm == 0x05 && at + size + sizeof(store) <= end &&
            at + size + static_cast<std::int32_t>(ins.disp.disp32) == table.address() &&
            std::memcmp(reinterpret_cast<const void*>(at + size), store, sizeof(store)) == 0)
            correctTable = true;
        at += size;
    }
    if (!correctTable) return nullptr;
    return reinterpret_cast<Construct>(candidate);
}
}
QueryPhantom::QueryPhantom(const RE::hkpShape* shape, const RE::hkTransform& transform, std::uint32_t filter) {
    static const Construct construct = Resolve();
    if (!construct || !shape) {
        logger::warn("[SpaceProbe] independent constructor unavailable: resolved={} shape={}", construct != nullptr,
            shape != nullptr);
        return;
    }
    auto* router = RE::hkMemoryRouter::GetInstancePtr();
    if (!router || !router->Heap) {
        logger::warn("[SpaceProbe] Havok query allocator unavailable");
        return;
    }
    constexpr auto bytes = static_cast<std::int32_t>(sizeof(RE::hkpCachingShapePhantom));
    void* allocation = router->Heap->BlockAlloc(bytes);
    if (!allocation) return;
    // Match the native allocator/constructor contract; no live engine object is written.
    std::memset(allocation, 0, bytes);
    auto* storage = static_cast<RE::hkpCachingShapePhantom*>(allocation);
    storage->memSizeAndFlags = static_cast<std::uint16_t>(bytes);
    object = construct(allocation, shape, &transform, filter);
    // The validated constructor returns its input storage.
    if (object != storage) {
        logger::critical("[SpaceProbe] query constructor returned unexpected storage; query disabled");
        object = nullptr;
    }
}
QueryPhantom::~QueryPhantom() {
    if (object) object->RemoveReference();
}
}

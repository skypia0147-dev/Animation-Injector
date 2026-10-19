#include "core/CarrySyncResolver.h"
#include <hde64.h>
#include <cstring>
#include <vector>
#include <algorithm>

namespace AnimationInjector::CarrySyncResolver {
namespace {
struct Instruction {
    std::uintptr_t at;
    hde64s code;
    unsigned size;
};
using Instructions = std::vector<Instruction>;
Instructions Decode(std::uintptr_t start, CodeRange range) {
    std::uintptr_t end = 0;
    if (!range(start, end) || end <= start || end - start > 0x1000) return {};
    Instructions result;
    for (auto at = start; at < end;) {
        // hde64 may inspect up to 15 bytes. Pad the end of the function locally.
        unsigned char bytes[16]{};
        std::memcpy(bytes, reinterpret_cast<const void*>(at), std::min<std::uintptr_t>(15, end - at));
        hde64s code{};
        const auto size = hde64_disasm(bytes, &code);
        if (!size || (code.flags & F_ERROR) || at + size > end) return {};
        result.push_back({at, code, size});
        at += size;
    }
    return result;
}
std::vector<std::uintptr_t> Calls(const Instructions& code) {
    std::vector<std::uintptr_t> calls;
    for (const auto& ins : code)
        if (ins.size == 5 && ins.code.opcode == 0xE8) {
            const auto destination = ins.at + ins.size + static_cast<std::int32_t>(ins.code.imm.imm32);
            if (std::find(calls.begin(), calls.end(), destination) == calls.end()) calls.push_back(destination);
        }
    return calls;
}
bool InstallsTable(std::uintptr_t constructor, std::uintptr_t table, CodeRange range) {
    const auto code = Decode(constructor, range);
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
        const auto& ins = code[i];
        // lea rax,[native vtable]; mov [rdi],rax -- verified constructor ABI.
        constexpr unsigned char store[]{0x48, 0x89, 0x07};
        if (ins.size == 7 && ins.code.opcode == 0x8D && ins.code.rex_w == 1 && ins.code.rex_r == 0 &&
            ins.code.rex_b == 0 && ins.code.modrm == 0x05 &&
            ins.at + ins.size + static_cast<std::int32_t>(ins.code.disp.disp32) == table &&
            code[i + 1].size == sizeof(store) &&
            std::memcmp(reinterpret_cast<const void*>(code[i + 1].at), store, sizeof(store)) == 0)
            return true;
    }
    return false;
}
bool CreatesSync(std::uintptr_t registration, std::uintptr_t table, CodeRange range) {
    // Register -> prepare -> native constructor. Limit to these two levels.
    unsigned matches = 0;
    for (auto prepare : Calls(Decode(registration, range)))
        for (auto constructor : Calls(Decode(prepare, range)))
            if (InstallsTable(constructor, table, range)) ++matches;
    return matches == 1;
}
}
std::uintptr_t Resolve(std::uintptr_t mountUpdate, std::uintptr_t syncVtable, CodeRange range) {
    if (!range || !mountUpdate || !syncVtable) return 0;
    std::uintptr_t result = 0;
    unsigned matches = 0;
    for (auto attach : Calls(Decode(mountUpdate, range))) {
        const auto code = Decode(attach, range);
        for (std::size_t i = 5; i < code.size(); ++i) {
            const auto& call = code[i];
            if (call.size != 5 || call.code.opcode != 0xE8) continue;
            // Native mount passes (follower, carrier, anchor, offset, false).
            // Verify whole instructions, not arbitrary bytes inside an operand.
            constexpr unsigned char physicsOff[]{0xC6, 0x44, 0x24, 0x20, 0x00};
            constexpr unsigned char actors[]{0x4C, 0x8B, 0xC0, 0x49, 0x8B, 0x16, 0x48, 0x8B, 0x0B};
            const auto& offset = code[i - 4];
            if (code[i - 5].size != sizeof(physicsOff) ||
                std::memcmp(reinterpret_cast<const void*>(code[i - 5].at), physicsOff, sizeof(physicsOff)) != 0 ||
                offset.size != 7 || offset.code.opcode != 0x8D || offset.code.rex_w != 1 || offset.code.rex_r != 1 ||
                offset.code.modrm != 0x0D || code[i - 3].size != 3 || code[i - 2].size != 3 || code[i - 1].size != 3 ||
                std::memcmp(reinterpret_cast<const void*>(code[i - 3].at), actors, sizeof(actors)) != 0)
                continue;
            const auto candidate = call.at + call.size + static_cast<std::int32_t>(call.code.imm.imm32);
            if (CreatesSync(candidate, syncVtable, range)) {
                result = candidate;
                ++matches;
            }
        }
    }
    return matches == 1 ? result : 0;
}
}

#include "core/PairInputGate.h"
#include <hde64.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace AnimationInjector::PairInputGate {
std::uintptr_t FindCall(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t managerGlobal) {
    if (!begin || end <= begin || end - begin > 0x10000 || !managerGlobal) return 0;
    struct Instruction {
        std::uintptr_t at = 0;
        unsigned size = 0;
        hde64s code{};
    };
    std::array<Instruction, 5> recent{};
    std::uintptr_t found = 0;
    for (auto at = begin; at < end;) {
        unsigned char bytes[16]{};
        std::memcpy(bytes, reinterpret_cast<const void*>(at), std::min<std::uintptr_t>(15, end - at));
        hde64s code{};
        const auto size = hde64_disasm(bytes, &code);
        if (!size || (code.flags & F_ERROR) || at + size > end) return 0;
        std::rotate(recent.begin(), recent.begin() + 1, recent.end());
        recent.back() = {at, size, code};
        const auto& arg = recent[0];
        const auto& load = recent[1];
        const auto& call = recent[2];
        const auto& test = recent[3];
        const auto& branch = recent[4];
        // LEA RDX,[RSP+local handle]; MOV RCX,[RIP+sync manager]; CALL;
        // TEST AL,AL; JNE blocked. An existing TDM call target is accepted
        // and chained by Install, but a changed instruction sequence is not.
        const bool handle = arg.code.opcode == 0x8D && arg.code.rex_w && !arg.code.rex_r && !arg.code.rex_b &&
                            !arg.code.rex_x && arg.code.modrm_reg == 2 && arg.code.modrm_rm == 4 &&
                            (arg.code.modrm_mod == 1 || arg.code.modrm_mod == 2) && arg.code.sib == 0x24;
        const bool manager = load.size == 7 && load.code.opcode == 0x8B && load.code.rex_w == 1 && !load.code.rex_r &&
                             !load.code.rex_x && !load.code.rex_b && load.code.modrm == 0x0D &&
                             load.at + load.size + static_cast<std::int32_t>(load.code.disp.disp32) == managerGlobal;
        const bool directCall = call.size == 5 && call.code.opcode == 0xE8;
        const bool result = test.size == 2 && test.code.opcode == 0x84 && test.code.modrm == 0xC0;
        const bool reject = (branch.code.opcode == 0x0F && branch.code.opcode2 == 0x85) || branch.code.opcode == 0x75;
        if (handle && manager && directCall && result && reject) {
            if (found) return 0;
            found = call.at;
        }
        at += size;
    }
    return found;
}
std::uintptr_t FindMotionCall(
    std::uintptr_t begin, std::uintptr_t end, std::uintptr_t fixedStringsGetter, std::uint32_t syncedMember) {
    if (!begin || end <= begin || end - begin > 0x10000 || !fixedStringsGetter) return 0;
    struct Instruction {
        std::uintptr_t at = 0;
        unsigned size = 0;
        hde64s code{};
    };
    std::array<Instruction, 8> recent{};
    std::uintptr_t found = 0;
    auto memory = [](const hde64s& a, const hde64s& b) {
        return a.modrm_mod == b.modrm_mod && a.modrm_rm == b.modrm_rm && a.rex_b == b.rex_b && a.rex_x == b.rex_x &&
               a.sib == b.sib && a.disp.disp32 == b.disp.disp32;
    };
    for (auto at = begin; at < end;) {
        unsigned char bytes[16]{};
        std::memcpy(bytes, reinterpret_cast<const void*>(at), std::min<std::uintptr_t>(15, end - at));
        hde64s code{};
        const auto size = hde64_disasm(bytes, &code);
        if (!size || (code.flags & F_ERROR) || at + size > end) return 0;
        std::rotate(recent.begin(), recent.begin() + 1, recent.end());
        recent.back() = {at, size, code};
        const auto& getter = recent[0];
        const auto& table = recent[1];
        const auto& name = recent[2];
        const auto& output = recent[3];
        const auto& holder = recent[4];
        const auto& call = recent[5];
        const auto& compare = recent[6];
        const auto& branch = recent[7];
        // FixedStrings::GetSingleton -> named member -> bool virtual read ->
        // compare that same output local. Do not select other graph queries.
        const bool singleton = getter.size == 5 && getter.code.opcode == 0xE8 &&
                               getter.at + 5 + static_cast<std::int32_t>(getter.code.imm.imm32) == fixedStringsGetter;
        const bool variable = name.code.opcode == 0x8D && name.code.rex_w && !name.code.rex_r && !name.code.rex_b &&
                              name.code.modrm_reg == 2 && name.code.modrm_mod == 2 && name.code.modrm_rm == 0 &&
                              name.code.disp.disp32 == syncedMember;
        const bool receiver = table.code.opcode == 0x8B && table.code.rex_w && holder.code.opcode == 0x8D &&
                              holder.code.rex_w && !holder.code.rex_r && holder.code.modrm_reg == 1 &&
                              (holder.code.modrm_mod == 1 || holder.code.modrm_mod == 2) &&
                              memory(table.code, holder.code);
        const bool local = output.code.opcode == 0x8D && output.code.rex_w && output.code.rex_r &&
                           output.code.modrm_reg == 0 && !output.code.rex_b && !output.code.rex_x &&
                           (output.code.modrm_mod == 1 || output.code.modrm_mod == 2) &&
                           (output.code.modrm_rm == 5 || (output.code.modrm_rm == 4 && output.code.sib == 0x24));
        // IAnimationGraphManagerHolder::GetGraphVariableImpl3, documented slot 0x12.
        const bool virtualRead = call.size == 7 && call.code.opcode == 0xFF && call.code.modrm_reg == 2 &&
                                 !call.code.rex_r && call.code.modrm_mod == 2 &&
                                 call.code.modrm_rm == table.code.modrm_reg && call.code.rex_b == table.code.rex_r &&
                                 call.code.disp.disp32 == 0x12 * sizeof(std::uintptr_t);
        const bool result = compare.code.opcode == 0x80 && compare.code.modrm_reg == 7 && compare.code.imm.imm8 == 0 &&
                            memory(output.code, compare.code);
        const bool reject = (branch.code.opcode == 0x0F && branch.code.opcode2 == 0x85) || branch.code.opcode == 0x75;
        if (singleton && variable && receiver && local && virtualRead && result && reject) {
            const auto displacement = branch.code.opcode == 0x75
                                          ? static_cast<std::int32_t>(static_cast<std::int8_t>(branch.code.imm.imm8))
                                          : static_cast<std::int32_t>(branch.code.imm.imm32);
            const auto target = branch.at + branch.size + displacement;
            if (found || target < begin || target >= end) return 0;
            found = call.at;
        }
        at += size;
    }
    return found;
}
}

#include "core/FireArrowResolver.h"
#include <hde64.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace AnimationInjector::FireArrowResolver {
namespace {
struct Instruction {
    std::uintptr_t at;
    unsigned size;
    hde64s code;
};
std::vector<Instruction> Decode(std::uintptr_t start, CodeRange range) {
    std::uintptr_t end = 0;
    if (!range || !range(start, end) || end <= start || end - start > 0x1000) return {};
    std::vector<Instruction> result;
    for (auto at = start; at < end;) {
        unsigned char bytes[16]{};
        std::memcpy(bytes, reinterpret_cast<const void*>(at), std::min<std::uintptr_t>(15, end - at));
        hde64s code{};
        const unsigned size = hde64_disasm(bytes, &code);
        if (!size || (code.flags & F_ERROR) || at + size > end) return {};
        result.push_back({at, size, code});
        at += size;
    }
    return result;
}
std::uintptr_t Call(const Instruction& i) {
    return i.size == 5 && i.code.opcode == 0xE8 ? i.at + 5 + static_cast<std::int32_t>(i.code.imm.imm32) : 0;
}
template<std::size_t N>
bool Bytes(const Instruction& i, const unsigned char (&b)[N]) {
    return i.size == N && std::memcmp(reinterpret_cast<const void*>(i.at), b, N) == 0;
}
bool LoadRCX(const Instruction& i) {
    // mov rcx,[rip+singleton] passes the same native argument as mouse release.
    return i.size == 7 && i.code.opcode == 0x8B && i.code.rex_w == 1 && i.code.modrm == 0x0D;
}
}
Route Resolve(std::uintptr_t processButton, CodeRange range) {
    Route result;
    unsigned matches = 0;
    std::vector<std::uintptr_t> visited;
    for (const auto& parent : Decode(processButton, range)) {
        const auto child = Call(parent);
        if (!child || std::find(visited.begin(), visited.end(), child) != visited.end()) continue;
        visited.push_back(child);
        const auto body = Decode(child, range);
        Route candidate;
        unsigned presses = 0, releases = 0;
        for (std::size_t i = 1; i + 4 < body.size(); ++i) {
            if (!LoadRCX(body[i - 1])) continue;
            const auto& ins = body[i];
            if (ins.size != 5 || ins.code.opcode != 0xBA) continue;  // mov edx, action
            if (ins.code.imm.imm32 == 49 && Bytes(body[i + 1], {0x44, 0x8D, 0x42, 0xD1}) && Call(body[i + 2])) {
                ++presses;
                candidate.pressCall = body[i + 2].at;
                candidate.dispatch = Call(body[i + 2]);
            }
            if (ins.code.imm.imm32 == 51 && Bytes(body[i + 1], {0x44, 0x8D, 0x42, 0xCF}) && Call(body[i + 2]) &&
                LoadRCX(body[i + 3]) && Call(body[i + 4])) {
                ++releases;
                candidate.releaseCall = body[i + 2].at;
                candidate.finishCharge = Call(body[i + 4]);
            }
        }
        if (presses != 1 || releases != 1 || !candidate) continue;
        const auto it =
            std::find_if(body.begin(), body.end(), [&](const auto& v) { return v.at == candidate.releaseCall; });
        if (it == body.end() || Call(*it) != candidate.dispatch) continue;
        std::uintptr_t end = 0;
        if (!range(candidate.dispatch, end) || !range(candidate.finishCharge, end)) continue;
        result = candidate;
        ++matches;
    }
    return matches == 1 ? result : Route{};
}
bool Validate(const Route& route, std::uintptr_t sourceActionMap, CodeRange range) {
    if (!route) return false;
    const auto dispatch = Decode(route.dispatch, range), charge = Decode(route.finishCharge, range),
               source = Decode(sourceActionMap, range);
    if (dispatch.empty() || charge.empty() || source.empty()) return false;
    // Re-read both call operands immediately before installing either hook.
    for (const auto site : {route.pressCall, route.releaseCall}) {
        unsigned char bytes[5];
        std::memcpy(bytes, reinterpret_cast<const void*>(site), 5);
        if (bytes[0] != 0xE8) return false;
        std::int32_t displacement;
        std::memcpy(&displacement, bytes + 1, 4);
        if (site + 5 + displacement != route.dispatch) return false;
    }
    // Native dispatch must share the action-data construction/processing/
    // destruction pipeline of the Address Library SourceActionMap entry point.
    std::vector<std::uintptr_t> common;
    for (const auto& ins : source)
        if (const auto call = Call(ins); call && std::any_of(dispatch.begin(), dispatch.end(), [&](const auto& d) {
                return Call(d) == call;
            }) && std::find(common.begin(), common.end(), call) == common.end())
            common.push_back(call);
    if (common.size() < 4) return false;
    // Charge finalization must check native bow type 7 and write the completed
    // timestamp only into an unfinished record. These are instruction signatures,
    // never offsets used to access actor memory from the plugin.
    const auto has = [&](auto predicate) { return std::any_of(charge.begin(), charge.end(), predicate); };
    return has([](const auto& i) { return Bytes(i, {0x80, 0xB8, 0x9D, 0x01, 0x00, 0x00, 0x07}); }) &&
           has([](const auto& i) { return Bytes(i, {0x48, 0x83, 0x3B, 0x00}); }) &&
           has([](const auto& i) { return Bytes(i, {0x48, 0x89, 0x03}); }) &&
           has([](const auto& i) { return i.code.opcode == 0xFF && i.code.modrm == 0x15; });
}
}

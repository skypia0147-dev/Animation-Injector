#pragma once
#include <hde64.h>
#include <cstdint>

// What sits at a fixed offset inside an engine function, judged by decoding whole instructions from the
// function's first byte. A lone 0xE8 byte proves nothing: on a build where the function moved, that byte can be
// part of another instruction's operand, and writing a call over it corrupts the function.
namespace AnimationInjector::CallSite {

enum class Shape {
    kCall,         // an instruction starts at the site and it is a 5-byte rel32 call
    kNotCall,      // the site is inside another instruction, or the instruction there is something else
    kUndecodable,  // the decoder gave up before or at the site: no evidence either way
};

inline Shape Classify(std::uintptr_t a_function, std::uintptr_t a_site) {
    if (a_site < a_function || a_site - a_function > 0x10000) return Shape::kNotCall;
    auto at = a_function;
    while (at < a_site) {
        hde64s ins{};
        const auto size = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
        if (!size || (ins.flags & F_ERROR)) return Shape::kUndecodable;
        at += size;
    }
    if (at != a_site) return Shape::kNotCall;
    hde64s ins{};
    const auto size = hde64_disasm(reinterpret_cast<const void*>(at), &ins);
    if (!size || (ins.flags & F_ERROR)) return Shape::kUndecodable;
    return size == 5 && ins.opcode == 0xE8 ? Shape::kCall : Shape::kNotCall;
}

}  // namespace AnimationInjector::CallSite

#include "core/CallSiteShape.h"
#include <hde64.h>
#include <cstdlib>
#include <iostream>
namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "SpaceDecoder failed: " << message << '\n';
        std::abort();
    }
}
}
int main() {
    // Actual RIP-relative vtable load used by the native constructor.
    unsigned char bytes[16]{0x48, 0x8D, 0x05, 0x7C, 0xB4, 0xE2, 0x00};
    hde64s ins{};
    auto size = hde64_disasm(bytes, &ins);
    if (size != 7 || (ins.flags & F_ERROR) || ins.rex_w != 1 || ins.rex_r || ins.rex_x || ins.rex_b ||
        ins.opcode != 0x8D || ins.modrm != 0x05)
        std::abort();
    std::cout << "SpaceDecoder: native LEA accepted, raw rex=" << unsigned(ins.rex) << "\n";

    // sub rsp,28h / lea rax,[rip+0E8h] / call +10h / ret. The lea operand holds an E8 byte at +7.
    using namespace AnimationInjector::CallSite;
    unsigned char function[48]{
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x05, 0xE8, 0x00, 0x00, 0x00, 0xE8, 0x10, 0x00, 0x00, 0x00, 0xC3};
    const auto base = reinterpret_cast<std::uintptr_t>(function);
    Check(Classify(base, base + 11) == Shape::kCall, "call at an instruction boundary accepted");
    Check(Classify(base, base + 7) == Shape::kNotCall, "E8 byte inside an operand rejected");
    Check(Classify(base, base + 4) == Shape::kNotCall, "another instruction at the site rejected");
    Check(Classify(base, base + 16) == Shape::kNotCall, "ret at the site rejected");
    Check(Classify(base, base - 1) == Shape::kNotCall, "site before the function rejected");
    Check(Classify(base, base + 0x10001) == Shape::kNotCall, "site far past the function rejected");
    std::cout << "CallSite: 1 call accepted, 5 non-call sites rejected\n";
}

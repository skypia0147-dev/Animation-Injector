#include <hde64.h>
#include <cstdlib>
#include <iostream>
int main() {
    // Actual RIP-relative vtable load used by the native constructor.
    unsigned char bytes[16]{0x48, 0x8D, 0x05, 0x7C, 0xB4, 0xE2, 0x00};
    hde64s ins{};
    auto size = hde64_disasm(bytes, &ins);
    if (size != 7 || (ins.flags & F_ERROR) || ins.rex_w != 1 || ins.rex_r || ins.rex_x || ins.rex_b ||
        ins.opcode != 0x8D || ins.modrm != 0x05)
        std::abort();
    std::cout << "SpaceDecoder: native LEA accepted, raw rex=" << unsigned(ins.rex) << "\n";
}

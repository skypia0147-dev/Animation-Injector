#include "core/PairInputGate.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using AnimationInjector::PairInputGate::FindCall;
int main(int argc, char** argv) {
    // Native AE PlayerControls function captured with TDM's call already installed.
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> code((std::istreambuf_iterator<char>(file)), {});
    assert(code.size() == 0x6F0);
    auto begin = reinterpret_cast<std::uintptr_t>(code.data());
    const auto global = begin + (0x3137780 - 0x79A970);
    assert(FindCall(begin, begin + code.size(), global) == begin + 0x153);
    // The chain target may be native, TDM, or another previous hook.
    const std::int32_t changedTarget = 0x13579;
    std::memcpy(code.data() + 0x154, &changedTarget, sizeof(changedTarget));
    assert(FindCall(begin, begin + code.size(), global) == begin + 0x153);
    assert(FindCall(begin, begin + code.size(), global + 8) == 0);
    code[0x153] = 0xE9;  // Do not overwrite a changed call instruction.
    assert(FindCall(begin, begin + code.size(), global) == 0);
    code[0x153] = 0xE8;
    assert(FindCall(begin, begin + 0x158, global) == 0);  // Missing result check.
    assert(FindCall(begin, begin, global) == 0);

    // Alternate stack displacement and short branch, without any fixed call offset.
    std::vector<unsigned char> fixture{0x90, 0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0,
        0, 0x84, 0xC0, 0x75, 0x00, 0xC3};
    begin = reinterpret_cast<std::uintptr_t>(fixture.data());
    assert(FindCall(begin, begin + fixture.size(), begin + 13) == begin + 13);
    // A duplicate query is ambiguous and must not patch either site.
    const auto second = fixture;
    fixture.insert(fixture.end(), second.begin(), second.end());
    begin = reinterpret_cast<std::uintptr_t>(fixture.data());
    const std::int32_t displacement = -static_cast<std::int32_t>(second.size());
    std::memcpy(fixture.data() + second.size() + 9, &displacement, sizeof(displacement));
    assert(FindCall(begin, begin + fixture.size(), begin + 13) == 0);
    std::cout
        << "PairInputGate: native/TDM fixture, operand resolution, alternative encoding, changed/truncated/ambiguous rejection passed\n";
}

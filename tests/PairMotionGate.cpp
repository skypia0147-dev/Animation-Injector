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

using AnimationInjector::PairInputGate::FindMotionCall;
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<unsigned char> native((std::istreambuf_iterator<char>(file)), {});
    assert(native.size() > 0x300);
    auto resolve = [](const std::vector<unsigned char>& bytes, std::uint32_t member = 0x578) {
        const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
        const auto getter = begin + 0x14EF60 - 0x6698D0;
        return FindMotionCall(begin, begin + bytes.size(), getter, member);
    };
    const auto begin = reinterpret_cast<std::uintptr_t>(native.data());
    // Real AE function with existing TDM hooks elsewhere in the same function.
    assert(resolve(native) == begin + 0x20D);
    assert(resolve(native, 0x570) == 0);  // bAllowRotation is a different query.
    assert(resolve(native, 0x580) == 0);  // Other motion query is not ours either.
    for (const auto offset : {0x1F2, 0x1FB, 0x202, 0x209, 0x20D, 0x214, 0x21B}) {
        auto changed = native;
        changed[offset] = 0x90;
        assert(resolve(changed) == 0);  // Changed signature fails closed.
    }
    auto changed = native;
    changed[0x210] = 0x80;  // Float getter instead of bool getter.
    assert(resolve(changed) == 0);
    changed = native;
    changed[0x216] ^= 8;  // Comparison reads a different stack local.
    assert(resolve(changed) == 0);
    changed.assign(native.begin(), native.begin() + 0x214);
    assert(resolve(changed) == 0);  // Truncated after the call.
    // Preserve RIP-relative targets when inserting unrelated instructions first.
    changed = native;
    changed.insert(changed.begin(), 16, 0x90);
    const auto shifted = reinterpret_cast<std::uintptr_t>(changed.data());
    assert(FindMotionCall(shifted, shifted + changed.size(), shifted + 16 + 0x14EF60 - 0x6698D0, 0x578) ==
           shifted + 16 + 0x20D);
    // Two matching read sites are ambiguous, so neither may be patched.
    changed = native;
    changed.insert(changed.end(), native.begin(), native.end());
    std::int32_t rel = 0;
    std::memcpy(&rel, changed.data() + native.size() + 0x1F3, 4);
    rel -= static_cast<std::int32_t>(native.size());
    std::memcpy(changed.data() + native.size() + 0x1F3, &rel, 4);
    assert(resolve(changed) == 0);
    std::cout
        << "PairMotionGate: real AE/TDM, relocated site, wrong variable/getter/local, modified/truncated/duplicate rejection passed\n";
}

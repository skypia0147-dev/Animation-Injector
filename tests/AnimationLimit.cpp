#include "core/AnimationLimit.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

// The binding cap follows the engine's index reading and OAR's limit (11-05 report: OAR raised to 65,534,
// our fixed 32,767 skipped every Dynamic Sprint Stop clip at 33,275 names).
int main() {
    using namespace AnimationInjector::AnimationLimit;
    // Vanilla, no OAR: the old fixed limits.
    constexpr auto vanilla = Compute(false, -1);
    static_assert(vanilla.maxBindingIndex == 0x7FFF && vanilla.bindingCap == 0x7FFF);
    static_assert(vanilla.nameTableLimit == 0x7FFF - 1024);
    // OAR at its default 32,767: indices below the count OAR stops at.
    constexpr auto oarDefault = Compute(false, 0x7FFF);
    static_assert(oarDefault.maxBindingIndex == 0x7FFE && oarDefault.nameTableLimit == 0x7FFF - 1024);
    // The report: OAR's patch on and uAnimationLimit 65,534. 33,275 names no longer stop us.
    constexpr auto raised = Compute(true, 65534);
    static_assert(raised.bindingCap == 65534 && raised.maxBindingIndex == 65533);
    static_assert(raised.nameTableLimit == 65534 - 1024 && 33275 < raised.nameTableLimit);
    // The INI raised but the engine still reads signed (patch off or failed): the engine decides.
    constexpr auto iniOnly = Compute(false, 65534);
    static_assert(iniOnly.bindingCap == 0x7FFF && iniOnly.maxBindingIndex == 0x7FFF);
    // Patched engine with OAR somewhere in between, and a patched engine without OAR.
    static_assert(Compute(true, 40000).maxBindingIndex == 39999 && Compute(true, 40000).bindingCap == 40000);
    static_assert(Compute(true, -1).maxBindingIndex == 0xFFFE);
    // A limit below the reserve leaves no room for names rather than a negative count.
    static_assert(Compute(true, 500).nameTableLimit == 0);

    // Only both loads rewritten to movzx count as unsigned.
    constexpr std::uint8_t movzx[2]{0x0F, 0xB7}, movsx[2]{0x0F, 0xBF}, other[2]{0xE8, 0x00};
    static_assert(ReadsUnsigned(movzx, movzx));
    static_assert(!ReadsUnsigned(movsx, movsx) && !ReadsUnsigned(movzx, movsx) && !ReadsUnsigned(movsx, movzx));
    static_assert(!ReadsUnsigned(other, movzx) && !ReadsUnsigned(movzx, other));

    // OAR's INI as OAR writes it: UTF-8 byte order mark first (GetPrivateProfileString missed [General]
    // behind it, 11-05 in-game log: "OAR limit 32767" with 65534 in the file).
    static_assert(OarIniLimit("\xEF\xBB\xBF[General]\r\nuAnimationLimit = 65534\r\nuHavokHeapSize = 1\r\n") == 65534);
    static_assert(OarIniLimit("[General]\nuAnimationLimit=40000\n") == 40000);
    static_assert(OarIniLimit("; c\n[ general ]\n  UANIMATIONLIMIT\t=  1234 ; note\n") == 1234);
    // Only [General]'s key, and nothing for a missing, empty or non-numeric value.
    static_assert(OarIniLimit("[Experimental]\nuAnimationLimit = 65534\n[General]\nuHavokHeapSize = 1\n") == -1);
    static_assert(OarIniLimit("[General]\nuAnimationLimit = \n") == -1);
    static_assert(OarIniLimit("[General]\nuAnimationLimit = x\n") == -1 && OarIniLimit("") == -1);
    static_assert(OarIniLimit("[General]\nuAnimationLimit = 99999999\n") == 0xFFFF);

    // The same checks at run time, so a constexpr slip in a future edit still fails the test.
    assert(Compute(true, 65534).nameTableLimit == 64510);
    assert(Compute(false, 65534).maxBindingIndex == 32767);
    std::cout << "AnimationLimit: ok\n";
    return 0;
}

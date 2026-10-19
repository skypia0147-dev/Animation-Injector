#include "runtime/CarryOwnership.h"
#include "runtime/PairRecoveryGate.h"
#include "runtime/PairTargetMath.h"
#include <limits>
#include <cstdlib>
#include <iostream>
using namespace AnimationInjector::CarryOwnership;
void TestCarryPlacement();
void TestCarryRelativeHandoff();
void TestCarrySceneRefresh();
void TestNativeCarryPlacement();
void TestNativeCarryTick();
void TestCarrySyncResolver(const char* path);
void require(bool value) {
    if (!value) std::abort();
}
int main(int argc, char** argv) {
    // Early-held input, late input, event-only, fallback, interruption and reset.
    for (float at : {0.0f, 0.25f, 3.0f}) {
        AnimationInjector::PairRecoveryGate gate{true, false, false, at};
        require(!gate.Consume(true, true));
        gate.ObserveClock(at - 0.01f);
        require(!gate.Consume(true, true));
        gate.ObserveClock(std::numeric_limits<float>::quiet_NaN());
        require(!gate.open);
        gate.ObserveClock(at);
        require(!gate.Consume(false, true));
        require(!gate.Consume(true, false));
        require(gate.Consume(true, true));
        require(!gate.Consume(true, true));
        gate = {};
        gate.ObserveClock(100);
        gate.ObserveEvent();
        require(!gate.Consume(true, true));
    }
    AnimationInjector::PairRecoveryGate eventOnly{true};
    eventOnly.ObserveClock(100);
    require(!eventOnly.Consume(true, true));
    eventOnly.ObserveEvent();
    require(!eventOnly.Consume(false, true));
    require(eventOnly.Consume(true, true));
    AnimationInjector::PairRecoveryGate fallback{true, false, false, 2};
    fallback.ObserveClock(2);
    require(fallback.Consume(true, true));
    std::cout
        << "Pair recovery: held/late input, authored clip time, event-only/fallback, interruption and load reset passed\n";
    Flags f;
    require(f.Acquire(0x21, 0xC) == 0x2D);
    require(f.Restore(0x6D) == 0x61);  // Preserve an unrelated engine update.
    require(f.Restore(0x61) == 0x61);  // Repeated cleanup is inert.
    require(f.Acquire(0x2D, 0xC) == 0x2D);
    require(f.Restore(0x2D) == 0x2D);  // Already active flags are never ours.
    for (unsigned bits = 0; bits < 256; ++bits) {
        bool v[8];
        for (unsigned i = 0; i < 8; ++i) v[i] = (bits & (1 << i)) != 0;
        const auto r = Completed(true, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        require(r == (bits == 255 ? PairResult::kSuccess : !v[0] ? PairResult::kWait : PairResult::kFailure));
        require(Completed(false, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]) == PairResult::kFailure);
    }
    std::cout << "Carry flag ownership, repeated restoration and 512 pair completion/interruption cases passed\n";
    require(SameScene({1, 60, false}, {1, 60, false}));
    require(SameScene({1, 60, false}, {2, 60, false}));  // A world grid boundary is not a teleport.
    require(SameScene({2, 60, false}, {1, 60, false}));
    require(!SameScene({1, 60, false}, {2, 61, false}));
    require(!SameScene({1, 0, false}, {2, 0, false}));
    require(!SameScene({0, 60, false}, {0, 60, false}));
    require(!SameScene({1, 60, false}, {0, 60, false}));
    require(!SameScene({0, 60, false}, {1, 60, false}));
    require(SameScene({1, 0, true}, {1, 0, true}));
    require(!SameScene({1, 0, true}, {2, 0, true}));
    require(!SameScene({1, 60, true}, {2, 60, false}));
    require(!SameScene({1, 60, false}, {2, 60, true}));
    std::cout << "12 exterior/interior/missing-cell/world-change cases passed\n";
    // A carried capsule retains its layer, part and collision group. Only the
    // no-collision bit is ours, even when another writer changes other fields.
    constexpr unsigned noCollision = 1u << 14;
    // Reported 0.12.90 sequence: old world's setter fails; MoveTo copies
    // 05CF401E into the new controller; reacquisition must not lose 00004000.
    ContactFlags transferContacts;
    auto packed = transferContacts.Acquire(0x05CF001E, noCollision);
    require(packed == 0x05CF401E && transferContacts.acquired == 0x4000);
    require(!transferContacts.Restore(packed, [](unsigned) { return false; }));
    require(transferContacts.acquired == 0x4000);
    packed = transferContacts.Acquire(packed, noCollision);
    require(transferContacts.acquired == 0x4000);
    require(transferContacts.Restore(packed, [&](unsigned restored) {
        packed = restored;
        return true;
    }));
    require(packed == 0x05CF001E && transferContacts.acquired == 0);
    // A new world's collision group and unrelated flags must survive cleanup.
    transferContacts.Acquire(0x05CF001E, noCollision);
    packed = 0x9876C020;
    transferContacts.Acquire(packed, noCollision);
    require(transferContacts.Restore(packed, [&](unsigned restored) {
        packed = restored;
        return true;
    }));
    require(packed == 0x98768020);
    // No-collision active before this carry belongs to someone else.
    transferContacts.Acquire(0x05CF401E, noCollision);
    bool wrote = false;
    require(transferContacts.Restore(0x05CF401E, [&](unsigned) {
        wrote = true;
        return true;
    }));
    require(!wrote && transferContacts.acquired == 0);
    // Engine already cleared the bit during reload: acknowledge without write.
    transferContacts.Acquire(0x05CF001E, noCollision);
    require(transferContacts.Restore(0xAAAA001E, [&](unsigned) {
        wrote = true;
        return true;
    }));
    require(!wrote && transferContacts.acquired == 0);
    std::cout
        << "Reported cell-drop filter leak reproduced: failed write retains ownership, new-world reacquire and confirmed cleanup passed\n";
    for (unsigned group : {0u, 1u, 57u, 65535u})
        for (unsigned layer : {0u, 30u, 32u}) {
            const unsigned filter = (group << 16) | layer | (3u << 8);
            Flags contacts;
            require(contacts.Acquire(filter, noCollision) == (filter | noCollision));
            require(contacts.Restore(filter | noCollision | (1u << 15)) == (filter | (1u << 15)));
            require(contacts.Restore(filter) == filter);
            require(contacts.Acquire(filter | noCollision, noCollision) == (filter | noCollision));
            require(contacts.Restore(filter | noCollision) == (filter | noCollision));
        }
    std::cout << "Carry collision-group restoration passed\n";
    using AnimationInjector::PairTargetMath::WithinRangeAndCone;
    require(!WithinRangeAndCone(82, 250, 90, 160));
    require(!WithinRangeAndCone(251, 250, 0, 160));
    require(!WithinRangeAndCone(std::numeric_limits<float>::quiet_NaN(), 250, 0, 160));
    require(!WithinRangeAndCone(82, 250, 180, 160));
    require(WithinRangeAndCone(82, 250, 0, 160));
    TestCarryPlacement();
    TestCarryRelativeHandoff();
    TestCarrySceneRefresh();
    TestNativeCarryPlacement();
    TestNativeCarryTick();
    // The native sync functions are captured game code, kept out of the public source.
    require(argc <= 2);
    if (argc == 2)
        TestCarrySyncResolver(argv[1]);
    else
        std::cout << "native carry fixture absent: sync resolver skipped\n";
}

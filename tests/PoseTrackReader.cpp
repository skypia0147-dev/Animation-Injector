#include "core/PoseTrackReader.h"
#include "core/PoseTrackScale.h"
#include "core/PoseTrackCopy.h"
#include <vector>
#include <iostream>
#include <limits>
#include <cstdlib>
using namespace AnimationInjector::PoseTrackReader;
void require(bool value) {
    if (!value) {
        std::cerr << "Pose bounds check failed\n";
        std::exit(1);
    }
}
int main() {
    std::array<std::byte, 160> bytes{};
    Master m{160, 3, {}};
    std::memcpy(bytes.data(), &m, 16);
    Header h{2, 2, 64, 48, 1, 0, 1, 0};
    auto put = [&] { std::memcpy(bytes.data() + 48, &h, 16); };
    put();
    std::array<float, 12> q{1, 2, 3, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    std::memcpy(bytes.data() + 112, q.data(), 48);
    require(Read(bytes, 2, 1).error == 0 && Read(bytes, 2, 1).qs[1] == 2);
    require(Read(bytes, 2, -1).error == 4 && Read(bytes, 2, 2).error == 4);
    require(Read(std::span(bytes).first(159), 2, 1).error == 1);
    h.flags = 4;
    put();
    require(Read(bytes, 2, 1).error == 3);
    h.flags = 0;
    h.offset = 63;
    put();
    require(Read(bytes, 2, 1).error == 5);
    h.offset = 65;
    put();
    require(Read(bytes, 2, 1).error == 5);
    h.offset = 64;
    h.count = 3;
    put();
    require(Read(bytes, 2, 1).error == 4);
    h.count = 2;
    h.stride = 32;
    put();
    require(Read(bytes, 2, 1).error == 3);
    h.stride = 48;
    put();
    q[0] = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bytes.data() + 112, q.data(), 48);
    require(Read(bytes, 2, 1).error == 6);
    require(Read(bytes, 3, 0).error == 2);
    // The write path: only a track the reader accepts is scaled, translation only, in place.
    q[0] = 1;
    std::memcpy(bytes.data() + 112, q.data(), 48);
    auto before = bytes;
    require(AnimationInjector::PoseTrackScale::ScaleTranslation(bytes, 2, 1, 0.5f) == 0);
    auto after = Read(bytes, 2, 1);
    require(after.qs[0] == 0.5f && after.qs[1] == 1 && after.qs[2] == 1.5f);
    require(std::memcmp(bytes.data() + 112 + 12, before.data() + 112 + 12, 36) == 0);  // rotation, scale untouched
    require(std::memcmp(bytes.data(), before.data(), 112) == 0);                       // other bones, headers
    before = bytes;
    require(AnimationInjector::PoseTrackScale::ScaleTranslation(bytes, 2, 2, 0.5f) == 4);
    require(AnimationInjector::PoseTrackScale::ScaleTranslation(bytes, 2, 1, 0.0f) == 7);
    h.flags = 4;
    put();
    before = bytes;
    require(AnimationInjector::PoseTrackScale::ScaleTranslation(bytes, 2, 1, 0.5f) == 3);
    require(bytes == before);
    // The carry's locked pose: a whole track copied out, then written over later outputs.
    using AnimationInjector::PoseTrackCopy::Apply;
    using AnimationInjector::PoseTrackCopy::Capture;
    using AnimationInjector::PoseTrackCopy::Pose;
    h.flags = 0;
    put();
    std::array<float, 12> first{4, 5, 6, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    std::memcpy(bytes.data() + 64, first.data(), 48);
    Pose pose;
    require(Capture(bytes, 2, pose) == 0 && pose.count == 2);
    auto target = bytes;
    std::array<float, 12> standing{9, 9, 9, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    std::memcpy(target.data() + 64, standing.data(), 48);
    std::memcpy(target.data() + 112, standing.data(), 48);
    require(Apply(target, 2, pose) == 0);
    require(target == bytes);  // both bones restored, headers untouched
    auto three = target;
    Header wider = h;
    wider.count = 1;
    std::memcpy(three.data() + 48, &wider, 16);
    before = three;
    require(Apply(three, 2, pose) == 9 && three == before);  // another skeleton: never written
    auto additive = target;
    Header blended = h;
    blended.flags = 4;
    std::memcpy(additive.data() + 48, &blended, 16);
    before = additive;
    require(Apply(additive, 2, pose) == 3 && additive == before);
    require(Capture(additive, 2, pose) == 3);
    auto broken = bytes;
    first[5] = std::numeric_limits<float>::infinity();
    std::memcpy(broken.data() + 64, first.data(), 48);
    Pose kept = pose;
    require(Capture(broken, 2, pose) == 6 && pose.count == kept.count);
    // The fixed target's root: bone 0 translation and rotation replaced, its scale and every other byte kept.
    using AnimationInjector::PoseTrackCopy::WriteRoot;
    const float grip[3]{7, 8, 9}, turn[4]{0, 0, 0.6f, 0.8f};
    auto rooted = bytes;
    require(WriteRoot(rooted, 2, grip, turn) == 0);
    const auto root = Read(rooted, 2, 0);
    require(root.error == 0 && root.qs[0] == 7 && root.qs[1] == 8 && root.qs[2] == 9 && root.qs[6] == 0.6f &&
            root.qs[7] == 0.8f);
    require(std::memcmp(rooted.data() + 64 + 12, bytes.data() + 64 + 12, 4) == 0);   // translation padding lane
    require(std::memcmp(rooted.data() + 64 + 32, bytes.data() + 64 + 32, 16) == 0);  // scale
    require(std::memcmp(rooted.data(), bytes.data(), 64) == 0 &&
            std::memcmp(rooted.data() + 112, bytes.data() + 112, 48) == 0);
    const float bad[3]{0, std::numeric_limits<float>::quiet_NaN(), 0};
    before = rooted;
    require(WriteRoot(rooted, 2, bad, turn) == 6 && rooted == before);
    require(WriteRoot(additive, 2, grip, turn) == 3);
    // More bones than the lock holds.
    std::vector<std::byte> large(16 + 48 + 257 * 48);
    Master lm{static_cast<std::int32_t>(large.size()), 3, {}};
    std::memcpy(large.data(), &lm, 16);
    Header lh{257, 257, 64, 48, 1, 0, 1, 0};
    std::memcpy(large.data() + 48, &lh, 16);
    for (int b = 0; b < 257; ++b) std::memcpy(large.data() + 64 + b * 48, q.data(), 48);
    require(Capture(large, 2, pose) == 8);
    std::cout << "Pose track bounds and format checks passed\n";
}

#include "PCH.h"
#include "core/CarryPoseLock.h"
#include "core/PoseTrackCopy.h"
#include "core/SeqLock.h"
#include <array>
#include <atomic>
#include <cmath>

namespace AnimationInjector::CarryPoseLock {
namespace {
constexpr int poseTrack = 2;  // hkbGeneratorOutput::TRACK_POSE
// Identities only: compared with the generate call's arguments, never dereferenced.
std::atomic<std::uintptr_t> character{0}, liftClip{0};
std::atomic<float> fixAt{-1.0f}, fixedAt{-1.0f};
// Two buffers: the hook fills the one not published, then publishes it. Once one is locked the hook
// only reads it (it stops capturing), so a locked buffer is never written again until Release.
std::array<PoseTrackCopy::Pose, 2> buffers;
std::atomic<int> published{-1}, locked{-1};
std::atomic<bool> pending{false};  // the lift ended before any capture: lock the next one
// The followed root: translation xyz, rotation xyzw. The main thread stores it once a frame; the hook
// never sees a half-written one (two alternating buffers could be overwritten while a hook read one).
SeqLock<7> followRoot;
std::atomic<std::uint32_t> writes{0}, rejected{0};

struct ClipGenerate {
    static void thunk(RE::hkbClipGenerator* clip, const RE::hkbContext& context,
        const PoseTrackReader::Output** children, PoseTrackReader::Output& output, float offset) {
        original(clip, context, children, output, offset);
        const auto id = character.load(std::memory_order_acquire);
        if (!id || reinterpret_cast<std::uintptr_t>(context.character) != id) return;
        if (const int lock = locked.load(std::memory_order_acquire); lock >= 0) {
            int result = PoseTrackCopy::Apply(output, poseTrack, buffers[lock]);
            if (std::array<float, 7> r; result == 0 && followRoot.Load(r))
                result = PoseTrackCopy::WriteRoot(output, poseTrack, std::span<const float, 3>(r.data(), 3),
                    std::span<const float, 4>(r.data() + 3, 4));
            ++(result == 0 ? writes : rejected);
            return;
        }
        if (reinterpret_cast<std::uintptr_t>(clip) != liftClip.load(std::memory_order_acquire)) return;
        const int next = published.load(std::memory_order_relaxed) == 0 ? 1 : 0;
        if (PoseTrackCopy::Capture(output, poseTrack, buffers[next]) != 0) return;
        published.store(next, std::memory_order_release);
        const bool ending = pending.exchange(false, std::memory_order_acq_rel);
        const float at = fixAt.load(std::memory_order_relaxed);
        const bool fixing = !ending && at >= 0 && clip->localTime >= at;
        if (fixing) fixedAt.store(clip->localTime, std::memory_order_relaxed);
        if (ending || fixing) locked.store(next, std::memory_order_release);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void Install() {
    REL::Relocation<std::uintptr_t> clip{RE::VTABLE_hkbClipGenerator[0]};
    ClipGenerate::original = clip.write_vfunc(0x17, ClipGenerate::thunk);
    logger::info("[CarryPoseLock] installed at hkbClipGenerator::Generate exit");
}
void Watch(std::uintptr_t target, std::uintptr_t clip, float at) {
    if (!target || !clip) return;
    fixAt.store(std::isfinite(at) ? at : -1.0f, std::memory_order_relaxed);
    character.store(target, std::memory_order_release);
    liftClip.store(clip, std::memory_order_release);
}
void CaptureNow() {
    if (!character.load(std::memory_order_acquire) || locked.load(std::memory_order_acquire) >= 0) return;
    if (const int last = published.load(std::memory_order_acquire); last >= 0)
        locked.store(last, std::memory_order_release);
    else
        pending.store(true, std::memory_order_release);
}
bool Captured() { return locked.load(std::memory_order_acquire) >= 0; }
float FixedAt() { return Captured() ? fixedAt.load(std::memory_order_relaxed) : -1.0f; }
void Follow(const RootLocal& value) {
    if (!Captured()) return;
    const auto& t = value.translation;
    const auto& q = value.rotation;
    followRoot.Store({t[0], t[1], t[2], q[0], q[1], q[2], q[3]});
}
void Rebind(std::uintptr_t target) {
    if (target && locked.load(std::memory_order_acquire) >= 0) character.store(target, std::memory_order_release);
}
void Release(const char* reason) {
    const bool was = locked.load(std::memory_order_acquire) >= 0;
    const float fixed = fixedAt.load(std::memory_order_relaxed);
    const bool followed = followRoot.Published();
    character.store(0, std::memory_order_release);
    liftClip.store(0, std::memory_order_release);
    pending.store(false, std::memory_order_release);
    locked.store(-1, std::memory_order_release);
    published.store(-1, std::memory_order_release);
    followRoot.Clear();
    fixAt.store(-1.0f, std::memory_order_relaxed);
    fixedAt.store(-1.0f, std::memory_order_relaxed);
    const auto w = writes.exchange(0), r = rejected.exchange(0);
    if (was)
        logger::info("[CarryPoseLock] released reason={} writes={} rejected={} fixedAt={:.3f} followed={}", reason, w,
            r, fixed, followed);
}
}

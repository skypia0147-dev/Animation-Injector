#include "PCH.h"
#include "core/NameIndex.h"
#include "core/AnimDataRegistry.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionSpec.h"
#include <cstddef>
#include <mutex>
#include <set>

namespace AnimationInjector::AnimData {

namespace {
// ── 미러 배치 (CommonLib RE/A/AnimationClipDataSingleton.h 의 사적 멤버) ──
struct ClipTriggerData {
    RE::BSFixedString name;
    float time;
    std::uint32_t pad;
};
struct ClipData {
    float motionSpeed;
    std::uint16_t boundIdx;
    std::uint16_t numTriggers;
    ClipTriggerData triggers[1];
};  // 0x18
static_assert(sizeof(ClipData) == 0x18);
struct RawArray {
    void* data;
    std::uint32_t cap;
    std::uint32_t pad;
    std::uint32_t size;
    std::uint32_t pad2;
};  // BSTArray 0x18
struct AnimationData {
    volatile std::uint32_t refCount;
    std::uint32_t pad04;
    RE::BSTHashMap<RE::BSFixedString, ClipData*> clips;  // 08  키 = 클립 이름
    RawArray boundDatas;                                 // 38
    RawArray hkxFiles;                                   // 50
};
static_assert(offsetof(AnimationData, clips) == 0x08 && offsetof(AnimationData, boundDatas) == 0x38 &&
              sizeof(AnimationData) == 0x68);
struct Singleton {
    void* vtbl;
    std::uint64_t f8;
    RE::BSTHashMap<RE::BSFixedString, AnimationData*> animDatas;
    RawArray mbHkxFiles;
};
static_assert(offsetof(Singleton, animDatas) == 0x10 && sizeof(Singleton) == 0x58);
// bound_datas 원소 0x28: 00 transPtr(NiPoint3[N] 뒤에 half[N] 시각, bit0 = 비정렬 플래그) 08 N 10 rotPtr 18 M 20 duration
struct Container {
    std::uintptr_t transPtr;
    std::uint32_t transCount;
    std::uint32_t pad0C;
    std::uintptr_t rotPtr;
    std::uint32_t rotCount;
    std::uint32_t pad1C;
    float duration;
    std::uint32_t pad24;
};
static_assert(sizeof(Container) == 0x28);
constexpr std::size_t kStride = 0x28;
constexpr std::uint16_t kNoBound = 0xFFFF;

Singleton* g_singleton = nullptr;
bool g_registered = false;
std::unordered_set<const void*> g_doneProjects;
struct OurElem {
    AnimationData* ad;
    std::string clip;
    std::uint16_t idx;
};
std::vector<OurElem> g_ourElems;
// Clip name (ignoring case) -> its g_ourElems entries, one per project: SetClipDuration runs on every
// clip activation and used to walk every registered clip.
std::unordered_map<std::string, std::vector<std::size_t>, NameHash, NameEqual> g_elemsByClip;

float HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1F;
    std::uint32_t mant = h & 0x3FF;
    std::uint32_t x;
    if (exp == 0) {
        if (mant == 0)
            x = sign;
        else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            x = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31)
        x = sign | 0x7F800000 | (mant << 13);
    else
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
// Every page of [p, p + n) is committed and readable. VirtualQuery never touches the memory, unlike
// IsBadReadPtr, which reads it and can consume a guard page (a thread's stack growth trigger).
bool Readable(const void* p, std::size_t n) {
    if (!p) return false;
    auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto end = at + n;
    if (end < at) return false;
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!::VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & readable))
            return false;
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}

Singleton* FindSingleton() {
    if (g_singleton) return g_singleton;
    REL::Relocation<std::uintptr_t> vt{
        RE::VTABLE_AnimationClipDataSingleton[0]};  // BSTSingletonSDM 정적 버퍼 → .data 에서 vtable 포인터로 찾는다
    const std::uintptr_t want = vt.address();
    const auto seg = REL::Module::get().segment(REL::Segment::data);
    const std::uintptr_t begin = seg.address(), end = begin + seg.size();
    for (std::uintptr_t p = begin & ~std::uintptr_t(7); p + 8 <= end; p += 8)
        if (*reinterpret_cast<const std::uintptr_t*>(p) == want) {
            g_singleton = reinterpret_cast<Singleton*>(p);
            break;
        }
    if (!g_singleton)
        logger::error("[AnimData] AnimationClipDataSingleton 을 .data 에서 못 찾음 — 루트 모션 경로가 열리지 않는다");
    return g_singleton;
}

// 배치 검증: 기존 클립들이 0x28 배치로 그럴듯하게 읽혀야 한다 (모션 블록 없는 클립 = boundIdx 0xFFFF)
bool ValidateLayout(AnimationData* ad) {
    const auto& bd = ad->boundDatas;
    if (!bd.data || bd.size == 0) return false;
    int checked = 0, withMotion = 0;
    for (const auto& kv : ad->clips) {
        const ClipData* cd = kv.second;
        if (!Readable(cd, sizeof(ClipData))) return false;
        if (cd->boundIdx == kNoBound) continue;
        if (cd->boundIdx >= bd.size) return false;
        const auto* c =
            reinterpret_cast<const Container*>(static_cast<const std::uint8_t*>(bd.data) + cd->boundIdx * kStride);
        if (!Readable(c, 0x20) || c->transCount > 256 || c->rotCount > 256) return false;
        const std::uintptr_t tp = c->transPtr & ~std::uintptr_t(1);
        if (c->transCount > 1) {
            if (!Readable(reinterpret_cast<const void*>(tp), c->transCount * 14)) return false;
            const auto* times = reinterpret_cast<const std::uint16_t*>(tp + c->transCount * 12);
            float prev = -1.0f;
            for (std::uint32_t k = 0; k < c->transCount; ++k) {
                const float t = HalfToFloat(times[k]);
                if (t < prev || t > 60.0f) return false;
                prev = t;
            }
            ++withMotion;
        }
        if (++checked >= 400) break;
    }
    logger::info(
        "[AnimData]   배치 검증: 검사 {} 모션 있음 {} → {}", checked, withMotion, withMotion > 0 ? "통과" : "탈락");
    return withMotion > 0;
}

// A registered clip with no motion keys of its own: registration opens the engine's root-motion path for it, and
// the motion comes from the HKX's AMR annotations (or its vanilla motion data) through the hook chain.
bool RegisterClip(AnimationData* ad, const char* a_project, const ActionSpec&, const ClipVariant& clip) {
    RE::BSFixedString key{clip.clipName.c_str()};
    if (ad->clips.find(key) != ad->clips.end()) return true;
    auto& bd = ad->boundDatas;
    // ClipData addresses bound data with a 16-bit index.
    if (bd.size >= 0xFFFF) {
        logger::error("[AnimData] {}: bound data full (65535); '{}' not registered", a_project, clip.clipName);
        return false;
    }
    if (bd.size >= bd.cap) {  // 엔진과 같은 MemoryManager 로 확장, 옛 버퍼는 둔다
        // Grow by half: growing by 16 copied (and kept) the whole buffer every 16 clips, which with
        // thousands of clips added up to hundreds of MB.
        const std::uint32_t newCap = std::min<std::uint32_t>(0xFFFF, bd.cap + std::max<std::uint32_t>(16, bd.cap / 2));
        auto* nb = static_cast<std::uint8_t*>(RE::malloc(newCap * kStride));
        if (!nb) return false;
        std::memset(nb, 0, newCap * kStride);
        if (bd.data && bd.size) std::memcpy(nb, bd.data, bd.size * kStride);
        bd.data = nb;
        bd.cap = newCap;
    }
    auto* elem = reinterpret_cast<Container*>(static_cast<std::uint8_t*>(bd.data) + bd.size * kStride);
    std::memset(elem, 0, kStride);
    elem->transPtr = 0;
    elem->transCount = 0;
    elem->duration = 1.0f;  // 실제 길이는 활성 때 SetClipDuration
    const auto idx = static_cast<std::uint16_t>(bd.size);
    bd.size++;
    auto* cd = static_cast<ClipData*>(RE::malloc(sizeof(ClipData)));
    if (!cd) return false;
    std::memset(cd, 0, sizeof(ClipData));
    cd->motionSpeed = 1.0f;
    cd->boundIdx = idx;
    cd->numTriggers = 0;
    ad->clips.insert(RE::BSTTuple<RE::BSFixedString, ClipData*>{key, cd});
    g_elemsByClip[clip.clipName].push_back(g_ourElems.size());
    g_ourElems.push_back({ad, clip.clipName, idx});
    logger::debug("[AnimData] {}: '{}' 등록 (boundIdx {})", a_project, clip.clipName, idx);
    return true;
}
}

bool Registered() { return g_registered; }

void SetClipDuration(const char* a_clipName, float a_seconds) {
    if (!a_clipName || a_seconds <= 0.05f) return;
    const auto found = g_elemsByClip.find(std::string_view(a_clipName));
    if (found == g_elemsByClip.end()) return;
    for (const auto index : found->second) {
        const auto& e = g_ourElems[index];
        auto* elem = reinterpret_cast<Container*>(static_cast<std::uint8_t*>(e.ad->boundDatas.data) + e.idx * kStride);
        if (elem->duration != a_seconds) elem->duration = a_seconds;
    }
}

namespace {
// The skeleton an animation-data project belongs to: DefaultMale / DefaultFemale are the character's (first person
// is not), a creature's is named as its behavior project file (WolfProject = wolfproject.hkx).
PairedRig::Kind ProjectRig(const char* name) {
    if (!name) return PairedRig::Kind::kUnsupported;
    if (_strnicmp(name, "Default", 7) == 0) return PairedRig::Kind::kCharacter;
    const std::string_view key(name);
    for (const auto& profile : PairedRig::profiles)
        for (const std::string_view file : {std::string_view(profile.project), std::string_view(profile.projectAlias)})
            if (file.ends_with(".hkx") && PairedRig::Equal(file.substr(0, file.size() - 4), key)) return profile.kind;
    return PairedRig::Kind::kUnsupported;
}
// The clips a project's animation data gets: every character action's in the character's projects (pairs included,
// as before, and a creature-started pair whose partner is the character), a creature's own actions and the pairs it
// starts in that creature's (and a creature sharing its folder). A creature partner's project never got its pairs.
bool PlaysIn(const ActionSpec& spec, PairedRig::Kind project) {
    if (project == PairedRig::Kind::kCharacter)
        return spec.rig == PairedRig::Kind::kCharacter ||
               (spec.paired.enabled && spec.paired.targetRig == PairedRig::Kind::kCharacter);
    return PairedRig::IsCreature(project) && PairedRig::SharesAssets(spec.rig, project);
}
}

namespace {
// ── Ladder clip copies (QueueCopies) ─────────────────────────────────────────
std::mutex g_copyMutex;
std::vector<ClipCopy> g_copies;
std::unordered_set<std::string> g_copyNames;  // queued copy names (ignoring case: lowered)
std::set<std::pair<const AnimationData*, std::string>> g_copyDone;
std::string LowerName(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
// One copy in one project. The ClipData is new (annotations are per copy); the motion block is shared.
void ApplyCopy(AnimationData* ad, const char* project, const ClipCopy& c) {
    const auto key = std::make_pair(static_cast<const AnimationData*>(ad), LowerName(c.copy));
    if (!g_copyDone.insert(key).second) return;
    RE::BSFixedString copyKey{c.copy.c_str()};
    if (ad->clips.find(copyKey) != ad->clips.end()) return;
    const auto found = ad->clips.find(RE::BSFixedString{c.source.c_str()});
    if (found == ad->clips.end() || !Readable(found->second, sizeof(ClipData))) {
        logger::warn(
            "[AnimData] {}: '{}' has no animation data; its ladder copy '{}' gets none", project, c.source, c.copy);
        return;
    }
    const ClipData* src = found->second;
    const std::size_t n = src->numTriggers;
    if (n && !Readable(src, offsetof(ClipData, triggers) + n * sizeof(ClipTriggerData))) return;
    std::vector<std::pair<RE::BSFixedString, float>> kept;
    if (c.annotations)
        for (std::size_t i = 0; i < n; ++i) kept.emplace_back(src->triggers[i].name, src->triggers[i].time);
    const std::size_t bytes =
        offsetof(ClipData, triggers) + std::max<std::size_t>(1, kept.size()) * sizeof(ClipTriggerData);
    auto* cd = static_cast<ClipData*>(RE::malloc(bytes));
    if (!cd) return;
    std::memset(cd, 0, bytes);
    cd->motionSpeed = src->motionSpeed * c.rate;
    cd->boundIdx = src->boundIdx;
    cd->numTriggers = static_cast<std::uint16_t>(kept.size());
    for (std::size_t i = 0; i < kept.size(); ++i) {
        new (&cd->triggers[i].name) RE::BSFixedString(kept[i].first);
        cd->triggers[i].time = kept[i].second;
    }
    ad->clips.insert(RE::BSTTuple<RE::BSFixedString, ClipData*>{copyKey, cd});
    logger::debug("[AnimData] {}: ladder copy '{}' of '{}' (speed {:.3f}, annotations {}/{})", project, c.copy,
        c.source, cd->motionSpeed, kept.size(), n);
}
// Every queued copy in every loaded character project. Caller holds g_copyMutex.
void ApplyCopiesLocked(const char* a_reason) {
    if (g_copies.empty()) return;
    auto* s = FindSingleton();
    if (!s) return;
    for (const auto& kv : s->animDatas) {
        const char* name = kv.first.c_str();
        if (ProjectRig(name) != PairedRig::Kind::kCharacter || !kv.second) continue;
        const auto before = kv.second->clips.size();
        for (const auto& c : g_copies) ApplyCopy(kv.second, name, c);
        if (kv.second->clips.size() != before)
            logger::info("[AnimData] ({}) {}: ladder copies registered {} (clips={})", a_reason, name,
                kv.second->clips.size() - before, kv.second->clips.size());
    }
}
}  // namespace

float ClipSpeed(const char* a_clip) {
    auto* s = FindSingleton();
    if (!s || !a_clip) return 0.0f;
    const RE::BSFixedString key{a_clip};
    for (const auto& kv : s->animDatas) {
        if (ProjectRig(kv.first.c_str()) != PairedRig::Kind::kCharacter || !kv.second) continue;
        auto* ad = kv.second;
        const auto found = ad->clips.find(key);
        if (found == ad->clips.end() || !Readable(found->second, sizeof(ClipData))) continue;
        const auto idx = found->second->boundIdx;
        if (idx == kNoBound || idx >= ad->boundDatas.size || !ad->boundDatas.data) continue;
        const auto* c =
            reinterpret_cast<const Container*>(reinterpret_cast<std::uintptr_t>(ad->boundDatas.data) + idx * kStride);
        if (!Readable(c, sizeof(Container)) || c->transCount < 1 || c->transCount > 4096 || c->duration <= 0.0f)
            continue;
        const auto* keys = reinterpret_cast<const RE::NiPoint3*>(c->transPtr & ~std::uintptr_t(1));
        if (!Readable(keys, c->transCount * sizeof(RE::NiPoint3))) continue;
        const auto& last = keys[c->transCount - 1];
        return std::sqrt(last.x * last.x + last.y * last.y) / c->duration;
    }
    return 0.0f;
}

void QueueCopies(std::vector<ClipCopy> a_copies) {
    std::lock_guard lock(g_copyMutex);
    for (auto& c : a_copies)
        if (g_copyNames.insert(LowerName(c.copy)).second) g_copies.push_back(std::move(c));
    ApplyCopiesLocked("QueueCopies");
}

bool TryRegister(const char* a_reason) {
    {
        // Ladder copies go into each character project as it loads, with or without actions.
        std::lock_guard lock(g_copyMutex);
        ApplyCopiesLocked(a_reason);
    }
    if (g_registered || Actions::Count() == 0) return g_registered;
    auto* s = FindSingleton();
    if (!s) return false;
    if (s->animDatas.size() == 0) {
        logger::info("[AnimData] ({}) 아직 프로젝트가 없음 — 나중에 다시", a_reason);
        return false;
    }
    int targets = 0, done = 0;
    for (const auto& kv : s->animDatas) {
        const char* name = kv.first.c_str();
        const auto rig = ProjectRig(name);
        if (rig == PairedRig::Kind::kUnsupported) continue;
        if (rig != PairedRig::Kind::kCharacter) {
            bool any = false;
            for (int a = 0; a < Actions::Count() && !any; ++a) any = PlaysIn(Actions::Spec(a), rig);
            if (!any) continue;  // a creature without actions of its own
        }
        ++targets;
        AnimationData* ad = kv.second;
        if (!ad) continue;
        if (g_doneProjects.count(ad)) {
            ++done;
            continue;
        }
        logger::info("[AnimData] ({}) 프로젝트 '{}': clips={} bound_datas={}/{}", a_reason, name, ad->clips.size(),
            ad->boundDatas.size, ad->boundDatas.cap);
        if (!ValidateLayout(ad)) {
            logger::warn("[AnimData] 배치 검증 실패 — 등록 안 함");
            // A creature project that does not read as expected loses its own actions' root motion only; the
            // character's projects (and other creatures) still register.
            // It is not tried again (once logged): it would otherwise run on every request.
            if (rig != PairedRig::Kind::kCharacter) {
                g_doneProjects.insert(ad);
                ++done;
                continue;
            }
            return false;
        }
        bool ok = true;
        int count = 0;
        for (int a = 0; a < Actions::Count() && ok; ++a) {
            const auto& spec = Actions::Spec(a);
            if (!PlaysIn(spec, rig)) continue;
            for (const auto& c : spec.clips) {
                ok = ok && RegisterClip(ad, name, spec, c);
                ++count;
            }
        }
        if (ok) {
            g_doneProjects.insert(ad);
            ++done;
            logger::info("[AnimData] {}: 클립 {}개 등록 완료 (clips={} bound_datas={}/{})", name, count,
                ad->clips.size(), ad->boundDatas.size, ad->boundDatas.cap);
        }
    }
    g_registered = targets > 0 && done == targets;
    return g_registered;
}

}  // namespace AnimationInjector::AnimData

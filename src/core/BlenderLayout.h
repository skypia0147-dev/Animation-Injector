#pragma once
#include "PCH.h"
#include "core/NodeFactory.h"

// hkbBlenderGenerator / hkbBlenderGeneratorChild (Havok 2010.2, 64-bit). CommonLib has only their RTTI and vtable ids.
// The field offsets follow the serialized member order (hkxcmd XML) and Havok's packing. Confirmed in game 11-14
// (SE 1.6.1170, a large mod list, LadderProbe): 185 direction blenders read at CreateSymbolIdMap gave flags 0x11, sync master
// -1, the Atlas weights to five digits (1HM_ForwardBlend 5.84995 / 58.4995 / 350.988 / 3509.88), child and clip vtables
// as expected, and empty per-instance arrays on every template. LocomotionSplit still self-checks each blender (child
// count, ascending weights, every child an hkbClipGenerator by vtable, PARAMETRIC_BLEND) and leaves one alone when a
// check fails.
namespace AnimationInjector::Havok {

struct BlenderGeneratorChildLayout {  // hkbBlenderGeneratorChild : hkbBindable, 0x48
    std::uint8_t bindable[0x30];  // 00  hkReferencedObject + variableBindingSet + cachedBindables + areBindablesCached
    RE::hkbGenerator* generator;  // 30
    RE::hkReferencedObject* boneWeights;  // 38  hkbBoneWeightArray*, may be null
    float weight;                         // 40  the axis value this slot stands for ("declared speed")
    float worldFromModelWeight;           // 44
};
static_assert(sizeof(BlenderGeneratorChildLayout) == 0x48);

struct BlenderGeneratorLayout {  // hkbBlenderGenerator : hkbGenerator, 0xA0
    enum Flags : std::int16_t {
        kSync = 0x1,
        kSmoothGeneratorWeights = 0x4,
        kDontDeactivateChildrenWithZeroWeights = 0x8,
        kParametricBlend = 0x10,
        kIsSampled = 0x20,
        kForceDensePose = 0x40,
    };
    std::uint8_t generator[0x48];                        // 00  hkbGenerator
    float referencePoseWeightThreshold;                  // 48
    float blendParameter;                                // 4C  bound to SpeedSampled
    float minCyclicBlendParameter;                       // 50
    float maxCyclicBlendParameter;                       // 54
    std::int16_t indexOfSyncMasterChild;                 // 58
    std::int16_t flags;                                  // 5A
    bool subtractLastChild;                              // 5C
    std::uint8_t pad5D[3];                               // 5D
    RE::hkArray<BlenderGeneratorChildLayout*> children;  // 60
    RE::hkArray<std::uint8_t> childrenInternalStates;    // 70  per instance; expected empty on the template
    RE::hkArray<std::int16_t> sortedChildren;            // 80  per instance; expected empty on the template
    float endIntervalWeight;                             // 90
    std::int32_t numActiveChildren;                      // 94
    std::int16_t beginIntervalIndex;                     // 98
    std::int16_t endIntervalIndex;                       // 9A
    bool initSync;                                       // 9C
    bool doSubtractiveBlend;                             // 9D
    std::uint8_t pad9E[2];                               // 9E
};
static_assert(sizeof(BlenderGeneratorLayout) == 0xA0);

// Every page of [p, p + n) committed and readable (VirtualQuery, never touching the memory): a layout this file reads
// at offsets CommonLib does not declare is checked before it is followed.
inline bool Readable(const void* p, std::size_t n) {
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
// A node pointer read from a layout: readable, an hkbNode with this name. Empty name: any.
inline bool NodeNamed(const void* node, const char* name) {
    if (!Readable(node, sizeof(RE::hkbNode))) return false;
    const char* actual = NodeFactory::NodeName(node);
    return Readable(actual, 1) && (!name || _stricmp(actual, name) == 0);
}

// BSiStateTaggingGenerator (Bethesda; the vanilla files serialize pDefaultGenerator, iStateToSetAs, iPriority).
struct StateTaggingLayout {
    std::uint8_t generator[0x48];         // 00  hkbGenerator
    std::uint64_t unk48;                  // 48  Bethesda generators start their members at 0x50, as CommonLib's
                                          //     BSSynchronizedClipGenerator and BSCyclicBlendTransitionGenerator do
                                          //     (11-14: reading iState at 0x50 gave a pointer's low half)
    RE::hkbGenerator* pDefaultGenerator;  // 50
    std::int32_t iStateToSetAs;           // 58
    std::int32_t iPriority;               // 5C
};
// hkbManualSelectorGenerator (Havok 2010.2): generators, selectedGeneratorIndex, currentGeneratorIndex.
struct ManualSelectorLayout {
    std::uint8_t generator[0x48];               // 00  hkbGenerator
    RE::hkArray<RE::hkbGenerator*> generators;  // 48
    std::int8_t selectedGeneratorIndex;         // 58
    std::int8_t currentGeneratorIndex;          // 59
};

}  // namespace AnimationInjector::Havok

#include "PCH.h"
#include "core/CallSite.h"
#include "core/CallSiteShape.h"

namespace AnimationInjector::CallSite {

Site Verify(std::uintptr_t a_function, std::uintptr_t a_site, const char* a_label) {
    const auto offset = a_site - a_function;
    const auto shape = Classify(a_function, a_site);
    if (shape == Shape::kNotCall) {
        logger::error(
            "[Hook] {} +0x{:X}: no call instruction starts here (different game build?); not hooked", a_label, offset);
        return {};
    }
    if (shape == Shape::kUndecodable) {
        // The decoder could not walk the function, which says nothing about the site. Keep the old check.
        if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
            logger::error("[Hook] {} +0x{:X}: not a call; not hooked", a_label, offset);
            return {};
        }
        logger::warn(
            "[Hook] {} +0x{:X}: function could not be decoded; site checked by its first byte only", a_label, offset);
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
    Site site;
    site.target = a_site + 5 + static_cast<std::intptr_t>(rel);
    const auto& text = REL::Module::get().segment(REL::Segment::textx);
    site.foreign = site.target < text.address() || site.target >= text.address() + text.size();
    if (!site.foreign) {
        // An engine call goes to the first byte of a function. Leaf functions have no unwind entry.
        DWORD64 imageBase = 0;
        const auto* entry = ::RtlLookupFunctionEntry(site.target, &imageBase, nullptr);
        if (entry && imageBase + entry->BeginAddress != site.target) {
            logger::error(
                "[Hook] {} +0x{:X}: call lands inside a function (different game build?); not hooked", a_label, offset);
            return {};
        }
    }
    site.ok = true;
    return site;
}

}  // namespace AnimationInjector::CallSite

#include "PCH.h"
#include "core/AnimationLimit.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace AnimationInjector::AnimationLimit {
namespace {
bool EngineReadsUnsigned() {
    // hkbClipGenerator::activate, the function OAR patches (same ids OAR uses).
    const REL::Relocation<std::uintptr_t> activate{REL::VariantID(58602, 59252, 0xA466D0)};
    const auto* code = reinterpret_cast<const std::uint8_t*>(activate.address());
    if (!code) return false;
    const std::uint8_t first[2]{code[0x160], code[0x161]};
    const std::uint8_t second[2]{code[0x176], code[0x177]};
    return ReadsUnsigned(first, second);
}

// OAR's [General] uAnimationLimit, which OAR reads from the same file at its own load. OAR's default
// when the file or key is missing.
int OarLimit() {
    if (!REX::W32::GetModuleHandleW(L"OpenAnimationReplacer.dll")) return -1;
    std::ifstream file(
        std::filesystem::current_path() / "Data/SKSE/Plugins/OpenAnimationReplacer.ini", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const int limit = OarIniLimit(text);
    return limit > 0 ? limit : kSignedMaxIndex;
}

Limits Resolve() {
    const Limits l = Compute(EngineReadsUnsigned(), OarLimit());
    logger::info("[AnimLimit] engine reads binding indices {} (max index {}), OAR limit {}; a project holds {} "
                 "bindings, names stop at {}",
        l.unsignedIndex ? "unsigned" : "signed", l.engineMaxIndex,
        l.oarLimit > 0 ? std::to_string(l.oarLimit) : std::string("none"), l.bindingCap, l.nameTableLimit);
    return l;
}
}

const Limits& Get() {
    static const Limits limits = Resolve();
    return limits;
}

}

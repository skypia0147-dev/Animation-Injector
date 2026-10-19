#include "PCH.h"
#include "core/PairedClipBinding.h"
#include "core/NodeFactory.h"
#include "core/SynchronizedSceneView.h"
#include "runtime/ActionRegistry.h"
#include "compat/OpenAnimationReplacerAPI-Animations.h"

namespace AnimationInjector::PairedClipBinding {
namespace {
std::recursive_mutex bindingMutex;

bool IsReplacement(RE::hkbClipGenerator* clip) {
    static const bool oarPresent = GetModuleHandleW(L"OpenAnimationReplacer.dll") != nullptr;
    static auto* api = []() -> OAR_API::Animations::IAnimationsInterface* {
        auto module = GetModuleHandleW(L"OpenAnimationReplacer.dll");
        if (!module) return nullptr;
        auto request = reinterpret_cast<OAR_API::Animations::_RequestPluginAPI_Animations>(
            GetProcAddress(module, "RequestPluginAPI_Animations"));
        if (!request) return nullptr;
        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        return request(OAR_API::Animations::InterfaceVersion::V1, plugin->GetName().data(), plugin->GetVersion());
    }();
    // Without OAR's ownership query, leave its selection untouched.
    if (!api) return oarPresent;
    const auto info = api->GetCurrentReplacementAnimationInfo(clip);
    return info.animationPath.c_str() && *info.animationPath.c_str();
}

struct Resolution {
    RE::hkbClipGenerator* clip;
    RE::hkbCharacterStringData* strings;
    std::uint16_t index;
};
std::optional<Resolution> Resolve(RE::BSSynchronizedClipGenerator* node, RE::hkbCharacter* character) {
    if (!NodeFactory::SameVTable(node, RE::VTABLE_BSSynchronizedClipGenerator) || !character) return {};
    auto* clip = node->clipGenerator;
    if (!NodeFactory::SameVTable(clip, RE::VTABLE_hkbClipGenerator)) return {};
    int action = -1, variant = -1;
    if (!Actions::FindClip(clip->name.c_str(), action, variant) || !Actions::Spec(action).paired.enabled) return {};
    auto* setup = character->setup.get();
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    auto* bindings = static_cast<RE::hkbAnimationBindingSet*>(character->animationBindingSet.get());
    if (!bindings && setup) bindings = setup->animationBindingSet.get();
    if (!strings || !bindings || !clip->animationName.c_str()) return {};
    // Resolve existing names only. Binding allocation and file loading remain
    // engine-owned; never append to a linked character's file table here.
    const int expected = NodeFactory::FindAnimationName(strings->animationNames, clip->animationName.c_str());
    const bool slot = expected >= 0 && expected < bindings->bindings._size && bindings->bindings._data[expected];
    if (clip->animationBindingIndex == expected) return {};
    const auto result = PairedClipIndex::Resolve(
        expected, bindings->bindings._size, slot, true, clip->animationControl.get() != nullptr, IsReplacement(clip));
    if (!result) return {};
    return Resolution{clip, strings, *result};
}

void PrepareClone(RE::BSSynchronizedClipGenerator* node, RE::hkbCharacter* character) {
    if (!node || node->cloneState != RE::hkbNode::CloneState::kClone || !node->clipGenerator ||
        node->clipGenerator->cloneState != RE::hkbNode::CloneState::kClone)
        return;
    if (const auto resolved = Resolve(node, character)) {
        logger::debug("[PairClipBinding] activation project='{}' node='{}' index={}->{} file='{}'",
            resolved->strings->name.c_str(), node->name.c_str(), resolved->clip->animationBindingIndex, resolved->index,
            resolved->clip->animationName.c_str());
        resolved->clip->animationBindingIndex = resolved->index;
    }
}
struct ActivateHook {
    static void Thunk(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context) {
        {
            std::lock_guard guard(bindingMutex);
            // OAR inspects both participants during its first PreActivate.
            // Prepare all available runtime clones before forwarding to it.
            auto* scene = node ? node->synchronizedScene : nullptr;
            if (NodeFactory::SameVTable(scene, RE::VTABLE_BGSSynchronizedAnimationInstance)) {
                const auto* view = reinterpret_cast<const SynchronizedSceneView*>(scene);
                if (view->actors.size() <= 8)
                    for (const auto& actor : view->actors) PrepareClone(actor.generator, actor.character);
            }
            PrepareClone(node, context.character);
        }
        original(node, context);
    }
    static inline REL::Relocation<decltype(Thunk)> original;
};
}

MarkScope::MarkScope(RE::BSSynchronizedClipGenerator* node, const RE::hkbContext& context) : lock(bindingMutex) {
    if (const auto resolved = Resolve(node, context.character)) {
        logger::debug("[PairClipBinding] mark project='{}' node='{}' index={}->{} file='{}' restoreAfterQuery=true",
            resolved->strings->name.c_str(), node->name.c_str(), resolved->clip->animationBindingIndex, resolved->index,
            resolved->clip->animationName.c_str());
        index.Set(resolved->clip->animationBindingIndex, resolved->index);
    }
}

void Install() {
    static bool installed = false;
    if (installed) return;
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_BSSynchronizedClipGenerator[0]};
    ActivateHook::original = table.write_vfunc(0x4, ActivateHook::Thunk);
    installed = true;
    logger::info("[PairClipBinding] project-local base binding hook installed");
}
}

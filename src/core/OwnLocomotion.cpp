#include "core/OwnLocomotion.h"
#include "core/AnimationLimit.h"
#include "core/BlenderLayout.h"
#include "core/HavokLayout.h"
#include "core/LocomotionDrive.h"
#include "core/NodeFactory.h"
#include "core/VanillaLadders.h"
#include "runtime/ActionRegistry.h"

#include <algorithm>
#include <unordered_map>

namespace AnimationInjector::OwnLocomotion {
namespace {
using Havok::BlenderGeneratorChildLayout;
using Havok::BlenderGeneratorLayout;
using Havok::NodeNamed;
using Havok::StateTaggingLayout;

// BSCyclicBlendTransitionGenerator (CommonLib: state @A8, so 0xB0). The vanilla files serialize pBlenderGenerator
// (0x50), EventToFreezeBlendValue and EventToCrossBlend (id + payload, 0x58..0x77), fBlendParameter, fTransitionDuration
// and eBlendCurve (to 0x84); from 0x88 on it is per-instance state (transition blender, transition effect, mode).
constexpr std::size_t kCyclicSize = 0xB0;
constexpr std::size_t kCyclicSerializedEnd = 0x88;
constexpr const char* kCyclicSuffix = "BSCyclicBlendTransitionGenerator";

// The two prefixes of a weapon kind, with their underscore: "2HM_" -> "2HW_", "1HM_" -> "H2H_".
struct Prefixes {
    std::string from, to;
    explicit Prefixes(TreeWeapon weapon)
        : from(std::string(TreeKindOf(weapon).from) + "_"), to(std::string(TreeKindOf(weapon).xml) + "_") {}
    // The kinds' names, for log lines.
    std::string From() const { return from.substr(0, from.size() - 1); }
    std::string To() const { return to.substr(0, to.size() - 1); }
    // "2HM_RunForward" -> "2HW_RunForward"; empty when the name has no source prefix.
    std::string Renamed(const char* name) const {
        if (!name || _strnicmp(name, from.c_str(), from.size()) != 0) return {};
        return to + (name + from.size());
    }
    // "Animations\2HM_RunForward.HKX" -> "Animations\2HW_RunForward.HKX" (the file name's prefix); empty without one.
    std::string RenamedFile(const char* path) const {
        std::string p = path ? path : "";
        const auto slash = p.find_last_of("\\/");
        const std::size_t at = slash == std::string::npos ? 0 : slash + 1;
        if (p.size() < at + from.size() || _strnicmp(p.c_str() + at, from.c_str(), from.size()) != 0) return {};
        p.replace(at, from.size(), to);
        return p;
    }
};

bool EndsWith(const char* text, const char* suffix) {
    const std::size_t n = text ? std::strlen(text) : 0, m = std::strlen(suffix);
    return n >= m && _stricmp(text + n - m, suffix) == 0;
}
bool Contains(const char* text, const std::string& part) {
    if (!text) return false;
    std::string lower(text), find(part);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : find) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(find) != std::string::npos;
}

// Through the game's file system (loose files, archives, MO2's virtual folder). Clip animation names are relative to
// the character project's folder.
bool FileExists(const std::string& animation) {
    RE::BSResourceNiBinaryStream stream("meshes\\actors\\character\\" + animation);
    return stream.good();
}

void CopyNodeBase(RE::hkbNode* node, const char* name) {
    std::memset(&node->name, 0, sizeof(node->name));
    NodeFactory::MakeString(node->name, name);
    NodeFactory::EmptyArray(node->cachedBindables);
    node->areBindablesCached = false;
    if (auto* set = node->variableBindingSet.get()) NodeFactory::AddRef(set);  // shared with the source node
}

BlenderGeneratorChildLayout* CopyChild(const BlenderGeneratorChildLayout* like, RE::hkbGenerator* generator) {
    auto* c = static_cast<BlenderGeneratorChildLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorChildLayout)));
    std::memcpy(c, like, sizeof(BlenderGeneratorChildLayout));
    NodeFactory::InitRefObj(c, RE::VTABLE_hkbBlenderGeneratorChild);
    auto* bindable = reinterpret_cast<RE::hkbBindable*>(c);
    NodeFactory::EmptyArray(bindable->cachedBindables);
    bindable->areBindablesCached = false;
    if (auto* set = bindable->variableBindingSet.get()) NodeFactory::AddRef(set);
    if (c->boneWeights) NodeFactory::AddRef(c->boneWeights);
    c->generator = generator;
    NodeFactory::AddRef(generator);
    return c;
}

BlenderGeneratorLayout* CopyBlender(
    const BlenderGeneratorLayout* src, const std::string& name, const std::vector<BlenderGeneratorChildLayout*>& kids) {
    auto* b = static_cast<BlenderGeneratorLayout*>(NodeFactory::AllocZero(sizeof(BlenderGeneratorLayout)));
    std::memcpy(b, src, sizeof(BlenderGeneratorLayout));
    NodeFactory::InitRefObj(b, RE::VTABLE_hkbBlenderGenerator);
    CopyNodeBase(reinterpret_cast<RE::hkbNode*>(b), name.c_str());
    auto* data = static_cast<BlenderGeneratorChildLayout**>(
        NodeFactory::AllocZero(kids.size() * sizeof(BlenderGeneratorChildLayout*)));
    std::copy(kids.begin(), kids.end(), data);
    NodeFactory::OwnArray(b->children, data, static_cast<std::int32_t>(kids.size()));
    NodeFactory::NoteCreatedNode();
    return b;
}

// The source tree, checked. Empty string: usable.
struct Source {
    RE::hkbNode* cyclic = nullptr;
    BlenderGeneratorLayout* direction = nullptr;
    std::vector<BlenderGeneratorLayout*> blenders;  // direction's children, in its order
    std::size_t clips = 0;
};
bool Templated(const BlenderGeneratorLayout* b) {
    return b->children._size >= 1 && b->children._size <= 32 && b->children._data && !b->childrenInternalStates._size &&
           !b->sortedChildren._size;
}
// `cyclic`: what the source kind's locomotion plays (its iState generator's generator, its block selector entry):
// "2HM_BSCyclicBlendTransitionGenerator", "H2H_1HM_BSCyclicBlendTransitionGenerator" (the one-handed tree's name in
// Melee_Locomotion_Behavior), "1HM_BSCyclicBlendTransitionGenerator" (BlockBehavior).
std::string Check(const Prefixes& p, RE::hkbNode* cyclic, Source& s) {
    s.cyclic = cyclic;
    const char* cyclicName = NodeNamed(s.cyclic, nullptr) ? NodeFactory::NodeName(s.cyclic) : nullptr;
    if (!cyclicName || !NodeFactory::SameVTable(s.cyclic, RE::VTABLE_BSCyclicBlendTransitionGenerator) ||
        !EndsWith(cyclicName, kCyclicSuffix) || !Contains(cyclicName, p.from))
        return fmt::format(
            "the {}locomotion is not a {}…{} ('{}')", p.from, p.from, kCyclicSuffix, cyclicName ? cyclicName : "?");
    auto* direction = reinterpret_cast<RE::BSCyclicBlendTransitionGenerator*>(s.cyclic)->generator;
    const char* directionName = NodeNamed(direction, nullptr) ? NodeFactory::NodeName(direction) : nullptr;
    if (!directionName || !NodeFactory::SameVTable(direction, RE::VTABLE_hkbBlenderGenerator) ||
        !Contains(directionName, p.from) || !EndsWith(directionName, "Direction_Blend"))
        return fmt::format("{} does not lead to the {}Direction_Blend blender ('{}')", cyclicName, p.from,
            directionName ? directionName : "?");
    s.direction = reinterpret_cast<BlenderGeneratorLayout*>(direction);
    if (!Templated(s.direction) || s.direction->children._size != 8)
        return fmt::format("{} has {} children or per-instance data", directionName, s.direction->children._size);
    for (int i = 0; i < 8; ++i) {
        const auto* child = s.direction->children._data[i];
        if (!child || !NodeFactory::SameVTable(child, RE::VTABLE_hkbBlenderGeneratorChild) ||
            !NodeFactory::SameVTable(child->generator, RE::VTABLE_hkbBlenderGenerator))
            return fmt::format("{} child {} is not a blender", directionName, i);
        auto* b = reinterpret_cast<BlenderGeneratorLayout*>(child->generator);
        const char* name = NodeFactory::NodeName(b);
        if (p.Renamed(name).empty() || !Templated(b))
            return fmt::format(
                "{} child {} ('{}') is not a {}direction ladder template", directionName, i, name, p.from);
        for (int k = 0; k < b->children._size; ++k) {
            const auto* c = b->children._data[k];
            if (!c || !NodeFactory::SameVTable(c, RE::VTABLE_hkbBlenderGeneratorChild) ||
                !NodeFactory::SameVTable(c->generator, RE::VTABLE_hkbClipGenerator) ||
                p.Renamed(NodeFactory::NodeName(c->generator)).empty())
                return fmt::format("{} slot {} is not a {}clip", name, k, p.from);
        }
        s.blenders.push_back(b);
        s.clips += static_cast<std::size_t>(b->children._size);
    }
    return {};
}
}  // namespace

const std::vector<std::string>& Files() {
    static const std::vector<std::string> files = [] {
        std::vector<std::string> out;
        static constexpr const char* kDirections[] = {
            "Forward", "ForwardRight", "Right", "BackwardRight", "Backward", "BackwardLeft", "Left", "ForwardLeft"};
        const auto same = [](const std::string& a, const std::string& b) {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        };
        for (std::size_t w = 0; w < kTreeKinds.size(); ++w) {
            const auto weapon = static_cast<TreeWeapon>(w);
            if (Actions::Locomotion().Use(weapon) != TreeUse::kOwn) continue;
            const Prefixes p(weapon);
            std::vector<std::string> found, missing;
            for (const char* direction : kDirections) {
                const auto* ladder =
                    VanillaLadders::Find("Melee_Locomotion_Behavior", p.from + direction + std::string("Blend"));
                if (!ladder) continue;
                for (const auto& slot : ladder->slots) {
                    const std::string file = p.RenamedFile(slot.animation);
                    if (file.empty()) continue;
                    const auto is = [&](const std::string& f) { return same(f, file); };
                    if (std::any_of(found.begin(), found.end(), is) || std::any_of(missing.begin(), missing.end(), is))
                        continue;
                    (FileExists(file) ? found : missing).push_back(file);
                }
            }
            logger::info("[Tree] {} use=\"own\": {} {} files found, {} not found (their slots play the {} file)",
                TreeKindOf(weapon).xml, found.size(), p.To(), missing.size(), p.From());
            out.insert(out.end(), found.begin(), found.end());
        }
        // <Stance> (11-23): each side's walk and run files that exist, in the direction order of LadderNames.
        for (std::size_t s = 0; s < 2; ++s) {
            const auto& stance = Actions::Locomotion().stances[s];
            if (!stance.declared) continue;
            std::vector<std::string> found, missing;
            for (const auto* prefix : {&stance.walk, &stance.run})
                for (std::size_t d = 0; d < LadderNames::kDirections.size(); ++d) {
                    const auto file = StanceFile(*prefix, static_cast<LadderDirection>(d));
                    if (file.empty()) continue;
                    const auto is = [&](const std::string& f) { return same(f, file); };
                    if (std::any_of(out.begin(), out.end(), is) || std::any_of(found.begin(), found.end(), is))
                        continue;
                    (FileExists(file) ? found : missing).push_back(file);
                }
            std::string list;
            for (const auto& f : missing) list += (list.empty() ? "" : ", ") + f;
            logger::info("[Stance] {}: {} files found, {} not found (those directions keep their own walk or run){}{}",
                kStanceSideNames[s], found.size(), missing.size(), missing.empty() ? "" : ": ", list);
            out.insert(out.end(), found.begin(), found.end());
        }
        return out;
    }();
    return files;
}

bool Listed(const std::string& file) {
    if (file.empty()) return false;
    for (const auto& f : Files())
        if (_stricmp(f.c_str(), file.c_str()) == 0) return true;
    return false;
}

namespace {
// The copy of a checked source tree (Check): clips, the eight direction ladders, the direction blend and the cyclic
// generator on top. `out` gets the direction blenders and the counts; `own` / `fallback` the files the clips play.
RE::hkbNode* CopyTree(const Prefixes& p, RE::hkbCharacter* character, const Source& source,
    std::vector<AnimData::ClipCopy>& copies, Built& out, std::vector<std::string>& own,
    std::vector<std::string>& fallback, std::size_t& clipCount) {
    // Clips. One copy per source clip node (a node two slots share stays shared); one file check per file.
    std::unordered_map<const RE::hkbGenerator*, RE::hkbClipGenerator*> clipCopies;
    std::unordered_map<std::string, std::string> fileChoice;  // source file (lowered) -> the file the copies play
    const auto copyClip = [&](RE::hkbClipGenerator* src) {
        if (const auto it = clipCopies.find(src); it != clipCopies.end()) return it->second;
        const std::string name = p.Renamed(NodeFactory::NodeName(src));
        const char* srcFile = src->animationName.c_str() ? src->animationName.c_str() : "";
        std::string key(srcFile);
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto choice = fileChoice.find(key);
        if (choice == fileChoice.end()) {
            const std::string candidate = p.RenamedFile(srcFile);
            const auto& registered = Files();
            const bool exists = !candidate.empty() &&
                                std::any_of(registered.begin(), registered.end(),
                                    [&](const std::string& f) { return _stricmp(f.c_str(), candidate.c_str()) == 0; });
            choice = fileChoice.emplace(key, exists ? candidate : std::string(srcFile)).first;
            (exists ? own : fallback).push_back(exists ? candidate : std::string(srcFile));
        }
        std::uint16_t binding = src->animationBindingIndex;
        std::string file = choice->second;
        if (_stricmp(file.c_str(), srcFile) != 0) {
            const int idx = NodeFactory::EnsureAnimationName(character, file.c_str());
            if (idx < 0 || idx > AnimationLimit::MaxBindingIndex()) {
                logger::warn("[Tree] {}: no animation index for '{}'; plays '{}'", name, file, srcFile);
                file = srcFile;
            } else
                binding = static_cast<std::uint16_t>(idx);
        }
        const bool ownFile = _stricmp(file.c_str(), srcFile) != 0;
        if (ownFile)
            ++out.ownFiles;
        else
            ++out.fallbackFiles;
        // Foot sync points. A vanilla source file has no annotations of its own: its clips take the source clip's
        // trigger array (FootLeft / FootRight). An own file carries its own (the 11-15 2HW files: walk FootLeft@0.167,
        // FootRight@1.0; 2HM WalkBackward has them at 0.1 and 0.933, RunLeft's are in another phase altogether), and
        // the clips the engine lets convert annotations (the 0.1x Slow slots, flags 0) converted them on top of the
        // shared array: two FootLeft sync points at different times, and the synced blend jumped between them every
        // frame while a stop crossed the Slow slot (block stops, the foot moved 25 a frame; source files in the same
        // tree: 3). So a clip playing an own file takes no trigger array, converts its file's annotations whatever the
        // source clip's flag was, and its animation data copy carries no source annotations: the file alone gives its
        // foot timing.
        auto* triggers = ownFile ? nullptr : src->triggers.get();
        if (triggers) NodeFactory::AddRef(triggers);
        constexpr int kDontConvert = 16;  // FLAG_DONT_CONVERT_ANNOTATIONS_TO_TRIGGERS
        const bool convert = ownFile || (src->flags & kDontConvert) == 0;
        auto* c = NodeFactory::MakeClip(name.c_str(), file.c_str(), binding, triggers, src, convert);
        c->cropStartAmountLocalTime = src->cropStartAmountLocalTime;
        c->cropEndAmountLocalTime = src->cropEndAmountLocalTime;
        c->startTime = src->startTime;
        c->playbackSpeed = src->playbackSpeed;
        c->enforcedDuration = src->enforcedDuration;
        c->userControlledTimeFraction = src->userControlledTimeFraction;
        c->mode = src->mode;
        c->flags = ownFile ? static_cast<decltype(c->flags)>(src->flags & ~kDontConvert) : src->flags;
        copies.push_back(AnimData::ClipCopy{NodeFactory::NodeName(src), name, 1.0f, !ownFile});
        ++out.nodes;
        clipCopies.emplace(src, c);
        return c;
    };

    // Direction ladders, then the direction blend over them, then the cyclic generator on top.
    std::vector<BlenderGeneratorChildLayout*> directionKids;
    for (int i = 0; i < 8; ++i) {
        const auto* src = source.blenders[static_cast<std::size_t>(i)];
        std::vector<BlenderGeneratorChildLayout*> kids;
        for (int k = 0; k < src->children._size; ++k) {
            const auto* child = src->children._data[k];
            kids.push_back(CopyChild(child, copyClip(static_cast<RE::hkbClipGenerator*>(child->generator))));
        }
        auto* b = CopyBlender(src, p.Renamed(NodeFactory::NodeName(src)), kids);
        ++out.nodes;
        out.directionBlenders.push_back(reinterpret_cast<RE::hkbNode*>(b));
        directionKids.push_back(CopyChild(source.direction->children._data[i], reinterpret_cast<RE::hkbGenerator*>(b)));
    }
    auto* direction = CopyBlender(source.direction, p.to + "Direction_Blend", directionKids);
    ++out.nodes;
    out.direction = reinterpret_cast<RE::hkbNode*>(direction);
    auto* cyclic = static_cast<RE::hkbNode*>(NodeFactory::AllocZero(kCyclicSize));
    std::memcpy(cyclic, source.cyclic, kCyclicSerializedEnd);
    NodeFactory::InitRefObj(cyclic, RE::VTABLE_BSCyclicBlendTransitionGenerator);
    CopyNodeBase(cyclic, (p.to + kCyclicSuffix).c_str());
    reinterpret_cast<RE::BSCyclicBlendTransitionGenerator*>(cyclic)->generator =
        reinterpret_cast<RE::hkbGenerator*>(direction);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(direction));
    NodeFactory::NoteCreatedNode();
    ++out.nodes;
    clipCount = clipCopies.size();
    return cyclic;
}

void LogFiles(const Prefixes& p, const std::vector<std::string>& own, const std::vector<std::string>& fallback) {
    std::string ownList, fallbackList;
    for (const auto& f : own) ownList += (ownList.empty() ? "" : ", ") + f;
    for (const auto& f : fallback) fallbackList += (fallbackList.empty() ? "" : ", ") + f;
    if (!ownList.empty()) logger::info("[Tree] {} files: {}", p.To(), ownList);
    if (!fallbackList.empty()) logger::info("[Tree] no {} file, plays the {} one: {}", p.To(), p.From(), fallbackList);
}

bool IsSwitch(RE::hkbNode* node) {
    return node && NodeFactory::SameVTable(node, RE::VTABLE_hkbManualSelectorGenerator) &&
           std::strncmp(NodeFactory::NodeName(node), "AI_Switch_", 10) == 0;
}

bool IsTag(RE::hkbNode* node) { return node && NodeFactory::SameVTable(node, RE::VTABLE_BSiStateTaggingGenerator); }

// 2HW: 2HW_iStateGen plays the copy; 2HW sneaking's upper body plays the copy's direction blend.
Built Build2HW(const Prefixes& p, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, const Found& found,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room) {
    Built out;
    if (!IsTag(found.tag2HW) || !IsTag(found.tag2HM)) {
        logger::warn("[Tree] 2HW use=\"own\": the 2HW/2HM iState generators were not found; axes move as before");
        return out;
    }
    auto* hw = reinterpret_cast<StateTaggingLayout*>(found.tag2HW);
    auto* hm = reinterpret_cast<StateTaggingLayout*>(found.tag2HM);
    Source source;
    std::string reason;
    if (hw->iStateToSetAs != 7 || hm->iStateToSetAs != 7 || !NodeNamed(hw->pDefaultGenerator, nullptr))
        reason = fmt::format(
            "the iState generators are not as expected (iState {}/{})", hw->iStateToSetAs, hm->iStateToSetAs);
    else
        reason = Check(p, hm->pDefaultGenerator, source);
    if (!reason.empty()) {
        logger::warn("[Tree] 2HW use=\"own\": {}; axes move as before", reason);
        return out;
    }
    const std::size_t need = 2 + source.blenders.size() + source.clips;
    if (!room(need)) {
        logger::warn("[Tree] 2HW use=\"own\": {} more nodes do not fit the node id budget; axes move as before", need);
        return out;
    }
    std::vector<std::string> own, fallback;
    std::size_t clips = 0;
    auto* cyclic = CopyTree(p, character, source, copies, out, own, fallback, clips);
    auto* direction = out.direction;

    // Hook up: 2HW locomotion, then the upper body of 2HW sneaking.
    const std::string before = NodeFactory::NodeName(hw->pDefaultGenerator);
    auto* walk = Switch(graph, cyclic, reinterpret_cast<RE::hkbNode*>(hw->pDefaultGenerator), "AI_Switch_2HW", room);
    out.nodes += walk != cyclic;
    hw->pDefaultGenerator = reinterpret_cast<RE::hkbGenerator*>(walk);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(walk));
    std::string sneakNote = "Sneak2HW_DirectionBlend not found; 2HW sneaking keeps its arm blend";
    if (found.sneak2HW && NodeFactory::SameVTable(found.sneak2HW, RE::VTABLE_hkbBlenderGenerator)) {
        auto* sneak = reinterpret_cast<BlenderGeneratorLayout*>(found.sneak2HW);
        sneakNote = "Sneak2HW_DirectionBlend has no 2HW_LocomotionArmBlend child; 2HW sneaking is unchanged";
        for (int i = 0; i < sneak->children._size; ++i) {
            auto* child = sneak->children._data[i];
            if (child && NodeNamed(child->generator, "2HW_LocomotionArmBlend")) {
                auto* upper = Switch(
                    graph, direction, reinterpret_cast<RE::hkbNode*>(child->generator), "AI_Switch_Sneak2HW", room);
                out.nodes += upper != direction;
                child->generator = reinterpret_cast<RE::hkbGenerator*>(upper);
                NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(upper));
                sneakNote = "2HW sneaking's upper body plays 2HW_Direction_Blend";
                break;
            }
        }
    }
    logger::info("[Tree] 2HW use=\"own\": 2HW locomotion {} -> 2HW_{} ({} nodes, {} clips); {}; {} 2HW files, {} files "
                 "fall back to 2HM",
        before, kCyclicSuffix, out.nodes, clips, sneakNote, own.size(), fallback.size());
    LogFiles(p, own, fallback);
    return out;
}

// H2H: a copy of 1HM_iStateGen over the tree copy; state 0 of the weapon state machine plays it.
Built BuildH2H(const Prefixes& p, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, const Found& found,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room) {
    Built out;
    const int unarmed = TreeKindOf(TreeWeapon::kH2H).handType;
    if (!IsTag(found.tag1HM) || !found.weapons || !NodeFactory::SameVTable(found.weapons, RE::VTABLE_hkbStateMachine)) {
        logger::warn("[Tree] H2H use=\"own\": 1HM_iStateGen or Melee_Direction_Behavior was not found; unarmed moves "
                     "as before");
        return out;
    }
    auto* tag = reinterpret_cast<StateTaggingLayout*>(found.tag1HM);
    auto* machine = reinterpret_cast<RE::hkbStateMachine*>(found.weapons);
    // The unarmed state: the one whose id is the hand type. It must play 1HM_iStateGen, and another state must play
    // it too (else the tree is unarmed's alone already, or another mod changed the states).
    Havok::StateInfoLayout* state = nullptr;
    int sharing = 0;
    std::string reason;
    if (machine->states._size < 2 || machine->states._size > 64 || !machine->states._data)
        reason = fmt::format("Melee_Direction_Behavior has {} states", machine->states._size);
    else {
        for (int i = 0; i < machine->states._size; ++i) {
            auto* info = reinterpret_cast<Havok::StateInfoLayout*>(machine->states._data[i]);
            if (!info || !NodeFactory::SameVTable(info, RE::VTABLE_hkbStateMachine__StateInfo)) {
                reason = fmt::format("state {} of Melee_Direction_Behavior is not a state", i);
                break;
            }
            if (reinterpret_cast<RE::hkbNode*>(info->generator) != found.tag1HM) continue;
            if (info->stateId == unarmed)
                state = info;
            else
                ++sharing;
        }
        if (reason.empty() && (!state || !sharing))
            reason = fmt::format("state {} of Melee_Direction_Behavior does not share 1HM_iStateGen with the "
                                 "one-handed states ({} other states play it)",
                unarmed, sharing);
    }
    Source source;
    if (reason.empty() && (tag->iStateToSetAs != 6 || !NodeNamed(tag->pDefaultGenerator, nullptr)))
        reason = fmt::format("1HM_iStateGen is not as expected (iState {})", tag->iStateToSetAs);
    if (reason.empty()) reason = Check(p, tag->pDefaultGenerator, source);
    if (!reason.empty()) {
        logger::warn("[Tree] H2H use=\"own\": {}; unarmed moves as before", reason);
        return out;
    }
    const std::size_t need = 3 + source.blenders.size() + source.clips;
    if (!room(need)) {
        logger::warn(
            "[Tree] H2H use=\"own\": {} more nodes do not fit the node id budget; unarmed moves as before", need);
        return out;
    }
    std::vector<std::string> own, fallback;
    std::size_t clips = 0;
    auto* cyclic = CopyTree(p, character, source, copies, out, own, fallback, clips);
    // The tagging generator: the serialized object is the whole class (pDefaultGenerator, iStateToSetAs, iPriority).
    auto* copy = static_cast<RE::hkbNode*>(NodeFactory::AllocZero(sizeof(StateTaggingLayout)));
    std::memcpy(copy, found.tag1HM, sizeof(StateTaggingLayout));
    NodeFactory::InitRefObj(copy, RE::VTABLE_BSiStateTaggingGenerator);
    CopyNodeBase(copy, "H2H_iStateGen");
    // The switch goes under the tagging generator, as 2HW's does: with the switch between the state and the tagging
    // generators the actor's iState read 0 while unarmed (11-20: SpeedSampled 217 for 243 running backward), on and
    // off alike. The engine takes iState from a tagging generator the state plays directly.
    auto* unarmedTree =
        Switch(graph, cyclic, reinterpret_cast<RE::hkbNode*>(tag->pDefaultGenerator), "AI_Switch_H2H", room);
    out.nodes += unarmedTree != cyclic;
    reinterpret_cast<StateTaggingLayout*>(copy)->pDefaultGenerator = reinterpret_cast<RE::hkbGenerator*>(unarmedTree);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(unarmedTree));
    NodeFactory::NoteCreatedNode();
    ++out.nodes;
    state->generator = reinterpret_cast<RE::hkbGenerator*>(copy);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(copy));
    logger::info("[Tree] H2H use=\"own\": state {} '{}' of Melee_Direction_Behavior 1HM_iStateGen -> H2H_iStateGen -> "
                 "H2H_{} ({} nodes, {} clips; {} one-handed states keep 1HM_iStateGen); {} H2H files, {} files fall "
                 "back to 1HM",
        unarmed, state->name.c_str() ? state->name.c_str() : "?", kCyclicSuffix, out.nodes, clips, sharing, own.size(),
        fallback.size());
    LogFiles(p, own, fallback);
    return out;
}
}  // namespace

Built Build(TreeWeapon weapon, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, const Found& found,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room) {
    const Prefixes p(weapon);
    return weapon == TreeWeapon::k2HW ? Build2HW(p, graph, character, found, copies, room)
                                      : BuildH2H(p, graph, character, found, copies, room);
}

Built BuildBlock(TreeWeapon weapon, RE::hkbBehaviorGraph* graph, RE::hkbCharacter* character, RE::hkbNode* selector,
    std::vector<AnimData::ClipCopy>& copies, const std::function<bool(std::size_t)>& room) {
    Built out;
    const Prefixes p(weapon);
    const auto& kind = TreeKindOf(weapon);
    if (!selector || !NodeFactory::SameVTable(selector, RE::VTABLE_hkbManualSelectorGenerator)) {
        logger::warn("[Tree] {} use=\"own\" (block): Shd_Locomotion_MSG not found in BlockBehavior; it blocks with "
                     "the {} legs",
            kind.xml, p.From());
        return out;
    }
    auto& g = reinterpret_cast<Havok::ManualSelectorLayout*>(selector)->generators;
    // iRightHandType: the weapon's entry and an entry of the kind whose tree it shares (2HM 5, a one-handed sword 1).
    const int own = kind.handType, shared = weapon == TreeWeapon::k2HW ? 5 : 1;
    const int last = std::max(own, shared);
    Source source;
    std::string reason;
    if (g._size <= last || !Havok::Readable(g._data, static_cast<std::size_t>(g._size) * sizeof(void*)) ||
        g._data[own] != g._data[shared])
        reason = fmt::format("Shd_Locomotion_MSG is not as expected ({} entries, entry {} {})", g._size, own,
            g._size > last ? NodeFactory::NodeName(g._data[own]) : "-");
    else
        reason = Check(p, g._data[shared], source);
    if (!reason.empty()) {
        logger::warn("[Tree] {} use=\"own\" (block): {}; it blocks with the {} legs", kind.xml, reason, p.From());
        return out;
    }
    const std::size_t need = 2 + source.blenders.size() + source.clips;
    if (!room(need)) {
        logger::warn("[Tree] {} use=\"own\" (block): {} more nodes do not fit the node id budget; it blocks with the "
                     "{} legs",
            kind.xml, need, p.From());
        return out;
    }
    std::vector<std::string> ownFiles, fallback;
    std::size_t clips = 0;
    auto* cyclic = CopyTree(p, character, source, copies, out, ownFiles, fallback, clips);
    auto* blocking = Switch(graph, cyclic, reinterpret_cast<RE::hkbNode*>(g._data[shared]),
        weapon == TreeWeapon::k2HW ? "AI_Switch_Block2HW" : "AI_Switch_BlockH2H", room);
    out.nodes += blocking != cyclic;
    g._data[own] = reinterpret_cast<RE::hkbGenerator*>(blocking);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(blocking));
    logger::info(
        "[Tree] {} use=\"own\" (block): Shd_Locomotion_MSG entry {} -> {}{} ({} nodes, {} clips); {} {} files, "
        "{} files fall back to {}",
        kind.xml, own, p.to, kCyclicSuffix, out.nodes, clips, ownFiles.size(), p.To(), fallback.size(), p.From());
    return out;
}

RE::hkbNode* Switch(RE::hkbBehaviorGraph* graph, RE::hkbNode* mod, RE::hkbNode* vanilla, const char* name,
    const std::function<bool(std::size_t)>& room) {
    const auto keep = [&](const char* reason) {
        logger::warn("[Tree] {}: {}; with the split off this place keeps the mod's node", name, reason);
        return mod;
    };
    const auto* sample = LocomotionDrive::SwitchSample();
    if (!mod || !vanilla || mod == vanilla) return mod;
    if (!sample || !NodeFactory::SameVTable(sample, RE::VTABLE_hkbManualSelectorGenerator))
        return keep("no vanilla selector bound to an int variable was found");
    if (!graph) return keep("its graph is not known");
    if (!room(1)) return keep("one more node does not fit the node id budget");
    auto* node = static_cast<RE::hkbNode*>(NodeFactory::AllocZero(sizeof(Havok::ManualSelectorLayout)));
    std::memcpy(node, sample, sizeof(Havok::ManualSelectorLayout));
    NodeFactory::InitRefObj(node, RE::VTABLE_hkbManualSelectorGenerator);
    std::memset(&node->variableBindingSet, 0, sizeof(node->variableBindingSet));  // BindSwitch gives it its own
    std::memset(&node->name, 0, sizeof(node->name));
    NodeFactory::MakeString(node->name, name);
    NodeFactory::EmptyArray(node->cachedBindables);
    node->areBindablesCached = false;
    auto* layout = reinterpret_cast<Havok::ManualSelectorLayout*>(node);
    auto* data = static_cast<RE::hkbGenerator**>(NodeFactory::AllocZero(2 * sizeof(RE::hkbGenerator*)));
    data[0] = reinterpret_cast<RE::hkbGenerator*>(mod);
    data[1] = reinterpret_cast<RE::hkbGenerator*>(vanilla);
    NodeFactory::OwnArray(layout->generators, data, 2);
    layout->selectedGeneratorIndex = 0;
    layout->currentGeneratorIndex = 0;
    if (!LocomotionDrive::BindSwitch(graph, node)) return keep("its binding could not be made");
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(mod));
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(vanilla));
    NodeFactory::NoteCreatedNode();
    return node;
}

bool FullBodyArms(RE::hkbBehaviorGraph* graph, RE::hkbNode* armPose, RE::hkbNode* readied,
    const std::function<bool(std::size_t)>& room) {
    const auto fail = [](const std::string& reason) {
        logger::warn(
            "[Tree] H2H use=\"own\" (arms): {}; unarmed keeps the H2H_LocomotionPose arms while moving", reason);
        return false;
    };
    if (!armPose || !NodeFactory::SameVTable(armPose, RE::VTABLE_hkbStateMachine))
        return fail("H2H_SprintPoseBehavior was not found in 1HM_Behavior");
    if (!readied || !NodeFactory::SameVTable(readied, RE::VTABLE_hkbManualSelectorGenerator))
        return fail("1HM_Readied_MSG was not found in 1HM_Behavior");
    auto* machine = reinterpret_cast<RE::hkbStateMachine*>(armPose);
    if (machine->states._size < 1 || machine->states._size > 16 || !machine->states._data)
        return fail(fmt::format("H2H_SprintPoseBehavior has {} states", machine->states._size));
    // The state that plays the pose clip: by the clip's name, as another mod may have reordered the states.
    Havok::StateInfoLayout* state = nullptr;
    for (int i = 0; i < machine->states._size; ++i) {
        auto* info = reinterpret_cast<Havok::StateInfoLayout*>(machine->states._data[i]);
        if (!info || !NodeFactory::SameVTable(info, RE::VTABLE_hkbStateMachine__StateInfo))
            return fail(fmt::format("state {} of H2H_SprintPoseBehavior is not a state", i));
        auto* generator = reinterpret_cast<RE::hkbNode*>(info->generator);
        // Done already (another character project shares the template).
        if (generator == readied || IsSwitch(generator)) return true;
        if (generator && NodeFactory::SameVTable(generator, RE::VTABLE_hkbClipGenerator) &&
            _stricmp(NodeFactory::NodeName(generator), "H2H_LocomotionPose") == 0)
            state = info;
    }
    if (!state) return fail("no state of H2H_SprintPoseBehavior plays the clip H2H_LocomotionPose");
    auto* arms = Switch(graph, readied, reinterpret_cast<RE::hkbNode*>(state->generator), "AI_Switch_H2HArms", room);
    state->generator = reinterpret_cast<RE::hkbGenerator*>(arms);
    NodeFactory::AddRef(reinterpret_cast<RE::hkReferencedObject*>(arms));
    logger::info("[Tree] H2H use=\"own\" (arms): state '{}' of H2H_SprintPoseBehavior H2H_LocomotionPose -> "
                 "1HM_Readied_MSG; unarmed moves with the whole body",
        state->name.c_str() ? state->name.c_str() : "?");
    return true;
}

}  // namespace AnimationInjector::OwnLocomotion

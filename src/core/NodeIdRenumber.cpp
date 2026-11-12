#include "PCH.h"
#include "core/NodeIdRenumber.h"

#include "core/GraphInjector.h"
#include "core/NodeFactory.h"

namespace AnimationInjector::NodeIdRenumber {

namespace {

// Address Library ids (2026-11-09): found by the 1.7.104 code (Ghidra) in a 1.6.1170 image read from the running game
// and in the 1.5.97 exe (Steam depot, SteamStub removed with Steamless). Each call site is checked before hooking.
// The link path is the same code in both: hkbBehaviorGraph link calls the numbering at +0x70.
constexpr REL::RelocationID kLinkCaller{57921, 58502};
constexpr REL::RelocationID kLinkNumber{57796, 58369};  // numbers the nodes, fills the id-to-state-machine table
constexpr std::ptrdiff_t kLinkCallOffset = 0x70;
// The graph load path: AE numbers inside one large function (graph second of 7 arguments), called at +0x2A6; SE has
// that loop as its own function (graph first of 4), called at +0x131.
constexpr REL::RelocationID kLoadCaller{63027, 63845};
constexpr REL::RelocationID kLoadNumber{63025, 63946};
constexpr std::ptrdiff_t kLoadCallOffsetSE = 0x131;
constexpr std::ptrdiff_t kLoadCallOffsetAE = 0x2A6;
constexpr REL::RelocationID kMapInsert{
    56771, 57198};  // hkPointerMap insert (map, allocator, key, value); replaces a key

// hkbBehaviorGraph::GlobalTransitionData + 0x50: an open-addressing map of 16-byte {key, value} entries, key ~0 free.
constexpr std::size_t kMachineTableOffset = 0x50;
struct PointerMap {
    std::uint64_t* entries;
    std::int32_t count;
    std::int32_t hashMod;  // capacity - 1
};
static_assert(sizeof(PointerMap) == 0x10);
constexpr std::uint64_t kFreeKey = ~0ull;

bool g_renumber = true;
bool g_installed = false;
bool g_loadInstalled = false;
std::mutex g_countedLock;
// What the injection pass said about a graph right before the engine numbers it. A graph object's address comes back
// for another graph later, so a note counts only for the root it was made with and for a short while.
struct Note {
    RE::hkbNode* root = nullptr;
    std::chrono::steady_clock::time_point at;
    std::vector<RE::hkbNode*> nodes;  // past the limit: the set to renumber
    std::size_t count = 0;            // its size, or the count of a graph that fits
    bool Current(RE::hkbBehaviorGraph* a_graph) const {
        return root && a_graph->rootGenerator.get() == root &&
               std::chrono::steady_clock::now() - at < std::chrono::seconds(60);
    }
};
std::unordered_map<RE::hkbBehaviorGraph*, Note> g_notes;
// Graphs this renumbered: the engine numbers a graph on both hooked paths, and the second finds it done.
struct Done {
    RE::hkbNode* root = nullptr;
    std::int16_t cloneBase = 0;
};
std::unordered_map<RE::hkbBehaviorGraph*, Done> g_done;
constexpr std::size_t kNotesKept = 512;

PointerMap* MachineTable(RE::hkbBehaviorGraph* a_graph) {
    auto* data = a_graph ? a_graph->globalTransitionData.get() : nullptr;
    return data ? reinterpret_cast<PointerMap*>(reinterpret_cast<std::uint8_t*>(data) + kMachineTableOffset) : nullptr;
}
// The engine keys the table by the id read as a signed 16-bit number.
std::uint64_t KeyOf(std::uint16_t a_id) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(a_id)));
}
// The engine's lookup, read only.
std::uint64_t Find(const PointerMap& a_map, std::uint64_t a_key) {
    if (a_map.hashMod <= 0 || !a_map.entries) return 0;
    const auto mod = static_cast<std::uint32_t>(a_map.hashMod);
    std::uint32_t slot = (static_cast<std::uint32_t>(a_key >> 4) * 0x9E3779B1u) & mod;
    for (std::uint32_t probes = 0; probes <= mod; ++probes) {
        const auto key = a_map.entries[slot * 2];
        if (key == kFreeKey) return 0;
        if (key == a_key) return a_map.entries[slot * 2 + 1];
        slot = (slot + 1) & mod;
    }
    return 0;
}
bool IsMachine(const RE::hkbNode* a_node) { return NodeFactory::SameVTable(a_node, RE::VTABLE_hkbStateMachine); }

std::string Describe(const RE::hkbNode* a_node) {
    const char* name = NodeFactory::NodeName(a_node);
    return fmt::format("{}{}#{}", name ? name : "?", IsMachine(a_node) ? "(machine)" : "", a_node->id);
}

void Check(RE::hkbBehaviorGraph* a_graph, const char* a_path) {
    if (!a_graph || !a_graph->rootGenerator) return;
    const auto started = std::chrono::steady_clock::now();
    const auto engineId = static_cast<std::uint16_t>(a_graph->numStaticNodes);
    std::vector<RE::hkbNode*> counted;
    {
        std::lock_guard lock(g_countedLock);
        // Renumbered on the other path already.
        if (const auto it = g_done.find(a_graph); it != g_done.end()) {
            if (it->second.root == a_graph->rootGenerator.get() && a_graph->numStaticNodes == it->second.cloneBase)
                return;
            g_done.erase(it);
        }
        if (const auto it = g_notes.find(a_graph); it != g_notes.end()) {
            auto& note = it->second;
            if (note.Current(a_graph)) {
                // Counted as fitting, and the engine's count agrees (it numbers a few more than the walk finds, never
                // fewer; a count that wrapped would read far below): nothing to walk.
                if (note.nodes.empty() && note.count + kFreeForClones <= kIdLimit && engineId >= note.count &&
                    engineId - note.count <= kFreeForClones)
                    return;
                counted = std::move(note.nodes);
            }
            g_notes.erase(it);
        }
    }
    // The counted set is what the engine numbered when their 16-bit counts agree (2026-11-09: 72330 and 72330): no walk.
    const bool exact = !counted.empty() && static_cast<std::uint16_t>(counted.size()) == engineId;
    std::vector<RE::hkbNode*> walked;
    if (!exact) walked = GraphInjector::AllNodes(a_graph->rootGenerator.get());
    if (std::max(walked.size(), counted.size()) + kFreeForClones <= kIdLimit) return;  // fits: nothing to say
    // Otherwise the counted set belongs to this graph only when the walk finds nearly all of it.
    std::size_t overlap = counted.size();
    std::vector<RE::hkbNode*> nodes, onlyCounted, withoutId;
    const std::size_t countedCount = counted.size();
    if (exact)
        nodes = std::move(counted);
    else {
        std::unordered_set<RE::hkbNode*> seen(walked.begin(), walked.end());
        overlap = 0;
        for (auto* node : counted) overlap += seen.contains(node) ? 1 : 0;
        const bool useCounted = !counted.empty() && overlap * 10 >= counted.size() * 9;
        // The nodes the engine numbered: what the walk finds and the counted set. The engine's 65536th node reads
        // 0xFFFF, the same as "no id" (2026-11-09), so an id of 0xFFFF does not leave a node out; the count check in
        // Renumber decides.
        for (auto* node : walked) {
            if (node->id == 0xFFFF) withoutId.push_back(node);
            nodes.push_back(node);
        }
        if (useCounted)
            for (auto* node : counted)
                if (seen.insert(node).second) {
                    onlyCounted.push_back(node);
                    nodes.push_back(node);
                }
    }
    const std::size_t walkedCount = walked.size();
    // What the engine numbered, read before any renumbering.
    const auto engineCount = static_cast<std::uint16_t>(a_graph->numStaticNodes);
    std::size_t machines = 0;
    for (auto* node : nodes) machines += IsMachine(node) ? 1 : 0;
    // The engine's own numbering, measured before anything changes it: the negative control in the same run.
    const auto brokenBefore = BrokenMachines(a_graph, nodes);
    Result result;
    if (g_renumber)
        result = Renumber(a_graph, nodes);
    else {
        result.nodes = nodes.size();
        result.machines = machines;
        result.skipped = "renumbering off (bNodeIdRenumber = false)";
    }
    const auto broken = BrokenMachines(a_graph, nodes);
    if (result.renumbered) {
        std::lock_guard lock(g_countedLock);
        if (g_done.size() >= kNotesKept) g_done.clear();
        g_done[a_graph] = {a_graph->rootGenerator.get(), a_graph->numStaticNodes};
    }
    const char* name = NodeFactory::NodeName(a_graph->rootGenerator.get());
    logger::info("[NodeIdRenumber] {} graph '{}': nodes={} (16-bit {}) engineCount={} walked={} counted={} overlap={} "
                 "onlyCounted={} withoutId={} machines={} renumbered={}{}{} sharedIds={} cloneBase={} topMachineId={} "
                 "brokenBefore={} brokenMachines={} ms={:.1f}",
        a_path, name ? name : "?", result.nodes, static_cast<std::uint16_t>(result.nodes), engineCount, walkedCount,
        countedCount, overlap, onlyCounted.size(), withoutId.size(), result.machines, result.renumbered,
        result.skipped ? " skipped=" : "", result.skipped ? result.skipped : "", result.sharedIds, result.cloneBase,
        result.topMachineId, brokenBefore, broken,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    // Which nodes the walk after the numbering misses, and which it finds without an id (first ones).
    std::string list;
    for (std::size_t i = 0; i < onlyCounted.size() && i < 80; ++i) list += (i ? ", " : "") + Describe(onlyCounted[i]);
    if (!list.empty()) logger::info("[NodeIdRenumber]   only in the counted set: {}", list);
    list.clear();
    for (std::size_t i = 0; i < withoutId.size() && i < 20; ++i) list += (i ? ", " : "") + Describe(withoutId[i]);
    if (!list.empty()) logger::info("[NodeIdRenumber]   found without an id: {}", list);
    if (broken)
        logger::error(
            "[NodeIdRenumber] '{}': {} state machines cannot be found by their id; a global transition into one "
            "can crash",
            name ? name : "?", broken);
}

struct LinkNumbering {
    static void thunk(RE::hkbBehaviorGraph* a_graph) {
        func(a_graph);
        Check(a_graph, "link");
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct LoadNumberingAE {
    static std::uint64_t thunk(
        void* a_1, RE::hkbBehaviorGraph* a_graph, void* a_3, void* a_4, void* a_5, void* a_6, void* a_7) {
        const auto result = func(a_1, a_graph, a_3, a_4, a_5, a_6, a_7);
        Check(a_graph, "load");
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct LoadNumberingSE {
    static void thunk(RE::hkbBehaviorGraph* a_graph, void* a_2, void* a_3, void* a_4) {
        func(a_graph, a_2, a_3, a_4);
        Check(a_graph, "load");
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// A call site still calls the expected function (another plugin hooking it, or another runtime, would differ).
bool CallsTarget(std::uintptr_t a_site, std::uintptr_t a_target) {
    const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
    if (code[0] != 0xE8) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, code + 1, sizeof(rel));
    return a_site + 5 + static_cast<std::intptr_t>(rel) == a_target;
}
template<class Hook>
bool HookSite(REL::RelocationID a_caller, std::ptrdiff_t a_offset, REL::RelocationID a_target, const char* a_label) {
    const auto site = a_caller.address() + a_offset;
    const auto target = a_target.address();
    if (!CallsTarget(site, target)) {
        logger::warn(
            "[NodeIdRenumber] {} numbering call not found at its place; that path stays the engine's", a_label);
        return false;
    }
    auto& trampoline = SKSE::GetTrampoline();
    Hook::func = trampoline.write_call<5>(site, Hook::thunk);
    return true;
}

}  // namespace

void Install(bool a_renumber) {
    g_renumber = a_renumber;
    if (REL::Module::IsVR()) {
        logger::info("[NodeIdRenumber] not installed: the engine addresses are known for SE and AE only");
        return;
    }
    const bool link = HookSite<LinkNumbering>(kLinkCaller, kLinkCallOffset, kLinkNumber, "link");
    const bool load = REL::Module::IsAE()
                          ? HookSite<LoadNumberingAE>(kLoadCaller, kLoadCallOffsetAE, kLoadNumber, "load")
                          : HookSite<LoadNumberingSE>(kLoadCaller, kLoadCallOffsetSE, kLoadNumber, "load");
    g_installed = link || load;
    g_loadInstalled = load;
    logger::info("[NodeIdRenumber] installed link={} load={} renumber={}", link, load, a_renumber);
}
bool Installed() { return g_installed; }
bool Active() { return g_loadInstalled && g_renumber; }
std::size_t Machines(const std::vector<RE::hkbNode*>& a_nodes) {
    std::size_t machines = 0;
    for (const auto* node : a_nodes) machines += IsMachine(node) ? 1 : 0;
    return machines;
}

namespace {
void Keep(RE::hkbBehaviorGraph* a_graph, std::vector<RE::hkbNode*> a_nodes, std::size_t a_count) {
    if (!a_graph) return;
    std::lock_guard lock(g_countedLock);
    // A graph noted and never numbered would stay; they are few, and the map is emptied rather than grown.
    if (g_notes.size() >= kNotesKept) g_notes.clear();
    g_done.erase(a_graph);
    g_notes[a_graph] = {a_graph->rootGenerator.get(), std::chrono::steady_clock::now(), std::move(a_nodes), a_count};
}
}  // namespace
void NoteCounted(RE::hkbBehaviorGraph* a_graph, std::vector<RE::hkbNode*> a_nodes) {
    const auto count = a_nodes.size();
    Keep(a_graph, std::move(a_nodes), count);
}
void NoteFits(RE::hkbBehaviorGraph* a_graph, std::size_t a_count) { Keep(a_graph, {}, a_count); }

Result Renumber(RE::hkbBehaviorGraph* a_graph, const std::vector<RE::hkbNode*>& nodes) {
    Result result;
    if (!a_graph || !a_graph->rootGenerator) {
        result.skipped = "no graph";
        return result;
    }
    result.nodes = nodes.size();
    std::vector<RE::hkbNode*> machines;
    for (auto* node : nodes)
        if (IsMachine(node)) machines.push_back(node);
    result.machines = machines.size();
    if (result.nodes + kFreeForClones <= kIdLimit) {
        result.skipped = "fits";
        return result;
    }
    // The engine numbered exactly this set: its 16-bit count is ours. A graph loaded again keeps the ids this gave it
    // (its count reads the machines' count then).
    const auto layout = LayoutFor(result.machines);
    if (static_cast<std::uint16_t>(a_graph->numStaticNodes) != static_cast<std::uint16_t>(result.nodes)) {
        result.skipped = static_cast<std::size_t>(a_graph->numStaticNodes) == layout.cloneBase
                             ? "already renumbered"
                             : "the engine numbered another set of nodes";
        return result;
    }
    if (!MachinesFit(result.machines)) {
        result.skipped = "too many state machines";
        return result;
    }
    auto* table = MachineTable(a_graph);
    if (!table) {
        result.skipped = "no global transition data";
        return result;
    }
    static REL::Relocation<int (*)(PointerMap*, void*, std::uint64_t, std::uint64_t)> insert{kMapInsert};
    auto* allocator = RE::hkContainerHeapAllocator::GetSingleton();

    // State machines first, each its own id, around the clone range (see LayoutFor); the other nodes share what is
    // left. Runtime clone ids must stay positive: the engine returns only ids >= numStaticNodes and >= 0 to its pool.
    const auto cloneBase = static_cast<std::uint16_t>(layout.cloneBase);
    const std::size_t sharedFirst = layout.sharedFirst;
    for (std::size_t i = 0; i < machines.size(); ++i) machines[i]->id = static_cast<std::uint16_t>(layout.MachineId(i));
    result.topMachineId = machines.empty() ? 0 : static_cast<std::uint16_t>(layout.MachineId(machines.size() - 1));
    std::size_t shared = sharedFirst;
    for (auto* node : nodes) {
        if (IsMachine(node)) continue;
        if (shared >= kIdLimit) shared = sharedFirst;
        node->id = static_cast<std::uint16_t>(shared++);
    }
    const std::size_t sharedRange = kIdLimit - sharedFirst;
    const std::size_t others = result.nodes - result.machines;
    result.sharedIds = others > sharedRange ? others - sharedRange : 0;

    // The table again, by the new ids.
    if (table->entries && table->hashMod > 0)
        for (std::int32_t i = 0; i <= table->hashMod; ++i) table->entries[static_cast<std::size_t>(i) * 2] = kFreeKey;
    table->count = 0;
    for (auto* machine : machines)
        insert(table, allocator, KeyOf(machine->id), reinterpret_cast<std::uint64_t>(machine));

    a_graph->numStaticNodes = static_cast<std::int16_t>(cloneBase);
    a_graph->nextUniqueID = static_cast<std::int16_t>(cloneBase);
    a_graph->uniqueIDPool._size = 0;
    result.cloneBase = cloneBase;
    result.renumbered = true;
    return result;
}

std::size_t BrokenMachines(RE::hkbBehaviorGraph* a_graph, const std::vector<RE::hkbNode*>& a_nodes) {
    auto* table = MachineTable(a_graph);
    if (!table) return 0;
    std::size_t broken = 0;
    for (auto* node : a_nodes)
        if (IsMachine(node) && Find(*table, KeyOf(node->id)) != reinterpret_cast<std::uint64_t>(node)) ++broken;
    return broken;
}

}  // namespace AnimationInjector::NodeIdRenumber

namespace AnimationInjector::NodeIdStress {

namespace {
// A copy of a_template with nothing of its own: no states, transitions, bindings or events. Never activated.
RE::hkbStateMachine* EmptyMachine(const RE::hkbStateMachine* a_template, const char* a_name) {
    using namespace NodeFactory;
    auto* machine = static_cast<RE::hkbStateMachine*>(AllocZero(sizeof(RE::hkbStateMachine)));
    if (!machine) return nullptr;
    std::memcpy(machine, a_template, sizeof(RE::hkbStateMachine));
    auto clear = [](auto& member) { std::memset(&member, 0, sizeof(member)); };
    machine->memSizeAndFlags = 0xFFFF;
    machine->referenceCount = 0x4000;
    clear(machine->variableBindingSet);
    EmptyArray(machine->cachedBindables);
    machine->areBindablesCached = false;
    machine->userData = 0;
    MakeString(machine->name, a_name);
    machine->id = 0xFFFF;
    machine->cloneState = RE::hkbNode::CloneState::kDefault;
    clear(machine->eventToSendWhenStateOrTransitionChanges);
    machine->eventToSendWhenStateOrTransitionChanges.id = RE::hkbEventBase::kNull;
    clear(machine->startStateChooser);
    machine->returnToPreviousStateEventID = -1;
    machine->randomTransitionEventID = -1;
    machine->transitionToNextHigherStateEventID = -1;
    machine->transitionToNextLowerStateEventID = -1;
    machine->syncVariableIndex = -1;
    machine->isActive = false;
    EmptyArray(machine->states);
    clear(machine->wildcardTransitions);
    EmptyArray(machine->activeTransitions);
    EmptyArray(machine->transitionFlags);
    EmptyArray(machine->wildcardTransitionFlags);
    EmptyArray(machine->delayedTransitions);
    machine->timeInState = 0;
    machine->lastLocalTime = 0;
    machine->stateOrTransitionChanged = false;
    machine->echoNextUpdate = false;
    NoteCreatedNode();
    return machine;
}
constexpr std::int32_t kStressStateId = 0x3FFF0000;
}  // namespace

std::size_t Inflate(
    RE::hkbBehaviorGraph* a_graph, RE::hkbStateMachine* a_machine, std::size_t a_count, std::size_t a_machineEvery) {
    const std::size_t kMachineEvery = std::max<std::size_t>(1, a_machineEvery);
    using namespace NodeFactory;
    if (!a_graph || !a_machine || a_count == 0) return 0;
    const auto started = std::chrono::steady_clock::now();
    auto* container = EmptyMachine(a_machine, "AI_NodeIdStress");
    if (!container) return 0;
    // A clip of the graph to copy: the dummy clips name an animation it already binds and never play.
    RE::hkbClipGenerator* clipTemplate = nullptr;
    for (auto* node : GraphInjector::AllNodes(a_machine))
        if (SameVTable(node, RE::VTABLE_hkbClipGenerator)) {
            clipTemplate = static_cast<RE::hkbClipGenerator*>(node);
            break;
        }
    if (!clipTemplate) return 0;
    auto** states = static_cast<RE::hkbStateMachine::StateInfo**>(AllocZero(sizeof(void*) * a_count));
    if (!states) return 0;
    // Like a real graph, most nodes are not state machines (a test game's 0_master: 1164 of 7297): one in kMachineEvery.
    // The clips come first so the machines are numbered last and are the ones whose ids wrap.
    const std::size_t machineCount = a_count / kMachineEvery;
    const std::size_t clipCount = a_count - machineCount;
    std::size_t made = 0, machines = 0;
    for (; made < a_count; ++made) {
        RE::hkbGenerator* dummy = nullptr;
        if (made < clipCount)
            dummy = MakeClip("AI_NodeIdStressClip", clipTemplate->animationName.c_str(),
                clipTemplate->animationBindingIndex, MakeTriggerArray({}), clipTemplate, false);
        else if (auto* machine = EmptyMachine(a_machine, "AI_NodeIdStressNode")) {
            dummy = reinterpret_cast<RE::hkbGenerator*>(machine);
            ++machines;
        }
        if (!dummy) break;
        states[made] = reinterpret_cast<RE::hkbStateMachine::StateInfo*>(
            MakeState("AI_NodeIdStressState", static_cast<std::int32_t>(made), dummy, nullptr));
    }
    OwnArray(container->states, states, static_cast<std::int32_t>(made));
    a_machine->states.push_back(reinterpret_cast<RE::hkbStateMachine::StateInfo*>(
        MakeState("AI_NodeIdStress", kStressStateId, reinterpret_cast<RE::hkbGenerator*>(container), nullptr)));
    logger::warn(
        "[NodeIdStress] test mode: {} nodes added to '{}' ({} empty state machines, {} idle clips; iNodeIdStressNodes) "
        "ms={:.1f}",
        made + 1, NodeName(a_machine) ? NodeName(a_machine) : "?", machines + 1, made - machines,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return made + 1;
}

}  // namespace AnimationInjector::NodeIdStress

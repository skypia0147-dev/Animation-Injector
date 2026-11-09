#pragma once

// Behavior graphs past 65535 nodes (the engine's node ids are 16-bit).
//
// The engine numbers a graph's nodes 0, 1, 2, ... when it links the graph, and past 65535 the ids wrap. Only two
// things look a node up by its id (Ghidra, SkyrimSE 1.7.104, 2026-11-09):
//   - the graph's id-to-state-machine table (GlobalTransitionData + 0x50) and the global transition list, both keyed
//     by a state machine's id: two machines sharing an id lose one, and a global transition into it reads null
//     (the 11-07 Trung53 crash);
//   - runtime clones (transition effects) draw ids from nextUniqueID, which the engine sets to the 16-bit node count:
//     past the limit it starts inside the static ids.
// The internal-state save/restore keyed by node id (hkbBehaviorGraph::get/setInternalState) has no caller in the game.
//
// So when a graph has more nodes than ids, this renumbers it right after the engine does: every state machine gets
// its own id first, runtime clones get a range no state machine uses, and the other nodes share the rest. The
// id-to-state-machine table is filled again with the new ids. A graph that fits is left exactly as the engine made it.
namespace AnimationInjector::NodeIdRenumber {

// Ids 0..0xFFFE; 0xFFFF means "no id".
inline constexpr std::size_t kIdLimit = 0xFFFF;
// Ids the engine's own numbering leaves for runtime clones before a graph counts as over the limit.
inline constexpr std::size_t kFreeForClones = 512;
// Ids kept for runtime clones, right after the state machines' ids.
inline constexpr std::size_t kCloneIds = 4096;
// State machine and clone ids must stay positive 16-bit numbers (the engine returns only ids >= 0 to its pool).
inline constexpr std::size_t kMachineIdLimit = 0x7FFF;
// State machines Animation Injector may add to a graph on top of its own (pair containers and the like).
inline constexpr std::size_t kInjectedMachinesReserve = 64;

struct Result {
    std::size_t nodes = 0;          // nodes the engine numbered
    std::size_t machines = 0;       // state machines among them
    std::size_t sharedIds = 0;      // ids handed out a second time (to nodes other than state machines)
    std::uint16_t cloneBase = 0;    // first id handed to runtime clones (numStaticNodes and nextUniqueID)
    bool renumbered = false;        // false: the graph fit, renumbering is off, or a check failed (see skipped)
    const char* skipped = nullptr;  // why it was left as the engine made it
};

// Installs the two call-site hooks after the engine's numbering functions (hkbBehaviorGraph link and the graph load
// path). Each site is checked for this runtime first (a call to the expected function); a site that differs is left
// unhooked and logged. The hooks always check a graph past the limit (BrokenMachines); they renumber it only when
// a_renumber is true ([Debug] bNodeIdRenumber), so the engine's own numbering can be measured as a negative control.
void Install(bool a_renumber);
bool Installed();
// The graph load path is hooked and renumbering is on: a character graph past the node id limit will be renumbered,
// so what has to fit is its state machines (core/GraphInjector's node budget, the loading panel).
bool Active();
// State machines among a_nodes.
std::size_t Machines(const std::vector<RE::hkbNode*>& a_nodes);

// The nodes the injection pass counted under a graph's root right before the engine numbers it. That is the set the
// engine numbers (2026-11-09: 72330 counted there, the engine's count 72330, while a walk after the numbering found
// 69 fewer); the hooks use it with what they find.
void NoteCounted(RE::hkbBehaviorGraph* a_graph, std::vector<RE::hkbNode*> a_nodes);

// Renumbers one graph as described above, given the nodes the engine numbered. Called by the hooks.
Result Renumber(RE::hkbBehaviorGraph* a_graph, const std::vector<RE::hkbNode*>& a_nodes);

// Integrity of a linked graph: state machines among a_nodes whose id does not find that same machine in the
// id-to-state-machine table (another machine, or none). 0 is healthy.
std::size_t BrokenMachines(RE::hkbBehaviorGraph* a_graph, const std::vector<RE::hkbNode*>& a_nodes);

}  // namespace AnimationInjector::NodeIdRenumber

// Test mode ([Debug] iNodeIdStressNodes = N, 0 = off): adds N inert nodes to the third-person 0_master before the
// engine numbers it, so a small load order passes the id limit. They hang under one extra state of a_machine that no
// transition enters; they only take ids. One in eight is an empty state machine, as in a real graph (2026-11-09: all
// machines made 66165 of them, more than there are ids, so nothing could be renumbered); numbered last, their wrapped
// ids make machines share ids. Negative control: with renumbering off BrokenMachines is above 0, with it on 0.
namespace AnimationInjector::NodeIdStress {

std::size_t Inflate(RE::hkbBehaviorGraph* a_graph, RE::hkbStateMachine* a_machine, std::size_t a_count);

}  // namespace AnimationInjector::NodeIdStress

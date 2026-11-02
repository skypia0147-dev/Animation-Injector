#pragma once
#include "PCH.h"
#include "runtime/EventRegistrySpec.h"

// The playing clips that turn a window (iframe, hyper armor, rotation, time scale) on and off by themselves
// (runtime/ClipWindowsJudge.h, user decision 11-02). A window a registered event or an AI. command turns on from
// such a clip ends with that clip, when it ends or is cut short, as a clip range does; any other waits for its off
// (runtime/EventEffects.h). Every clip of every actor is read once when it starts: its registered window events
// and its AI. window commands; only the clips that own a window are kept.
namespace AnimationInjector::ClipWindows {
struct Entry {
    std::size_t event = 0;  // the registry's event index
    std::string name;
    std::vector<EventOp> ops;
};
// When the registry is built: its events.
void Build(const std::vector<Entry>& entries);

// Any thread (the clip hooks): `clip` started on `actor` with `animation` bound, or stopped.
void Activated(const void* clip, const RE::hkaAnimation* animation, RE::Actor* actor);
void Deactivated(const void* clip);
// Any thread (the synchronized clip hook, after the clip inside it started): `clip` plays in a pair with this sync
// prefix, so its actor's annotations are the ones with that prefix ("NPC" or a null one, "2_").
void Synchronized(const void* clip, const char* prefix);

// Main thread. The playing (or just stopped) clip of `actor` that owns the window `on` turns on through `event`;
// 0 = none.
std::uint64_t Find(RE::ActorHandle actor, std::size_t event, EventOp on);
// Main thread. Whether the clip is still kept (a function delayed past its end finds it gone).
bool Known(std::uint64_t clip);
// Main thread, after the frame's events: the clips that stopped, whose windows end. A stopped clip is kept for one
// more frame, for its events still queued.
std::vector<std::uint64_t> Sweep();
// Load / new game.
void ResetOnLoad();
}

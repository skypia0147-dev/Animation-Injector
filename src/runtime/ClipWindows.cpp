#include "runtime/ClipWindows.h"
#include "runtime/AnnotationCommands.h"
#include "runtime/ClipWindowsJudge.h"
#include <mutex>

namespace AnimationInjector::ClipWindows {
namespace {
using ClipWindowsJudge::Own;
using ClipWindowsJudge::Role;
// The window of a window function, or -1; `on` tells which end it is.
int WindowOf(EventOp op, bool& on) {
    switch (op) {
    case EventOp::kIFrameOn: on = true; return 0;
    case EventOp::kIFrameOff: on = false; return 0;
    case EventOp::kArmorOn: on = true; return 1;
    case EventOp::kArmorOff: on = false; return 1;
    case EventOp::kRotateOn: on = true; return 2;
    case EventOp::kRotateOff: on = false; return 2;
    case EventOp::kTimeOn: on = true; return 3;
    case EventOp::kTimeOff: on = false; return 3;
    default: return -1;
    }
}
struct Named {
    std::string name;
    Role role;
};
// An AI. command's part in the windows: its window's on (without a span: one with a span ends by itself) or off.
const Role* CommandRole(std::string_view payload) {
    static const auto roles = [] {
        std::array<Role, 2 * ClipWindowsJudge::kWindows> r{};
        for (std::size_t w = 0; w < ClipWindowsJudge::kWindows; ++w) {
            r[2 * w] = {ClipWindowsJudge::kCommand, static_cast<std::uint8_t>(1u << w), 0};
            r[2 * w + 1] = {ClipWindowsJudge::kCommand, 0, static_cast<std::uint8_t>(1u << w)};
        }
        return r;
    }();
    const auto command = AnnotationCommands::Parse(payload);
    if (!command || command->span > 0.0f) return nullptr;
    bool on = false;
    const int w = WindowOf(command->op, on);
    return w < 0 ? nullptr : &roles[2 * static_cast<std::size_t>(w) + (on ? 0 : 1)];
}
bool IsCommand(const char* text) {
    return (text[0] == 'A' || text[0] == 'a') && (text[1] == 'I' || text[1] == 'i') && text[2] == '.';
}
// Built once and read-only afterwards; `ready` publishes it to the clip hooks.
std::vector<Named> named;
std::atomic_bool ready{false};

struct Record {
    std::uint64_t id = 0;
    const void* clip = nullptr;  // identity only, never read
    RE::ActorHandle actor;
    // By part (ClipWindowsJudge::PairPart): the annotations as written, a paired clip's support's ("NPC"), its lead's
    // ("2_"); `part` is the clip's, 0 until a synchronized clip tells its prefix.
    std::array<std::vector<Own>, 3> own;
    int part = 0;
    bool stopped = false;
    int sweeps = 0;  // sweeps seen since it stopped
};
std::mutex lock;
std::vector<Record> records;
std::atomic<std::size_t> kept{0};  // records.size(), read without the lock
std::uint64_t next = 0;
}

void Build(const std::vector<Entry>& entries) {
    ready.store(false, std::memory_order_release);
    named.clear();
    for (const auto& e : entries) {
        Role role{e.event};
        for (const auto op : e.ops) {
            bool on = false;
            if (const int w = WindowOf(op, on); w >= 0) (on ? role.on : role.off) |= static_cast<std::uint8_t>(1u << w);
        }
        if (role.on || role.off) named.push_back({e.name, role});
    }
    ready.store(true, std::memory_order_release);
}

// The role of an annotation's text: an AI. window command's or a registered window event's; null for any other.
const Role* RoleOf(std::string_view text) {
    if (text.size() > 3 && IsCommand(text.data())) return CommandRole(text.substr(3));
    for (const auto& n : named)
        if (text.size() == n.name.size() && _strnicmp(text.data(), n.name.c_str(), text.size()) == 0) return &n.role;
    return nullptr;
}

void Activated(const void* clip, const RE::hkaAnimation* animation, RE::Actor* actor) {
    if (!actor || !animation || !ready.load(std::memory_order_acquire)) return;
    // By part: as written; and for a paired clip's two actors, their own prefixed ones and the unprefixed ones.
    std::array<std::vector<ClipWindowsJudge::Note>, 3> notes;
    for (const auto& track : animation->annotationTracks)
        for (const auto& note : track.annotations) {
            const char* text = note.text.c_str();
            if (!text || !*text) continue;
            const std::string_view written = text;
            const auto* asWritten = RoleOf(written);
            if (asWritten) notes[0].push_back({note.time, asWritten});
            auto body = written;
            const int part = ClipWindowsJudge::PairPart(body);
            const auto* role = part ? RoleOf(body) : asWritten;
            if (!role) continue;
            if (part != 2) notes[1].push_back({note.time, role});
            if (part != 1) notes[2].push_back({note.time, role});
        }
    Record record{0, clip, actor->GetHandle()};
    bool owns = false;
    for (std::size_t p = 0; p < notes.size(); ++p) {
        record.own[p] = ClipWindowsJudge::Judge(notes[p]);
        owns |= !record.own[p].empty();
    }
    if (!owns) return;
    std::lock_guard guard(lock);
    record.id = ++next;
    records.push_back(std::move(record));
    kept.store(records.size());
}

void Synchronized(const void* clip, const char* prefix) {
    if (!kept.load()) return;
    const int part = ClipWindowsJudge::PartOfPrefix(prefix);
    std::lock_guard guard(lock);
    for (auto it = records.rbegin(); it != records.rend(); ++it)
        if (it->clip == clip && !it->stopped) {
            it->part = part;
            return;
        }
}

void Deactivated(const void* clip) {
    if (!kept.load()) return;
    std::lock_guard guard(lock);
    for (auto& r : records)
        if (r.clip == clip && !r.stopped) r.stopped = true;
}

std::uint64_t Find(RE::ActorHandle actor, std::size_t event, EventOp on) {
    bool isOn = false;
    const int w = WindowOf(on, isOn);
    if (w < 0 || !isOn || !kept.load()) return 0;
    const Own want{event, static_cast<std::size_t>(w)};
    std::lock_guard guard(lock);
    std::uint64_t found = 0;
    for (const auto& r : records)
        if (const auto& own = r.own[r.part]; r.actor == actor && std::find(own.begin(), own.end(), want) != own.end()) {
            found = r.id;
            if (!r.stopped) break;  // a playing clip before one that just stopped
        }
    return found;
}

bool Known(std::uint64_t clip) {
    std::lock_guard guard(lock);
    return std::any_of(records.begin(), records.end(), [&](const Record& r) { return r.id == clip; });
}

std::vector<std::uint64_t> Sweep() {
    std::vector<std::uint64_t> ended;
    if (!kept.load()) return ended;
    std::lock_guard guard(lock);
    for (auto& r : records) {
        // An actor unloaded with its graph may never stop its clips.
        if (!r.stopped && !r.actor.get()) r.stopped = true;
        if (!r.stopped) continue;
        ended.push_back(r.id);
        ++r.sweeps;
    }
    std::erase_if(records, [](const Record& r) { return r.sweeps >= 2; });
    kept.store(records.size());
    return ended;
}

void ResetOnLoad() {
    std::lock_guard guard(lock);
    records.clear();
    kept.store(0);
}
}

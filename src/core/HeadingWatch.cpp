#include "PCH.h"
#include "core/HeadingWatch.h"

#include <hde64.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace AnimationInjector::HeadingWatch {
namespace {
using SetHeadingFn = void (*)(RE::Actor*, float);
using SetAngleFn = void (*)(RE::TESObjectREFR*, const RE::NiPoint3&);
SetHeadingFn origSetHeading = nullptr;
SetAngleFn origSetAngle = nullptr;

constexpr std::size_t kMaxCallers = 8;  // distinct callers kept per watched start
constexpr unsigned kMaxLines = 4;       // first-sighting lines per watched start

struct Caller {
    std::string who;     // the module that called the setter; "engine" (with the nearest mod in its stack) otherwise
    std::string via;     // the setter's immediate caller
    const char* setter;  // SetHeading or SetAngle
    unsigned count = 0;
};
struct Watched {
    std::uintptr_t scene = 0;
    std::uint32_t actor = 0;
    std::vector<Caller> callers;
    unsigned lines = 0;
    unsigned dropped = 0;  // calls past kMaxCallers distinct callers
};
std::mutex mutex;
std::vector<Watched> watched;
std::atomic<unsigned> watchedCount{0};
thread_local unsigned ownTurns = 0;

HMODULE ModuleOf(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module))
        return nullptr;
    return module;
}
// module+offset; the engine's offsets map to their Address Library ids offline (tools/engine_caller.py).
std::string Describe(const void* address) {
    const HMODULE module = ModuleOf(address);
    char path[MAX_PATH]{};
    if (!module || !GetModuleFileNameA(module, path, MAX_PATH))
        return fmt::format("?{:X}", reinterpret_cast<std::uintptr_t>(address));
    const char* name = std::strrchr(path, '\\');
    return fmt::format("{}+{:X}", name ? name + 1 : path,
        reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
}

// Windows' own frames (a worker thread's start) never turn anyone.
bool SystemModule(HMODULE module) {
    static const HMODULE system[] = {
        GetModuleHandleA("kernel32.dll"), GetModuleHandleA("KERNELBASE.dll"), GetModuleHandleA("ntdll.dll")};
    return std::ranges::find(system, module) != std::end(system);
}

// Who called a setter that turns a watched actor. The immediate caller decides: a module calling it is the one
// turning (11-10 test: a mod that hooks the frame update sat above Animation Injector's own call and took the blame
// when the stack was searched for the first mod). An engine call names the nearest mod in its stack only as a hint.
__declspec(noinline) void Record(std::uint32_t actor, float yaw, const char* setter) {
    void* frames[12]{};
    const auto count = RtlCaptureStackBackTrace(2, 12, frames, nullptr);  // skip this and the setter hook
    if (!count) return;
    static const HMODULE game = GetModuleHandleA(nullptr);
    static const HMODULE self = ModuleOf(reinterpret_cast<const void*>(&Record));
    std::lock_guard lock(mutex);
    const auto watch = std::ranges::find(watched, actor, &Watched::actor);
    if (watch == watched.end()) return;
    const std::string via = Describe(frames[0]);
    std::string who;
    if (const HMODULE first = ModuleOf(frames[0]); first && first != game && !SystemModule(first))
        who = via;
    else {
        for (USHORT i = 1; i < count && who.empty(); ++i)
            if (const HMODULE module = ModuleOf(frames[i]);
                module && module != game && module != self && !SystemModule(module))
                who = "engine, mod in its stack " + Describe(frames[i]);
        if (who.empty()) who = "engine";
    }
    auto caller = std::ranges::find_if(
        watch->callers, [&](const Caller& c) { return c.who == who && c.via == via && c.setter == setter; });
    if (caller == watch->callers.end()) {
        if (watch->callers.size() >= kMaxCallers) {
            ++watch->dropped;
            return;
        }
        watch->callers.push_back({who, via, setter, 0});
        caller = std::prev(watch->callers.end());
        if (watch->lines++ < kMaxLines) {
            std::string stack;
            for (USHORT i = 0; i < count && i < 6; ++i) stack += (i ? " < " : "") + Describe(frames[i]);
            logger::info("[HeadingWatch] scene={:X} actor={:08X} turned by {} through {} yaw={:.4f} stack: {}",
                watch->scene, actor, who, setter, yaw, stack);
        }
    }
    ++caller->count;
}

// A call that leaves the heading where it is (the engine re-sets it every frame) turns no one.
bool Turns(RE::TESObjectREFR* ref, float yaw) {
    constexpr float tau = 6.28318531f;
    float off = std::fmod(yaw - ref->GetAngleZ(), tau);
    if (off > tau / 2) off -= tau;
    if (off < -tau / 2) off += tau;
    return std::fabs(off) > 0.001f;
}

void SetHeadingHook(RE::Actor* actor, float angle) {
    if (watchedCount.load(std::memory_order_relaxed) && !ownTurns && actor && Turns(actor, angle))
        Record(actor->GetFormID(), angle, "SetHeading");
    origSetHeading(actor, angle);
}
void SetAngleHook(RE::TESObjectREFR* ref, const RE::NiPoint3& angle) {
    if (watchedCount.load(std::memory_order_relaxed) && !ownTurns && ref && Turns(ref, angle.z))
        Record(ref->GetFormID(), angle.z, "SetAngle");
    origSetAngle(ref, angle);
}

// Entry hook: the first instructions (at least 5 bytes) move to a trampoline stub that jumps back, and the entry
// jumps to the hook. Refused when any of them is relative (a branch, RIP-relative data: another mod's hook already
// there, or a different build), so a refusal leaves the setter untouched.
template<class Fn>
bool HookEntry(REL::RelocationID id, void* hook, Fn& original, const char* name) {
    const auto entry = id.address();
    std::size_t length = 0;
    while (length < 5) {
        hde64s ins{};
        const auto size = hde64_disasm(reinterpret_cast<const void*>(entry + length), &ins);
        const bool ripRelative = (ins.flags & F_MODRM) && ins.modrm_mod == 0 && ins.modrm_rm == 5;
        const bool flow = ins.opcode == 0xC3 || ins.opcode == 0xCC || ins.opcode == 0xE8 || ins.opcode == 0xE9 ||
                          ins.opcode == 0xEB || (ins.opcode == 0xFF && (ins.modrm_reg == 2 || ins.modrm_reg == 4));
        if (!size || (ins.flags & F_ERROR) || (ins.flags & F_RELATIVE) || ripRelative || flow) {
            std::uint8_t bytes[8]{};
            std::memcpy(bytes, reinterpret_cast<const void*>(entry), sizeof(bytes));
            std::string text;
            for (const auto byte : bytes) text += fmt::format("{:02X} ", byte);
            logger::warn("[HeadingWatch] {} not watched: its entry is not a plain prologue ({})", name, text);
            return false;
        }
        length += size;
    }
    auto& trampoline = SKSE::GetTrampoline();
    auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(length + 14));
    std::memcpy(stub, reinterpret_cast<const void*>(entry), length);
    stub[length] = 0xFF;  // jmp [rip+0] back to the rest of the setter
    stub[length + 1] = 0x25;
    const std::uint32_t zero = 0;
    const std::uint64_t back = entry + length;
    std::memcpy(stub + length + 2, &zero, sizeof(zero));
    std::memcpy(stub + length + 6, &back, sizeof(back));
    original = reinterpret_cast<Fn>(stub);
    trampoline.write_branch<5>(entry, hook);
    return true;
}
}  // namespace

void Install() {
    if (REL::Module::IsVR()) return;
    const bool heading = HookEntry(
        RELOCATION_ID(36248, 37230), reinterpret_cast<void*>(&SetHeadingHook), origSetHeading, "Actor::SetHeading");
    const bool angle = HookEntry(
        RELOCATION_ID(19359, 19786), reinterpret_cast<void*>(&SetAngleHook), origSetAngle, "TESObjectREFR::SetAngle");
    logger::info("[HeadingWatch] installed SetHeading={} SetAngle={}", heading, angle);
}

void Watch(std::uintptr_t scene, std::uint32_t actorFormID) {
    if (!origSetHeading && !origSetAngle) return;
    std::lock_guard lock(mutex);
    std::erase_if(watched, [&](const Watched& w) { return w.scene == scene || w.actor == actorFormID; });
    watched.push_back({scene, actorFormID});
    watchedCount.store(static_cast<unsigned>(watched.size()), std::memory_order_relaxed);
}

std::string Stop(std::uintptr_t scene) {
    std::lock_guard lock(mutex);
    const auto watch = std::ranges::find(watched, scene, &Watched::scene);
    if (watch == watched.end()) return {};
    std::string summary;
    for (const auto& caller : watch->callers)
        summary += fmt::format("{}{} x{} ({}{}{})", summary.empty() ? "" : ", ", caller.who, caller.count,
            caller.setter, caller.who == caller.via ? "" : ", caller ", caller.who == caller.via ? "" : caller.via);
    if (watch->dropped) summary += fmt::format(", {} more calls from other callers", watch->dropped);
    watched.erase(watch);
    watchedCount.store(static_cast<unsigned>(watched.size()), std::memory_order_relaxed);
    return summary;
}

OwnTurn::OwnTurn() { ++ownTurns; }
OwnTurn::~OwnTurn() { --ownTurns; }

}  // namespace AnimationInjector::HeadingWatch

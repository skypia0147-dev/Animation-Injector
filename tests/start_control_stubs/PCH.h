#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
namespace RE {
struct NiPoint2 {
    float x = 0, y = 0;
};
struct NiAVObject {};
struct BSFixedString {
    explicit BSFixedString(const char*) {}
};
template<class T>
struct NiPointer {
    T* ptr = nullptr;
    void reset(T* p = nullptr) { ptr = p; }
    T* get() const { return ptr; }
};
class Actor;
inline std::array<Actor*, 8> registry{};
inline unsigned handleReads = 0;
struct ActorHandle {
    unsigned id = 0;
    Actor* get() const {
        ++handleReads;
        return registry[id];
    }
    bool operator==(const ActorHandle&) const = default;
};
class Actor {
public:
    unsigned id = 0;
    bool loaded = true, dead = false, ragdoll = false, player = false, rotation = true, writeFails = false;
    NiAVObject object{}, replacement{};
    NiAVObject* scene = &object;
    explicit Actor(unsigned value) : id(value) { registry[id] = this; }
    bool Is3DLoaded() const { return loaded; }
    bool IsDead() const { return dead; }
    bool IsInRagdollState() const { return ragdoll; }
    bool IsPlayerRef() const { return player; }
    ActorHandle GetHandle() const { return {id}; }
    unsigned GetFormID() const { return id; }
    NiAVObject* Get3D(bool) const { return scene; }
    bool GetGraphVariableBool(const BSFixedString&, bool& out) {
        out = rotation;
        return true;
    }
    bool SetGraphVariableBool(const BSFixedString&, bool value) {
        if (writeFails) return false;
        rotation = value;
        return true;
    }
};
}
inline unsigned long currentThread = 99;
inline bool gameTaskActive = false;
namespace SKSE {
using PluginHandle = std::uint32_t;
inline PluginHandle GetPluginHandle() { return 7; }
struct TaskInterface {
    std::vector<std::function<void()>> tasks;
    void AddTask(std::function<void()> task) { tasks.push_back(std::move(task)); }
    void Drain(unsigned long thread = 15096) {
        const auto before = currentThread;
        currentThread = thread;
        gameTaskActive = true;
        auto pending = std::move(tasks);
        tasks.clear();
        for (auto& task : pending) task();
        currentThread = before;
        gameTaskActive = false;
    }
};
inline TaskInterface taskInterface;
inline TaskInterface* GetTaskInterface() { return &taskInterface; }
}
namespace logger {
template<class... T>
void info(const char*, T&&...) {}
template<class... T>
void warn(const char*, T&&...) {}
template<class... T>
void debug(const char*, T&&...) {}
}
void* GetModuleHandleW(const wchar_t*);
void* GetProcAddress(void*, const char*);
inline unsigned long GetCurrentThreadId() { return currentThread; }

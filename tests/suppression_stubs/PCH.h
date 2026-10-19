#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <cstddef>
#include <unordered_set>
namespace logger {
template<class... T>
void debug(const char*, T&&...) {}
}
namespace RE {
// Interned like the engine's string pool: equal text shares one pointer, and equality is identity.
struct BSFixedString {
    const char* text = "";
    BSFixedString() = default;
    BSFixedString(const std::string& v) : text(Intern(v)) {}
    BSFixedString(const char* v) : text(Intern(v ? v : "")) {}
    const char* c_str() const { return text; }
    bool empty() const { return !*text; }
    bool operator==(const BSFixedString& other) const { return text == other.text; }
    static const char* Intern(const std::string& v) {
        static std::unordered_set<std::string> pool;
        return pool.insert(v).first->c_str();
    }
};
struct Actor {
    bool loaded = true, dead = false, ragdoll = false;
    bool Is3DLoaded() const { return loaded; }
    bool IsDead() const { return dead; }
    bool IsInRagdollState() const { return ragdoll; }
};
struct PlayerCharacter : Actor {
    static PlayerCharacter* GetSingleton() {
        static PlayerCharacter p;
        return &p;
    }
};
struct Console {
    static constexpr const char* MENU_NAME = "Console";
};
struct UI {
    bool paused = false, console = false;
    static UI* GetSingleton() {
        static UI u;
        return &u;
    }
    bool GameIsPaused() const { return paused; }
    bool IsMenuOpen(const char*) const { return console; }
};
}

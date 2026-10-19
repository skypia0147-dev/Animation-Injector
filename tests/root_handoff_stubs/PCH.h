#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <utility>
namespace logger {
template<class... T>
void info(const char*, T&&...) {}
template<class... T>
void debug(const char*, T&&...) {}
}
namespace RE {
struct NiPoint3 {
    float x = 0, y = 0, z = 0;
    NiPoint3 operator+(const NiPoint3& b) const { return {x + b.x, y + b.y, z + b.z}; }
    NiPoint3 operator-(const NiPoint3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    NiPoint3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
struct Matrix {
    float yaw = 0;
    NiPoint3 operator*(const NiPoint3& p) const {
        return {std::cos(yaw) * p.x - std::sin(yaw) * p.y, std::sin(yaw) * p.x + std::cos(yaw) * p.y, p.z};
    }
};
struct Transform {
    NiPoint3 translate;
    Matrix rotate;
    float scale = 1;
};
struct NiAVObject {
    Transform local, world;
    NiAVObject* parent = nullptr;
};
struct ActorHandle {
    unsigned id = 0;
    bool operator==(const ActorHandle&) const = default;
};
struct hkVector4 {
    struct Quad {
        float m128_f32[4]{};
    } quad;
    hkVector4() = default;
    hkVector4(float x, float y, float z, float w) : quad{{x, y, z, w}} {}
};
struct Controller {
    hkVector4 position;
    int writes = 0;
    void SetPositionImpl(hkVector4 p, bool, bool) {
        position = p;
        ++writes;
    }
    void GetPositionImpl(hkVector4& out, bool) { out = position; }
};
struct bhkWorld {
    static float GetWorldScale() { return 1; }
    static float GetWorldScaleInverse() { return 1; }
};
struct ActorState {
    int knock = 0;
    int GetKnockState() const { return knock; }
};
struct TESObjectREFR {
    NiPoint3 position;
    int writes = 0;
    void SetPosition(const NiPoint3& p) {
        position = p;
        ++writes;
    }
};
struct Actor : TESObjectREFR {
    unsigned id = 1;
    bool loaded = true, dead = false, ragdoll = false, air = false;
    float yaw = 0;
    Controller controller;
    ActorState state;
    bool Is3DLoaded() const { return loaded; }
    bool IsDead() const { return dead; }
    bool IsInRagdollState() const { return ragdoll; }
    bool IsInMidair() const { return air; }
    ActorState* AsActorState() { return &state; }
    Controller* GetCharController() { return &controller; }
    NiPoint3 GetPosition() const { return position; }
    ActorHandle GetHandle() const { return {id}; }
    float GetAngleZ() const { return yaw; }
};
}

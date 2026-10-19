#include "PCH.h"
#include "core/RagdollTrace.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace AnimationInjector::RagdollTrace {
namespace {
// Shortest text that reads back as the same float; JSON has no NaN or infinity.
void Number(std::string& out, float v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, result.ptr);
}
void Text(std::string& out, std::string_view v) {
    out += '"';
    for (const char c : v) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else
                out += c;
        }
    }
    out += '"';
}
template<std::size_t N>
void Array(std::string& out, const std::array<float, N>& v) {
    out += '[';
    for (std::size_t i = 0; i < N; ++i) {
        if (i) out += ',';
        Number(out, v[i]);
    }
    out += ']';
}
void Key(std::string& out, std::string_view key) {
    if (out.back() != '{') out += ',';
    Text(out, key);
    out += ':';
}
void Field(std::string& out, std::string_view key, float v) {
    Key(out, key);
    Number(out, v);
}
void Field(std::string& out, std::string_view key, std::string_view v) {
    Key(out, key);
    Text(out, v);
}
void Field(std::string& out, std::string_view key, long long v) {
    Key(out, key);
    out += std::to_string(v);
}
template<std::size_t N>
void Field(std::string& out, std::string_view key, const std::array<float, N>& v) {
    Key(out, key);
    Array(out, v);
}
void WritePose(std::string& out, const Pose& p) {
    for (const float v : p.position) {
        out += ',';
        Number(out, v);
    }
    for (const float v : p.rotation) {
        out += ',';
        Number(out, v);
    }
}
std::string HeaderLine(const Header& h, std::size_t steps, std::size_t dropped) {
    std::string out = "{";
    Field(out, "format", "AnimationInjector.RagdollTrace");
    Field(out, "version", 1LL);
    Field(out, "units", "length: game units, angle: radians, time: seconds, mass: kg");
    Field(out, "rig", h.rig);
    Field(out, "profile", h.profile);
    Field(out, "anchor", h.anchor);
    Field(out, "actorScale", h.actorScale);
    Field(out, "follow", h.follow);
    Field(out, "gravity", h.gravity);
    Key(out, "solver");
    out += '{';
    Field(out, "tau", h.solverTau);
    Field(out, "damping", h.solverDamping);
    Field(out, "steps", static_cast<long long>(h.solverSteps));
    Field(out, "microSteps", static_cast<long long>(h.solverMicroSteps));
    out += '}';
    Field(out, "steps", static_cast<long long>(steps));
    Field(out, "dropped", static_cast<long long>(dropped));
    Key(out, "bodies");
    out += '[';
    for (std::size_t i = 0; i < h.bodies.size(); ++i) {
        const auto& b = h.bodies[i];
        out += i ? ",{" : "{";
        Field(out, "bone", b.bone);
        Key(out, "simulated");
        out += b.simulated ? "true" : "false";
        Field(out, "mass", b.mass);
        Field(out, "inertia", b.inertia);
        Field(out, "centerOfMass", b.centerOfMass);
        Field(out, "linearDamping", b.linearDamping);
        Field(out, "angularDamping", b.angularDamping);
        Field(out, "gravityFactor", b.gravityFactor);
        Field(out, "timeFactor", b.timeFactor);
        Field(out, "maxLinearVelocityCode", static_cast<long long>(b.maxLinearVelocityCode));
        Field(out, "maxAngularVelocityCode", static_cast<long long>(b.maxAngularVelocityCode));
        out += '}';
    }
    out += ']';
    Key(out, "joints");
    out += '[';
    for (std::size_t i = 0; i < h.joints.size(); ++i) {
        const auto& j = h.joints[i];
        out += i ? ",{" : "{";
        Field(out, "a", j.bodyA);
        Field(out, "b", j.bodyB);
        Field(out, "friction", j.friction);
        Field(out, "cone", j.cone);
        Field(out, "plane", j.plane);
        Field(out, "twist", j.twist);
        out += '}';
    }
    out += ']';
    Key(out, "widened");
    out += '[';
    for (std::size_t i = 0; i < h.widened.size(); ++i) {
        const auto& w = h.widened[i];
        out += i ? ",{" : "{";
        Field(out, "a", w.bodyA);
        Field(out, "b", w.bodyB);
        Field(out, "limit", w.limit);
        Field(out, "from", w.from);
        Field(out, "to", w.to);
        out += '}';
    }
    out += ']';
    Field(out, "step",
        "[time, delta, attachment position(3) rotation wxyz(4), then per body: position(3) rotation wxyz(4) "
        "linear velocity(3) angular velocity(3)]");
    out += '}';
    return out;
}
}

void Recorder::Begin(Header header, std::size_t capacity) {
    Clear();
    header_ = std::move(header);
    capacity_ = capacity;
    steps_.reserve(capacity);
    states_.reserve(capacity * header_.bodies.size());
    active_ = capacity > 0 && !header_.bodies.empty();
}
void Recorder::Add(float delta, const Pose& attachment, std::span<const BodyState> bodies) noexcept {
    if (!active_) return;
    // Both vectors were reserved at Begin; staying inside that keeps this allocation-free.
    if (steps_.size() >= capacity_ || bodies.size() != header_.bodies.size()) {
        ++dropped_;
        return;
    }
    time_ += delta;
    steps_.push_back({time_, delta, attachment});
    states_.insert(states_.end(), bodies.begin(), bodies.end());
}
bool Recorder::Write(const std::filesystem::path& file) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::string line = HeaderLine(header_, steps_.size(), dropped_);
    line += '\n';
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    const auto count = header_.bodies.size();
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        const auto& s = steps_[i];
        line = "[";
        Number(line, s.time);
        line += ',';
        Number(line, s.delta);
        WritePose(line, s.attachment);
        for (std::size_t k = 0; k < count; ++k) {
            const auto& b = states_[i * count + k];
            WritePose(line, b.pose);
            for (const float v : b.linear) {
                line += ',';
                Number(line, v);
            }
            for (const float v : b.angular) {
                line += ',';
                Number(line, v);
            }
        }
        line += "]\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    return static_cast<bool>(out.flush());
}
void Recorder::Clear() noexcept {
    active_ = false;
    header_ = {};
    std::vector<Step>().swap(steps_);  // give the memory back; a trace can hold megabytes
    std::vector<BodyState>().swap(states_);
    capacity_ = 0;
    dropped_ = 0;
    time_ = 0;
}

std::filesystem::path NextFile(std::string_view rig, std::size_t keep) {
    const auto logs = SKSE::log::log_directory();
    if (!logs) return {};
    const auto folder = *logs / "AnimationInjector" / "RagdollTrace";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    // Oldest first; leave room for the file about to be written.
    std::vector<std::filesystem::directory_entry> traces;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.is_regular_file(error) && entry.path().extension() == ".jsonl") traces.push_back(entry);
    std::sort(traces.begin(), traces.end(), [](const auto& a, const auto& b) {
        std::error_code e;
        return a.last_write_time(e) < b.last_write_time(e);
    });
    for (std::size_t i = 0; keep && i < traces.size() && traces.size() - i >= keep; ++i)
        std::filesystem::remove(traces[i].path(), error);
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_s(&local, &seconds);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    return folder / fmt::format("{}-{}-{:03}.jsonl", rig.empty() ? "unknown" : rig, stamp, millis);
}
}

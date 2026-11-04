#include "PCH.h"
#include "runtime/RagdollProfile.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <pugixml.hpp>

namespace AnimationInjector::RagdollProfile {
namespace {
constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;
// Paths as UTF-8 for logs: string() converts through the code page and throws on unmapped names.
std::string Text(const std::filesystem::path& file) {
    const auto text = file.u8string();
    return {text.begin(), text.end()};
}
bool Same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}
std::optional<float> Number(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    float value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
// Collects the first error; every reader returns false once one is set.
struct Reader {
    std::string& error;
    bool Fail(std::string_view where, std::string_view why) {
        if (error.empty()) error = fmt::format("{}: {}", where, why);
        return false;
    }
    bool Scalar(pugi::xml_node node, const char* name, float low, float high, std::optional<float>& out) {
        const auto attribute = node.attribute(name);
        if (!attribute) return true;
        const auto value = Number(attribute.value());
        if (!value || *value < low || *value > high)
            return Fail(fmt::format("<{} {}=\"{}\">", node.name(), name, attribute.value()),
                fmt::format("expected a number in [{}, {}]", low, high));
        out = value;
        return true;
    }
    // "minimum maximum" in degrees, stored as radians.
    bool Range(pugi::xml_node node, const char* name, float limit, std::optional<std::pair<float, float>>& out) {
        const auto attribute = node.attribute(name);
        if (!attribute) return true;
        const std::string_view text = attribute.value();
        const auto split = text.find_first_of(" \t,");
        const auto low = split == text.npos ? std::nullopt : Number(text.substr(0, split));
        const auto high = split == text.npos ? std::nullopt : Number(text.substr(split + 1));
        if (!low || !high || *low > *high || *low < -limit || *high > limit)
            return Fail(fmt::format("<{} {}=\"{}\">", node.name(), name, text),
                fmt::format("expected \"minimum maximum\" degrees within [-{}, {}]", limit, limit));
        out = std::pair{*low * kRadians, *high * kRadians};
        return true;
    }
    bool Known(pugi::xml_node node, std::initializer_list<std::string_view> names) {
        for (const auto attribute : node.attributes())
            if (std::none_of(names.begin(), names.end(), [&](auto n) { return n == attribute.name(); }))
                return Fail(fmt::format("<{}>", node.name()), fmt::format("unknown attribute '{}'", attribute.name()));
        return true;
    }
};
}
std::optional<Profile> Parse(const std::filesystem::path& file, std::string_view rig, std::string& error) {
    error.clear();
    pugi::xml_document document;
    if (const auto loaded = document.load_file(file.c_str()); !loaded) {
        error = fmt::format("XML error at offset {}: {}", loaded.offset, loaded.description());
        return std::nullopt;
    }
    Reader read{error};
    const auto root = document.document_element();
    if (std::string_view(root.name()) != "Ragdoll") {
        read.Fail("root", "expected <Ragdoll>");
        return std::nullopt;
    }
    if (!read.Known(root, {"rig", "version"})) return std::nullopt;
    if (std::string_view(root.attribute("version").value()) != "1") {
        read.Fail("<Ragdoll version>", "expected version=\"1\"");
        return std::nullopt;
    }
    const std::string_view name = root.attribute("rig").value();
    if (name.empty() || (!rig.empty() && !Same(name, rig))) {
        read.Fail("<Ragdoll rig>", fmt::format("'{}' does not match rig '{}'", name, rig));
        return std::nullopt;
    }
    Profile profile;
    profile.source = Text(file);
    bool motion = false;
    for (const auto node : root.children()) {
        if (node.type() != pugi::node_element) continue;
        const std::string_view tag = node.name();
        if (tag == "Motion") {
            if (motion) {
                read.Fail("<Motion>", "only one is allowed");
                return std::nullopt;
            }
            motion = true;
            std::optional<float> follow, gravity, drag, spin, inertia, friction;
            if (!read.Known(node, {"follow", "gravity", "drag", "spin", "inertia", "friction"}) ||
                !read.Scalar(node, "follow", 0, 1, follow) || !read.Scalar(node, "gravity", 1, 4, gravity) ||
                !read.Scalar(node, "drag", 0, 10, drag) || !read.Scalar(node, "spin", 0, 10, spin) ||
                !read.Scalar(node, "inertia", 1, 10, inertia) || !read.Scalar(node, "friction", 0, 100, friction))
                return std::nullopt;
            auto& m = profile.motion;
            m.follow = follow.value_or(m.follow);
            m.gravity = gravity.value_or(m.gravity);
            m.linearDamping = drag.value_or(m.linearDamping);
            m.angularDamping = spin.value_or(m.angularDamping);
            m.inertia = inertia.value_or(m.inertia);
            m.jointFriction = friction.value_or(m.jointFriction);
        } else if (tag == "Body") {
            CarryPartialRagdoll::BodyTuning body;
            body.bone = node.attribute("bone").value();
            std::optional<float> cone;
            if (!read.Known(node, {"bone", "simulate", "drag", "spin", "gravity", "inertia", "friction", "cone",
                                      "plane", "twist", "swingFrom"}) ||
                !read.Scalar(node, "drag", 0, 10, body.linearDamping) ||
                !read.Scalar(node, "spin", 0, 10, body.angularDamping) ||
                !read.Scalar(node, "gravity", 1, 4, body.gravity) ||
                !read.Scalar(node, "inertia", 1, 10, body.inertia) ||
                !read.Scalar(node, "friction", 0, 100, body.friction) || !read.Scalar(node, "cone", 0, 180, cone) ||
                !read.Range(node, "plane", 90, body.plane) || !read.Range(node, "twist", 180, body.twist))
                return std::nullopt;
            if (body.bone.empty()) {
                read.Fail("<Body>", "bone is required");
                return std::nullopt;
            }
            if (std::any_of(profile.bodies.begin(), profile.bodies.end(),
                    [&](const auto& other) { return Same(other.bone, body.bone); })) {
                read.Fail(fmt::format("<Body bone=\"{}\">", body.bone), "the bone is listed twice");
                return std::nullopt;
            }
            if (const auto simulate = node.attribute("simulate")) {
                const std::string_view value = simulate.value();
                if (value != "true" && value != "false") {
                    read.Fail(fmt::format("<Body bone=\"{}\" simulate>", body.bone), "expected true or false");
                    return std::nullopt;
                }
                body.simulate = value == "true";
            }
            if (cone) body.cone = *cone * kRadians;
            if (const auto swing = node.attribute("swingFrom")) {
                body.swingFrom = swing.value();
                if (body.swingFrom.empty()) {
                    read.Fail(fmt::format("<Body bone=\"{}\" swingFrom>", body.bone), "empty bone name");
                    return std::nullopt;
                }
            }
            profile.bodies.push_back(std::move(body));
        } else {
            read.Fail(fmt::format("<{}>", tag), "unknown element");
            return std::nullopt;
        }
    }
    return profile;
}
std::filesystem::path ActionProfilePath(const std::filesystem::path& actionXml, std::string_view liftId) {
    if (liftId.empty() || liftId == "." || liftId == ".." || liftId.find_first_of("/\\:") != liftId.npos) return {};
    auto file = actionXml.parent_path() / kFolder;
    file /= std::filesystem::path(std::u8string(liftId.begin(), liftId.end()));  // declarations are UTF-8
    file += ".xml";
    return file;
}
Profile BuiltIn(PairedRig::Kind) {
    // Vanilla for every rig (user decision 11-04: "a default must not be needed, and if there is one it is vanilla"):
    // the game's own ragdoll values. Damping is applied as at least this (0 keeps the native value), gravity 1 adds
    // no extra gravity, inertia 1 keeps it, friction < 0 keeps the native joints, no follow share, every body
    // simulates. A lift's own values belong in its file beside the action XML.
    Profile profile;
    profile.source = "built-in (vanilla)";
    auto& m = profile.motion;
    m.follow = 0;
    m.gravity = 1;
    m.linearDamping = 0;
    m.angularDamping = 0;
    m.inertia = 1;
    m.jointFriction = -1;
    return profile;
}
Profile Load(PairedRig::Kind rig, const std::filesystem::path& actionXml, std::string_view liftId) {
    const std::string name = PairedRig::Name(rig);
    const auto log = [&](const Profile& profile) {
        const auto& m = profile.motion;
        logger::info(
            "[RagdollProfile] rig '{}' from {}: follow={} gravity={} drag={} spin={} inertia={} friction={} bodies={}",
            name, profile.source, m.follow, m.gravity, m.linearDamping, m.angularDamping, m.inertia, m.jointFriction,
            profile.bodies.size());
    };
    std::error_code ec;
    std::string error;
    if (const auto file = ActionProfilePath(actionXml, liftId); !file.empty()) {
        if (std::filesystem::is_regular_file(file, ec)) {
            if (auto profile = Parse(file, name, error)) {
                profile->from = Source::kAction;
                log(*profile);
                return std::move(*profile);
            }
            logger::warn("[RagdollProfile] {} rejected: {}; built-in values", Text(file), error);
        }
    }
    auto profile = BuiltIn(rig);
    log(profile);
    return profile;
}
}

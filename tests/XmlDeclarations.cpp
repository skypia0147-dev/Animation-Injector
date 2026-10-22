#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/RagdollProfile.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <spdlog/sinks/callback_sink.h>

using namespace AnimationInjector;
namespace {
struct Digest {
    std::ostringstream out;
    Digest() { out << std::setprecision(std::numeric_limits<float>::max_digits10); }
    template<class... T>
    void add(const T&... v) {
        ((out << v << '\x1f'), ...);
        out << '\n';
    }
    void strings(const std::vector<std::string>& v) {
        add(v.size());
        for (auto& s : v) add(s);
    }
    void recovery(const RecoverySpec& r) {
        add(r.enabled, r.event, r.fallback, r.move, r.sprint, r.jump, r.blend, r.recoverEvent);
    }
    void slot(const SlotWindow& w) {
        add(w.enabled, w.onEvent, w.offEvent, w.onAt, w.onFallback, w.offAt, w.offRel, w.offFallback);
    }
    void function(const EventFunctionSpec& f) {
        add(static_cast<int>(f.op), static_cast<int>(f.target), f.delay, f.plugin, f.form, f.releasePhysics, f.stamina,
            f.magicka, f.strength, f.duration, f.factor, static_cast<int>(f.arrow.mode), f.arrow.damage);
    }
};
void qteDigest(Digest& d, const QteSpec& q) {
    d.add(q.id, q.event, q.control, q.stopEvent, q.replaceActive, q.feedback, q.at, q.fallback, q.key, q.gamepad,
        q.inputOwner, q.eventSource, q.speed, q.successWindow, q.perfectWindow, q.scale, static_cast<int>(q.anchor),
        q.x, q.y, q.delay, q.reducedMotion, static_cast<int>(q.type), q.pressCount, q.perfectBefore, q.sequenceCount,
        static_cast<int>(q.channel), static_cast<int>(q.design));
    for (unsigned i = 0; i < q.sequenceCount; ++i) d.add(q.sequence[i].key, q.sequence[i].gamepad);
    for (const auto& r : q.results) d.add(r.event, r.recipient, r.soundPlugin, r.soundForm);
}
std::string snapshot(const ActionSpec& a) {
    Digest d;
    d.add(a.id, a.events.exit, a.safetyDuration, a.exitTriggerBeforeEnd, a.exitFallback, a.enterBlend, a.exitBlend);
    d.add(static_cast<int>(a.suppress.when));
    d.strings(a.suppress.events);
    d.add(a.rotationLocked);
    d.add(a.fireArrow.enabled, a.fireArrow.at, a.fireArrow.onRelease);
    for (const auto& q : a.qtes) qteDigest(d, q);
    // Node IDs label diagnostics; the branches define execution. A branch into a Stop node ends the flow
    // exactly like a missing one (ConditionFlow::Select), and the editor writes only the latter, so Stop
    // nodes are folded away. A flow of nothing but Stop nodes keeps one: a declared flow that never plays
    // differs from no flow (ActionRegistry starts directly). The nodes and their tests are numbered in the
    // order a walk from the entry meets them (a group's members, then pass, then fail), so the order they
    // are written in does not count (the editor writes a Play right after the branch that reaches it);
    // nodes the walk never meets follow in written order, so they still compare.
    const auto& nodes = a.flow.nodes;
    const int count = static_cast<int>(nodes.size());
    const auto live = [&](int id) { return id >= 0 && id < count && nodes[id].kind != ConditionFlow::Kind::kStop; };
    std::vector<int> order, number(nodes.size(), ConditionFlow::absent);
    std::vector<int> stack;
    if (live(a.flow.entry)) stack.push_back(a.flow.entry);
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        if (number[id] != ConditionFlow::absent) continue;
        number[id] = static_cast<int>(order.size());
        order.push_back(id);
        const auto& n = nodes[id];
        // Pushed in reverse, so members come out first, then pass, then fail.
        if (live(n.fail)) stack.push_back(n.fail);
        if (live(n.pass)) stack.push_back(n.pass);
        for (auto it = n.operands.rbegin(); it != n.operands.rend(); ++it)
            if (live(*it)) stack.push_back(*it);
    }
    for (int id = 0; id < count; ++id)
        if (live(id) && number[id] == ConditionFlow::absent) {
            number[id] = static_cast<int>(order.size());
            order.push_back(id);
        }
    const auto fold = [&](int id) { return live(id) ? number[id] : ConditionFlow::absent; };
    std::vector<int> tests, testNumber(a.flow.predicates.size(), ConditionFlow::absent);
    const auto useTest = [&](int p) {
        if (p < 0 || static_cast<std::size_t>(p) >= testNumber.size() || testNumber[p] != ConditionFlow::absent) return;
        testNumber[p] = static_cast<int>(tests.size());
        tests.push_back(p);
    };
    for (int id : order) useTest(nodes[id].predicate);
    for (int p = 0; p < static_cast<int>(testNumber.size()); ++p) useTest(p);
    const auto test = [&](int p) {
        return p < 0 || static_cast<std::size_t>(p) >= testNumber.size() ? p : testNumber[p];
    };
    const bool onlyStops = !nodes.empty() && order.empty();
    d.add(onlyStops ? 0 : fold(a.flow.entry));
    d.strings(a.flowActions);
    if (onlyStops)
        d.add(static_cast<int>(ConditionFlow::Kind::kStop), ConditionFlow::absent, ConditionFlow::absent,
            ConditionFlow::absent, ConditionFlow::absent);
    for (int id : order) {
        const auto& n = nodes[id];
        d.add(static_cast<int>(n.kind), test(n.predicate), fold(n.pass), fold(n.fail), n.action);
        for (int child : n.operands) d.add(fold(child));
    }
    for (int i : tests) {
        const auto& p = a.flow.predicates[i];
        d.add(static_cast<int>(p.test), static_cast<int>(p.subject), p.minimum, p.candidate, p.distance, p.direction,
            p.ground.rise, p.ground.drop, p.ground.slope, p.ground.radius, p.plugin, p.form, p.equals);
        d.add(p.leftHand, static_cast<int>(p.equippedType), static_cast<int>(p.attackState));
        d.add(p.option, p.compare, p.measure, p.value, p.upper);
    }
    for (auto& c : a.clips) d.add(c.tag, c.file, c.syncRole, c.clipName, c.enterEvent, c.airEnterEvent);
    for (auto& w : a.cancels) {
        d.add(w.id, w.openEvent, w.closeEvent, w.openAt, w.closeAt, w.openFallback, w.closeFallback, w.mode, w.blend);
        d.strings(w.vanillaMatches);
    }
    auto& t = a.trigger;
    d.add(t.key, t.gamepad, t.onKeyUp, t.onEvent, t.onEventPlayer, t.onEventNpc, t.eventCombatTarget);
    auto& c = a.condition;
    d.add(c.inAir, c.cooldown);
    auto& r = a.recovery;
    d.add(r.enabled, r.event, r.fallback, r.move, r.sprint, r.jump, r.blend, r.recoverEvent);
    d.slot(a.rotate);
    d.slot(a.iframe);
    d.slot(a.hyperArmor);
    auto& cost = a.cost;
    d.add(cost.enabled, cost.event, cost.at, cost.fallback, cost.stamina, cost.magicka);
    d.add(a.attack.enabled, a.attack.data);
    auto& release = a.carryRelease;
    d.add(release.enabled, release.event, release.at, release.fallback);
    d.add(a.carry.enabled);
    if (a.carry.enabled) {
        const auto& hold = a.carry.hold;
        d.add(a.carry.bone, a.carry.targetRoot, a.carry.putdownAction, a.carry.key, a.carry.gamepad,
            static_cast<int>(a.carry.mode), a.carry.part, a.carry.fixAt, static_cast<int>(a.carry.weapon), hold.pose,
            hold.blendIn, hold.blendOut);
        for (const auto& bone : hold.bones) d.add(bone.name, bone.weight);
    }
    auto& p = a.paired;
    d.add(p.enabled, static_cast<int>(p.targetRig), p.lossBlend, p.lossExitEvent, p.range, p.angle, p.getToMarkTime,
        p.markErrorThreshold, p.reorientSupport, p.applyMotionFromRoot);
    auto& f = a.pairFeatures;
    d.add(f.enabled, f.cameraReset, f.cameraRestoreOnEnd, f.cameraTimeout, f.interruptOnThirdPartyHit);
    for (auto& t : f.timed) {
        d.add(t.at);
        d.function(t.function);
    }
    for (auto& l : a.links)
        d.add(l.action, l.partnerAction, l.event, l.openEvent, l.closeEvent, l.key, l.gamepad, l.openAt, l.closeAt,
            l.buffered, l.closeRelativeToEnd, l.blend);
    if (a.view != ViewSpec{})  // absent before 0.12.225: older dumps stay equal
        d.add("view", static_cast<int>(a.view.view), static_cast<int>(a.view.firstPerson));
    return d.out.str();
}
// Everything the snapshot leaves out (names Finalize derives, input references, diagnostic labels):
// --dump compares whole declarations before and after a parser change.
std::string fullSnapshot(const ActionSpec& a) {
    Digest d;
    d.out << snapshot(a);
    d.add("source", a.source);
    d.add("inputs", a.inputs.size());
    for (const auto& i : a.inputs) d.add(i.id, i.key, i.gamepad);
    d.add("triggerInput", a.trigger.inputId);
    for (const auto& w : a.cancels) d.add("cancel", w.nativeCancelEvent);
    for (const auto& q : a.qtes) d.add("qte", q.alternateKey);
    d.strings(a.flow.nodeIds);
    // The raw branches snapshot folds Stop nodes out of; only flows that have one, so other dumps stay equal.
    if (std::ranges::any_of(a.flow.nodes, [](const auto& n) { return n.kind == ConditionFlow::Kind::kStop; })) {
        d.add("flow", a.flow.entry);
        for (const auto& n : a.flow.nodes) d.add(static_cast<int>(n.kind), n.pass, n.fail);
    }
    for (const auto& l : a.links) d.add("link", l.enterEvent, l.airEnterEvent, l.flowParent);
    return d.out.str();
}
std::string snapshot(const EventRuleSpec& e) {
    Digest d;
    d.add(e.name, e.functions.size());
    for (const auto& f : e.functions) d.function(f);
    for (const auto& q : e.qtes) qteDigest(d, q);  // event QTEs (0.12.248)
    return d.out.str();
}
// The registry of a file as the runtime merges it, independent of the rows' order.
std::vector<std::string> registrySnapshot(std::vector<EventRuleSpec> events) {
    std::vector<std::string> out;
    for (const auto& e : events) out.push_back(snapshot(e));
    std::sort(out.begin(), out.end());
    return out;
}
bool load(const wchar_t* path, std::vector<ActionSpec>& out, std::vector<EventRuleSpec>& events) {
    return Declarations::LoadXmlFile(path, out, events);
}
bool load(const wchar_t* path, std::vector<ActionSpec>& out) {
    std::vector<EventRuleSpec> events;
    return load(path, out, events);
}
// --dump <list>: each listed file's verdict, logged warnings/errors (path shown as XML) and full
// declarations, for a before/after comparison of the loader.
int dump(const wchar_t* list) {
    std::ifstream in{std::filesystem::path(list)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto path = std::filesystem::path(std::u8string(line.begin(), line.end()));
        const std::string shown = Declarations::PathText(path);
        std::vector<std::string> messages;
        auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& m) {
            std::string text(m.payload.data(), m.payload.size());
            for (std::size_t at = 0; !shown.empty() && (at = text.find(shown, at)) != std::string::npos; at += 3)
                text.replace(at, shown.size(), "XML");
            messages.push_back(std::string(spdlog::level::to_string_view(m.level).data()) + " " + text);
        });
        auto previous = spdlog::default_logger();
        auto log = std::make_shared<spdlog::logger>("xmldump", sink);
        log->set_level(spdlog::level::warn);
        spdlog::set_default_logger(log);
        std::vector<ActionSpec> out;
        std::vector<EventRuleSpec> events;
        const bool ok = Declarations::LoadXmlFile(path, out, events);
        spdlog::set_default_logger(previous);
        std::cout << "== " << line << "\nvalid=" << ok << " actions=" << out.size() << '\n';
        for (const auto& m : messages) std::cout << "log " << m << '\n';
        for (const auto& a : out) std::cout << "-- " << a.id << '\n' << fullSnapshot(a);
        for (const auto& e : events) std::cout << "-- event " << e.name << '\n' << snapshot(e);
    }
    return 0;
}
// Editor contract (--json): one JSON object on stdout. "valid" is the plugin's own verdict;
// every runtime warning or error it logged is reported, with the file path shown as "XML".
void jsonString(std::ostream& o, std::string_view s) {
    o << '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (c < 0x20) {
                char code[8];
                std::snprintf(code, sizeof code, "\\u%04x", static_cast<unsigned>(c));
                o << code;
            } else
                o << static_cast<char>(c);
        }
    }
    o << '"';
}
void jsonList(std::ostream& o, const std::vector<std::string>& v) {
    o << '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o << ',';
        jsonString(o, v[i]);
    }
    o << ']';
}
int validateJson(const wchar_t* path) {
    std::vector<std::string> errors, warnings;
    // Messages name the file exactly as the plugin formats it (UTF-8, Declarations::PathText).
    const std::string shown = Declarations::PathText(path);
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& m) {
        std::string text(m.payload.data(), m.payload.size());
        for (std::size_t at = 0; !shown.empty() && (at = text.find(shown, at)) != std::string::npos; at += 3)
            text.replace(at, shown.size(), "XML");
        (m.level >= spdlog::level::err ? errors : warnings).push_back(std::move(text));
    });
    auto previous = spdlog::default_logger();
    auto log = std::make_shared<spdlog::logger>("xmlcheck", sink);
    log->set_level(spdlog::level::warn);
    spdlog::set_default_logger(log);
    std::vector<ActionSpec> out;
    std::vector<EventRuleSpec> events;
    const bool ok = load(path, out, events);
    spdlog::set_default_logger(previous);  // The sink refers to locals of this frame.
    std::vector<std::string> ids, names;
    for (const auto& a : out) ids.push_back(a.id);
    for (const auto& e : events) names.push_back(e.name);
    std::cout << "{\"format\":1,\"valid\":" << (ok ? "true" : "false") << ",\"actions\":";
    jsonList(std::cout, ids);
    std::cout << ",\"events\":";
    jsonList(std::cout, names);
    std::cout << ",\"errors\":";
    jsonList(std::cout, errors);
    std::cout << ",\"warnings\":";
    jsonList(std::cout, warnings);
    std::cout << "}\n";
    return ok ? 0 : 2;
}
}
int wmain(int argc, wchar_t** argv) {
    spdlog::set_level(spdlog::level::warn);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--json") return validateJson(argv[2]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--dump") return dump(argv[2]);
    if (argc == 4 && std::wstring_view(argv[1]) == L"--compare") {
        std::vector<ActionSpec> a, b;
        std::vector<EventRuleSpec> ea, eb;
        if (!load(argv[2], a, ea) || !load(argv[3], b, eb)) return 2;
        if (a.size() != b.size()) {
            std::cerr << "Action count differs\n";
            return 3;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
            if (snapshot(a[i]) != snapshot(b[i])) {
                std::cerr << "Semantic mismatch: " << a[i].id << "\nV1:\n"
                          << snapshot(a[i]) << "\nV2:\n"
                          << snapshot(b[i]);
                return 4;
            }
        if (registrySnapshot(ea) != registrySnapshot(eb)) {
            std::cerr << "Event registry differs (" << ea.size() << " vs " << eb.size() << " events)\n";
            return 4;
        }
        std::cout << "Equivalent: " << a.size() << " actions, " << ea.size() << " registered events\n";
        return 0;
    }
    // Ragdoll profiles (runtime/RagdollProfile): the same parser the plugin uses at pickup.
    if (argc == 3 && std::wstring_view(argv[1]) == L"--ragdoll") {
        std::string error;
        const auto profile = AnimationInjector::RagdollProfile::Parse(argv[2], {}, error);
        if (!profile) {
            std::cerr << "Rejected: " << error << '\n';
            return 2;
        }
        const auto& m = profile->motion;
        std::cout << "Valid ragdoll profile: follow=" << m.follow << " gravity=" << m.gravity
                  << " drag=" << m.linearDamping << " spin=" << m.angularDamping << " inertia=" << m.inertia
                  << " friction=" << m.jointFriction << " bodies=" << profile->bodies.size() << '\n';
        for (const auto& body : profile->bodies)
            std::cout << "Body " << body.bone << " simulate=" << body.simulate << " cone=" << body.cone.value_or(-1)
                      << " swingFrom=" << body.swingFrom << '\n';
        return 0;
    }
    // Which profile a carry would read (runtime/RagdollProfile::Resolve): the lift clip's own, the
    // rig's, or none, under a Data folder.
    if (argc == 5 && std::wstring_view(argv[1]) == L"--ragdoll-resolve") {
        const auto narrow = [](std::wstring_view text) {
            const std::filesystem::path path(text);
            const auto utf8 = path.u8string();
            return std::string(utf8.begin(), utf8.end());
        };
        const auto rig = AnimationInjector::PairedRig::FromName(narrow(argv[3]));
        const auto profile = AnimationInjector::RagdollProfile::Resolve(argv[2], rig, narrow(argv[4]));
        if (!profile) {
            std::cout << "Resolved: none\n";
            return 0;
        }
        const auto relative = std::filesystem::path(profile->source).lexically_relative(argv[2]).generic_u8string();
        std::cout << "Resolved: "
                  << (profile->from == AnimationInjector::RagdollProfile::Source::kClip ? "clip" : "rig") << ' '
                  << std::string(relative.begin(), relative.end()) << " bodies=" << profile->bodies.size() << '\n';
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--ragdoll-reject") {
        std::string error;
        if (AnimationInjector::RagdollProfile::Parse(argv[2], {}, error)) {
            std::cerr << "Invalid ragdoll profile was accepted\n";
            return 5;
        }
        std::cout << "Rejected: " << error << '\n';
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--reject") {
        std::vector<ActionSpec> out;
        ActionSpec sentinel;
        sentinel.id = "sentinel";
        out.push_back(sentinel);
        const bool ok = load(argv[2], out);
        if (ok || out.size() != 1 || out[0].id != "sentinel") {
            std::cerr << "Invalid file was accepted or partially appended\n";
            return 5;
        }
        std::cout << "Rejected atomically\n";
        return 0;
    }
    if (argc == 2) {
        std::vector<ActionSpec> out;
        std::vector<EventRuleSpec> events;
        if (!load(argv[1], out, events)) return 2;
        std::cout << "Valid: " << out.size() << " actions, " << events.size() << " registered events\n";
        for (auto& a : out) {
            std::cout << a.id << " key=" << a.trigger.key << " clips=" << a.clips.size()
                      << " targetRig=" << PairedRig::Name(a.paired.targetRig) << '\n';
            if (a.carryRelease.enabled)
                std::cout << "CarryRelease event=" << a.carryRelease.event << " at=" << a.carryRelease.at
                          << " fallback=" << a.carryRelease.fallback << '\n';
            if (a.recovery.enabled)
                std::cout << "Recovery event=" << a.recovery.event << " at=" << a.recovery.fallback
                          << " blend=" << a.recovery.blend << " move=" << a.recovery.move
                          << " exit=" << a.recovery.recoverEvent << '\n';
            if (a.carry.enabled)
                std::cout << "Carry bone=" << a.carry.bone << " root=" << a.carry.targetRoot
                          << " putdown=" << a.carry.putdownAction << " key=" << a.carry.key
                          << " pad=" << a.carry.gamepad << " enterBlend=" << a.enterBlend << " target="
                          << (a.carry.mode == AnimationInjector::CarryTargetMode::kPhysics ? "physics" : "pose")
                          << " part=" << a.carry.part << " fixAt=" << a.carry.fixAt
                          << " weapon=" << AnimationInjector::CarryWeaponName(a.carry.weapon) << '\n';
            if (a.carry.hold.Enabled()) {
                std::cout << "Hold pose=" << a.carry.hold.pose << " blendIn=" << a.carry.hold.blendIn
                          << " blendOut=" << a.carry.hold.blendOut;
                for (const auto& bone : a.carry.hold.bones) std::cout << " [" << bone.name << "=" << bone.weight << ']';
                std::cout << '\n';
            }
        }
        return 0;
    }
    std::cout
        << "AnimationInjectorXmlCheck <file.xml>\n  --compare <v1.xml> <v2.xml>\n  --reject <invalid.xml>\n  --json <file.xml>\n"
           "  --ragdoll <profile.xml>\n  --ragdoll-reject <invalid.xml>\n  --ragdoll-resolve <Data> <rig> <lift clip>\n";
    return 1;
}

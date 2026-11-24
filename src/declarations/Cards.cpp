#include "Cards.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>

namespace AnimationInjector::Cards {
namespace {
using Node = pugi::xml_node;
using ConditionFlow::absent;

std::string get(Node n, const char* key) { return n.attribute(key).as_string(""); }
// The element as the author wrote it: its path and id.
std::string where(Node n) { return n.path() + (n.attribute("id") ? " [" + get(n, "id") + "]" : ""); }
[[noreturn]] void fail(Node n, const std::string& reason) { throw std::runtime_error(where(n) + ": " + reason); }
bool guard(std::string_view kind) { return kind == "Check" || kind == "And" || kind == "Or"; }
bool cardPart(std::string_view kind) { return guard(kind) || kind == "Row" || kind == "True" || kind == "False"; }
std::vector<Node> elements(Node n) {
    std::vector<Node> out;
    for (auto c : n.children()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
            fail(n, "Text inside the element is not allowed");
        if (c.type() == pugi::node_element) out.push_back(c);
    }
    return out;
}
void attributes(Node n, std::initializer_list<std::string_view> names) {
    for (auto a : n.attributes())
        if (std::find(names.begin(), names.end(), a.name()) == names.end())
            fail(n, "Unknown attribute '" + std::string(a.name()) + "'");
}

// One compilation: a card and the shared cards it reaches, into one program.
struct Builder {
    const std::vector<std::string>& actions;
    const std::vector<bool>& pairs;
    const Shared& shared;
    ConditionFlowXml::Program program;
    Compiled flags;
    std::map<std::string, int> sharedEntry;  // a shared card copied in once per program (K6)
    std::vector<std::string> visiting;       // shared cards being compiled, for loops

    int add(Node from, ConditionFlow::Node node) {
        if (program.nodes.size() >= ConditionFlow::limit)
            fail(from, "The condition card is too large (up to 256 nodes)");
        program.nodes.push_back(std::move(node));
        program.nodeIds.push_back(std::string(from.name()) + std::to_string(program.nodes.size()));
        return static_cast<int>(program.nodes.size() - 1);
    }
    int actionIndex(Node at, const std::string& name) {
        const auto it = std::find(actions.begin(), actions.end(), name);
        if (it == actions.end()) fail(at, "Undefined action: " + name);
        return static_cast<int>(it - actions.begin());
    }
    int play(Node at, const std::string& name) {
        ConditionFlow::Node n;
        n.kind = ConditionFlow::Kind::kAction;
        n.action = actionIndex(at, name);
        flags.playsPair |= pairs[n.action];
        return add(at, std::move(n));
    }
    // A condition (K1, K2): a node without ports, which a chain gives ports or a group takes as an operand. `inAny`:
    // it sits among conditions of which one is enough (an OR row's); `inGroup`: it is a group's member.
    int condition(Node g, bool inAny, bool inGroup) {
        const std::string_view kind = g.name();
        if (!guard(kind)) fail(g, "Conditions are written as <Check>, <And>, <Or>");
        if (g.attribute("not") && get(g, "not") != "true" && get(g, "not") != "false")
            fail(g, "not must be true or false");
        int id;
        if (kind == "Check") {
            std::vector<ConditionFlowXml::Predicate> predicates;
            std::string error;
            if (!ConditionFlowXml::ReadChecks(g, predicates, error)) throw std::runtime_error(error);
            std::vector<int> tests;
            for (auto& predicate : predicates) {
                ConditionFlow::Node n;
                n.kind = ConditionFlow::Kind::kTest;
                n.predicate = static_cast<int>(program.predicates.size());
                program.predicates.push_back(std::move(predicate));
                tests.push_back(add(g, std::move(n)));
            }
            if (tests.size() == 1)
                id = tests.front();
            else {
                ConditionFlow::Node any;
                any.kind = ConditionFlow::Kind::kAny;
                any.operands = std::move(tests);
                id = add(g, std::move(any));
            }
        } else {
            // A group is one level, the opposite of what holds it (0.12.301): <Or> among AND conditions, <And> in an
            // OR row.
            if (inGroup) fail(g, "A group holds no group (one level only)");
            if ((kind == "And") != inAny)
                fail(g, inAny ? "A group in an OR row is written <And> (the same operator is not grouped)"
                              : "A group among AND conditions is written <Or> (the same operator is not grouped)");
            attributes(g, {"not"});
            ConditionFlow::Node n;
            n.kind = kind == "And" ? ConditionFlow::Kind::kAll : ConditionFlow::Kind::kAny;
            for (auto c : elements(g)) n.operands.push_back(condition(c, !inAny, true));
            if (n.operands.size() < 2) fail(g, std::string(kind) + " group needs two or more conditions");
            id = add(g, std::move(n));
        }
        if (get(g, "not") != "true") return id;
        ConditionFlow::Node n;
        n.kind = ConditionFlow::Kind::kNot;
        n.operands.push_back(id);
        return add(g, std::move(n));
    }
    // Conditions in a chain (K3): each passes to the next, the last to `pass`; any failure goes to `fail`. `any` (an
    // OR row): one Any node of them all instead.
    int chain(const std::vector<Node>& conditions, int pass, int failTo, bool any = false) {
        if (any) {
            ConditionFlow::Node n;
            n.kind = ConditionFlow::Kind::kAny;
            for (auto c : conditions) n.operands.push_back(condition(c, true, false));
            n.pass = pass;
            n.fail = failTo;
            return add(conditions.front(), std::move(n));
        }
        int next = pass;
        for (auto it = conditions.rbegin(); it != conditions.rend(); ++it) {
            const int id = condition(*it, false, false);
            program.nodes[id].pass = next;
            program.nodes[id].fail = failTo;
            next = id;
        }
        return next;
    }
    int sharedCard(Node at, const std::string& name) {
        if (auto it = sharedEntry.find(name); it != sharedEntry.end()) return it->second;
        const auto it = shared.find(name);
        if (it == shared.end()) fail(at, "Undefined condition card: " + name);
        if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
            std::string loop;
            for (const auto& c : visiting) loop += c + " → ";
            fail(at, "Condition cards point at each other in a loop: " + loop + name);
        }
        visiting.push_back(name);
        const int entry = card(it->second);
        visiting.pop_back();
        sharedEntry[name] = entry;
        return entry;
    }
    // Where a <True>/<False> goes: to, condition or the card it holds.
    int destination(Node d) {
        if (d.attribute("to") && d.attribute("condition")) fail(d, "Write either to or condition, not both");
        if (d.attribute("to") || d.attribute("condition")) {
            attributes(d, {"to", "condition"});
            if (!elements(d).empty()) fail(d, "Where to or condition is written, no card content is written");
            return d.attribute("to") ? play(d, get(d, "to")) : sharedCard(d, get(d, "condition"));
        }
        attributes(d, {});
        return card(d);
    }
    // One card (K3-K5): guards, then rows or one True, then False.
    int card(Node owner) {
        std::vector<Node> guards, rows;
        Node yes, no;
        for (auto c : elements(owner)) {
            const std::string_view kind = c.name();
            if (no) fail(c, "<False> goes at the end of a card");
            if (guard(kind)) {
                if (!rows.empty() || yes) fail(c, "Common conditions go before rows and True");
                guards.push_back(c);
            } else if (kind == "Row") {
                if (yes)
                    fail(c,
                        "True is the only row of a card with common conditions only: it is not used with other rows");
                rows.push_back(c);
            } else if (kind == "True") {
                if (yes) fail(c, "A card has at most one True");
                if (!rows.empty())
                    fail(c,
                        "True is the only row of a card with common conditions only: it is not used with other rows");
                yes = c;
            } else if (kind == "False")
                no = c;
            else
                fail(c,
                    "Element not allowed in a condition card (common conditions <Check>, <And>, <Or>, <Row>, <True>, <False>)");
        }
        const int falseEntry = no ? destination(no) : absent;
        int body = falseEntry;
        if (yes)
            body = destination(yes);
        else if (!rows.empty()) {
            int next = falseEntry;
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) next = row(*it, next);
            body = next;
        }
        return chain(guards, body, falseEntry);
    }
    // One row (K4): its conditions pass to its destination; a failure tries `next`.
    int row(Node r, int next) {
        attributes(r, {"to", "condition", "match"});
        if (r.attribute("match") && get(r, "match") != "any") fail(r, "match takes only any (absent: every condition)");
        if (r.attribute("to") && r.attribute("condition")) fail(r, "Write either to or condition, not both");
        std::vector<Node> conditions;
        Node inner;
        for (auto c : elements(r)) {
            if (inner) fail(c, "<Condition> goes at the end of a row");
            if (guard(c.name()))
                conditions.push_back(c);
            else if (std::string_view(c.name()) == "Condition")
                inner = c;
            else
                fail(c, "Element not allowed in a row (conditions <Check>, <And>, <Or> and a following <Condition>)");
        }
        if (conditions.empty())
            fail(r,
                "A row without conditions is not written: if the common conditions alone decide, use <True>; the rest is <False>");
        const int ways = (r.attribute("to") ? 1 : 0) + (r.attribute("condition") ? 1 : 0) + (inner ? 1 : 0);
        if (ways != 1) fail(r, "A row needs one of to, condition or a following <Condition>");
        int target;
        if (r.attribute("to"))
            target = play(r, get(r, "to"));
        else if (r.attribute("condition"))
            target = sharedCard(r, get(r, "condition"));
        else {
            if (inner.attribute("id"))
                fail(inner, "A <Condition> inside a row takes no id (shared cards go at the root)");
            attributes(inner, {});
            target = card(inner);
        }
        return chain(conditions, target, next, !r.attribute("match").empty());
    }
};
}

bool HasCard(pugi::xml_node owner) {
    if (owner.attribute("condition")) return true;
    for (auto c : owner.children())
        if (c.type() == pugi::node_element && cardPart(c.name())) return true;
    return false;
}

bool Compile(pugi::xml_node owner, const std::vector<std::string>& actions, const std::vector<bool>& pairs,
    const Shared& shared, Compiled& out, std::string& error) {
    error.clear();
    try {
        Builder b{actions, pairs, shared};
        int entry;
        if (owner.attribute("to") && owner.attribute("condition"))
            fail(owner, "Write either to or condition, not both");
        if (owner.attribute("condition")) {
            for (auto c : elements(owner))
                if (cardPart(c.name())) fail(c, "Where condition is written, no card content is written");
            entry = b.sharedCard(owner, get(owner, "condition"));
        } else if (owner.attribute("to")) {
            // to="X" with guards: a card whose True is X.
            std::vector<Node> guards;
            for (auto c : elements(owner)) {
                if (!cardPart(c.name())) continue;
                if (!guard(c.name()))
                    fail(c, "Where to is written, only common conditions (<Check>, <And>, <Or>) go with it");
                guards.push_back(c);
            }
            const int target = b.play(owner, get(owner, "to"));
            entry = b.chain(guards, target, absent);
        } else
            entry = b.card(owner);
        if (entry == absent) fail(owner, "The condition card has nothing to play (its rows, True and False are empty)");
        b.program.entry = entry;
        const auto valid =
            ConditionFlow::Validate(b.program.nodes, b.program.entry, b.program.predicates.size(), actions.size());
        if (valid != ConditionFlow::Error::kNone)
            fail(owner, "The condition card cannot be made into an execution flow (" +
                            std::to_string(static_cast<int>(valid)) + ")");
        out = std::move(b.flags);
        out.program = std::move(b.program);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}

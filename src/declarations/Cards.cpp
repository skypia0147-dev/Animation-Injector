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
            fail(n, "요소 안의 텍스트는 허용하지 않습니다");
        if (c.type() == pugi::node_element) out.push_back(c);
    }
    return out;
}
void attributes(Node n, std::initializer_list<std::string_view> names) {
    for (auto a : n.attributes())
        if (std::find(names.begin(), names.end(), a.name()) == names.end())
            fail(n, "알 수 없는 속성 '" + std::string(a.name()) + "'");
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
        if (program.nodes.size() >= ConditionFlow::limit) fail(from, "조건 카드가 너무 큽니다(노드 256개까지)");
        program.nodes.push_back(std::move(node));
        program.nodeIds.push_back(std::string(from.name()) + std::to_string(program.nodes.size()));
        return static_cast<int>(program.nodes.size() - 1);
    }
    int actionIndex(Node at, const std::string& name) {
        const auto it = std::find(actions.begin(), actions.end(), name);
        if (it == actions.end()) fail(at, "정의되지 않은 액션: " + name);
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
        if (!guard(kind)) fail(g, "조건은 <Check>·<And>·<Or>로 씁니다");
        if (g.attribute("not") && get(g, "not") != "true" && get(g, "not") != "false")
            fail(g, "not은 true 또는 false여야 합니다");
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
            if (inGroup) fail(g, "묶음 안에는 묶음을 넣지 않습니다(한 단계까지)");
            if ((kind == "And") != inAny)
                fail(g, inAny ? "OR 행 안의 묶음은 <And>로 씁니다(같은 연산자는 묶지 않습니다)"
                              : "AND 조건 사이의 묶음은 <Or>로 씁니다(같은 연산자는 묶지 않습니다)");
            attributes(g, {"not"});
            ConditionFlow::Node n;
            n.kind = kind == "And" ? ConditionFlow::Kind::kAll : ConditionFlow::Kind::kAny;
            for (auto c : elements(g)) n.operands.push_back(condition(c, !inAny, true));
            if (n.operands.size() < 2) fail(g, std::string(kind) + " 묶음에는 조건이 둘 이상 필요합니다");
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
        if (it == shared.end()) fail(at, "정의되지 않은 조건 카드: " + name);
        if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
            std::string loop;
            for (const auto& c : visiting) loop += c + " → ";
            fail(at, "조건 카드가 서로를 가리켜 순환합니다: " + loop + name);
        }
        visiting.push_back(name);
        const int entry = card(it->second);
        visiting.pop_back();
        sharedEntry[name] = entry;
        return entry;
    }
    // Where a <True>/<False> goes: to, condition or the card it holds.
    int destination(Node d) {
        if (d.attribute("to") && d.attribute("condition")) fail(d, "to와 condition 중 하나만 씁니다");
        if (d.attribute("to") || d.attribute("condition")) {
            attributes(d, {"to", "condition"});
            if (!elements(d).empty()) fail(d, "to·condition을 쓴 곳에는 카드 내용을 쓰지 않습니다");
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
            if (no) fail(c, "<False>는 카드의 맨 끝에 씁니다");
            if (guard(kind)) {
                if (!rows.empty() || yes) fail(c, "공통 조건은 행·True보다 앞에 씁니다");
                guards.push_back(c);
            } else if (kind == "Row") {
                if (yes) fail(c, "True는 공통 조건만 쓴 카드의 유일한 행입니다: 다른 행과 함께 쓰지 않습니다");
                rows.push_back(c);
            } else if (kind == "True") {
                if (yes) fail(c, "True는 카드에 하나만 씁니다");
                if (!rows.empty())
                    fail(c, "True는 공통 조건만 쓴 카드의 유일한 행입니다: 다른 행과 함께 쓰지 않습니다");
                yes = c;
            } else if (kind == "False")
                no = c;
            else
                fail(c, "조건 카드에 쓸 수 없는 요소입니다(공통 조건 <Check>·<And>·<Or>, <Row>, <True>, <False>)");
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
        if (r.attribute("match") && get(r, "match") != "any") fail(r, "match는 any만 씁니다(없으면 모든 조건)");
        if (r.attribute("to") && r.attribute("condition")) fail(r, "to와 condition 중 하나만 씁니다");
        std::vector<Node> conditions;
        Node inner;
        for (auto c : elements(r)) {
            if (inner) fail(c, "<Condition>은 행의 맨 끝에 씁니다");
            if (guard(c.name()))
                conditions.push_back(c);
            else if (std::string_view(c.name()) == "Condition")
                inner = c;
            else
                fail(c, "행에 쓸 수 없는 요소입니다(조건 <Check>·<And>·<Or>와 이어지는 <Condition>)");
        }
        if (conditions.empty())
            fail(r, "조건 없는 행은 쓰지 않습니다: 공통 조건만으로 정하면 <True>, 나머지는 <False>로 씁니다");
        const int ways = (r.attribute("to") ? 1 : 0) + (r.attribute("condition") ? 1 : 0) + (inner ? 1 : 0);
        if (ways != 1) fail(r, "행에는 to, condition, 이어지는 <Condition> 중 하나가 필요합니다");
        int target;
        if (r.attribute("to"))
            target = play(r, get(r, "to"));
        else if (r.attribute("condition"))
            target = sharedCard(r, get(r, "condition"));
        else {
            if (inner.attribute("id"))
                fail(inner, "행 안의 <Condition>에는 id를 쓰지 않습니다(함께 쓰는 카드는 루트에)");
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
        if (owner.attribute("to") && owner.attribute("condition")) fail(owner, "to와 condition 중 하나만 씁니다");
        if (owner.attribute("condition")) {
            for (auto c : elements(owner))
                if (cardPart(c.name())) fail(c, "condition을 쓴 곳에는 카드 내용을 쓰지 않습니다");
            entry = b.sharedCard(owner, get(owner, "condition"));
        } else if (owner.attribute("to")) {
            // to="X" with guards: a card whose True is X.
            std::vector<Node> guards;
            for (auto c : elements(owner)) {
                if (!cardPart(c.name())) continue;
                if (!guard(c.name())) fail(c, "to를 쓴 곳에는 공통 조건(<Check>·<And>·<Or>)만 붙입니다");
                guards.push_back(c);
            }
            const int target = b.play(owner, get(owner, "to"));
            entry = b.chain(guards, target, absent);
        } else
            entry = b.card(owner);
        if (entry == absent) fail(owner, "조건 카드에 재생할 곳이 없습니다(행·True·False가 비었습니다)");
        b.program.entry = entry;
        const auto valid =
            ConditionFlow::Validate(b.program.nodes, b.program.entry, b.program.predicates.size(), actions.size());
        if (valid != ConditionFlow::Error::kNone)
            fail(owner, "조건 카드를 실행 흐름으로 만들 수 없습니다(" + std::to_string(static_cast<int>(valid)) + ")");
        out = std::move(b.flags);
        out.program = std::move(b.program);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}

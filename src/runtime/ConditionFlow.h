#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace AnimationInjector::ConditionFlow {
enum class Value { kFalse, kTrue, kUnavailable };
enum class Kind { kTest, kAll, kAny, kNot, kAction, kStop };
constexpr int absent = -1;
constexpr std::size_t limit = 256;

// Nodes store resolved indices, never actor pointers or mutable game state.
struct Node {
    Kind kind = Kind::kStop;
    int predicate = absent;
    std::vector<int> operands;
    int pass = absent;
    int fail = absent;
    int action = absent;
};
enum class Error { kNone, kTooLarge, kReference, kShape, kCycle, kBudget };
struct Result {
    int action = absent;
    Error error = Error::kNone;
    bool unavailable = false;
};
inline bool IsCondition(Kind kind) {
    return kind == Kind::kTest || kind == Kind::kAll || kind == Kind::kAny || kind == Kind::kNot;
}

// Validate all components, including disconnected drafts, before publication.
inline Error Validate(std::span<const Node> nodes, int entry, std::size_t predicates, std::size_t actions) {
    if (nodes.size() > limit) return Error::kTooLarge;
    const auto valid = [&](int id) { return id >= 0 && static_cast<std::size_t>(id) < nodes.size(); };
    if (entry != absent && !valid(entry)) return Error::kReference;
    for (const auto& n : nodes) {
        if (!IsCondition(n.kind) && n.kind != Kind::kAction && n.kind != Kind::kStop) return Error::kShape;
        if ((n.pass != absent && !valid(n.pass)) || (n.fail != absent && !valid(n.fail))) return Error::kReference;
        if (!IsCondition(n.kind) && (n.pass != absent || n.fail != absent)) return Error::kShape;
        if (n.kind == Kind::kTest && (n.predicate < 0 || static_cast<std::size_t>(n.predicate) >= predicates))
            return Error::kReference;
        if (n.kind == Kind::kAction && (n.action < 0 || static_cast<std::size_t>(n.action) >= actions))
            return Error::kReference;
        if (n.kind != Kind::kAction && n.action != absent) return Error::kShape;
        if (n.kind != Kind::kTest && n.predicate != absent) return Error::kShape;
        if (n.kind == Kind::kNot && n.operands.size() != 1) return Error::kShape;
        if ((n.kind == Kind::kAll || n.kind == Kind::kAny) && n.operands.empty()) return Error::kShape;
        if ((n.kind == Kind::kTest || !IsCondition(n.kind)) && !n.operands.empty()) return Error::kShape;
        for (int child : n.operands) {
            if (!valid(child)) return Error::kReference;
            if (!IsCondition(nodes[child].kind) || nodes[child].pass != absent || nodes[child].fail != absent)
                return Error::kShape;
        }
    }
    std::vector<unsigned char> colors(nodes.size());
    const auto visit = [&](auto&& self, int id) -> bool {
        if (id == absent) return true;
        if (colors[id] == 1) return false;
        if (colors[id] == 2) return true;
        colors[id] = 1;
        const auto& n = nodes[id];
        for (int child : n.operands)
            if (!self(self, child)) return false;
        if (!self(self, n.pass) || !self(self, n.fail)) return false;
        colors[id] = 2;
        return true;
    };
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (!visit(visit, static_cast<int>(i))) return Error::kCycle;
    return Error::kNone;
}

// Evaluation only selects a destination. Starting it and charging costs belong to the caller.
// A fresh invocation is required at transition time; do not cache buffered-input results.
template<class ReadPredicate>
Result Select(
    std::span<const Node> nodes, int entry, std::size_t predicates, std::size_t actions, ReadPredicate&& read) {
    Result result;
    result.error = Validate(nodes, entry, predicates, actions);
    if (result.error != Error::kNone) return result;
    std::vector<Value> cache(nodes.size(), Value::kUnavailable);
    std::vector<bool> evaluated(nodes.size());
    std::size_t budget = limit * 2;
    const auto value = [&](auto&& self, int id) -> Value {
        if (evaluated[id]) return cache[id];
        if (budget == 0) {
            result.error = Error::kBudget;
            return Value::kUnavailable;
        }
        --budget;
        const auto& n = nodes[id];
        Value v = Value::kUnavailable;
        if (n.kind == Kind::kTest)
            v = read(n.predicate);
        else if (n.kind == Kind::kNot) {
            const auto child = self(self, n.operands[0]);
            v = child == Value::kUnavailable ? child : child == Value::kTrue ? Value::kFalse : Value::kTrue;
        } else {
            bool unknown = false;
            bool combined = n.kind == Kind::kAll;
            // Inspect every child so an unavailable participant cannot be hidden by OR/NOT.
            for (int child : n.operands) {
                const auto answer = self(self, child);
                unknown |= answer == Value::kUnavailable;
                if (n.kind == Kind::kAll)
                    combined &= answer == Value::kTrue;
                else
                    combined |= answer == Value::kTrue;
            }
            v = unknown ? Value::kUnavailable : combined ? Value::kTrue : Value::kFalse;
        }
        evaluated[id] = true;
        cache[id] = v;
        result.unavailable |= v == Value::kUnavailable;
        return v;
    };
    for (int id = entry; id != absent;) {
        if (budget == 0) {
            result.error = Error::kBudget;
            break;
        }
        --budget;
        const auto& n = nodes[id];
        if (n.kind == Kind::kStop) break;
        if (n.kind == Kind::kAction) {
            result.action = n.action;
            break;
        }
        const auto answer = value(value, id);
        if (result.error != Error::kNone) break;
        id = answer == Value::kTrue ? n.pass : n.fail;
    }
    if (result.error != Error::kNone) result.action = absent;
    return result;
}
}

#include "runtime/ConditionFlow.h"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace AnimationInjector::ConditionFlow;
static void Check(bool ok) {
    if (!ok) std::abort();
}
int main() {
    std::vector<Node> graph = {{Kind::kTest, 0, {}, 2, 1}, {Kind::kTest, 1, {}, 3, absent},
        {Kind::kAction, absent, {}, absent, absent, 0}, {Kind::kAction, absent, {}, absent, absent, 1}};
    for (const auto first : {Value::kFalse, Value::kTrue, Value::kUnavailable})
        for (const auto second : {Value::kFalse, Value::kTrue, Value::kUnavailable}) {
            std::array<int, 2> calls{};
            auto r = Select(graph, 0, 2, 2, [&](int i) {
                ++calls[i];
                return i == 0 ? first : second;
            });
            Check(r.error == Error::kNone);
            Check(r.action == (first == Value::kTrue ? 0 : second == Value::kTrue ? 1 : absent));
            Check(calls[0] == 1 && calls[1] == (first == Value::kTrue ? 0 : 1));
        }
    // A subsequent execution must read new state, including after buffered input.
    Value live = Value::kTrue;
    Check(Select(graph, 0, 2, 2, [&](int) { return live; }).action == 0);
    live = Value::kFalse;
    Check(Select(graph, 0, 2, 2, [&](int) { return live; }).action == absent);
    // A destination chosen before cleanup is not authoritative at dispatch.
    // The fallback and primary can exchange places as the live scene changes.
    std::array<Value, 2> scene{Value::kTrue, Value::kTrue};
    auto choose = [&] { return Select(graph, 0, 2, 2, [&](int i) { return scene[i]; }).action; };
    Check(choose() == 0);
    scene[0] = Value::kFalse;
    Check(choose() == 1);
    scene[0] = Value::kTrue;
    Check(choose() == 0);
    scene = {Value::kFalse, Value::kUnavailable};
    Check(choose() == absent);
    // Rejoining branches are valid; execution cycles, even disconnected, are not.
    graph[1].pass = 2;
    Check(Validate(graph, 0, 2, 2) == Error::kNone);
    graph[1].fail = 0;
    Check(Validate(graph, 2, 2, 2) == Error::kCycle);
    graph[1].fail = 99;
    int calls = 0;
    Check(Select(graph, 0, 2, 2,
              [&](int) {
                  ++calls;
                  return Value::kTrue;
              }).error == Error::kReference &&
          calls == 0);
    graph = {{Kind::kNot, absent, {1}, 3, absent}, {Kind::kTest, 0}, {Kind::kTest, 1},
        {Kind::kAction, absent, {}, absent, absent, 0}};
    Check(Select(graph, 0, 2, 1, [](int) { return Value::kUnavailable; }).action == absent);
    Check(Select(graph, 0, 2, 1, [](int) { return Value::kFalse; }).action == 0);
    for (auto kind : {Kind::kAll, Kind::kAny}) {
        graph[0].kind = kind;
        graph[0].operands = {1, 2};
        auto r = Select(graph, 0, 2, 1, [](int i) { return i ? Value::kUnavailable : Value::kTrue; });
        Check(r.action == absent && r.unavailable);
    }
    graph[0].operands = {0};
    Check(Validate(graph, 0, 2, 1) != Error::kNone);
    graph = {{Kind::kAny, absent, {1, 2}, 3, absent}, {Kind::kTest, 0}, {Kind::kNot, absent, {1}},
        {Kind::kAction, absent, {}, absent, absent, 0}};
    calls = 0;
    Check(Select(graph, 0, 1, 1,
              [&](int) {
                  ++calls;
                  return Value::kFalse;
              }).action == 0 &&
          calls == 1);
    graph[1] = {Kind::kNot, absent, {2}};
    Check(Validate(graph, 0, 1, 1) == Error::kCycle);
    graph = {{Kind::kStop}};
    Check(Select(graph, absent, 0, 0, [](int) { return Value::kTrue; }).action == absent);
    graph[0].kind = static_cast<Kind>(999);
    Check(Validate(graph, 0, 0, 0) == Error::kShape);
    graph.assign(limit + 1, {});
    Check(Validate(graph, 0, 0, 0) == Error::kTooLarge);
    std::cout << "Condition branching, unavailable propagation, fresh evaluation and cycle checks passed\n";
}

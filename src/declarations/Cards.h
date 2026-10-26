#pragma once
#include "declarations/ConditionFlowXml.h"
#include <pugixml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Condition cards (XML format 6, docs/설계-형식6-조건카드.md): what a start, a transition or a QTE result
// plays is `to="X"`, `condition="C"` (a card several places share, <Condition id="C"> at the root) or a card as its
// content:
//
//   card       := guard* (row+ | <True ...>)? <False ...>?
//   guard      := <Check kind ...> | group                                       (not="true" inverts any of them)
//   group      := <Or><Check/>{2,}</Or> among AND conditions, <And><Check/>{2,}</And> in an OR row (0.12.301: one
//                 level, the opposite of what holds it)
//   row        := <Row to="X">guard*</Row> | <Row condition="C">guard*</Row>      (match="any": one guard is enough)
//               | <Row>guard* <Condition>card</Condition></Row>
//   True/False := to="X" | condition="C" | a card as its content
//
// When every guard holds, the first row whose conditions hold is taken; when a guard fails or no row holds, False
// is (nothing plays without one). True is the one row of a card whose conditions are all guards. A card reached
// from another decides alone: when it picks nothing, nothing plays.
//
// A card is compiled straight into the runtime's flow program (no XML in between):
//  K1  <Check> -> a test node (ConditionFlowXml::ReadChecks), several values (type="sword,dagger") -> an Any node of
//      one test each; not="true" -> a Not node around it.
//  K2  <And>/<Or> -> All/Any nodes, their checks as operands; an OR row's guards -> one Any node.
//  K3  guards pass in a chain; a failing guard goes to False's first node (or ends).
//  K4  each row's failure goes to the next row, the last one's to False; a row that holds goes to its destination:
//      to -> an Action node, a card -> that card's first node, condition -> the shared card (K6).
//  K5  True follows the guards.
//  K6  a shared card is copied into each program that names it, once per program; cards naming each other in a
//      loop are refused ("조건 카드가 서로를 가리켜 순환합니다: C -> D -> C").
//  K7  the program's limits stay the runtime's (ConditionFlow::limit nodes).
namespace AnimationInjector::Cards {
// A file's shared cards (<Condition id> at the root), by id.
using Shared = std::unordered_map<std::string, pugi::xml_node>;

// A compiled card and what the selecting action that holds it needs (the start rules' T6).
struct Compiled {
    ConditionFlowXml::Program program;
    // It can play a pair: the selecting action holding it is a Pair.
    bool playsPair = false;
};

// Whether `owner` (a <Start>, <Transition>, QTE result, <True>/<False> or <Condition>) decides through a card: a
// card as its content or condition="C". False for to="X" alone.
bool HasCard(pugi::xml_node owner);

// Compiles the card `owner` decides by (to="X" with guards counts as a card whose True is X). `actions` are the
// file's action ids in document order (a play node names its index), `pairs` which of them are Pairs. Atomic: on
// failure `out` is untouched and `error` holds the path of the format 6 element at fault and the reason.
bool Compile(pugi::xml_node owner, const std::vector<std::string>& actions, const std::vector<bool>& pairs,
    const Shared& shared, Compiled& out, std::string& error);
}

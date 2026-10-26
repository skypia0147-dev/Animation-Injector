#include "declarations/Cards.h"
#include "declarations/ConditionFlowXml.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace AnimationInjector;
int main(int argc, char** argv) {
    try {
        // tests/data/grapple.xml: the Grapple mod's declaration (air and equipped-type routing, 09-18).
        if (argc != 2) throw std::runtime_error("expected staged Grapple XML path");
        pugi::xml_document doc;
        if (!doc.load_file(argv[1])) throw std::runtime_error("cannot read XML");
        const auto root = doc.document_element();
        std::vector<std::string> names;
        std::vector<bool> pairs;
        Cards::Shared shared;
        for (auto n : root.children()) {
            const std::string_view kind = n.name();
            if (kind == "Action" || kind == "Pair") {
                names.emplace_back(n.attribute("id").value());
                pairs.push_back(kind == "Pair");
            } else if (kind == "Condition")
                shared.emplace(n.attribute("id").value(), n);
        }
        constexpr std::array<unsigned, 8> forms{0x82C, 0x82D, 0x82E, 0x82F, 0x830, 0x831, 0x832, 0x833};
        unsigned cases = 0;
        for (bool back : {false, true}) {
            const std::string host = back ? "GrappleBack" : "GrappleFront";
            // Format 6: the grab's conditions are the condition card of its start.
            const auto start = root.find_child_by_attribute("Start", "id", host.c_str());
            if (!start) throw std::runtime_error("no start " + host);
            Cards::Compiled compiled;
            std::string error;
            if (!Cards::Compile(start, names, pairs, shared, compiled, error)) throw std::runtime_error(error);
            const auto& program = compiled.program;
            for (unsigned mask = 0; mask < 256; ++mask)
                for (bool failure : {false, true})
                    for (bool unarmed : {false, true})
                        for (bool drawn : {false, true})
                            for (auto floor : {ConditionFlow::Value::kFalse, ConditionFlow::Value::kTrue,
                                     ConditionFlow::Value::kUnavailable})
                                for (auto room : {ConditionFlow::Value::kFalse, ConditionFlow::Value::kTrue,
                                         ConditionFlow::Value::kUnavailable})
                                    for (bool air : {false, true}) {
                                        const auto has = [&](unsigned bit) { return (mask & (1u << bit)) != 0; };
                                        const bool supported = floor == ConditionFlow::Value::kTrue,
                                                   clear = room == ConditionFlow::Value::kTrue;
                                        std::string expected;
                                        // A front grab never starts in the air; the back grab does not test it.
                                        if (!back && air) expected = "";
                                        // OAR's preserved special routes precede the converted ground routes.
                                        else if (has(2) || has(3))
                                            expected = back ? "GrappleAirBack" : "GrappleAirFront";
                                        else if (has(5))
                                            expected = back ? "GrappleForHonorBack" : "GrappleForHonorFront";
                                        else if (!back && failure && (has(0) || has(1)) && (unarmed || !drawn))
                                            expected = "GrappleResistanceFront";
                                        else if (back) {
                                            if (has(6))
                                                expected = supported ? "GrappleAttackBack" : "";
                                            else if (has(0) || has(1) || has(4))
                                                expected = supported ? "GrappleSuplex" : "";
                                            else
                                                expected = back ? "GrappleSuplex" : "GrappleRKO";
                                        } else {
                                            if (has(4) && !has(7) && supported && clear)
                                                expected = "GrapplePounding";
                                            else if (has(6))
                                                expected = supported
                                                               ? (clear ? "GrappleAttackFront" : "GrappleAttackRear")
                                                               : "";
                                            else if (has(0) || has(1))
                                                expected =
                                                    supported ? (clear ? "GrappleGiantSwing" : "GrappleRKO") : "";
                                            else if (has(4))
                                                expected = supported ? "GrappleRKO" : "";
                                            else
                                                expected = back ? "GrappleSuplex" : "GrappleRKO";
                                        }
                                        const auto selected = ConditionFlow::Select(program.nodes, program.entry,
                                            program.predicates.size(), names.size(), [&](int i) {
                                                const auto& p = program.predicates[i];
                                                using T = ConditionFlowXml::Test;
                                                using V = ConditionFlow::Value;
                                                if (p.test == T::kAir) return air ? V::kTrue : V::kFalse;
                                                if (p.test == T::kEquippedType) {
                                                    if (p.leftHand ||
                                                        p.equippedType != ConditionFlowXml::EquippedType::kUnarmed)
                                                        throw std::runtime_error("unexpected equipped type");
                                                    return unarmed ? V::kTrue : V::kFalse;
                                                }
                                                if (p.test == T::kWeaponDrawn) return drawn ? V::kTrue : V::kFalse;
                                                if (p.test == T::kGround) return floor;
                                                if (p.test == T::kSpace) return room;
                                                if (p.test == T::kGlobal) {
                                                    if (p.form != 0x804 || p.equals != 1)
                                                        throw std::runtime_error("unexpected global");
                                                    return failure ? V::kTrue : V::kFalse;
                                                }
                                                if (p.test == T::kMagicEffect) {
                                                    for (unsigned b = 0; b < forms.size(); ++b)
                                                        if (p.form == forms[b]) return has(b) ? V::kTrue : V::kFalse;
                                                }
                                                throw std::runtime_error("unexpected predicate");
                                            });
                                        if (selected.error != ConditionFlow::Error::kNone)
                                            throw std::runtime_error("evaluation failed");
                                        const auto actual =
                                            selected.action < 0 ? std::string{} : names[selected.action];
                                        if (actual != expected)
                                            throw std::runtime_error(host + " mask=" + std::to_string(mask) +
                                                                     " expected=" + expected + " actual=" + actual);
                                        ++cases;
                                    }
        }
        // Every new destination must retain both role variants of all QTE links.
        for (auto n : root.children("Pair"))
            if (!n.child("Start")) {
                unsigned count = 0;
                for (auto link : n.children("Transition")) {
                    if (!link.attribute("partnerTo") || !link.attribute("event"))
                        throw std::runtime_error("lost role transition");
                    ++count;
                }
                if (count != 6) throw std::runtime_error("lost QTE stage links");
            }
        std::cout << "Grapple selection cases passed: " << cases << "; QTE role links preserved\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

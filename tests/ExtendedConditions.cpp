#include "declarations/ConditionFlowXml.h"
#include "runtime/ConditionValues.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AnimationInjector;
void Check(bool v) {
    if (!v) std::abort();
}
bool Read(const std::string& attrs) {
    pugi::xml_document doc;
    doc.load_string(("<Check " + attrs + "/>").c_str());
    ConditionFlowXml::Predicate p;
    std::string error;
    return ConditionFlowXml::ReadCheck(doc.document_element(), p, error);
}
int main() {
    for (const char* value : {"forward", "back", "left", "right", "none"})
        Check(Read(std::string("kind='movementDirection' option='") + value + "'"));
    Check(!Read("kind='movementDirection'"));
    Check(!Read("kind='movementDirection' option='diagonal'"));
    Check(!Read("kind='movementDirection' option='left' value='1'"));
    ConditionValues::Request movement;
    movement.CaptureMovement(2);
    auto pendingMovement = movement;
    pendingMovement.CaptureMovement(3);
    Check(pendingMovement.movementDirection == 2);
    ConditionValues::Request newMovement;
    newMovement.CaptureMovement(3);
    Check(newMovement.movementDirection == 3);
    // An event trigger's sprint flag is taken when the event arrives and survives the queue.
    ConditionValues::Request sprint;
    Check(!sprint.sprintingCaptured);
    sprint.CaptureSprinting(true);
    auto queuedSprint = sprint;
    queuedSprint.CaptureSprinting(false);
    Check(queuedSprint.sprintingCaptured && queuedSprint.sprinting);
    for (const char* kind :
        {"essential", "protected", "blocking", "sprinting", "swimming", "mounted", "lineOfSight", "interior"})
        for (const char* subject : {"self", "partner"})
            Check(Read(std::string("kind='") + kind + "' subject='" + subject + "'"));
    for (const char* kind : {"race", "faction", "keyword", "wornKeyword", "perk", "spell", "location"}) {
        const auto prefix = std::string("kind='") + kind + "'";
        Check(Read(prefix + " plugin='Skyrim.esm' form='123'"));
        Check(!Read(prefix));
        Check(!Read(prefix + " plugin='Skyrim.esm' form='FF123456'"));
    }
    Check(Read("kind='equippedKeyword' option='either' plugin='Skyrim.esm' form='123'"));
    for (const char* value : {"character", "creature", "chicken", "draugr", "wolf"})
        Check(Read(std::string("kind='rig' option='") + value + "'"));
    Check(!Read("kind='rig' option='unknown'"));
    for (const char* value : {"player", "npc", "animal", "creature", "undead", "daedra", "dwarven", "dragon"})
        Check(Read(std::string("kind='actorType' option='") + value + "'"));
    Check(!Read("kind='actorType' option='pc'"));
    for (const char* value : {"ragdoll", "recovering", "bleedingOut", "killmove", "paired", "staggered", "child"})
        Check(Read(std::string("kind='actorStatus' option='") + value + "'"));
    Check(Read("kind='relationship' option='hostile'"));
    Check(Read("kind='relationship' option='teammate'"));
    for (const char* kind : {"distance", "relativeAngle", "heightDifference", "scale", "scaleRatio", "height"})
        for (const char* comparison : {"lt", "le", "eq", "ge", "gt", "range"}) {
            const auto prefix = std::string("kind='") + kind + "' compare='" + comparison + "' value='20'";
            Check(Read(prefix + (std::string_view(comparison) == "range" ? " upper='30'" : "")));
        }
    for (const char* value : {"health", "stamina", "magicka"})
        for (const char* measure : {"current", "percent"})
            Check(Read(std::string("kind='resource' option='") + value + "' measure='" + measure +
                       "' compare='le' value='20'"));
    for (const char* attrs : {"kind='resource' option='health' measure='percent' compare='le' value='101'",
             "kind='relativeAngle' compare='le' value='181'", "kind='distance' compare='range' value='20' upper='10'",
             "kind='distance' compare='le' value='nan'", "kind='distance' compare='le' value='10' upper='20'",
             "kind='alive' compare='le' value='10'", "kind='chance' value='101'", "kind='chance' value='-1'",
             "kind='chance' value='30' compare='le'", "kind='rig' option='character' form='123'",
             "kind='resource' option='health' compare='le' value='20'"})
        Check(!Read(attrs));
    Check(Read("kind='chance' value='0'"));
    Check(Read("kind='chance' value='100'"));
    using ConditionValues::Compare;
    Check(Compare(20, "le", 20, 0));
    Check(!Compare(20, "lt", 20, 0));
    Check(Compare(20, "ge", 20, 0));
    Check(Compare(20, "range", 20, 30));
    Check(Compare(30, "range", 20, 30));
    Check(!Compare(31, "range", 20, 30));
    Check(!Compare(std::numeric_limits<double>::quiet_NaN(), "le", 20, 0));
    ConditionValues::Request request;
    const bool first = request.Chance(1, 2, 30);
    auto queued = request;
    for (int i = 0; i < 100; ++i) Check(queued.Chance(1, 2, 30) == first);
    Check(!queued.Chance(1, 2, 0));
    Check(queued.Chance(1, 2, 100));
    int hits = 0;
    for (int i = 0; i < 10000; ++i) {
        ConditionValues::Request next;
        hits += next.Chance(1, 2, 30);
    }
    Check(hits > 2700 && hits < 3300);

    // 0.12.296: previous action, falling, slope, actor value, game hour, weather.
    Check(Read("kind='previousAction' actions='A,B' within='0.8' ended='completed'"));
    Check(Read("kind='previousAction' actions='A'"));
    Check(!Read("kind='previousAction'"));
    Check(!Read("kind='previousAction' actions='A' within='700'"));
    Check(!Read("kind='previousAction' actions='A' ended='half'"));
    Check(!Read("kind='slope' actions='A' option='any' compare='ge' value='10'"));
    {
        pugi::xml_document doc;
        doc.load_string("<Check kind='previousAction' actions='A, ,B' within='1.5' ended='cancelled'/>");
        ConditionFlowXml::Predicate pr;
        std::string error;
        Check(ConditionFlowXml::ReadCheck(doc.document_element(), pr, error));
        Check(pr.test == ConditionFlowXml::Test::kPreviousAction && pr.actionNames.size() == 3 &&
              pr.actionNames[0] == "A" && pr.within == 1.5f && pr.ended == "cancelled");
    }
    Check(Read("kind='falling' option='falling'"));
    Check(Read("kind='falling' option='rising'"));
    Check(Read("kind='falling' option='falling' compare='ge' value='200'"));
    Check(!Read("kind='falling'"));
    Check(!Read("kind='falling' option='down'"));
    for (const char* option : {"downhill", "uphill", "any"})
        Check(Read(std::string("kind='slope' option='") + option + "' compare='ge' value='25'"));
    Check(!Read("kind='slope' option='any' compare='ge' value='91'"));
    Check(!Read("kind='slope' option='any'"));
    Check(Read("kind='actorValue' option='OneHanded' compare='ge' value='50'"));
    Check(Read("kind='actorValue' option='level' compare='range' value='10' upper='30'"));
    Check(Read("kind='actorValue' option='Health' measure='base' compare='gt' value='100'"));
    Check(!Read("kind='actorValue' option='Luck' compare='ge' value='1'"));
    Check(!Read("kind='actorValue' option='Health' measure='percent' compare='ge' value='1'"));
    Check(Read("kind='gameHour' compare='range' value='20' upper='5'"));
    Check(Read("kind='gameHour' compare='ge' value='18'"));
    Check(!Read("kind='gameHour' compare='ge' value='25'"));
    Check(!Read("kind='slope' option='any' compare='range' value='30' upper='10'"));
    for (const char* option : {"pleasant", "cloudy", "rainy", "snow"})
        Check(Read(std::string("kind='weather' option='") + option + "'"));
    Check(!Read("kind='weather' option='storm'"));
    std::cout << "Extended condition parser, comparisons, request probability: passed\n";
}

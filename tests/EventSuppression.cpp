#include "runtime/EventSuppression.h"
#include "runtime/ActionRegistry.h"
#include "runtime/InputSink.h"
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector;
int main() {
    unsigned checks = 0;
    auto check = [&](bool ok) {
        ++checks;
        if (!ok) throw std::runtime_error("suppression case " + std::to_string(checks));
    };
    try {
        auto* player = RE::PlayerCharacter::GetSingleton();
        RE::Actor npc;
        auto& spec = Actions::specs[0];
        spec.suppress.events = {"blockStart", "bashStart"};
        EventSuppression::Load(Actions::specs);
        check(EventSuppression::Enabled());
        Actions::selected = 1;
        check(!EventSuppression::Consume(player, "unrelated"));
        check(Actions::queries == 0);
        check(!EventSuppression::Consume(&npc, "blockStart"));
        check(Actions::queries == 0);
        check(EventSuppression::Consume(player, "blockStart"));
        check(Actions::queries == 1);
        Actions::selected = -1;
        check(!EventSuppression::Consume(player, "bashStart"));
        EventSuppression::BeginInput();
        EventSuppression::RecordRequest(0, 1, true);
        Actions::runners[1] = {true, 0};
        check(EventSuppression::Consume(player, "bashStart"));
        EventSuppression::RecordRequest(0, 1, false);
        check(EventSuppression::Consume(player, "bashStart"));
        EventSuppression::EndInput();
        check(!EventSuppression::Consume(player, "bashStart"));
        spec.suppress.when = SuppressSpec::When::kActive;
        check(EventSuppression::Consume(player, "bashStart"));  // Active origin follows the Flow-selected action.
        Actions::runners[1].active = false;
        check(!EventSuppression::Consume(player, "bashStart"));
        Actions::selected = 1;
        check(!EventSuppression::Consume(player, "bashStart"));
        spec.suppress.when = SuppressSpec::When::kBoth;
        check(EventSuppression::Consume(player, "bashStart"));
        EventSuppression::BeginInput();
        EventSuppression::RecordRequest(0, 1, false);
        check(!EventSuppression::Consume(player, "blockStart"));
        EventSuppression::EndInput();
        Actions::runners[1].active = true;
        player->dead = true;
        check(!EventSuppression::Consume(player, "blockStart"));
        player->dead = false;
        player->ragdoll = true;
        check(!EventSuppression::Consume(player, "blockStart"));
        player->ragdoll = false;
        player->loaded = false;
        check(!EventSuppression::Consume(player, "blockStart"));
        player->loaded = true;
        RE::UI::GetSingleton()->paused = true;
        check(!EventSuppression::Consume(player, "blockStart"));
        RE::UI::GetSingleton()->paused = false;
        RE::UI::GetSingleton()->console = true;
        check(!EventSuppression::Consume(player, "blockStart"));
        RE::UI::GetSingleton()->console = false;
        EventSuppression::BeginInput();
        EventSuppression::RecordRequest(0, 1, true);
        EventSuppression::Load(Actions::specs);
        spec.suppress.when = SuppressSpec::When::kStartable;
        Actions::selected = -1;
        check(!EventSuppression::Consume(player, "blockStart"));
        Actions::chance = true;
        Actions::selected = 1;
        const int before = Actions::queries;
        check(!EventSuppression::Consume(player, "blockStart"));
        check(Actions::queries == before);
        EventSuppression::BeginInput();
        EventSuppression::RecordRequest(0, 1, true);
        Actions::runners[1] = {true, 0};
        check(EventSuppression::Consume(player, "blockStart"));
        EventSuppression::EndInput();
        Actions::chance = false;
        // An NPC (10-29): a start on blockHitStart keeps that event from its vanilla graph while it can start.
        Actions::runners[1] = {false, -1};
        spec.trigger.onEvent = "blockHitStart";
        spec.suppress = {SuppressSpec::When::kStartable, {"blockHitStart", "blockStart"}};
        EventSuppression::Load(Actions::specs);
        Actions::selected = 1;
        check(EventSuppression::Consume(&npc, "blockHitStart"));
        // Its other blocked event is the player's control: an NPC still blocks with it.
        check(!EventSuppression::Consume(&npc, "blockStart"));
        check(EventSuppression::Consume(player, "blockStart"));
        // Not startable for this NPC, or a chance to roll: the vanilla block hit plays.
        Actions::selected = -1;
        check(!EventSuppression::Consume(&npc, "blockHitStart"));
        Actions::selected = 1;
        Actions::chance = true;
        check(!EventSuppression::Consume(&npc, "blockHitStart"));
        Actions::chance = false;
        // A start for the player only (actor="player") leaves an NPC's event alone.
        spec.trigger.onEventNpc = false;
        EventSuppression::Load(Actions::specs);
        check(!EventSuppression::Consume(&npc, "blockHitStart"));
        check(EventSuppression::Consume(player, "blockHitStart"));
        // While an NPC plays the action, the action's block holds for it too.
        spec.suppress.when = SuppressSpec::When::kActive;
        Actions::selected = -1;
        check(!EventSuppression::Consume(&npc, "blockStart"));
        Actions::runners[1] = {true, 0};
        check(EventSuppression::Consume(&npc, "blockStart"));
        npc.dead = true;
        check(!EventSuppression::Consume(&npc, "blockStart"));
        npc.dead = false;
        Actions::runners[1] = {false, -1};
        spec.trigger = {};
        spec.suppress.events.clear();
        EventSuppression::Load(Actions::specs);
        check(!EventSuppression::Enabled());
        std::cout << "Production suppression dispatcher checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

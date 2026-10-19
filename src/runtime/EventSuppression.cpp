#include "PCH.h"
#include "runtime/EventSuppression.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/SuppressionPolicy.h"
#include <unordered_map>

namespace AnimationInjector::EventSuppression {
namespace {
struct Rule {
    int action;
    RE::BSFixedString event;
};
std::vector<Rule> rules;
// Rules by interned event text (BSFixedString equality is identity), in rule order.
std::unordered_map<const char*, std::vector<std::size_t>> byEvent;
thread_local InputAcceptance input;
}
void Load(const std::vector<ActionSpec>& specs) {
    rules.clear();
    byEvent.clear();
    for (int a = 0; a < static_cast<int>(specs.size()); ++a)
        for (const auto& event : specs[a].suppress.events) rules.push_back({a, RE::BSFixedString(event)});
    for (std::size_t i = 0; i < rules.size(); ++i)
        if (!rules[i].event.empty()) byEvent[rules[i].event.c_str()].push_back(i);
    input.Reset(static_cast<int>(specs.size()));
}
bool Enabled() { return !rules.empty(); }
void BeginInput() { input.Begin(Actions::Count()); }
void EndInput() { input.End(); }
void RecordRequest(int source, int destination, bool accepted) { input.Record(source, destination, accepted); }
bool Consume(RE::Actor* actor, const RE::BSFixedString& event) {
    if (!actor || actor != RE::PlayerCharacter::GetSingleton() || !event.c_str()) return false;
    // Most animation notifications never enter any condition evaluator.
    const auto found = event.empty() ? byEvent.end() : byEvent.find(event.c_str());
    if (found == byEvent.end()) return false;
    auto* ui = RE::UI::GetSingleton();
    if (!actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() || !ui || ui->GameIsPaused() ||
        ui->IsMenuOpen(RE::Console::MENU_NAME))
        return false;
    for (const std::size_t index : found->second) {
        const auto& rule = rules[index];
        const auto& spec = Actions::Spec(rule.action);
        const auto when = spec.suppress.when;
        const auto consumed = [&](const char* reason, int destination) {
            logger::debug("[Suppress] event='{}' source={} destination={} reason={}", event.c_str(), rule.action,
                destination, reason);
            return true;
        };
        if (when != SuppressSpec::When::kStartable) {
            int active = -1;
            if (Actions::FindRunner(actor, [&](int i, ActionRunner& runner) {
                    active = i;
                    return runner.IsActive() && (i == rule.action || runner.RequestOrigin() == rule.action);
                }))
                return consumed("active", active);
        }
        if (when == SuppressSpec::When::kActive) continue;
        const auto accepted = input.Result(rule.action);
        if (accepted != InputAcceptance::unseen) {
            if (accepted >= 0) {
                auto runner = Actions::RunnerRef(actor, accepted);
                if (runner && runner->IsActive()) return consumed("accepted-input", accepted);
            }
            continue;
        }
        // Without an accepted input request, a speculative roll must not swallow
        // a vanilla event. Accepted requests were handled above with their result.
        if (Actions::HasChance(rule.action)) continue;
        // The same read-only preflight is used immediately before keyboard starts.
        if (const int selected = Actions::SelectStart(actor, rule.action); selected >= 0)
            return consumed("startable", selected);
    }
    return false;
}
}

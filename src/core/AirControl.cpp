#include "PCH.h"
#include "core/AirControl.h"
#include "core/NodeFactory.h"
#include "runtime/ActionRegistry.h"
#include "Settings.h"
#include <mutex>
#include <string_view>

namespace AnimationInjector::AirControl {
namespace {
struct Route {
    RE::hkRefPtr<RE::hkbBehaviorGraphData> data;
    std::string machine;
    int action = -1;
    std::vector<std::int32_t> states;
    std::int32_t fall = -1, directionalFall = -1;
    unsigned logged = 0;
};
std::mutex g_routeMutex;
std::vector<Route> g_routes;
thread_local unsigned g_resumeDepth = 0;

bool HoldFall(RE::hkbStateMachine* a_machine, const RE::hkbContext& a_context, std::int32_t a_event) {
    if (g_resumeDepth || a_event < 0 || !a_machine || !a_context.behavior || !a_context.behavior->data) return false;
    const auto* name = a_machine->name.c_str();
    if (!name) return false;
    const std::string_view machineName{name};
    std::lock_guard lock(g_routeMutex);
    for (auto& route : g_routes) {
        if (route.data.get() != a_context.behavior->data.get() ||
            (a_event != route.fall && a_event != route.directionalFall) || route.machine != machineName ||
            std::find(route.states.begin(), route.states.end(), a_machine->currentStateID) == route.states.end())
            continue;
        if (FrameworkSettings::Get().debug && route.logged < 16) {
            ++route.logged;
            logger::debug("[AirControl] hold fall action={} state={} event={} directional={}", route.action,
                a_machine->currentStateID, a_event, a_event == route.directionalFall);
        }
        return true;
    }
    return false;
}

struct HandleEvent {
    static void thunk(RE::hkbStateMachine* a_machine, const RE::hkbContext& a_context, const RE::hkbEvent a_event) {
        // Filter only this injected state machine. Other graphs still receive
        // the event, and all landing/interruption events reach the original.
        if (!HoldFall(a_machine, a_context, static_cast<std::int32_t>(a_event.id)))
            original(a_machine, a_context, a_event);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};

}

void RegisterRoute(RE::hkbBehaviorGraph* a_graph, RE::hkbStateMachine* a_machine, int a_action,
    const std::vector<std::int32_t>& a_states) {
    if (!a_graph || !a_graph->data || !a_graph->data->stringData || !a_machine || !a_machine->name.c_str()) return;
    auto& events = a_graph->data->stringData->eventNames;
    Route route;
    route.data = a_graph->data;
    route.machine = a_machine->name.c_str();
    route.action = a_action;
    route.states = a_states;
    route.fall = NodeFactory::FindName(events, "JumpFall");
    route.directionalFall = NodeFactory::FindName(events, "JumpFallDirectional");
    // An explicitly declared exit takes precedence over the automatic hold.
    const int exit = NodeFactory::FindName(events, Actions::Spec(a_action).events.exit.c_str());
    if (route.fall == exit) route.fall = -1;
    if (route.directionalFall == exit) route.directionalFall = -1;
    logger::info("[AirControl] registered action={} states={} fall={} directionalFall={}", a_action,
        route.states.size(), route.fall, route.directionalFall);
    std::lock_guard lock(g_routeMutex);
    for (auto& old : g_routes) {
        if (old.data.get() == route.data.get() && old.machine == route.machine && old.action == route.action) {
            old = std::move(route);
            return;
        }
    }
    g_routes.push_back(std::move(route));
}

void Install() {
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkbStateMachine[0]};
    HandleEvent::original = table.write_vfunc(0x6, HandleEvent::thunk);
    logger::info("[AirControl] state-local fall filter installed (independent of debug)");
}

bool ResumeFalling(RE::Actor* a_actor) {
    if (!a_actor || !a_actor->Is3DLoaded()) return false;
    struct ResumeScope {
        ResumeScope() { ++g_resumeDepth; }
        ~ResumeScope() { --g_resumeDepth; }
    } scope;
    static RE::BSFixedString fall{"JumpFall"};
    return a_actor->NotifyAnimationGraph(fall);
}

void ResetOnLoad() {
    std::lock_guard lock(g_routeMutex);
    for (auto& route : g_routes) route.logged = 0;
}
}

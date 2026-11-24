#include "runtime/Signals.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/BodyPart.h"
#include "runtime/PartRunner.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace AnimationInjector::Signals {

namespace {
struct Table {
    std::vector<std::string> variables;
    std::vector<int> ofAction;  // action -> index into variables, or -1
};
Table g_table;
std::once_flag g_built;

const Table& Built() {
    std::call_once(g_built, [] {
        Table t;
        const int count = Actions::Count();
        t.ofAction.assign(static_cast<std::size_t>(count), -1);
        // Graph variable names compare without case: MyBow_Charge and mybow_charge are one variable.
        std::unordered_map<std::string, int> byName;
        for (int a = 0; a < count; ++a) {
            const auto& name = Actions::Spec(a).signal;
            if (name.empty()) continue;
            std::string key = name;
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto [it, added] = byName.try_emplace(key, static_cast<int>(t.variables.size()));
            if (added) t.variables.push_back(name);
            t.ofAction[static_cast<std::size_t>(a)] = it->second;
        }
        if (!t.variables.empty())
            logger::info("[Signal] {} OAR signal variables declared ({} actions carry one)", t.variables.size(),
                std::count_if(t.ofAction.begin(), t.ofAction.end(), [](int i) { return i >= 0; }));
        g_table = std::move(t);
    });
    return g_table;
}

// The variables as engine strings, made once on the main thread (global rule 4-3, principle 3).
std::vector<RE::BSFixedString> g_fixed;

// An actor a signal action started on. `written`: what the actor's graph holds for each variable (as last written
// successfully); a variable never written is false (the graph's initial value).
struct Watched {
    RE::ActorHandle handle;
    RE::FormID id = 0;
    std::vector<std::uint8_t> written;
    std::vector<std::uint8_t> refusedLogged;
};
std::vector<Watched> g_watched;  // main thread
std::vector<std::uint8_t> g_wanted;
}  // namespace

const std::vector<std::string>& Variables() { return Built().variables; }

int SignalOf(int action) {
    const auto& t = Built();
    return action >= 0 && action < static_cast<int>(t.ofAction.size()) ? t.ofAction[static_cast<std::size_t>(action)]
                                                                       : -1;
}

void Touch(RE::Actor* actor) {
    if (!actor) return;
    const auto id = actor->GetFormID();
    for (const auto& w : g_watched)
        if (w.id == id) return;
    const auto count = Variables().size();
    g_watched.push_back(
        {actor->GetHandle(), id, std::vector<std::uint8_t>(count, 0), std::vector<std::uint8_t>(count, 0)});
}

void Update() {
    if (g_watched.empty()) return;
    const auto& variables = Variables();
    if (g_fixed.size() != variables.size()) {
        g_fixed.clear();
        for (const auto& v : variables) g_fixed.emplace_back(v.c_str());
    }
    for (auto it = g_watched.begin(); it != g_watched.end();) {
        auto& w = *it;
        const auto actor = w.handle.get();
        // Gone (deleted, or its reference reused): its graph went with it.
        if (!actor || actor->GetFormID() != w.id) {
            it = g_watched.erase(it);
            continue;
        }
        g_wanted.assign(variables.size(), 0);
        bool any = false;
        const auto mark = [&](int action) {
            if (const int s = SignalOf(action); s >= 0) {
                g_wanted[static_cast<std::size_t>(s)] = 1;
                any = true;
            }
        };
        ActionRunner* runner = nullptr;
        for (int a = Actions::NextRunner(actor.get(), -1, &runner); a >= 0;
            a = Actions::NextRunner(actor.get(), a, &runner))
            if (runner && runner->IsActive()) mark(a);
        // A part action waiting for its layer's crossing plays next: its signal holds through the wait.
        for (std::size_t layer = 0; layer < kBodyLayers; ++layer) {
            mark(PartRunner::Playing(actor.get(), static_cast<int>(layer)));
            mark(PartRunner::Queued(actor.get(), static_cast<int>(layer)));
        }
        if (actor->Is3DLoaded())
            for (std::size_t i = 0; i < variables.size(); ++i) {
                if (g_wanted[i] == w.written[i]) continue;
                const bool on = g_wanted[i] != 0;
                if (actor->SetGraphVariableBool(g_fixed[i], on)) {
                    w.written[i] = g_wanted[i];
                    logger::info("[Signal] {:08X} {} = {}", w.id, variables[i], on);
                } else {
                    // The active graph lacks it (the player in first person): tried again each frame.
                    if (!std::exchange(w.refusedLogged[i], std::uint8_t{1}))
                        logger::info(
                            "[Signal] {:08X} {} = {} refused by the active graph; tried again", w.id, variables[i], on);
                }
            }
        const bool holds = std::any_of(w.written.begin(), w.written.end(), [](std::uint8_t v) { return v != 0; });
        // Nothing plays and nothing written true is left to undo. One still true (unloaded, or refused) is kept until it
        // is written false (the player's graph outlives a cell change) or the actor is gone.
        if (!any && !holds)
            it = g_watched.erase(it);
        else
            ++it;
    }
}

void Reset() {
    if (!g_watched.empty()) logger::info("[Signal] load: {} watched actors forgotten", g_watched.size());
    g_watched.clear();
}

}  // namespace AnimationInjector::Signals

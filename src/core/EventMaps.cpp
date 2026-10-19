#include "PCH.h"
#include "core/EventMaps.h"
#include "RE/B/BShkbAnimationGraph.h"
#include "runtime/PairedRig.h"
#include "runtime/ActionRegistry.h"
#include "core/NameIndex.h"
#include "core/FullBodyEntryNames.h"
#include "core/PairEntryNames.h"
#include <unordered_set>

namespace AnimationInjector::EventMaps {

namespace {
struct StrHash {
    std::uint32_t operator()(char* a) const noexcept {
        return RE::BSCRC32<std::string_view>()(std::string_view(a ? a : ""));
    }
};
struct StrEq {
    bool operator()(char* a, char* b) const noexcept { return a && b && _stricmp(a, b) == 0; }
};
using StrMap = RE::BSTHashMap<char*, std::int32_t, StrHash, StrEq>;

// A project DB already handled, keyed by its address. The address alone is no proof: a creature
// project DB freed on unload can be reallocated at the same address, and a stale entry then skipped
// the new DB's registration (code review 2026-09-23 R-11). Each visit checks the identity again:
// one event key this module added must still be in the map, or, when none was added, the project
// name must be the same.
struct Handled {
    char* added = nullptr;  // an interned key this module inserted
    bool pointerMode = true;
    std::string project;
};
std::unordered_map<const void*, Handled> g_done;
std::unordered_map<std::string, RE::BSFixedString> g_fixed;  // 인턴 포인터를 살려 두기 위해 보관

int FindEventName(RE::hkArray<RE::hkStringPtr>& a_names, const char* a_name) {
    for (int i = 0; i < a_names._size; ++i) {
        const char* s = a_names._data[i].c_str();
        if (s && _stricmp(s, a_name) == 0) return i;
    }
    return -1;
}

// The project's variable lookup must resolve `names` (graph variables this module added) for
// SetGraphVariable*/GetGraphVariable*. Each is added when missing, with its index in the master
// graph; the reverse name array is extended only when it is the unchanged sequence. false when the
// lookup's key mode cannot be told or a name is not a master variable.
bool EnsureVariableLookup(RE::BShkbAnimationGraph* graph, std::initializer_list<const char*> names) {
    auto* db = graph ? graph->projectDBData : nullptr;
    auto* data = graph && graph->behaviorGraph ? graph->behaviorGraph->data.get() : nullptr;
    if (!db || !data || !data->stringData) return false;
    auto& variables = data->stringData->variableNames;
    static const RE::BSFixedString probe("bAnimationDriven");
    char* probeKey = const_cast<char*>(probe.data());
    auto& native = db->unk70;
    auto& strings = *reinterpret_cast<StrMap*>(&native);
    const bool pointerMode = native.find(probeKey) != native.end();
    if (!pointerMode && strings.find(probeKey) == strings.end()) return false;
    for (const char* name : names) {
        const int index = FindEventName(variables, name);
        if (index < 0) return false;
        bool present = false;
        for (const auto& entry : native)
            if (entry.first && _stricmp(entry.first, name) == 0) {
                if (entry.second != index) return false;
                present = true;
                break;
            }
        if (present) continue;
        auto fixed = g_fixed.try_emplace(name, RE::BSFixedString(name)).first;
        char* key = const_cast<char*>(fixed->second.data());
        if (pointerMode)
            native.emplace(key, index);
        else
            strings.emplace(key, index);
        bool reverseMatches = db->unkE8.size() == static_cast<std::uint32_t>(index);
        for (std::uint32_t i = 0; reverseMatches && i < db->unkE8.size(); ++i)
            reverseMatches =
                db->unkE8[i] && variables._data[i].c_str() && _stricmp(db->unkE8[i], variables._data[i].c_str()) == 0;
        if (reverseMatches) db->unkE8.push_back(key);
        logger::info("[EventMap] variable lookup added '{}'={} reverseExtended={}", name, index, reverseMatches);
    }
    return true;
}

bool EnsureCreatureVariables(RE::BShkbAnimationGraph* graph, const PairedRig::Profile& profile) {
    auto* db = graph ? graph->projectDBData : nullptr;
    auto* data = graph && graph->behaviorGraph ? graph->behaviorGraph->data.get() : nullptr;
    if (!db || !data || !data->stringData) return false;
    auto& names = data->stringData->variableNames;
    static const RE::BSFixedString probe("bAnimationDriven");
    char* probeKey = const_cast<char*>(probe.data());
    auto& native = db->unk70;
    auto& strings = *reinterpret_cast<StrMap*>(&native);
    const bool pointerMode = native.find(probeKey) != native.end();
    const bool stringMode = !pointerMode && strings.find(probeKey) != strings.end();
    if (!pointerMode && !stringMode) return false;
    // Verify this is the variable map with unchanged native indices before
    // extending it. No offsets, indices, or another project's map are copied.
    for (const char* name : profile.probes) {
        const int index = FindEventName(names, name);
        bool matched = false;
        for (const auto& entry : native)
            if (entry.first && _stricmp(entry.first, name) == 0 && entry.second == index && index >= 0) matched = true;
        if (!matched) return false;
    }
    for (const char* name : {"bIsSynced", "DisablePitch"}) {
        const int index = FindEventName(names, name);
        if (index < 0) return false;
        bool present = false;
        for (const auto& entry : native)
            if (entry.first && _stricmp(entry.first, name) == 0) {
                if (entry.second != index) return false;
                present = true;
                break;
            }
        if (present) continue;
        auto fixed = g_fixed.try_emplace(name, RE::BSFixedString(name)).first;
        char* key = const_cast<char*>(fixed->second.data());
        if (pointerMode)
            native.emplace(key, index);
        else
            strings.emplace(key, index);
        // The reverse-name array is optional for lookup. Extend it only when
        // every existing entry proves it is the same variable-name sequence.
        bool reverseMatches = db->unkE8.size() == static_cast<std::uint32_t>(index);
        for (std::uint32_t i = 0; reverseMatches && i < db->unkE8.size(); ++i)
            reverseMatches =
                db->unkE8[i] && names._data[i].c_str() && _stricmp(db->unkE8[i], names._data[i].c_str()) == 0;
        if (reverseMatches) db->unkE8.push_back(key);
        logger::info(
            "[PairRig] Creature variable lookup added '{}'={} reverseExtended={}", name, index, reverseMatches);
    }
    return true;
}
}

void EnsureAllLoaded(const std::vector<std::string>& a_names) {
    auto* pl = RE::ProcessLists::GetSingleton();
    if (!pl) return;
    for (auto& h : pl->highActorHandles) {
        auto a = h.get();
        if (a && a.get()->Is3DLoaded()) Ensure(a.get(), a_names);
    }
}

void Ensure(RE::Actor* p, const std::vector<std::string>& a_names) {
    if (!p) return;
    RE::BSAnimationGraphManagerPtr mgr;
    if (!p->GetAnimationGraphManager(mgr) || !mgr) return;
    auto& rd = mgr->GetRuntimeData();
    if (rd.activeGraph >= mgr->graphs.size()) return;
    auto& g = mgr->graphs[rd.activeGraph];
    if (!g) return;
    auto* db = g->projectDBData;
    auto* master = g->behaviorGraph;
    if (!db || !master) return;
    const std::string projectName = g->projectName.c_str() ? g->projectName.c_str() : "";
    auto& pm = db->unkA0;
    auto& sm = *reinterpret_cast<StrMap*>(&db->unkA0);
    if (const auto it = g_done.find(db); it != g_done.end()) {
        const auto& h = it->second;
        const bool same = h.added ? (h.pointerMode ? pm.find(h.added) != pm.end() : sm.find(h.added) != sm.end())
                                  : h.project == projectName;
        if (same) return;
        logger::info("[EventMap] project DB at {:X} was replaced (now '{}'); registering again",
            reinterpret_cast<std::uintptr_t>(db), projectName);
        g_done.erase(it);
    }
    auto* data = master->data.get();
    if (!data || !data->stringData.get()) return;
    auto& names = data->stringData->eventNames;

    auto* setup = g->characterInstance.setup.get();
    auto* characterData = setup ? setup->data.get() : nullptr;
    auto* characterStrings = characterData ? characterData->stringData.get() : nullptr;
    const char* project = characterStrings ? characterStrings->name.c_str() : nullptr;
    const auto rig = project ? PairedRig::CharacterName(project) : PairedRig::Kind::kUnsupported;
    const auto* profile = PairedRig::Find(rig);
    bool needsCreatureVariables = false;
    if (PairedRig::IsCreature(rig))
        for (int i = 0; i < Actions::Count(); ++i) {
            const auto& spec = Actions::Spec(i);
            if (spec.paired.enabled && PairedRig::SharesAssets(spec.paired.targetRig, rig) &&
                PairedRig::Equal(PairedRig::Find(spec.paired.targetRig)->root, profile->root))
                needsCreatureVariables = true;
        }
    if (needsCreatureVariables && !EnsureCreatureVariables(g.get(), *profile)) {
        logger::error("[PairRig] {} variable lookup validation failed; native start remains gated by graph-state reads",
            profile->name);
        return;
    }
    const char* probe = profile ? profile->eventProbe : "weaponSwing";
    auto fixedProbe = g_fixed.try_emplace(probe, RE::BSFixedString(probe)).first;
    char* probeKey = const_cast<char*>(fixedProbe->second.data());
    const bool ptrMode = pm.find(probeKey) != pm.end();
    const bool strMode = !ptrMode && sm.find(probeKey) != sm.end();
    if (!ptrMode && !strMode) {
        logger::warn(
            "[EventMap] native event lookup failed project='{}' character='{}' probe='{}'; registration skipped",
            g->projectName.c_str() ? g->projectName.c_str() : "?", project ? project : "?", probeKey);
        g_done[db] = {nullptr, true, projectName};
        return;
    }

    // The shared full-body entry's variables (core/FullBodyEntry.h): character graphs that have them.
    if (rig == PairedRig::Kind::kCharacter &&
        FindEventName(data->stringData->variableNames, FullBodyEntry::kStateVariable) >= 0 &&
        !EnsureVariableLookup(g.get(), {FullBodyEntry::kStateVariable, FullBodyEntry::kBlendVariable}))
        logger::error("[FullBodyEntry] project '{}' variable lookup could not be extended; shared entries will not "
                      "select the action",
            projectName);
    // The shared pair entry's variables (core/PairEntry.h), likewise.
    if (rig == PairedRig::Kind::kCharacter &&
        FindEventName(data->stringData->variableNames, PairEntry::kStateVariable) >= 0 &&
        !EnsureVariableLookup(g.get(), {PairEntry::kStateVariable, PairEntry::kExitBlendVariable,
                                           PairEntry::kLossBlendVariable, PairEntry::kRecoveryBlendVariable}))
        logger::error("[PairEntry] project '{}' variable lookup could not be extended; shared pair entries will not "
                      "select the pair",
            projectName);

    // The engine builds a project's map from its master graph's events. When the map already covers
    // every master event (ours included, registered there at load), nothing can be added: checking
    // 40,000 declared names one by one took ~70 ms on the main thread when such a project first
    // appeared mid-game.
    if (names._size > 0 && db->unkA0.size() >= static_cast<std::size_t>(names._size) &&
        db->unkD0.size() >= static_cast<std::size_t>(names._size)) {
        const char* last = names._data[names._size - 1].c_str();
        auto lastFixed = g_fixed.try_emplace(last ? last : "", RE::BSFixedString(last ? last : "")).first;
        char* lastKey = const_cast<char*>(lastFixed->second.data());
        if (last && *last && (ptrMode ? pm.find(lastKey) != pm.end() : sm.find(lastKey) != sm.end())) {
            logger::info(
                "[EventMap] project='{}' map already holds its master graph's {} events", projectName, names._size);
            g_done[db] = {nullptr, ptrMode, projectName};
            return;
        }
    }
    int added = 0, existed = 0, missing = 0;
    char* firstAdded = nullptr;
    // One pass over each table, then a lookup per name. Scanning both tables for every declared name
    // grew with the square of the declared events (0.12.208). Same matches as those scans: the
    // first master name equal ignoring case, and any map key equal ignoring case (keys added here
    // included, so a repeated name counts as existing).
    NameIndex masterNames;
    masterNames.Reset(names._size, [&](int i) { return names._data[i].c_str(); });
    std::unordered_set<std::string, NameHash, NameEqual> present;
    for (const auto& e : db->unkA0)
        if (e.first) present.emplace(e.first);
    for (const auto& nm : a_names) {
        const int id = masterNames.Find(nm.c_str());
        if (id < 0) {
            // Projects the actions are not injected into do not have their events (0.12.209).
            if (++missing <= 3) logger::debug("[EventMap] '{}' 가 마스터 eventNames 에 없음", nm);
            continue;
        }
        auto it = g_fixed.find(nm);
        if (it == g_fixed.end()) it = g_fixed.emplace(nm, RE::BSFixedString{nm.c_str()}).first;
        char* key = const_cast<char*>(it->second.data());
        const bool have = !present.emplace(nm).second;
        if (!have) {
            if (ptrMode)
                db->unkA0.emplace(key, id);
            else
                sm.emplace(key, id);
            ++added;
            if (!firstAdded) firstAdded = key;
        } else
            ++existed;
        const auto n = db->unkD0.size();
        if (static_cast<int>(n) == id)
            db->unkD0.push_back(key);
        else if (id > static_cast<int>(n))
            logger::warn("[EventMap] unkD0 크기 {} < id {} — 태그 배열에 빈칸", n, id);
    }
    logger::info("[EventMap] project='{}' 이벤트 {}개 등록 (추가 {}, 기존 {}, 그래프에 없음 {}) 키방식={}",
        g->projectName.c_str() ? g->projectName.c_str() : "", a_names.size(), added, existed, missing,
        ptrMode ? "포인터" : "문자열");
    g_done[db] = {firstAdded, ptrMode, projectName};
}

}  // namespace AnimationInjector::EventMaps

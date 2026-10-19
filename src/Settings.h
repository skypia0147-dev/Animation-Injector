#pragma once
#include "PCH.h"
#include "runtime/ActionSpec.h"

namespace AnimationInjector {

// 프레임워크 설정: Data\SKSE\Plugins\AnimationInjector.ini
struct FrameworkSettings {
    // [Inject]
    bool injectState = true;          // false = 자가검증 로그만, 삽입 없음
    std::int32_t stateIdBase = 7700;  // First injected stateId; each machine hands out free ids from here
    int wildcardPriority = 5000;      // 진입 와일드카드·상속 전이 우선순위 (MCO 5000). 공격 중첩 상태 캔슬에 필요
    bool convertAnnotations = true;   // HKX 주석 → 트리거/이벤트 (슬롯 모델 전제)
    // [Display]
    bool loadingProgress = true;
    // [Preload] (core/AnimationPreloader.h)
    bool preloadOrdinary = true;      // ordinary action animations too, not only paired ones
    int preloadOrdinaryBudgetMB = 0;  // their file size per project; 0 = no limit
    // [Compatibility]
    bool fireArrow = true;
    int nativePairAlignment = 2;  // 0 off, 1 Animation Injector pairs only, 2 every native paired scene
    int pairEndPosition = 2;  // end position (core/NativePairEnd.h): 0 off, 1 Animation Injector pairs, 2 every pair
    // [Debug]
    bool debug = false;
    bool spaceProbe = false;
    bool ragdollTrace = false;  // Carry physics trace for HKX_Edit calibration (core/RagdollTrace).

    static FrameworkSettings& Get();
    void Load();
};

// 액션 선언 로더 — 지금은 폴더 INI (Data\SKSE\Plugins\AnimationInjector\<모드>\*.ini). M1 에서 XML 로 교체.
namespace Declarations {
// `events`: also collects every file's event registry entries (runtime/EventRegistrySpec.h), in file
// name order.
std::vector<ActionSpec> LoadAll(std::vector<EventRuleSpec>* events = nullptr);
// LoadAll over any folder laid out like Data/SKSE/Plugins/AnimationInjector (offline scale bench).
std::vector<ActionSpec> LoadAllFrom(const std::filesystem::path& root, std::vector<EventRuleSpec>* events = nullptr);
// Shared entry point for the plugin and offline declaration verification.
bool LoadXmlFile(const std::filesystem::path& file, std::vector<ActionSpec>& output);
bool LoadXmlFile(
    const std::filesystem::path& file, std::vector<ActionSpec>& output, std::vector<EventRuleSpec>& events);
// Key name resolver used by the plugin. Unknown names return UINT32_MAX; tests must use
// this instead of a stand-in so validation sees the same values as the game.
std::uint32_t ResolveKeyName(const char* name);
// A path as UTF-8 text, for logs and declaration sources. path::string() converts to the ANSI code
// page and throws for a character that page cannot hold (a Korean mod folder on an English Windows).
std::string PathText(const std::filesystem::path& path);
}

}  // namespace AnimationInjector

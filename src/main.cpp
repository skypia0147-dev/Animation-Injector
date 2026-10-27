#include "core/AnimationFileLifetime.h"
#include "core/PairRootScale.h"
#include "core/CarryPoseLock.h"
#include "core/PairRootRuntime.h"
#include "core/NativePairEnd.h"
#include "PCH.h"
#include "Version.h"
#include "core/ExitBlendHooks.h"
#include "core/PairInputGate.h"
#include "runtime/PairFeatures.h"
#include "runtime/EventEffects.h"
#include "runtime/ActionHistory.h"
#include "runtime/EventRegistry.h"
#include "runtime/ActionLinks.h"
#include "Settings.h"
#include "core/GraphInjector.h"
#include "core/ClipHooks.h"
#include "core/MotionHook.h"
#include "core/AnimDataRegistry.h"
#include "core/GraphEventHooks.h"
#include "core/AirControl.h"
#include "core/PlayerHooks.h"
#include "core/HitDispatch.h"
#include "core/PairedStartHooks.h"
#include "core/AnimationPreloader.h"
#include "core/PairedClipBinding.h"
#include "core/LoadingProgress.h"
#include "core/AnimationLoadObserver.h"
#include "ui/LoadingMenu.h"
#include "ui/QteMenu.h"
#include "runtime/ActionRegistry.h"
#include "runtime/MainThread.h"
#include "runtime/InputSink.h"
#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "runtime/CarryHold.h"
#include "runtime/PairStartCamera.h"
#include "runtime/ActorImmunity.h"
#include "runtime/AnnotationCommands.h"
#include <spdlog/sinks/basic_file_sink.h>

// Runtime behavior injection, XML declarations and actor action lifecycles.
SKSEPluginInfo(.Version = {AI_VERSION_MAJOR, AI_VERSION_MINOR, AI_VERSION_PATCH, 0}, .Name = "AnimationInjector",
    .Author = "Smooth", .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace {
// spdlog opens its file through a narrow path in the ANSI code page, and path::string() throws for a
// character that page cannot hold (a Documents folder under a user name in another script). Such a
// folder is opened through its 8.3 short form; without one there is no log file, and the game goes on.
std::optional<std::string> AnsiPath(const std::filesystem::path& dir, const char* file) {
    try {
        return (dir / file).string();
    } catch (const std::system_error&) {
    }
    std::wstring shortDir(MAX_PATH, L'\0');
    const auto length = ::GetShortPathNameW(dir.c_str(), shortDir.data(), static_cast<DWORD>(shortDir.size()));
    if (!length || length >= shortDir.size()) return std::nullopt;
    shortDir.resize(length);
    try {
        return (std::filesystem::path(shortDir) / file).string();
    } catch (const std::system_error&) {
        return std::nullopt;
    }
}
void SetupLog() {
    auto dir = SKSE::log::log_directory();
    if (!dir) return;
    // Preserve one previous run for regression reports.
    std::error_code logCopyError;
    std::filesystem::copy_file(*dir / "AnimationInjector.log", *dir / "AnimationInjector.previous.log",
        std::filesystem::copy_options::overwrite_existing, logCopyError);
    const auto path = AnsiPath(*dir, "AnimationInjector.log");
    if (!path) return;
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(*path, true);
    auto lg = std::make_shared<spdlog::logger>("log", std::move(sink));
    spdlog::set_default_logger(std::move(lg));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}
// The previous session ends before a load and before a new game (kNewGame comes without kPreLoadGame):
// pair features release their leases (immunity, spells, controls, time), a carry releases the
// target, head tracking and its start control, and every per-session table is dropped.
void EndSession() {
    using namespace AnimationInjector;
    Actions::BeforeGameLoad();
    ActionLinks::ResetOnLoad();
    PairFeatures::End(PairedSession::ReadStatus().ticket, false);
    PairFeatures::ResetOnLoad();
    EventRegistry::ResetOnLoad();
    AnnotationCommands::ResetOnLoad();
    EventEffects::ResetOnLoad();
    ActionHistory::ResetOnLoad();
    PairedSession::ResetOnLoad();
    PairRootScale::Reset();
    // Every holder has let go above; a lease still held would keep its actor a ghost in the next session.
    if (const auto left = ActorImmunity::ResetOnLoad())
        logger::warn("[Immunity] {} leases were still held at the session end; released", left);
}
// Other plugins clear what they learned of event ownership when a game starts (FH_Grapple_Plugin in
// its own load handler): announce the registered events again once every handler has run.
void AnnounceEventsNextFrame() {
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] { AnimationInjector::EventRegistry::Announce(); });
}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    // Our logger preserves the previous run before opening a fresh log file.
    SKSE::Init(a_skse, false);
    AnimationInjector::MainThread::Capture();
    SetupLog();
    logger::info("AnimationInjector {} (format 6 only, no older forms) loaded (runtime {})", AI_VERSION_STRING,
        REL::Module::get().version().string());
    AnimationInjector::Carry::InstallSerialization();
    SKSE::AllocTrampoline(4096);

    using namespace AnimationInjector;
    FrameworkSettings::Get().Load();
    LoadingProgress::SetEnabled(FrameworkSettings::Get().loadingProgress);
    Actions::Load();           // XML declarations and runners
    GraphInjector::Install();  // CreateSymbolIdMap / LoadClips
    ExitBlendHooks::Install();
    ClipHooks::Install();  // hkbClipGenerator Activate/Deactivate
    if (FrameworkSettings::Get().loadingProgress) AnimationLoadObserver::Install();
    Actions::InstallHooks();     // PlayerCharacter::Update
    PlayerHooks::Install();      // NotifyAnimationGraph: HyperArmor, native cancels, event triggers
    AirControl::Install();       // Install before the optional diagnostic wrapper.
    GraphEventHooks::Install();  // hkbStateMachine::handleEvent: handoff guard, exit-blend capture, diagnostics

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
        using namespace AnimationInjector;
        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            PairedClipBinding::Install();
            LoadingMenu::Register();
            QteMenu::Register();
            logger::info("[PairB] coexistence: OAR={} Grapple={}",
                REX::W32::GetModuleHandleW(L"OpenAnimationReplacer.dll") != nullptr,
                REX::W32::GetModuleHandleW(L"FH_Grapple_Plugin.dll") != nullptr);
            if (REX::W32::GetModuleHandleW(L"PairedAnimationImprovements.dll") &&
                FrameworkSettings::Get().nativePairAlignment == 2)
                logger::warn(
                    "[Compatibility] Paired Animation Improvements is loaded and iNativePairAlignment=2: both adjust every "
                    "paired start. Set iNativePairAlignment=1 to limit alignment to Animation Injector pairs.");
            PairRootRuntime::Install();
            NativePairEnd::Install();
            PairedStartHooks::Install();
            AnimationFileLifetime::Install();
            AnimationPreloader::Install();
            AnimationPreloader::InstallCellEvents();
            PairStartCamera::Install();
            PairRootScale::Install();
            CarryPoseLock::Install();  // After PairRootScale: it locks the scale-corrected pose.
            CarryHold::Install();
            PairInputGate::Install();
            MotionHook::Install();  // Install outside the AMR hook.
            AnimData::TryRegister("kDataLoaded");
            // Observe release input before the ordinary sink can start a carry.
            Carry::Register();
            InputSink::Register();
            HitDispatch::Register();  // Gameplay hit dispatch and optional diagnostics
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            AnimationPreloader::ResetProgress();
            AnimationLoadObserver::ResetProgress();
            LoadingProgress::Reset(true);
            logger::info("[LoadingUI] initial save-load session opened");
            EndSession();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            AnimationPreloader::InstallCellEvents();
            AnimationPreloader::RequestCheck();
            LoadingProgress::GameLoaded();
            LoadingMenu::Show();
            QteMenu::Show();
            Actions::OnGameLoad();
            EventEffects::PostLoad();
            if (const auto restored = ActorImmunity::PostLoad())
                logger::info("[Immunity] {} leases held when the save was written were undone", restored);
            AnnounceEventsNextFrame();
            break;
        case SKSE::MessagingInterface::kNewGame:
            EndSession();
            AnimationPreloader::InstallCellEvents();
            AnimationPreloader::RequestCheck();
            AnimationPreloader::ResetProgress();
            AnimationLoadObserver::ResetProgress();
            LoadingProgress::Reset();
            LoadingProgress::GameLoaded();
            logger::info("[LoadingUI] initial new-game session opened");
            LoadingMenu::Show();
            QteMenu::Show();
            Actions::OnGameLoad();
            AnnounceEventsNextFrame();
            break;
        default: break;
        }
    });
    return true;
}

// BSInputDevice definitions required by the pinned CommonLib fork.
namespace RE {
BSIInputDevice::~BSIInputDevice() {}
BSKeyboardDevice::~BSKeyboardDevice() {}
BSWin32KeyboardDevice::~BSWin32KeyboardDevice() {}
void BSWin32KeyboardDevice::Unk_09() {}
void BSWin32KeyboardDevice::Unk_0A() {}
void BSWin32KeyboardDevice::ClearInputState() {}
void BSWin32KeyboardDevice::Initialize() {}
void BSWin32KeyboardDevice::Poll(float) {}
void BSWin32KeyboardDevice::Shutdown() {}
bool BSInputDevice::IsEnabled() const { return true; }
bool BSInputDevice::GetKeyCodeFromID(std::int32_t, std::uint32_t&) { return false; }
std::uint32_t BSInputDevice::GetMappingKey(RE::BSFixedString) { return 0xFFFFFFFF; }
bool BSInputDevice::GetButtonNameFromID(std::int32_t, RE::BSFixedString&) { return false; }
BSInputDevice::~BSInputDevice() {}
}  // namespace RE

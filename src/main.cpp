#include "core/AnimationFileLifetime.h"
#include "core/NodeIdRenumber.h"
#include "core/PairRootScale.h"
#include "core/CarryPoseLock.h"
#include "core/PairRootRuntime.h"
#include "core/NativePairEnd.h"
#include "PCH.h"
#include "Version.h"
#include "core/AnimationLimit.h"
#include "core/ExitBlendHooks.h"
#include "core/PairInputGate.h"
#include "runtime/PairFeatures.h"
#include "runtime/EventEffects.h"
#include "runtime/GripHold.h"
#include "runtime/ActionHistory.h"
#include "runtime/HitHistory.h"
#include "runtime/EventHistory.h"
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
#include "core/LocomotionDrive.h"
#include "core/UpperLayer.h"
#include "runtime/PartRunner.h"
#include "core/PairedStartHooks.h"
#include "core/HeadingWatch.h"
#include "core/AnimationPreloader.h"
#include "core/PairedClipBinding.h"
#include "core/LoadingProgress.h"
#include "core/AnimationLoadObserver.h"
#include "ui/LoadingMenu.h"
#include "ui/QteMenu.h"
#include "runtime/ActionRegistry.h"
#include "runtime/PairStartControl.h"
#include "runtime/MainThread.h"
#include "runtime/InputSink.h"
#include "runtime/TdmMovement.h"
#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "runtime/CarryHold.h"
#include "runtime/PairStartCamera.h"
#include "runtime/ActorImmunity.h"
#include "runtime/AnnotationCommands.h"
#include "core/StartupRecord.h"
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
// The previous launch's startup record goes into this log (it died during startup when it did not finish), and
// this launch's starts (core/StartupRecord.h).
void BeginStartupRecord() {
    const auto dir = SKSE::log::log_directory();
    if (!dir) return;
    const auto about =
        std::format("AnimationInjector {}, runtime {}", AI_VERSION_STRING, REL::Module::get().version().string());
    const auto previous = AnimationInjector::StartupRecord::Begin(*dir / "AnimationInjector.startup.txt", about);
    for (const auto& line : previous.lines)
        if (previous.complete)
            logger::info("[Startup] {}", line);
        else
            logger::warn("[Startup] {}", line);
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
    GripHold::ReleaseAll();
    LocomotionDrive::ResetOnLoad();
    ActionHistory::ResetOnLoad();
    HitHistory::ResetOnLoad();
    EventHistory::ResetOnLoad();
    PairedSession::ResetOnLoad();
    PairRootScale::Reset();
    // Every holder has let go above; a lease still held would keep its actor a ghost in the next session.
    if (const auto left = ActorImmunity::ResetOnLoad())
        logger::warn("[Immunity] {} leases were still held at the session end; released", left);
}
// The loading panel stopped waiting for jobs that stopped finishing (core/LoadingProgressModel.h kStallLimit): what
// they were waiting for goes to the log, so a report shows why without a test build (11-24).
void ReportStall(std::size_t jobs) {
    using namespace AnimationInjector;
    logger::warn("[LoadingUI] stopped waiting: {} jobs had not finished and none finished for {:.0f} s", jobs,
        LoadingProgress::Model::kStallLimit);
    AnimationPreloader::ReportPending();
    AnimationLoadObserver::ReportPending();
}
// Other plugins clear what they learned of event ownership when a game starts (FH_Grapple_Plugin in
// its own load handler): announce the registered events again once every handler has run.
void AnnounceEventsNextFrame() {
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] { AnimationInjector::EventRegistry::Announce(); });
}
}

namespace {
constexpr std::size_t kTrampolineSize = 1024;
// Once every hook is installed (kDataLoaded): the trampoline space they took. A hook added later that brings it near
// the request warns here before a list with more hooks runs out (a request past it ends the game at load: "Failed to handle allocation request").
void LogTrampolineUse() {
    const auto& trampoline = SKSE::GetTrampoline();
    const auto used = trampoline.allocated_size(), capacity = trampoline.capacity();
    if (used * 5 > capacity * 4)
        logger::warn("[Trampoline] {} of {} bytes used: past 80%, raise kTrampolineSize (main.cpp)", used, capacity);
    else
        logger::info("[Trampoline] {} of {} bytes used", used, capacity);
}
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    // Our logger preserves the previous run before opening a fresh log file.
    SKSE::Init(a_skse, false);
    AnimationInjector::MainThread::Capture();
    SetupLog();
    logger::info("AnimationInjector {} (format 6 only, no older forms) loaded (runtime {})", AI_VERSION_STRING,
        REL::Module::get().version().string());
    BeginStartupRecord();
    AnimationInjector::Carry::InstallSerialization();
    // From SKSE's branch pool, which every plugin shares (users read the request in skse64.log; 4096 until 1.0.9, the
    // largest of the 74 requests in a large mod list's log). What the hooks take, 14 bytes a call-site hook, 8 a 6-byte one, the code they
    // generate: MotionHook up to about 400 (more when another mod such as AMR hooks one of its sites), HeadingWatch
    // about 100, PairedStartHooks and PairRootRuntime about 70 each, GraphInjector, PairInputGate and NodeIdRenumber
    // about 64. LogTrampolineUse reports the real figure once every hook is in.
    SKSE::AllocTrampoline(kTrampolineSize);

    using namespace AnimationInjector;
    FrameworkSettings::Get().Load();
    LoadingProgress::SetEnabled(FrameworkSettings::Get().loadingProgress);
    Actions::Load();             // XML declarations and runners
    GraphInjector::Install();    // CreateSymbolIdMap / LoadClips
    LocomotionDrive::Install();  // <Locomotion>: the split blenders' per-actor values, per actor update
    UpperLayer::Install();       // body= actions: the upper layer's stabilizer (generate exits)
    PartRunner::Install();       // body= actions: a sprint the start stopped stays stopped while its key is held
    GripHold::Install();         // gripHold: RootBehaviorGraph generate exit (both views)
    NodeIdRenumber::Install(FrameworkSettings::Get().nodeIdRenumber);  // graphs past 65535 nodes
    // A carry start gives a bAllowRotation true back only to an action still wanting it.
    PairStartControl::SetRotationOwner(Actions::RotationWindowOpen);
    ExitBlendHooks::Install();
    ClipHooks::Install();  // hkbClipGenerator Activate/Deactivate
    if (FrameworkSettings::Get().loadingProgress) AnimationLoadObserver::Install();
    Actions::InstallHooks();     // PlayerCharacter::Update
    PlayerHooks::Install();      // NotifyAnimationGraph: HyperArmor, event triggers
    AirControl::Install();       // Install before the optional diagnostic wrapper.
    GraphEventHooks::Install();  // hkbStateMachine::handleEvent: handoff guard, exit-blend capture, diagnostics
    StartupRecord::Mark("plugin-loaded");

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
        using namespace AnimationInjector;
        switch (msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            StartupRecord::Mark("all-plugins-loaded");
            // Slots other plugins check at their own load stay theirs until every plugin has loaded
            // (core/ClipHooks.h); no graph activates before the game data is loaded.
            ClipHooks::InstallActivation();
            ExitBlendHooks::InstallActivation();
            break;
        case SKSE::MessagingInterface::kPostPostLoad: StartupRecord::Mark("post-post-load"); break;
        case SKSE::MessagingInterface::kInputLoaded: StartupRecord::Mark("input-loaded"); break;
        case SKSE::MessagingInterface::kDataLoaded:
            Actions::OnDataLoaded();
            TdmMovement::Load();  // Optional: movement conditions and the Rotate-window turn follow TDM's settings.
            PairedClipBinding::Install();
            LoadingProgress::SetStallReporter(ReportStall);
            LoadingMenu::Register();
            QteMenu::Register();
            logger::info("[PairB] coexistence: OAR={} Grapple={}",
                REX::W32::GetModuleHandleW(L"OpenAnimationReplacer.dll") != nullptr,
                REX::W32::GetModuleHandleW(L"FH_Grapple_Plugin.dll") != nullptr);
            AnimationLimit::Get();  // Logs the binding cap: OAR has patched the engine by now if it raises it.
            PairRootRuntime::Install();
            NativePairEnd::Install();
            PairedStartHooks::Install();
            // Who else turns an attacker during a paired start ([Debug] bHeadingWatch).
            if (FrameworkSettings::Get().headingWatch)
                HeadingWatch::Install();
            else
                logger::info("[HeadingWatch] off (bHeadingWatch=false): the engine's setters are left untouched");
            AnimationFileLifetime::Install();
            AnimationPreloader::Install();
            AnimationPreloader::InstallCellEvents();
            PairStartCamera::Install();
            PairRootScale::Install();
            CarryPoseLock::Install();  // After PairRootScale: it locks the scale-corrected pose.
            CarryHold::Install();
            PairInputGate::Install();
            MotionHook::Install();  // Install outside the AMR hook.
            LogTrampolineUse();     // The last hook that takes trampoline space is in.
            AnimData::TryRegister("kDataLoaded");
            // Observe release input before the ordinary sink can start a carry.
            Carry::Register();
            InputSink::Get()->SetPressTiming(
                {FrameworkSettings::Get().pressHoldTime, FrameworkSettings::Get().pressDoubleWindow});
            // A running action's key transitions tell their presses apart with the same times.
            ActionLinks::SetPressTiming(
                {FrameworkSettings::Get().pressHoldTime, FrameworkSettings::Get().pressDoubleWindow});
            InputSink::Register();
            HitDispatch::Register();  // Gameplay hit dispatch and optional diagnostics
            StartupRecord::Finish("data-loaded");
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            AnimationPreloader::ResetProgress();
            AnimationLoadObserver::ResetProgress();
            LoadingProgress::Reset(true);
            logger::info("[LoadingUI] initial save-load session opened");
            LoadingMenu::Show();  // closed while idle in the world: the session is drawn from its start
            EndSession();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            AnimationPreloader::InstallCellEvents();
            AnimationPreloader::RequestCheck();
            LoadingProgress::GameLoaded();
            LoadingMenu::Show();
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

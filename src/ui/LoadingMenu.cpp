#include "PCH.h"
#include "ui/LoadingMenu.h"
#include "core/LoadingProgress.h"
#include "Settings.h"

namespace AnimationInjector::LoadingMenu {
namespace {
constexpr std::string_view name = "AnimationInjectorLoading";
bool registered = false;

bool Rectangle(RE::GFxValue& clip, double width, double height, unsigned color, double alpha) {
    const RE::GFxValue fill[]{RE::GFxValue(color), RE::GFxValue(alpha)};
    bool ok = clip.Invoke("beginFill", nullptr, fill, 2);
    for (const auto point : {std::pair{0.0, 0.0}, {width, 0.0}, {width, height}, {0.0, height}, {0.0, 0.0}}) {
        const RE::GFxValue args[]{RE::GFxValue(point.first), RE::GFxValue(point.second)};
        ok &= clip.Invoke("lineTo", nullptr, args, 2);
    }
    return clip.Invoke("endFill") && ok;
}
bool Text(RE::GFxValue& parent, RE::GFxValue& field, const char* fieldName, int depth, double y) {
    const RE::GFxValue args[]{RE::GFxValue(fieldName), RE::GFxValue(depth), RE::GFxValue(12.0), RE::GFxValue(y),
        RE::GFxValue(296.0), RE::GFxValue(24.0)};
    if (!parent.Invoke("createTextField", nullptr, args, 6) || !parent.GetMember(fieldName, &field)) return false;
    field.SetMember("selectable", RE::GFxValue(false));
    field.SetMember("type", RE::GFxValue("dynamic"));
    return true;
}

class Menu final : public RE::IMenu {
public:
    static RE::IMenu* Create() { return new Menu(); }
    Menu() {
        depthPriority = 100;
        menuFlags.set(RE::UI_MENU_FLAGS::kAlwaysOpen, RE::UI_MENU_FLAGS::kRequiresUpdate,
            RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu,
            RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
        inputContext = Context::kNone;
        auto* manager = RE::BSScaleformManager::GetSingleton();
        if (!manager || !manager->LoadMovie(this, uiMovie, name.data()) || !uiMovie) {
            logger::error("[LoadingUI] cannot load Interface/AnimationInjectorLoading.swf");
            return;
        }
        RE::GFxValue root, background, track;
        const bool created = uiMovie->GetVariable(&root, "_root") && root.CreateEmptyMovieClip(&panel, "loading", 1) &&
                             panel.CreateEmptyMovieClip(&background, "background", 1) &&
                             panel.CreateEmptyMovieClip(&track, "track", 2) &&
                             panel.CreateEmptyMovieClip(&bar, "bar", 3) && Text(panel, title, "title", 4, 5.0) &&
                             Text(panel, label, "label", 5, 26.0);
        if (!created || !Rectangle(background, 320, 64, 0x181A1D, 90) || !Rectangle(track, 296, 4, 0x4C5157, 100) ||
            !Rectangle(bar, 296, 4, 0x9FC5CE, 100)) {
            logger::error("[LoadingUI] could not create Scaleform display objects");
            uiMovie->SetVisible(false);
            return;
        }
        RE::GFxValue::DisplayInfo position;
        position.SetX(12);
        position.SetY(54);
        track.SetDisplayInfo(position);
        bar.SetDisplayInfo(position);
        root.Invoke("stop");
        title.SetTextHTML("<font face='$EverywhereFont' size='15' color='#F3F4F5'>Animation Injector</font>");
        uiMovie->SetVisible(false);
        ready = true;
        logger::info("[LoadingUI] overlay created; show delay=0, input unchanged");
    }
    RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& message) override {
        // An always-open, input-free host keeps completed short batches until
        // the next UI frame. Visibility belongs solely to the progress model.
        if (message.type == RE::UI_MESSAGE_TYPE::kHide) return RE::UI_MESSAGE_RESULTS::kIgnore;
        if (message.type == RE::UI_MESSAGE_TYPE::kScaleformEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
        return RE::IMenu::ProcessMessage(message);
    }
    void AdvanceMovie(float delta, std::uint32_t) override {
        if (!ready || !uiMovie) return;
        // No menu/player queries or progress mutex after session completion.
        if (!visible && !LoadingProgress::IsActive()) return;
        if (auto* ui = RE::UI::GetSingleton(); ui && !ui->IsShowingMenus()) {
            uiMovie->SetVisible(false);
            visible = false;
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
        const bool loadingMenu = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        auto* player = RE::PlayerCharacter::GetSingleton();
        // A save's initial graph/preload jobs can arrive after PostLoadGame.
        // Do not consume the completion tail while its loading screen or
        // world setup is still active. Cell travel never reopens a session.
        const bool worldReady = ui && !ui->GameIsPaused() && !loadingMenu && player && player->Is3DLoaded();
        const auto state = LoadingProgress::Advance(delta, !loadingMenu && (mainMenu || worldReady));
        if (state.visible != visible) {
            visible = state.visible;
            uiMovie->SetVisible(visible);
            logger::info("[LoadingUI] {} ready={} total={} pending={} failed={} cancelled={}",
                visible ? "show" : "hide", state.ready, state.total, state.pending, state.failed, state.cancelled);
        }
        if (!visible) return;
        const auto rect = uiMovie->GetVisibleFrameRect();
        RE::GFxValue::DisplayInfo position;
        position.SetX(rect.right - 344);
        position.SetY(rect.top + 88);
        position.SetAlpha(state.alpha * 100);
        panel.SetDisplayInfo(position);
        if (revision != state.revision) {
            revision = state.revision;
            RE::GFxValue::DisplayInfo fill;
            fill.SetXScale(state.Fraction() * 100.0);
            bar.SetDisplayInfo(fill);
            const char* caption =
                state.pending
                    ? (state.phase == LoadingProgress::Phase::kRegistration ? "Registering" : "Loading animations")
                : state.failed ? "Load failed"
                : state.ready  ? "Ready"
                               : "Cancelled";
            char text[256];
            if (state.failed)
                std::snprintf(text, sizeof(text),
                    "<font face='$EverywhereFont' size='13' color='#E9BE7A'>%s  %zu / %zu  (%zu failed)</font>",
                    caption, state.ready, state.total - state.cancelled, state.failed);
            else
                std::snprintf(text, sizeof(text),
                    "<font face='$EverywhereFont' size='13' color='#D2D7DC'>%s  %zu / %zu</font>", caption, state.ready,
                    state.total - state.cancelled);
            label.SetTextHTML(text);
            if (!state.pending)
                logger::info("[LoadingUI] completed ready={} failed={} cancelled={}", state.ready, state.failed,
                    state.cancelled);
        }
        uiMovie->Advance(std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.1f) : 0.0f);
    }
    void PostDisplay() override {
        if (ready && visible && uiMovie) uiMovie->Display();
    }

private:
    RE::GFxValue panel, bar, title, label;
    std::uint64_t revision = 0;
    bool ready = false, visible = false;
};
}
void Show() {
    if (!registered) return;
    auto* ui = RE::UI::GetSingleton();
    auto* queue = RE::UIMessageQueue::GetSingleton();
    static const RE::BSFixedString menuName(name);
    if (ui && queue && !ui->IsMenuOpen(name)) queue->AddMessage(menuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
}
void Register() {
    if (registered || !FrameworkSettings::Get().loadingProgress) return;
    auto* ui = RE::UI::GetSingleton();
    if (!ui) {
        logger::error("[LoadingUI] UI singleton unavailable at registration");
        return;
    }
    ui->Register(name, Menu::Create);
    registered = true;
    Show();
}
}

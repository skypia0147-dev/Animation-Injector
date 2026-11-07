#include "PCH.h"
#include "ui/LoadingMenu.h"
#include "core/LoadingProgress.h"
#include "Settings.h"
#include <format>
#include <string>

namespace AnimationInjector::LoadingMenu {
namespace {
constexpr std::string_view name = "AnimationInjectorLoading";
bool registered = false;

// The panel (stage units; the movie is scaled to the screen): a title row with the progress, then one row per known
// engine limit (core/LoadingProgressModel.h Usage). It sits in the top right corner under where OAR draws its
// loading box, about half the width of the earlier panel.
constexpr double kWidth = 168, kPad = 7, kInner = kWidth - 2 * kPad;
constexpr double kRowHeight = 22, kTitleHeight = 26;
constexpr double kTop = 34, kRight = 4;
constexpr unsigned kProgressColor = 0x9FC5CE, kUsageColor = 0x5DCAA5, kWarnColor = 0xEF9F27, kShortColor = 0xE24B4A;

bool Rectangle(RE::GFxValue& clip, double width, double height, unsigned color, double alpha) {
    const RE::GFxValue fill[]{RE::GFxValue(color), RE::GFxValue(alpha)};
    bool ok = clip.Invoke("beginFill", nullptr, fill, 2);
    for (const auto point : {std::pair{0.0, 0.0}, {width, 0.0}, {width, height}, {0.0, height}, {0.0, 0.0}}) {
        const RE::GFxValue args[]{RE::GFxValue(point.first), RE::GFxValue(point.second)};
        ok &= clip.Invoke("lineTo", nullptr, args, 2);
    }
    return clip.Invoke("endFill") && ok;
}
bool Text(RE::GFxValue& parent, RE::GFxValue& field, const char* fieldName, int depth, double x, double y, double w) {
    const RE::GFxValue args[]{RE::GFxValue(fieldName), RE::GFxValue(depth), RE::GFxValue(x), RE::GFxValue(y),
        RE::GFxValue(w), RE::GFxValue(18.0)};
    if (!parent.Invoke("createTextField", nullptr, args, 6) || !parent.GetMember(fieldName, &field)) return false;
    field.SetMember("selectable", RE::GFxValue(false));
    field.SetMember("type", RE::GFxValue("dynamic"));
    return true;
}
void Place(RE::GFxValue& clip, double x, double y) {
    RE::GFxValue::DisplayInfo position;
    position.SetX(x);
    position.SetY(y);
    clip.SetDisplayInfo(position);
}
// A bar's fill: redrawn in its color, scaled to the fraction.
void Fill(RE::GFxValue& bar, unsigned color, float fraction) {
    bar.Invoke("clear");
    Rectangle(bar, kInner, 3, color, 100);
    RE::GFxValue::DisplayInfo scale;
    scale.SetXScale(std::clamp(fraction, 0.0f, 1.0f) * 100.0);
    bar.SetDisplayInfo(scale);
}
// 65535 -> "65,535".
std::string Grouped(std::size_t value) {
    auto text = std::to_string(value);
    for (auto i = static_cast<std::ptrdiff_t>(text.size()) - 3; i > 0; i -= 3)
        text.insert(static_cast<std::size_t>(i), ",");
    return text;
}
std::string Html(const char* text, int size, const char* color, bool right = false) {
    return std::format("<p align='{}'><font face='$EverywhereFont' size='{}' color='{}'>{}</font></p>",
        right ? "right" : "left", size, color, text);
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
        RE::GFxValue root;
        bool created = uiMovie->GetVariable(&root, "_root") && root.CreateEmptyMovieClip(&panel, "loading", 1) &&
                       panel.CreateEmptyMovieClip(&background, "background", 1) &&
                       Text(panel, title, "title", 2, kPad - 2, 3, kInner + 4) &&
                       Text(panel, count, "count", 3, kPad - 2, 3, kInner + 4) &&
                       panel.CreateEmptyMovieClip(&progress.track, "progressTrack", 4) &&
                       panel.CreateEmptyMovieClip(&progress.bar, "progressBar", 5);
        int depth = 6;
        for (auto* row : {&rows[0], &rows[1]}) {
            const auto suffix = std::to_string(depth);
            created = created && Text(panel, row->label, ("label" + suffix).c_str(), depth, kPad - 2, 0, kInner + 4) &&
                      Text(panel, row->value, ("value" + suffix).c_str(), depth + 1, kPad - 2, 0, kInner + 4) &&
                      panel.CreateEmptyMovieClip(&row->track, ("track" + suffix).c_str(), depth + 2) &&
                      panel.CreateEmptyMovieClip(&row->bar, ("bar" + suffix).c_str(), depth + 3);
            depth += 4;
        }
        if (!created || !Rectangle(background, kWidth, kTitleHeight + 2 * kRowHeight, 0x181A1D, 90) ||
            !Rectangle(progress.track, kInner, 3, 0x4C5157, 100) ||
            !Rectangle(rows[0].track, kInner, 3, 0x4C5157, 100) ||
            !Rectangle(rows[1].track, kInner, 3, 0x4C5157, 100)) {
            logger::error("[LoadingUI] could not create Scaleform display objects");
            uiMovie->SetVisible(false);
            return;
        }
        Place(progress.track, kPad, 20);
        Place(progress.bar, kPad, 20);
        title.SetTextHTML(Html("Animation Injector", 11, "#F3F4F5").c_str());
        rows[0].name = "Graph nodes";
        rows[1].name = "Animations";
        root.Invoke("stop");
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
            logger::info(
                "[LoadingUI] {} ready={} total={} pending={} failed={} cancelled={} nodes={}/{}{} animations={}/{}{}",
                visible ? "show" : "hide", state.ready, state.total, state.pending, state.failed, state.cancelled,
                state.nodes.used, state.nodes.limit, state.nodes.shortfall ? " (left out)" : "", state.animations.used,
                state.animations.limit, state.animations.shortfall ? " (left out)" : "");
        }
        if (!visible) return;
        const auto rect = uiMovie->GetVisibleFrameRect();
        RE::GFxValue::DisplayInfo position;
        position.SetX(rect.right - kWidth - kRight);
        position.SetY(rect.top + kTop);
        position.SetAlpha(state.alpha * 100);
        panel.SetDisplayInfo(position);
        if (revision != state.revision) {
            revision = state.revision;
            Draw(state);
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
    struct Bar {
        RE::GFxValue track, bar;
    };
    struct Row : Bar {
        RE::GFxValue label, value;
        const char* name = "";
    };
    void Draw(const LoadingProgress::Snapshot& state) {
        const auto shown = state.total - state.cancelled;
        const auto progressText = state.pending  ? std::format("{} / {}", state.ready, shown)
                                  : state.failed ? std::format("{} failed", state.failed)
                                                 : std::string("Ready");
        count.SetTextHTML(Html(progressText.c_str(), 10, state.failed ? "#E9BE7A" : "#D2D7DC", true).c_str());
        Fill(progress.bar, kProgressColor, state.Fraction());
        double y = kTitleHeight;
        const LoadingProgress::Usage* usages[]{&state.nodes, &state.animations};
        for (int i = 0; i < 2; ++i) {
            auto& row = rows[i];
            const auto& usage = *usages[i];
            const bool known = usage.Known();
            for (auto* clip : {&row.label, &row.value, &row.track, &row.bar})
                clip->SetMember("_visible", RE::GFxValue(known));
            if (!known) continue;
            const unsigned color = usage.shortfall ? kShortColor : usage.Warn() ? kWarnColor : kUsageColor;
            const char* text = usage.shortfall ? "#F09595" : usage.Warn() ? "#EF9F27" : "#D2D7DC";
            row.label.SetTextHTML(Html(row.name, 10, "#B4B2A9").c_str());
            const auto value = Grouped(usage.used) + " / " + Grouped(usage.limit);
            row.value.SetTextHTML(Html(value.c_str(), 10, text, true).c_str());
            Place(row.label, kPad - 2, y - 2);
            Place(row.value, kPad - 2, y - 2);
            Place(row.track, kPad, y + 15);
            Place(row.bar, kPad, y + 15);
            Fill(row.bar, color, usage.Fraction());
            y += kRowHeight;
        }
        RE::GFxValue::DisplayInfo height;
        height.SetYScale((y + 2) / (kTitleHeight + 2 * kRowHeight) * 100.0);
        background.SetDisplayInfo(height);
    }
    RE::GFxValue panel, background, title, count;
    Bar progress;
    std::array<Row, 2> rows;
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

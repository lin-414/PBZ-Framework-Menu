// The menu panel: sidebar of mod sections -> tabbed pages. Pages are either
// schema-backed (auto-rendered form, framework persists the values) or custom
// render callbacks supplied through the PBZFM C API (widget API inside).
#include "menu.h"
#include "config.h"
#include "i18n.h"
#include "layout.h"
#include "common.h"
#include "registry.h"
#include "schema.h"
#include "theme.h"

#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pbzfm {

namespace {

// The framework's own config/i18n, borrowed from pbzfm.cpp through DrawMenu so
// the settings page can render like any client page.
Config* s_cfg = nullptr;
I18n* s_i18n = nullptr;
MenuUi* s_ui = nullptr;
bool s_frameworkRegistered = false;

// ASCII case-insensitive substring; non-ASCII UTF-8 bytes compare as-is, so
// CJK names filter by exact byte prefix too.
bool SidebarFilterMatch(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
    const size_t nlen = strlen(needle);
    const size_t hlen = strlen(hay);
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        size_t j = 0;
        while (j < nlen && fold(hay[i + j]) == fold(needle[j])) ++j;
        if (j == nlen) return true;
    }
    return false;
}

void FrameworkSettingsPage(void*) {
    Config& cfg = *s_cfg;
    I18n& i18n = *s_i18n;
    MenuUi& ui = *s_ui;

    ImGui::TextDisabled("%s", i18n.T("hint"));
    ImGui::Separator();

    int langCount = 0;
    const char* const* langs = I18n::KnownLanguages(&langCount);
    if (ImGui::BeginCombo(i18n.T("language"), i18n.Current())) {
        for (int i = 0; i < langCount; ++i) {
            bool selected = cfg.language == langs[i];
            if (ImGui::Selectable(langs[i], selected) && !selected) {
                if (i18n.SetLanguage(langs[i])) {
                    cfg.language = langs[i];
                    ui.requestSave = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat(i18n.T("ui_scale"), &cfg.uiScale, 0.7f, 1.8f, "%.2fx");
    ImGui::SliderFloat(i18n.T("ui_opacity"), &cfg.uiOpacity, 0.4f, 1.0f);

    {
        // Theme picker: "ink" is the built-in 玄墨鎏金, anything else is a
        // themes\<name>.json over the built-in style (layout paddings stay
        // fixed for the host automation).
        std::vector<std::string> names = theme::ListThemeNames();
        std::string cur = cfg.theme == "ink" ? i18n.T("theme_builtin") : cfg.theme;
        if (ImGui::BeginCombo(i18n.T("theme"), cur.c_str())) {
            for (const std::string& n : names) {
                std::string label = (n == "ink") ? i18n.T("theme_builtin") : n;
                bool sel = cfg.theme == n;
                if (ImGui::Selectable(label.c_str(), sel) && !sel) {
                    cfg.theme = n;
                    theme::ApplyThemeByName(n);
                    ui.requestSave = true;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    ImGui::TextUnformatted(i18n.T("toggle_key"));
    ImGui::SameLine(0.0f, 12.0f);
    char keyLabel[96];
    snprintf(keyLabel, sizeof(keyLabel), "[%s]", Config::VkName(cfg.toggleVk));
    bool capturingToggle = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                           g_captureKind.load(std::memory_order_relaxed) == CAP_TOGGLE;
    if (capturingToggle) {
        theme::PushCapturingStyle();
        ImGui::Button(i18n.T("capturing"), ImVec2(150.0f, 0.0f));
        theme::PopCapturingStyle();
    } else if (ImGui::Button(keyLabel, ImVec2(150.0f, 0.0f))) {
        g_captureKind.store(CAP_TOGGLE, std::memory_order_relaxed);
        g_captureMode.store(1, std::memory_order_relaxed);
    }

    ImGui::Separator();

    // N卡/PixPin 这类全局截图工具的热键在菜单打开期间被输入隔离拦截（它们也无法
    // 用鼠标选区），所以框架自带一张"含界面"的屏幕截图。
    if (ImGui::Button(i18n.T("screenshot"), ImVec2(150.0f, 0.0f))) {
        ui.requestScreenshot = true;
        ui.shotFlash = 2.0f;
    }
    if (ui.shotFlash > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n.T("shot_saved"));
    }

    // Passthrough status: what the ini lets through to screenshot tools while
    // the menu owns the input (configured in PBZFM.ini, read at load time).
    {
        std::string chords;
        for (size_t i = 0; i < cfg.passthroughKeys.size(); ++i) {
            if (i) chords += ", ";
            chords += Config::ChordToString(cfg.passthroughKeys[i]);
        }
        ImGui::TextDisabled("%s: %s", i18n.T("passthrough"),
                            chords.empty() ? "-" : chords.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n.T("passthrough_hint"));
    }

    ImGui::Separator();

    if (ImGui::Button(i18n.T("save"), ImVec2(150.0f, 0.0f))) {
        ui.requestSave = true;
        ui.saveFlash = 2.0f;
    }
    if (ui.saveFlash > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n.T("saved"));
    }
}

} // namespace

void DrawMenu(Config& cfg, I18n& i18n, MenuUi& ui, float dt) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontScaleMain = cfg.uiScale;
    if (ui.saveFlash > 0.0f) ui.saveFlash -= dt;
    if (ui.shotFlash > 0.0f) ui.shotFlash -= dt;
    ui.capturing = g_captureMode.load(std::memory_order_relaxed) == 1;
    s_cfg = &cfg;
    s_i18n = &i18n;
    s_ui = &ui;

    // FirstUseEver: the user's drag-move/resize persists for the session
    // (ImGuiCond_Always snapped the window back every frame).
    ImGui::SetNextWindowPos(ImVec2(layout::kPanelX, layout::kPanelY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(layout::kPanelW, layout::kPanelH), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::WithAlpha(theme::kPanel, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::WithAlpha(theme::kInk, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::WithAlpha(ImVec4(0.078f, 0.071f, 0.059f, 1.0f), cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kSilver);   // title text: nameplate silver-white

    ImGui::Begin(i18n.T("title"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(4);

    // Title-bar close button: square, hairline frame, cinnabar on hover - the
    // game's corner-exit idiom. Widget flow: cursor is moved into the title
    // bar and restored afterwards so content layout stays untouched.
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float titleH = ImGui::GetFrameHeight();
        const float btnS = titleH - 8.0f;
        const ImVec2 cursorBack = ImGui::GetCursorPos();
        ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - 4.0f - btnS, wp.y + 4.0f));
        // The window clip right after Begin excludes the title bar; replace it
        // (intersect=false) with exactly the title-bar strip for this widget.
        ImGui::PushClipRect(ImVec2(wp.x + 1.0f, wp.y + 1.0f), ImVec2(wp.x + ws.x - 1.0f, wp.y + titleH), false);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::WithAlpha(theme::kCinnabarDp, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kCinnabar);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));   // center the glyph in the square
        if (ImGui::Button("\u00d7", ImVec2(btnS, btnS))) {
            g_closeRequest.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n.T("close_menu"));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopClipRect();
        ImGui::SetCursorPos(cursorBack);
    }

    // PBZ-flavored chrome: gold hairline under the title bar, silver corner
    // brackets, cinnabar seal square top-right. Through the foreground list -
    // the window list's clip right after Begin excludes the title bar, and
    // user prims appended to window/child lists get dropped (1.92 quirk);
    // the framework owns the only ImGui window, so fg order == window order.
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float titleH = ImGui::GetFrameHeight();
        const ImU32 gold = ImGui::GetColorU32(theme::WithAlpha(theme::kGold, 0.70f));
        const ImU32 silver = ImGui::GetColorU32(theme::WithAlpha(theme::kSilverDim, 0.70f));
        const ImU32 ink = ImGui::GetColorU32(theme::WithAlpha(theme::kInk, 0.85f));
        dl->AddLine(ImVec2(wp.x + 1.0f, wp.y + titleH), ImVec2(wp.x + ws.x - 1.0f, wp.y + titleH), gold, 2.0f);

        const float inset = 3.0f, arm = 16.0f;
        const float lx = wp.x + inset, ty = wp.y + inset;
        const float rx = wp.x + ws.x - inset - 1.0f, by = wp.y + ws.y - inset - 1.0f;
        dl->AddLine(ImVec2(lx, ty + arm), ImVec2(lx, ty), silver);
        dl->AddLine(ImVec2(lx, ty), ImVec2(lx + arm, ty), silver);
        dl->AddLine(ImVec2(lx, by - arm), ImVec2(lx, by), silver);
        dl->AddLine(ImVec2(lx, by), ImVec2(lx + arm, by), silver);
        dl->AddLine(ImVec2(rx - arm, by), ImVec2(rx, by), silver);
        dl->AddLine(ImVec2(rx, by - arm), ImVec2(rx, by), silver);

        // seal square pairs with the close button in the title-bar's right side
        const float sealS = 12.0f;
        const float btnS = titleH - 8.0f;
        const ImVec2 sc(wp.x + ws.x - 4.0f - btnS - 6.0f - sealS, wp.y + (titleH - sealS) * 0.5f);
        dl->AddRectFilled(sc, ImVec2(sc.x + sealS, sc.y + sealS), ImGui::GetColorU32(theme::kCinnabar));
        dl->AddRect(sc, ImVec2(sc.x + sealS, sc.y + sealS), ink);
    }

    // -2 = first frame, -1 = framework settings page, else a registry index.
    static int selSection = -2;
    static int selPage = 0;

    // Hold the registry for the whole frame: registration from other threads
    // waits at most one frame; our own recursive calls stay legal.
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    auto& sections = RegistrySections();

    // The framework keeps its settings page in the registry like any client,
    // but it is chrome, not a mod: the sidebar skips it and the bottom-left
    // corner button switches to it (selSection == -1). Labels are stored as
    // raw i18n keys: the render sites below run them through T(), so a
    // language switch applies without re-registering.
    Section* fw = RegistryGetOrCreateSection("framework_section");
    if (fw && fw->pages.empty()) {
        Page p;
        p.label = "page_settings";
        p.fn = &FrameworkSettingsPage;
        RegistryAddPage(fw, p);
        s_frameworkRegistered = true;
    }

    int fwIdx = -1;
    for (int i = 0; i < (int)sections.size(); ++i) {
        if (sections[i].get() == fw) { fwIdx = i; break; }
    }
    // First frame (or a vanished section): open on the first mod section,
    // straight onto settings when none has registered yet.
    if (selSection == -2 || selSection == fwIdx || selSection >= (int)sections.size()) {
        selSection = -1;
        for (int i = 0; i < (int)sections.size(); ++i) {
            if (sections[i].get() != fw) { selSection = i; break; }
        }
    }
    if (selSection >= 0 && selPage >= (int)sections[selSection]->pages.size()) {
        selPage = 0;
    }

    // Footer band: both columns stop above it so the footer buttons stay
    // inside the fixed-size window (a full-height content child used to push
    // them past the bottom edge, out of view).
    const float footBand = st.ItemSpacing.y + ImGui::GetFrameHeight();

    ImGui::BeginChild("##sections", ImVec2(190.0f, -footBand));
    RegistryResetSectionRects();
    // Sidebar search: filters sections by (translated or raw) name substring;
    // the current selection stays put even when filtered out.
    static char search[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", i18n.T("search_hint"), search, sizeof(search));
    int shownSections = 0;
    for (int i = 0; i < (int)sections.size(); ++i) {
        if (sections[i].get() == fw) continue;   // reached through the corner button
        if (!SidebarFilterMatch(i18n.T(sections[i]->name.c_str()), search) &&
            !SidebarFilterMatch(sections[i]->name.c_str(), search)) {
            continue;
        }
        ++shownSections;
        const bool sel = selSection == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
        if (ImGui::Selectable(i18n.T(sections[i]->name.c_str()), sel)) {
            selSection = i;
            selPage = 0;
        }
        if (sel) ImGui::PopStyleColor();
        ImVec2 ra = ImGui::GetItemRectMin();
        ImVec2 rb = ImGui::GetItemRectMax();
        if (sel) {   // cinnabar accent bar, like the game's parry marks
            ImGui::GetForegroundDrawList()->AddRectFilled(
                ImVec2(ra.x, ra.y), ImVec2(ra.x + 3.0f, rb.y),
                ImGui::GetColorU32(theme::kCinnabar));
        }
        float rect[4] = { ra.x, ra.y, rb.x, rb.y };
        RegistryRecordSectionRect(i, rect);
    }
    if (shownSections == 0) {
        ImGui::TextDisabled("-");
    }
    ImGui::EndChild();
    if (!s_frameworkRegistered) {
        Log("[ui] frame 1: sidebar done (%d sections)", (int)sections.size());
    }
    ImGui::SameLine();

    ImGui::BeginChild("##content", ImVec2(0.0f, -footBand));
    ConfigDef* currentSchema = nullptr;
    Section* sec = nullptr;
    if (selSection == -1) {
        sec = fw;
    } else if (selSection >= 0 && selSection < (int)sections.size()) {
        sec = sections[selSection].get();
    }
    if (sec && !sec->pages.empty()) {
        if (ImGui::BeginTabBar("##pages")) {
            for (int p = 0; p < (int)sec->pages.size(); ++p) {
                Page& pg = sec->pages[p];
                if (ImGui::BeginTabItem(i18n.T(pg.label.c_str()))) {
                    selPage = p;
                    if (pg.schemaCfg) currentSchema = (ConfigDef*)pg.schemaCfg;
                    if (pg.fn) {
                        pg.fn(pg.ud);
                    } else if (pg.schemaCfg) {
                        RenderSchemaForm(*(ConfigDef*)pg.schemaCfg, i18n, dt);
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::TextDisabled("(no pages)");
    }
    ImGui::EndChild();

    // Footer row, pinned to the panel's bottom-left corner: the framework
    // settings switch (gold while open) and save for the schema page being
    // viewed. Closing lives in the title bar (x / Esc / hotkey).
    ImGui::SetCursorPosX(st.WindowPadding.x);
    const bool settingsOpen = selSection == -1;
    if (settingsOpen) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
    if (ImGui::Button(i18n.T("page_settings"), ImVec2(150.0f, 0.0f))) {
        selSection = -1;
        selPage = 0;
    }
    if (settingsOpen) ImGui::PopStyleColor();
    if (currentSchema) {
        ImGui::SameLine();
        if (ImGui::Button(i18n.T("save"), ImVec2(150.0f, 0.0f))) {
            SchemaSaveConfigNow(currentSchema);
            ui.saveFlash = 2.0f;
        }
    }
    if (ui.saveFlash > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n.T("saved"));
    }

    ImGui::End();
}

} // namespace pbzfm

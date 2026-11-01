// The menu panel: sidebar section tree -> page content. A section node can
// carry its own page (schema-backed or a custom render callback) and child
// sections (the nested-path API); the tree replaces the old flat sidebar +
// tab-bar pair. Nodes with a single page render as one flat row, so simple
// mods look exactly like before; only real trees grow carets.
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
#include <set>
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

// First node carrying content, depth-first: used to land somewhere sensible
// when a selection vanishes or on the first frame.
const Section* FirstContentNode(const Section& n, int* pageIdx) {
    if (!n.pages.empty()) {
        *pageIdx = 0;
        return &n;
    }
    for (const auto& c : n.children) {
        if (const Section* hit = FirstContentNode(*c, pageIdx)) return hit;
    }
    return nullptr;
}

// A node stays visible under the search filter when it, one of its pages, or
// any descendant matches. While filtering the whole tree renders expanded.
bool SubtreeMatchesFilter(const Section& n, const char* needle) {
    if (SidebarFilterMatch(n.name.c_str(), needle)) return true;
    for (const auto& p : n.pages) {
        if (SidebarFilterMatch(p.label.c_str(), needle)) return true;
    }
    for (const auto& c : n.children) {
        if (SubtreeMatchesFilter(*c, needle)) return true;
    }
    return false;
}

// Mark every ancestor of the node expanded (a newly auto-selected leaf must
// not hide inside a collapsed branch).
void ExpandAncestors(std::set<std::string>& expanded, const Section& n) {
    for (const Section* p = n.parent; p; p = p->parent) {
        expanded.insert(RegistryPathOf(p));
    }
}

struct TreeCtx {
    I18n* i18n;
    const char* search;                 // "" = no filter
    std::string* selPath;
    int* selPage;
    bool* selSettings;
    std::set<std::string>* expanded;
    int rootIdx = -1;                   // automation rect index for depth-0 rows
    int* visibleRoots = nullptr;
};

// One row = one Selectable, caret glyph embedded in the label. Click rules:
//  - node without content (a pure group): toggle expand + select first leaf;
//  - node with content: select it; clicking it again toggles its children;
//  - page rows (a multi-page node's own pages) select (node, page).
void RenderTreeNode(Section& node, int depth, TreeCtx& tc) {
    const bool filtering = tc.search[0] != 0;
    if (filtering && !SubtreeMatchesFilter(node, tc.search)) return;

    const std::string path = RegistryPathOf(&node);
    const bool open = filtering || tc.expanded->count(path) != 0;
    const bool expandable = !node.children.empty() || node.pages.size() > 1;
    const bool sel = !*tc.selSettings && *tc.selPath == path;

    ImGui::PushID(path.c_str());
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + depth * 14.0f);
    char label[512];
    if (expandable) {
        snprintf(label, sizeof(label), "%s %s", open ? "\xe2\x96\xbc" : "\xe2\x96\xb6",
                 tc.i18n->T(node.name.c_str()));
    } else {
        snprintf(label, sizeof(label), "%s", tc.i18n->T(node.name.c_str()));
    }
    if (sel) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
    if (ImGui::Selectable(label, sel)) {
        *tc.selSettings = false;
        if (!node.pages.empty()) {
            // Select the node's content; clicking an already-selected parent
            // toggles its children (there is no separate caret widget).
            const bool wasSelected = sel;
            *tc.selPath = path;
            *tc.selPage = 0;
            if (expandable) {
                if (wasSelected) {
                    if (open) tc.expanded->erase(path);
                    else tc.expanded->insert(path);
                } else if (!open) {
                    tc.expanded->insert(path);
                }
            }
        } else if (!node.children.empty()) {
            if (open) tc.expanded->erase(path);
            else tc.expanded->insert(path);
            int idx = 0;
            if (const Section* f = FirstContentNode(node, &idx)) {
                *tc.selPath = RegistryPathOf(f);
                *tc.selPage = idx;
                ExpandAncestors(*tc.expanded, *f);
            }
        }
    }
    if (sel) {
        ImGui::PopStyleColor();
        ImVec2 ra = ImGui::GetItemRectMin();
        ImVec2 rb = ImGui::GetItemRectMax();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(ra.x, ra.y), ImVec2(ra.x + 3.0f, rb.y),
            ImGui::GetColorU32(theme::kCinnabar));
    }
    if (depth == 0) {
        ++*tc.visibleRoots;
        if (tc.rootIdx >= 0) {
            ImVec2 ra = ImGui::GetItemRectMin();
            ImVec2 rb = ImGui::GetItemRectMax();
            float rect[4] = { ra.x, ra.y, rb.x, rb.y };
            RegistryRecordSectionRect(tc.rootIdx, rect);
        }
    }

    // The node's own pages render as child rows once it holds more than one
    // (single-page nodes are the row itself - no extra hop).
    if (open && node.pages.size() > 1) {
        for (int pgi = 0; pgi < (int)node.pages.size(); ++pgi) {
            ImGui::PushID(pgi);
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + (depth + 1) * 14.0f);
            const bool pageSel = sel && *tc.selPage == pgi;
            if (pageSel) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
            if (ImGui::Selectable(tc.i18n->T(node.pages[pgi].label.c_str()), pageSel)) {
                *tc.selSettings = false;
                *tc.selPath = path;
                *tc.selPage = pgi;
            }
            if (pageSel) {
                ImGui::PopStyleColor();
                ImVec2 ra = ImGui::GetItemRectMin();
                ImVec2 rb = ImGui::GetItemRectMax();
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    ImVec2(ra.x, ra.y), ImVec2(ra.x + 3.0f, rb.y),
                    ImGui::GetColorU32(theme::kCinnabar));
            }
            ImGui::PopID();
        }
    }
    if (open) {
        for (const auto& c : node.children) {
            RenderTreeNode(*c, depth + 1, tc);
        }
    }
    ImGui::PopID();
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

    // Tree selection, session-only by design: the selected node path, which of
    // its pages, whether the corner settings button owns the content, and the
    // expanded set. All of it resets when the DLL reloads.
    static std::string selPath;
    static int selPage = 0;
    static bool selSettings = false;
    static std::set<std::string> expanded;

    // Hold the registry for the whole frame: registration from other threads
    // waits at most one frame; our own recursive calls stay legal.
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    auto& sections = RegistrySections();

    // The framework keeps its settings page in the registry like any client,
    // but it is chrome, not a mod: the tree skips it and the bottom-left
    // corner button switches to it. Labels are stored as raw i18n keys: the
    // render sites run them through T(), so a language switch applies without
    // re-registering.
    Section* fw = RegistryGetOrCreateSection("framework_section");
    if (fw && fw->pages.empty()) {
        Page p;
        p.label = "page_settings";
        p.fn = &FrameworkSettingsPage;
        RegistryAddPage(fw, p);
        s_frameworkRegistered = true;
    }

    // First frame (or a vanished selection): open on the first mod content,
    // straight onto settings when none has registered yet.
    if (!selSettings && (selPath.empty() || !RegistryFindPath(selPath.c_str()))) {
        selPath.clear();
        selPage = 0;
        for (const auto& r : sections) {
            if (r.get() == fw) continue;
            int idx = 0;
            if (const Section* f = FirstContentNode(*r, &idx)) {
                selPath = RegistryPathOf(f);
                selPage = idx;
                ExpandAncestors(expanded, *f);
                break;
            }
        }
    }

    // Footer band: both columns stop above it so the footer buttons stay
    // inside the fixed-size window (a full-height content child used to push
    // them past the bottom edge, out of view).
    const float footBand = st.ItemSpacing.y + ImGui::GetFrameHeight();

    ImGui::BeginChild("##sections", ImVec2(190.0f, -footBand));
    RegistryResetSectionRects();
    // Sidebar search: filters whole subtrees by (translated or raw) name
    // substring; while filtering everything renders expanded and the current
    // selection stays put even when filtered out.
    static char search[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", i18n.T("search_hint"), search, sizeof(search));
    TreeCtx tc;
    tc.i18n = &i18n;
    tc.search = search;
    tc.selPath = &selPath;
    tc.selPage = &selPage;
    tc.selSettings = &selSettings;
    tc.expanded = &expanded;
    int visibleRoots = 0;
    tc.visibleRoots = &visibleRoots;
    for (int i = 0; i < (int)sections.size(); ++i) {
        if (sections[i].get() == fw) continue;   // reached through the corner button
        tc.rootIdx = i;
        RenderTreeNode(*sections[i], 0, tc);
    }
    if (visibleRoots == 0) {
        ImGui::TextDisabled("-");
    }
    ImGui::EndChild();
    if (!s_frameworkRegistered) {
        Log("[ui] frame 1: sidebar done (%d root sections)", (int)sections.size());
    }
    ImGui::SameLine();

    ImGui::BeginChild("##content", ImVec2(0.0f, -footBand));
    ConfigDef* currentSchema = nullptr;
    Section* contentNode = nullptr;
    int contentPage = 0;
    if (selSettings) {
        contentNode = fw;
    } else if (!selPath.empty()) {
        contentNode = RegistryFindPath(selPath.c_str());
        contentPage = selPage;
    }
    if (contentNode && !contentNode->pages.empty()) {
        if (contentPage < 0 || contentPage >= (int)contentNode->pages.size()) contentPage = 0;
        Page& pg = contentNode->pages[contentPage];
        if (pg.schemaCfg) currentSchema = (ConfigDef*)pg.schemaCfg;
        if (contentNode->pages.size() > 1) {
            // Rare v1 shape (several pages on one node): keep a local tab
            // bar. The tree covers everything else.
            if (ImGui::BeginTabBar("##pages")) {
                for (int p = 0; p < (int)contentNode->pages.size(); ++p) {
                    Page& tp = contentNode->pages[p];
                    if (ImGui::BeginTabItem(i18n.T(tp.label.c_str()))) {
                        selPage = p;
                        if (tp.schemaCfg) currentSchema = (ConfigDef*)tp.schemaCfg;
                        if (tp.fn) {
                            tp.fn(tp.ud);
                        } else if (tp.schemaCfg) {
                            RenderSchemaForm(*(ConfigDef*)tp.schemaCfg, i18n, dt);
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        } else if (pg.fn) {
            pg.fn(pg.ud);
        } else if (pg.schemaCfg) {
            RenderSchemaForm(*(ConfigDef*)pg.schemaCfg, i18n, dt);
        }
    } else {
        ImGui::TextDisabled("(no pages)");
    }
    ImGui::EndChild();

    // Footer row, pinned to the panel's bottom-left corner: the framework
    // settings switch (gold while open) and save for the schema page being
    // viewed. Closing lives in the title bar (x / Esc / hotkey).
    ImGui::SetCursorPosX(st.WindowPadding.x);
    const bool settingsOpen = selSettings;
    if (settingsOpen) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
    if (ImGui::Button(i18n.T("page_settings"), ImVec2(150.0f, 0.0f))) {
        selSettings = true;
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

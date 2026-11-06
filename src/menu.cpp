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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <memory>

namespace pbzfm {

namespace {

// The framework's own config/i18n, borrowed from pbzfm.cpp through DrawMenu so
// the settings page can render like any client page.
Config* s_cfg = nullptr;
I18n* s_i18n = nullptr;
MenuUi* s_ui = nullptr;
bool s_frameworkRegistered = false;

// The single effective zoom used by every fixed-size widget. theme's
// ApplyUiScale() sets FontScaleMain from the same value, so deriving it back
// from the font keeps menu code independent of whether the scale came from
// the ini slider, a future auto-DPI rule, or a panel-resize zoom.
float UiScaleNow() {
    const ImGuiStyle& st = ImGui::GetStyle();
    return st.FontSizeBase > 0.0f ? ImGui::GetFontSize() / st.FontSizeBase : st.FontScaleMain;
}

// Rightmost pixel a tree row can reach, including indentation, caret and page
// child rows. Measured with the *current* font, so the sidebar can react to a
// language switch, a long mod name, or a UI scale change without hardcoding a
// column width that eventually clips.
float MeasureTreeNodeRight(const Section& node, int depth, I18n& i18n, float uiScale) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float indent = 14.0f * uiScale;
    const bool expandable = !node.children.empty() || node.pages.size() > 1;
    float right = st.WindowPadding.x + depth * indent;
    if (expandable) right += (12.0f + 2.0f) * uiScale;
    right += ImGui::CalcTextSize(i18n.T(node.name.c_str())).x;

    if (node.pages.size() > 1) {
        for (const auto& p : node.pages) {
            const float pageLeft = st.WindowPadding.x + (depth + 1) * indent;
            right = std::max(right, pageLeft + ImGui::CalcTextSize(i18n.T(p.label.c_str())).x);
        }
    }
    for (const auto& child : node.children) {
        right = std::max(right, MeasureTreeNodeRight(*child, depth + 1, i18n, uiScale));
    }
    return right;
}

float ComputeSidebarWidth(const std::vector<std::unique_ptr<Section>>& sections, const Section* skip,
                          I18n& i18n, float uiScale, float availableW) {
    float contentRight = 0.0f;
    for (const auto& node : sections) {
        if (node.get() == skip) continue;
        contentRight = std::max(contentRight, MeasureTreeNodeRight(*node, 0, i18n, uiScale));
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    // contentRight is measured from the child's content origin; add the far
    // padding, a scrollbar and a small safety gap. At 1.0 this is a little
    // wider than the historical 190px when the labels need it, and exactly
    // 190px when they do not.
    const float needed = contentRight + st.WindowPadding.x + st.ScrollbarSize + 4.0f * uiScale;
    const float minW = layout::kSidebarW * uiScale;
    const float maxW = std::max(minW, availableW * 0.60f);
    return std::clamp(needed, minW, maxW);
}

// The panel is a zoomable surface: its initial size and resize limits follow
// the UI scale, and a scale change (the settings slider) scales the current
// panel by the same ratio. Without this the panel stays 660x480 while the
// glyphs grow - the exact combination that clips the sidebar at 4K.
struct PanelState {
    bool initialized = false;
    float scale = 1.0f;
    ImVec2 size = ImVec2(layout::kPanelW, layout::kPanelH);   // logical, unclamped
    ImVec2 pos = ImVec2(layout::kPanelX, layout::kPanelY);    // logical, unclamped
    ImVec2 lastActualSize = ImVec2(0, 0);
    ImVec2 lastActualPos = ImVec2(0, 0);
    ImVec2 lastDisplaySize = ImVec2(0, 0);
    bool lastActualValid = false;
    bool lastDisplayValid = false;
};

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

struct TreeCtx {
    I18n* i18n;
    const char* search;                 // "" = no filter
    std::string* selPath;
    int* selPage;
    bool* selSettings;
    std::set<std::string>* expanded;
    float uiScale = 1.0f;               // fixed indent/caret/selection bar sizes
    int rootIdx = -1;                   // automation rect index for depth-0 rows
    int* visibleRoots = nullptr;
};

// Fallback for a deliberately narrow panel: even the auto-sized sidebar is
// capped, so show the full label on hover instead of leaving the user with a
// permanently truncated row.
void TooltipIfClipped(const char* text, float textLeft) {
    if (!ImGui::IsItemHovered()) return;
    const ImVec2 wp = ImGui::GetWindowPos();
    const float scrollbar = ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    const float right = wp.x + ImGui::GetWindowContentRegionMax().x - scrollbar;
    const float textRight = textLeft + ImGui::CalcTextSize(text).x;
    if (textRight > right) ImGui::SetTooltip("%s", text);
}

// One row = one Selectable, caret glyph embedded in the label. A click on an
// expandable row ALWAYS toggles the branch (that is the collapse affordance -
// there is no separate caret widget) and selects the row; a node without its
// own content lands the selection on its first page when it expands.
void RenderTreeNode(Section& node, int depth, TreeCtx& tc) {
    const bool filtering = tc.search[0] != 0;
    if (filtering && !SubtreeMatchesFilter(node, tc.search)) return;

    const std::string path = RegistryPathOf(&node);
    const bool open = filtering || tc.expanded->count(path) != 0;
    const bool expandable = !node.children.empty() || node.pages.size() > 1;
    const bool sel = !*tc.selSettings && *tc.selPath == path;

    ImGui::PushID(path.c_str());
    const float rowX = ImGui::GetStyle().WindowPadding.x + depth * 14.0f * tc.uiScale;
    const float caretW = 12.0f * tc.uiScale;
    if (expandable) {
        // The caret is a drawn triangle, not a text glyph: U+25B6 (closed
        // caret) is missing from the CJK fonts the framework loads and
        // rendered as "?" in the collapsed state.
        ImGui::SetCursorPosX(rowX);
        if (ImGui::InvisibleButton("##caret", ImVec2(caretW, ImGui::GetTextLineHeight()))) {
            if (open) tc.expanded->erase(path);
            else tc.expanded->insert(path);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetItemRectMin();
        ImVec2 b = ImGui::GetItemRectMax();
        const float cx = (a.x + b.x) * 0.5f;
        const float cy = (a.y + b.y) * 0.5f;
        const float r = ImGui::GetTextLineHeight() * 0.28f;
        const ImU32 col = (ImGui::IsItemHovered() || sel)
            ? ImGui::GetColorU32(theme::kGoldBright)
            : ImGui::GetColorU32(theme::kSilverDim);
        if (open) {
            dl->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.8f), ImVec2(cx + r, cy - r * 0.8f),
                                  ImVec2(cx, cy + r * 0.8f), col);
        } else {
            dl->AddTriangleFilled(ImVec2(cx - r * 0.8f, cy - r), ImVec2(cx - r * 0.8f, cy + r),
                                  ImVec2(cx + r * 0.8f, cy), col);
        }
        ImGui::SameLine(0.0f, 2.0f * tc.uiScale);
    }
    ImGui::SetCursorPosX(expandable ? rowX + caretW + 2.0f * tc.uiScale : rowX);
    const float textLeft = ImGui::GetCursorScreenPos().x;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
    if (ImGui::Selectable(tc.i18n->T(node.name.c_str()), sel)) {
        *tc.selSettings = false;
        if (expandable) {
            if (open) tc.expanded->erase(path);
            else tc.expanded->insert(path);
        }
        if (!node.pages.empty()) {
            *tc.selPath = path;
            *tc.selPage = 0;
        } else if (!node.children.empty() && !open) {
            // a pure group just expanded: land the selection on its first page;
            // collapsing leaves the current selection (and content) alone
            int idx = 0;
            if (const Section* f = FirstContentNode(node, &idx)) {
                *tc.selPath = RegistryPathOf(f);
                *tc.selPage = idx;
            }
        }
    }
    TooltipIfClipped(tc.i18n->T(node.name.c_str()), textLeft);
    if (sel) {
        ImGui::PopStyleColor();
        ImVec2 ra = ImGui::GetItemRectMin();
        ImVec2 rb = ImGui::GetItemRectMax();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(ra.x, ra.y), ImVec2(ra.x + 3.0f * tc.uiScale, rb.y),
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
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + (depth + 1) * 14.0f * tc.uiScale);
            const float pageTextLeft = ImGui::GetCursorScreenPos().x;
            const bool pageSel = sel && *tc.selPage == pgi;
            if (pageSel) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
            if (ImGui::Selectable(tc.i18n->T(node.pages[pgi].label.c_str()), pageSel)) {
                *tc.selSettings = false;
                *tc.selPath = path;
                *tc.selPage = pgi;
            }
            TooltipIfClipped(tc.i18n->T(node.pages[pgi].label.c_str()), pageTextLeft);
            if (pageSel) {
                ImGui::PopStyleColor();
                ImVec2 ra = ImGui::GetItemRectMin();
                ImVec2 rb = ImGui::GetItemRectMax();
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    ImVec2(ra.x, ra.y), ImVec2(ra.x + 3.0f * tc.uiScale, rb.y),
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
    const float scale = UiScaleNow();

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
    ImGui::SliderFloat(i18n.T("ui_scale"), &cfg.uiScale, kUiScaleMin, kUiScaleMax, "%.2fx");
    ui.scaleEditing = ImGui::IsItemActive();
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
                    theme::ApplyUiScale(scale);   // theme reload captured the base; restore live scale now
                    ui.requestSave = true;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    ImGui::TextUnformatted(i18n.T("toggle_key"));
    ImGui::SameLine(0.0f, 12.0f * scale);
    char keyLabel[96];
    snprintf(keyLabel, sizeof(keyLabel), "[%s]", Config::VkName(cfg.toggleVk));
    const float toggleBtnW = std::min(
        150.0f * scale,
        std::max(1.0f, ImGui::GetContentRegionAvail().x));
    bool capturingToggle = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                           g_captureKind.load(std::memory_order_relaxed) == CAP_TOGGLE;
    if (capturingToggle) {
        theme::PushCapturingStyle();
        ImGui::Button(i18n.T("capturing"), ImVec2(toggleBtnW, 0.0f));
        theme::PopCapturingStyle();
    } else if (ImGui::Button(keyLabel, ImVec2(toggleBtnW, 0.0f))) {
        g_captureKind.store(CAP_TOGGLE, std::memory_order_relaxed);
        g_captureMode.store(1, std::memory_order_relaxed);
    }

    ImGui::Separator();

    // N卡/PixPin 这类全局截图工具的热键在菜单打开期间被输入隔离拦截（它们也无法
    // 用鼠标选区），所以框架自带一张"含界面"的屏幕截图。
    if (ImGui::Button(i18n.T("screenshot"), ImVec2(std::min(150.0f * scale, std::max(1.0f, ImGui::GetContentRegionAvail().x)), 0.0f))) {
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

    if (ImGui::Button(i18n.T("save"), ImVec2(std::min(150.0f * scale, std::max(1.0f, ImGui::GetContentRegionAvail().x)), 0.0f))) {
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
    const float requestedScale = std::clamp(cfg.uiScale, kUiScaleMin, kUiScaleMax);
    const bool scaleEditingLastFrame = ui.scaleEditing;   // consumed below
    ui.scaleEditing = false;
    static PanelState panel;
    // Defer the whole scale change while the slider is held: applying the new
    // font but keeping the old panel width would squeeze the content (and the
    // slider itself) out of view. After release, font, panel, sidebar and all
    // fixed widths switch together.
    const bool deferScale = scaleEditingLastFrame && panel.initialized &&
                            std::fabs(requestedScale - panel.scale) > 0.0005f;
    const float uiScale = deferScale ? panel.scale : requestedScale;
    theme::ApplyUiScale(uiScale);
    ImGuiStyle& st = ImGui::GetStyle();
    if (ui.saveFlash > 0.0f) ui.saveFlash -= dt;
    if (ui.shotFlash > 0.0f) ui.shotFlash -= dt;
    ui.capturing = g_captureMode.load(std::memory_order_relaxed) == 1;
    s_cfg = &cfg;
    s_i18n = &i18n;
    s_ui = &ui;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    // While the slider is held the panel keeps its old scale; after release
    // the next frame settles the new size.
    const float panelLayoutScale = uiScale;
    const float minPanelW = layout::kPanelMinW * panelLayoutScale;
    const float minPanelH = layout::kPanelMinH * panelLayoutScale;
    const auto clampPanel = [&](ImVec2& size, ImVec2& pos) {
        const float maxW = std::max(minPanelW, disp.x);
        const float maxH = std::max(minPanelH, disp.y);
        size.x = std::clamp(size.x, minPanelW, maxW);
        size.y = std::clamp(size.y, minPanelH, maxH);
        pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, disp.x - size.x));
        pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, disp.y - size.y));
    };

    const bool firstFrame = !panel.initialized;
    bool scaleChanged = false;
    if (firstFrame) {
        panel.scale = uiScale;
        panel.size = ImVec2(layout::kPanelW * uiScale, layout::kPanelH * uiScale);
        panel.pos = ImVec2(layout::kPanelX, layout::kPanelY);
        panel.initialized = true;
    } else if (std::fabs(uiScale - panel.scale) > 0.0005f) {
        // The ui_scale slider changes the size of the whole menu, not just the
        // font: keep the user's relative panel size when crossing scales.
        const float ratio = uiScale / panel.scale;
        panel.scale = uiScale;
        panel.size.x *= ratio;
        panel.size.y *= ratio;
        scaleChanged = true;
    }

    // Keep the logical (unclamped) panel size/position so a later scale change
    // does not inherit a size that was only clipped by this display's edges.
    ImVec2 displaySize = panel.size;
    ImVec2 displayPos = panel.pos;
    const float maxPanelW = std::max(minPanelW, disp.x);
    const float maxPanelH = std::max(minPanelH, disp.y);
    clampPanel(displaySize, displayPos);

    if (firstFrame) {
        ImGui::SetNextWindowPos(displayPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(displaySize, ImGuiCond_FirstUseEver);
    } else if (scaleChanged) {
        ImGui::SetNextWindowPos(displayPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
    }

    // Resize limits: the floor keeps sidebar + content readable at any ui scale;
    // the ceiling is the game window itself, so the menu can at most fill the
    // overlay it lives in. Begin() clamps SizeFull through these constraints, so
    // the FirstUseEver size is covered too on game windows smaller than the
    // scaled panel.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(minPanelW, minPanelH),
        ImVec2(maxPanelW, maxPanelH));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::WithAlpha(theme::kPanel, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::WithAlpha(theme::kInk, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::WithAlpha(ImVec4(0.078f, 0.071f, 0.059f, 1.0f), cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kSilver);   // title text: nameplate silver-white

    ImGui::Begin(i18n.T("title"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(4);
    // Keep the logical geometry in sync with a user drag, but do not bake a
    // display-edge clamp into it (the review case: 1.8x panel height is
    // clamped to the 720p client, then lowering the scale must not shrink it
    // twice). Comparing against the previous actual rect distinguishes a real
    // user resize from a window/display resize, where the window size can stay
    // stale for one frame.
    const ImVec2 actualSize = ImGui::GetWindowSize();
    const ImVec2 actualPos = ImGui::GetWindowPos();
    const bool actualSizeChanged = panel.lastActualValid &&
        (std::fabs(actualSize.x - panel.lastActualSize.x) > 0.5f ||
         std::fabs(actualSize.y - panel.lastActualSize.y) > 0.5f);
    const bool actualPosChanged = panel.lastActualValid &&
        (std::fabs(actualPos.x - panel.lastActualPos.x) > 0.5f ||
         std::fabs(actualPos.y - panel.lastActualPos.y) > 0.5f);
    const bool displayChanged = panel.lastDisplayValid &&
        (std::fabs(disp.x - panel.lastDisplaySize.x) > 0.5f ||
         std::fabs(disp.y - panel.lastDisplaySize.y) > 0.5f);
    if (!firstFrame && !scaleChanged && !displayChanged && actualSizeChanged) {
        if (actualSize.x < maxPanelW - 0.5f) panel.size.x = actualSize.x;
        if (actualSize.y < maxPanelH - 0.5f) panel.size.y = actualSize.y;
    }
    if (!firstFrame && !scaleChanged && !displayChanged && actualPosChanged) {
        panel.pos = actualPos;
    }
    panel.lastActualSize = actualSize;
    panel.lastActualPos = actualPos;
    panel.lastActualValid = true;
    panel.lastDisplaySize = disp;
    panel.lastDisplayValid = true;

    // Title-bar close button: square, hairline frame, cinnabar on hover - the
    // game's corner-exit idiom. Widget flow: cursor is moved into the title
    // bar and restored afterwards so content layout stays untouched.
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float titleH = ImGui::GetFrameHeight();
        const float btnS = titleH - 8.0f * uiScale;
        const ImVec2 cursorBack = ImGui::GetCursorPos();
        ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - 4.0f * uiScale - btnS, wp.y + 4.0f * uiScale));
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
        dl->AddLine(ImVec2(wp.x + 1.0f, wp.y + titleH), ImVec2(wp.x + ws.x - 1.0f, wp.y + titleH), gold,
                    2.0f * uiScale);

        const float inset = 3.0f * uiScale, arm = 16.0f * uiScale;
        const float lx = wp.x + inset, ty = wp.y + inset;
        const float rx = wp.x + ws.x - inset - 1.0f, by = wp.y + ws.y - inset - 1.0f;
        dl->AddLine(ImVec2(lx, ty + arm), ImVec2(lx, ty), silver);
        dl->AddLine(ImVec2(lx, ty), ImVec2(lx + arm, ty), silver);
        dl->AddLine(ImVec2(lx, by - arm), ImVec2(lx, by), silver);
        dl->AddLine(ImVec2(lx, by), ImVec2(lx + arm, by), silver);
        dl->AddLine(ImVec2(rx - arm, by), ImVec2(rx, by), silver);
        dl->AddLine(ImVec2(rx, by - arm), ImVec2(rx, by), silver);

        // seal square pairs with the close button in the title-bar's right side
        const float sealS = 12.0f * uiScale;
        const float btnS = titleH - 8.0f * uiScale;
        const ImVec2 sc(wp.x + ws.x - 4.0f * uiScale - btnS - 6.0f * uiScale - sealS,
                        wp.y + (titleH - sealS) * 0.5f);
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

    // No auto-selection: until the user picks a category the content area
    // shows a hint, and the tree stays fully collapsed. The last selection
    // is remembered for the session (selPath persists across menu open/close).

    // Footer band: both columns stop above it so the footer buttons stay
    // inside the fixed-size window (a full-height content child used to push
    // them past the bottom edge, out of view).
    const float footBand = st.ItemSpacing.y + ImGui::GetFrameHeight();
    const float sidebarW = ComputeSidebarWidth(sections, fw, i18n, uiScale,
                                               ImGui::GetContentRegionAvail().x);

    ImGui::BeginChild("##sections", ImVec2(sidebarW, -footBand));
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
    tc.uiScale = uiScale;
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
    if (!contentNode) {
        // Nothing picked yet (or the picked node was removed): invite instead
        // of silently showing some mod's page.
        ImGui::TextDisabled("%s", i18n.T("select_hint"));
    } else if (contentNode->pages.empty()) {
        ImGui::TextDisabled("(no pages)");
    } else {
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
    }
    ImGui::EndChild();

    // Footer row, pinned to the panel's bottom-left corner: the framework
    // settings switch (gold while open) and save for the schema page being
    // viewed. Closing lives in the title bar (x / Esc / hotkey).
    ImGui::SetCursorPosX(st.WindowPadding.x);
    const bool settingsOpen = selSettings;
    const float footerBtnW = 150.0f * uiScale;
    if (settingsOpen) ImGui::PushStyleColor(ImGuiCol_Text, theme::kGoldBright);
    if (ImGui::Button(i18n.T("page_settings"), ImVec2(footerBtnW, 0.0f))) {
        selSettings = true;
        selPage = 0;
    }
    if (settingsOpen) ImGui::PopStyleColor();
    if (currentSchema) {
        ImGui::SameLine();
        if (ImGui::Button(i18n.T("save"), ImVec2(footerBtnW, 0.0f))) {
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

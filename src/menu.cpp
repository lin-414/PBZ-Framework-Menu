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

#include <imgui.h>
#include <cstdio>

namespace pbzfm {

namespace {

// The framework's own config/i18n, borrowed from pbzfm.cpp through DrawMenu so
// the settings page can render like any client page.
Config* s_cfg = nullptr;
I18n* s_i18n = nullptr;
MenuUi* s_ui = nullptr;
bool s_frameworkRegistered = false;

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

    ImGui::Separator();

    ImGui::TextUnformatted(i18n.T("toggle_key"));
    ImGui::SameLine(0.0f, 12.0f);
    char keyLabel[96];
    snprintf(keyLabel, sizeof(keyLabel), "[%s]", Config::VkName(cfg.toggleVk));
    bool capturingToggle = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                           g_captureKind.load(std::memory_order_relaxed) == CAP_TOGGLE;
    if (capturingToggle) {
        ImGui::Button(i18n.T("capturing"), ImVec2(150.0f, 0.0f));
    } else if (ImGui::Button(keyLabel, ImVec2(150.0f, 0.0f))) {
        g_captureKind.store(CAP_TOGGLE, std::memory_order_relaxed);
        g_captureMode.store(1, std::memory_order_relaxed);
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
    ui.capturing = g_captureMode.load(std::memory_order_relaxed) == 1;
    s_cfg = &cfg;
    s_i18n = &i18n;
    s_ui = &ui;

    // FirstUseEver: the user's drag-move/resize persists for the session
    // (ImGuiCond_Always snapped the window back every frame).
    ImGui::SetNextWindowPos(ImVec2(layout::kPanelX, layout::kPanelY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(layout::kPanelW, layout::kPanelH), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.11f, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.06f, 0.06f, 0.09f, cfg.uiOpacity));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.10f, 0.16f, cfg.uiOpacity));

    ImGui::Begin(i18n.T("title"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(3);

    static int selSection = 0;
    static int selPage = 0;

    // Hold the registry for the whole frame: registration from other threads
    // waits at most one frame; our own recursive calls stay legal.
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    auto& sections = RegistrySections();

    // Framework section always first; its settings page is a normal custom page.
    // Labels are stored as raw i18n keys: the render sites below run them
    // through T(), so a language switch applies without re-registering.
    Section* fw = RegistryGetOrCreateSection("framework_section");
    if (fw && fw->pages.empty()) {
        Page p;
        p.label = "page_settings";
        p.fn = &FrameworkSettingsPage;
        RegistryAddPage(fw, p);
        s_frameworkRegistered = true;
    }

    if (selSection >= (int)sections.size()) selSection = 0;
    if (selSection >= 0 && selSection < (int)sections.size() &&
        selPage >= (int)sections[selSection]->pages.size()) {
        selPage = 0;
    }

    ImGui::BeginChild("##sections", ImVec2(190.0f, -34.0f));
    RegistryResetSectionRects();
    for (int i = 0; i < (int)sections.size(); ++i) {
        if (ImGui::Selectable(i18n.T(sections[i]->name.c_str()), selSection == i)) {
            selSection = i;
            selPage = 0;
        }
        ImVec2 ra = ImGui::GetItemRectMin();
        ImVec2 rb = ImGui::GetItemRectMax();
        float rect[4] = { ra.x, ra.y, rb.x, rb.y };
        RegistryRecordSectionRect(i, rect);
    }
    ImGui::EndChild();
    if (!s_frameworkRegistered) {
        Log("[ui] frame 1: sidebar done (%d sections)", (int)sections.size());
    }
    ImGui::SameLine();

    ImGui::BeginChild("##content");
    ConfigDef* currentSchema = nullptr;
    if (selSection >= 0 && selSection < (int)sections.size() && !sections[selSection]->pages.empty()) {
        Section& sec = *sections[selSection];
        if (ImGui::BeginTabBar("##pages")) {
            for (int p = 0; p < (int)sec.pages.size(); ++p) {
                Page& pg = sec.pages[p];
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

    // Footer: save applies to the schema page being viewed; close always.
    if (currentSchema) {
        if (ImGui::Button(i18n.T("save"), ImVec2(150.0f, 0.0f))) {
            SchemaSaveConfigNow(currentSchema);
            ui.saveFlash = 2.0f;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(i18n.T("close_menu"), ImVec2(150.0f, 0.0f))) {
        g_closeRequest.store(true, std::memory_order_release);
    }
    if (ui.saveFlash > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n.T("saved"));
    }

    ImGui::End();
}

} // namespace pbzfm

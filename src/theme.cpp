#include "theme.h"

namespace pbzfm { namespace theme {

void ApplyPbzTheme(float windowOpacity) {
    ImGuiStyle& s = ImGui::GetStyle();

    // Sharp, hairline-outlined geometry - the game's HUD frames everything
    // with 1px strokes and has no rounded corner anywhere.
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding = 0.0f;

    s.WindowBorderSize = 1.0f;   // silver hairline around the panel
    s.ChildBorderSize = 0.0f;    // inner regions divide by color, not stroke
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;    // every widget outlined like a HUD bar
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 2.0f; // selected-tab gold underline thickness

    // Kept identical to the pre-theme layout so the host's exported-rect
    // automation keeps hitting the same controls.
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 6);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 11.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);  // centered title, like boss nameplates
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

    s.FontScaleMain = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                      = kRice;
    c[ImGuiCol_TextDisabled]              = kRiceDim;
    c[ImGuiCol_WindowBg]                  = WithAlpha(kPanel, windowOpacity);
    c[ImGuiCol_ChildBg]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]                   = WithAlpha(ImVec4(0.075f, 0.071f, 0.066f, 1.0f), 0.97f);
    c[ImGuiCol_Border]                    = WithAlpha(kSilverDim, 0.55f);
    c[ImGuiCol_BorderShadow]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]                   = kFrame;
    c[ImGuiCol_FrameBgHovered]            = kFrameHover;
    c[ImGuiCol_FrameBgActive]             = kFrameActive;
    c[ImGuiCol_TitleBg]                   = kInk;
    c[ImGuiCol_TitleBgActive]             = ImVec4(0.078f, 0.071f, 0.059f, 1.0f);  // warm shift when focused
    c[ImGuiCol_TitleBgCollapsed]          = WithAlpha(kInk, 0.65f);
    c[ImGuiCol_MenuBarBg]                 = kInk;
    c[ImGuiCol_ScrollbarBg]               = WithAlpha(kInk, 0.90f);
    c[ImGuiCol_ScrollbarGrab]             = ImVec4(0.227f, 0.212f, 0.180f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]      = kGold;
    c[ImGuiCol_ScrollbarGrabActive]       = kGoldBright;
    c[ImGuiCol_CheckMark]                 = kCinnabar;            // checkbox = cinnabar seal mark
    c[ImGuiCol_SliderGrab]                = kSilver;              // HUD health-bar silver
    c[ImGuiCol_SliderGrabActive]          = kGoldBright;
    c[ImGuiCol_Button]                    = kButton;
    c[ImGuiCol_ButtonHovered]             = kButtonHov;
    c[ImGuiCol_ButtonActive]              = kButtonAct;
    c[ImGuiCol_Header]                    = kFrameHover;
    c[ImGuiCol_HeaderHovered]             = kButtonHov;
    c[ImGuiCol_HeaderActive]              = kButtonAct;
    c[ImGuiCol_Separator]                 = WithAlpha(kSilverDim, 0.45f);
    c[ImGuiCol_SeparatorHovered]          = kGold;
    c[ImGuiCol_SeparatorActive]           = kGoldBright;
    c[ImGuiCol_ResizeGrip]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]         = WithAlpha(kGold, 0.60f);
    c[ImGuiCol_ResizeGripActive]          = WithAlpha(kGoldBright, 0.80f);
    c[ImGuiCol_InputTextCursor]           = kGoldBright;
    c[ImGuiCol_TabHovered]                = kFrameHover;
    c[ImGuiCol_Tab]                       = ImVec4(0.098f, 0.090f, 0.078f, 1.0f);
    c[ImGuiCol_TabSelected]               = ImVec4(0.133f, 0.122f, 0.098f, 1.0f);
    c[ImGuiCol_TabSelectedOverline]       = kGold;                // gold underline on the active tab
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.078f, 0.071f, 0.063f, 1.0f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.133f, 0.122f, 0.098f, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(kGold, 0.40f);
    c[ImGuiCol_PlotLines]                 = WithAlpha(kGold, 0.80f);
    c[ImGuiCol_PlotLinesHovered]          = kGoldBright;
    c[ImGuiCol_PlotHistogram]             = WithAlpha(kGold, 0.55f);
    c[ImGuiCol_PlotHistogramHovered]      = kGoldBright;
    c[ImGuiCol_TableHeaderBg]             = ImVec4(0.125f, 0.114f, 0.094f, 1.0f);
    c[ImGuiCol_TableBorderStrong]         = ImVec4(0.290f, 0.267f, 0.220f, 1.0f);
    c[ImGuiCol_TableBorderLight]          = ImVec4(0.200f, 0.184f, 0.153f, 1.0f);
    c[ImGuiCol_TableRowBg]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]             = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextLink]                  = kGoldBright;
    c[ImGuiCol_TextSelectedBg]            = WithAlpha(kGold, 0.35f);
    c[ImGuiCol_TreeLines]                 = ImVec4(0.290f, 0.267f, 0.220f, 1.0f);
    c[ImGuiCol_DragDropTarget]            = kGold;
    c[ImGuiCol_NavCursor]                 = WithAlpha(kGold, 0.90f);
    c[ImGuiCol_NavWindowingHighlight]     = WithAlpha(kRice, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]         = WithAlpha(kInk, 0.65f);
    c[ImGuiCol_ModalWindowDimBg]          = WithAlpha(kInk, 0.70f);
}

void PushCapturingStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(kCinnabarDp, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(kCinnabar, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kCinnabar);
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(kCinnabar, 0.80f));
}

void PopCapturingStyle() {
    ImGui::PopStyleColor(4);
}

} } // namespace pbzfm::theme

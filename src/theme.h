// PBZFM theme - "ink & gold" (玄墨鎏金). Palette and shapes distilled from the
// game's HUD components (boss bar / nameplates / skill diamonds): near-black
// ink panels, silver hairline frames, bronze-gold highlights, cinnabar-red
// marks, sharp corners everywhere. ApplyPbzTheme assigns every ImGuiCol so no
// default-theme value leaks through; menu.cpp reuses the palette for the
// hand-drawn decorations (title hairline, corner brackets, seal square).
#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace pbzfm { namespace theme {

// Palette (RGB, gamma-space ImVec4; a=1 unless noted).
inline const ImVec4 kInk        = ImVec4(0.047f, 0.043f, 0.039f, 1.00f); // 玄墨 - title/scrollbar bg
inline const ImVec4 kPanel      = ImVec4(0.075f, 0.071f, 0.066f, 0.94f); // 窗口底
inline const ImVec4 kFrame      = ImVec4(0.114f, 0.106f, 0.094f, 0.95f); // 控件底
inline const ImVec4 kFrameHover = ImVec4(0.149f, 0.137f, 0.118f, 0.95f);
inline const ImVec4 kFrameActive= ImVec4(0.180f, 0.165f, 0.137f, 0.95f);
inline const ImVec4 kButton     = ImVec4(0.137f, 0.125f, 0.106f, 1.00f);
inline const ImVec4 kButtonHov  = ImVec4(0.227f, 0.204f, 0.165f, 1.00f);
inline const ImVec4 kButtonAct  = ImVec4(0.333f, 0.286f, 0.184f, 1.00f);
inline const ImVec4 kSilver     = ImVec4(0.851f, 0.835f, 0.788f, 1.00f); // 银 - HUD 白
inline const ImVec4 kSilverDim  = ImVec4(0.545f, 0.525f, 0.482f, 1.00f); // 暗银 - hairlines
inline const ImVec4 kGold       = ImVec4(0.788f, 0.643f, 0.408f, 1.00f); // 鎏金
inline const ImVec4 kGoldBright = ImVec4(0.914f, 0.804f, 0.576f, 1.00f);
inline const ImVec4 kCinnabar   = ImVec4(0.824f, 0.329f, 0.247f, 1.00f); // 朱砂
inline const ImVec4 kCinnabarDp = ImVec4(0.560f, 0.180f, 0.129f, 1.00f);
inline const ImVec4 kRice       = ImVec4(0.910f, 0.890f, 0.835f, 1.00f); // 宣纸白
inline const ImVec4 kRiceDim    = ImVec4(0.486f, 0.463f, 0.424f, 1.00f);

inline ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// Full-replacement style: layout paddings match the values the test host's
// click geometry was tuned against (FramePadding 10,6 / ItemSpacing 10,6 /
// WindowPadding 12,10) at ui scale 1.0 - only colors, rounding and border
// sizes change here. ApplyUiScale() derives the effective per-frame style.
void ApplyPbzTheme(float windowOpacity);

// Rebuilds the ImGui style from the unscaled theme baseline and applies the
// global UI scale to BOTH the font and every spacing/metric (padding, item
// spacing, scrollbar, grab size, ...). FontScaleMain alone only grows glyphs:
// fixed-width columns then clip their text. Call once per frame, before any
// window or widget is submitted. Safe to call with a changing value every
// frame: the base snapshot is never scaled in place, so scales do not
// compound across frames.
void ApplyUiScale(float uiScale);

// Recording-state styling for key-capture buttons (cinnabar, like the game's
// parry marks). Pair PushCapturingStyle/PopCapturingStyle.
void PushCapturingStyle();
void PopCapturingStyle();

// Theme files (v0.4): "ink" is the built-in palette; any other name loads
// ModRoot\themes\<name>.json over the built-in style. Colors ("Text",
// "ButtonHovered", ... as "#RRGGBB"/"#RRGGBBAA"), corner rounding and border
// sizes are overridable; layout paddings are NOT (the host's exported-rect
// automation depends on them). A missing or unparsable file keeps the
// built-in style. Render thread only.
void ApplyThemeByName(const std::string& nameUtf8);
// M4: the list is cached; rescan the themes directory only on demand (the
// picker calls this when its popup opens, so a newly dropped theme file is
// still picked up without scanning the disk on every frame).
void RefreshThemeNames();
// "ink" first, then the sorted stems of ModRoot\themes\*.json (cached).
const std::vector<std::string>& ListThemeNames();

} } // namespace pbzfm::theme

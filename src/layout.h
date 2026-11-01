// Panel layout constants shared between the in-game menu (menu.cpp, ImGui
// units) and the test host. The host clicks the demo mod's section/tab/checkbox
// at these predictable positions; the demo button itself is queried at runtime
// through the demo mod's PBZFMDEMO_GetButton export.
#pragma once

namespace pbzfm { namespace layout {

constexpr float kPanelX = 100.0f;   // panel top-left, overlay client coords
constexpr float kPanelY = 80.0f;
constexpr float kPanelW = 660.0f;   // panel size (fixed, sidebar + content)
constexpr float kPanelH = 480.0f;
constexpr float kSidebarW = 190.0f;
constexpr float kPanelMinW = 420.0f;  // resize floor: sidebar + minimal readable content
constexpr float kPanelMinH = 320.0f;  // title/tab bar + a usable content column

// Click targets relative to the game-window client top-left. Based on style:
// title 30, WindowPadding.y 10, sidebar item height ~30, tab bar ~30.
// Fine-tuned against the first host screenshot of the new layout.
constexpr float kDemoSectionCy = 165.0f;  // second sidebar entry (demo mod)
constexpr float kFirstBoolCy   = 181.0f;  // first schema item (checkbox) center
constexpr float kTab2Cx        = 402.0f;  // second page tab center x
constexpr float kTabCy         = 145.0f;  // page tab row center y

// Footer settings button (panel bottom-left), center. Inside the 150-wide
// button for either frame height (font 13..18 at ui_scale 1.0).
constexpr float kSettingsBtnCx = 187.0f;  // kPanelX + WindowPadding.x + 75
constexpr float kSettingsBtnCy = 540.0f;  // kPanelY + kPanelH - WindowPadding.y - frameH/2

} } // namespace pbzfm::layout

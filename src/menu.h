#pragma once

namespace pbzfm {

struct Config;
class I18n;

struct MenuUi {
    float saveFlash = 0.0f;   // seconds remaining of the "saved" hint
    float shotFlash = 0.0f;   // seconds remaining of the "screenshot saved" hint
    bool  capturing = false;  // mirrored from g_captureMode for display
    bool  requestSave = false; // set by the save button / language switch
    bool  requestScreenshot = false; // set by the screenshot button
    bool  scaleEditing = false; // ui_scale slider held: defer panel resize to avoid jumping under the cursor
};

void DrawMenu(Config& cfg, I18n& i18n, MenuUi& ui, float dt);

} // namespace pbzfm

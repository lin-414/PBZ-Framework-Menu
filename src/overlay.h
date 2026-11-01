#pragma once
#include <string>

namespace pbzfm {

struct Config;
class I18n;
struct GameWindow;

// Owns the DirectComposition overlay window, the private D3D11 device, the
// ImGui context and the render loop. Runs entirely on its own thread and
// never touches the game's swap chain (frame-generation safe).
bool OverlayStart(GameWindow* gw, Config* cfg, I18n* i18n, const std::wstring& iniPath);
void OverlayStop();

} // namespace pbzfm

#pragma once
#include <string>

struct ID3D11Device;

namespace pbzfm {

struct Config;
class I18n;
struct GameWindow;

// The overlay's private D3D11 device (null until the overlay is up). The
// texture uploader creates its SRVs on it from the render thread.
ID3D11Device* OverlayDevice();

// Owns the DirectComposition overlay window, the private D3D11 device, the
// ImGui context and the render loop. Runs entirely on its own thread and
// never touches the game's swap chain (frame-generation safe).
bool OverlayStart(GameWindow* gw, Config* cfg, I18n* i18n, const std::wstring& iniPath);
void OverlayStop();

} // namespace pbzfm

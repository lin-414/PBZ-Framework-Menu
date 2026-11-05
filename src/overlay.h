#pragma once
#include <string>
#include <vector>

struct ID3D11Device;

namespace pbzfm {

struct Config;
class I18n;
struct GameWindow;

// M10: BGRA (DIB pixels, alpha meaningless) -> PNG bytes with alpha forced to
// 255. Shared by the screenshot path and the host's alpha regression test.
bool EncodeOpaqueBgraPng(const unsigned char* bgra, int w, int h, std::vector<unsigned char>& png);

// The overlay's private D3D11 device (null until the overlay is up). The
// texture uploader creates its SRVs on it from the render thread.
ID3D11Device* OverlayDevice();

// Owns the DirectComposition overlay window, the private D3D11 device, the
// ImGui context and the render loop. Runs entirely on its own thread and
// never touches the game's swap chain (frame-generation safe).
bool OverlayStart(GameWindow* gw, Config* cfg, I18n* i18n, const std::wstring& iniPath);
// Joins the render thread, then restores the game's WndProc. Returns false when
// either step could not be guaranteed (see the log for the explicit error).
bool OverlayStop();

} // namespace pbzfm

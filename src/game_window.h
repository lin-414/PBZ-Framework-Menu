#pragma once
#include <windows.h>

namespace pbzfm {

// Locates the game's main window ("UnrealWindow" class in this process),
// tracks its client geometry on screen and subclasses its WndProc so the
// overlay can ask the window thread to show/hide the cursor (ShowCursor is
// per-thread, the trick proven by PBZPhotoMode).
struct GameWindow {
    HWND hwnd = nullptr;
    int  x = 0, y = 0, w = 0, h = 0;   // client origin on screen + client size

    bool Find();              // locate + attach (subclass). false when none found.
    bool Detach();            // restore the original WndProc; false when it could not be guaranteed
    bool Valid() const;       // hwnd still alive, visible, same class
    bool UpdateGeometry();    // refresh x/y/w/h; returns true when changed
    void ShowCursor(bool show); // posts to the window's owning thread
    void UnclipCursor();        // releases any cursor clip the game set
};

} // namespace pbzfm

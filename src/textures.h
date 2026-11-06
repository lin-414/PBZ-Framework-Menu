#pragma once
// Runtime textures for the v2 API (PBZFM_LoadTexture / Image / DisposeTexture).
// Handles are handed out on the calling thread; decode (stb_image) and the D3D
// upload happen on the render thread (the device context lives there), so
// registration never touches D3D from a foreign thread and a texture that has
// not loaded yet simply draws nothing until the upload lands.
#include <string>

namespace pbzfm {

// Opaque handle, never null for a valid path; repeated loads of the same path
// share one entry. Relative paths resolve against the framework mod root.
// The handle is an internal slot id + generation (review H4), so a handle whose
// texture was released is stale, never dangling.
void* TexturesLoad(const char* path_utf8);
// Flags the texture for release; the GPU memory goes away on the next render
// frame. Safe from any thread and safe on a stale handle (ignored + logged).
void  TexturesDispose(void* texture);
// True once the SRV is live; only the render thread may draw.
bool  TexturesDraw(void* texture, float w, float h);
// Render thread, once per frame before ImGui::NewFrame: complete pending
// uploads, reap disposed entries. A failed load releases its slot, so the same
// path can be retried with a fresh LoadTexture.
void  TexturesProcessPending();
// Render thread teardown: release every SRV before the device goes away.
void  TexturesShutdown();

} // namespace pbzfm

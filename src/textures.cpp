#include "textures.h"

#include "common.h"
#include "overlay.h"
#include "texture_slots.h"

#include <d3d11.h>
#include <imgui.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using Microsoft::WRL::ComPtr;

namespace pbzfm {

namespace {

// Review H4: a corrupt or oversized image must never stall the render thread or
// exhaust process memory. 32 MiB on disk and 16 MP decoded (64 MiB RGBA) covers
// every sane icon/screenshot.
constexpr size_t   kMaxFileBytes = 32u << 20;
constexpr int      kMaxDim = 8192;
constexpr uint64_t kMaxPixels = 16ull << 20;

struct TexEntry {
    std::string pathKey;              // utf8 key handed to LoadTexture (dedupe)
    std::wstring path;                // resolved file path
    int w = 0, h = 0;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::atomic<bool> dispose{ false };   // flagged by DisposeTexture
    bool ready = false;               // srv valid (render thread only)
    bool failed = false;              // decode/upload failed; slot is released for retry
};

// Review H4: the void* handle is a slot-map id - (index, generation) packed into
// the pointer the C API already promises - instead of a TexEntry*. A stale
// handle mismatches its generation and is ignored, so Dispose/Draw can never
// dereference freed memory. The packing and the slot table live in
// texture_slots.h (device-free) so framework_test can cover stale-handle and
// failed-load-retry logic; only the render thread deletes entries (process /
// reap / shutdown), foreign threads only flip the dispose flag and touch the
// table under g_texMtx.
std::mutex g_texMtx;
TextureSlotTable<TexEntry> g_slots;
struct Pending {
    unsigned slot = 0;
    unsigned generation = 0;
    TexEntry* entry = nullptr;
};
std::vector<Pending> g_pendingLoad;         // waiting for the render thread
bool g_shutdown = false;                    // no SRV can be created after teardown
std::atomic<int> g_staleHandleLogs{ 0 };

// Tell a mod author that a cached handle died, but cap the noise so a broken
// mod cannot flood the log (review H4).
void LogStaleHandle(const char* op) {
    const int n = g_staleHandleLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) Log("[textures] %s on stale texture handle ignored", op);
}

// A path is anchored when it is absolute (drive letter or UNC).
bool IsAnchored(const std::string& u) {
    if (u.size() >= 2 && ((u[0] >= 'A' && u[0] <= 'Z') || (u[0] >= 'a' && u[0] <= 'z')) && u[1] == ':') return true;
    return u.size() >= 2 && u[0] == '\\' && u[1] == '\\';
}

std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

// Bounded read (review H4): the size is checked before opening the file, so a
// rogue multi-GB image cannot be pulled into RAM by the render thread.
std::vector<unsigned char> ReadFileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return {};
    const uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size > kMaxFileBytes) {
        Log("[textures] file too large (%llu bytes, limit %llu): %ls",
            (unsigned long long)size, (unsigned long long)kMaxFileBytes, path.c_str());
        return {};
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::vector<unsigned char> out;
    out.reserve((size_t)size);
    unsigned char buf[65536];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + rd);
        if (out.size() > kMaxFileBytes) { out.clear(); break; }   // file grew after stat
    }
    fclose(f);
    return out;
}

} // namespace

void* TexturesLoad(const char* path_utf8) {
    if (!path_utf8 || !*path_utf8) return nullptr;
    const std::string key = path_utf8;
    std::lock_guard<std::mutex> lock(g_texMtx);
    if (g_shutdown) return nullptr;   // overlay torn down: no SRV could ever land

    auto found = g_slots.byPath.find(key);
    if (found != g_slots.byPath.end()) {
        return (void*)MakeTextureHandle(found->second, g_slots.slots[found->second].generation);
    }

    // Reuse a free slot; Acquire bumps the generation so every handle handed out
    // for this index goes stale (review H4).
    TexEntry* entry = nullptr;
    const unsigned idx = g_slots.Acquire(entry);
    entry->pathKey = key;
    entry->path = IsAnchored(key) ? Utf8ToWide(key) : ModRoot() + L"\\" + Utf8ToWide(key);
    g_slots.byPath[key] = idx;
    g_pendingLoad.push_back({ idx, g_slots.slots[idx].generation, entry });
    Log("[textures] load requested: %s", key.c_str());
    return (void*)MakeTextureHandle(idx, g_slots.slots[idx].generation);
}

void TexturesDispose(void* texture) {
    std::lock_guard<std::mutex> lock(g_texMtx);
    TexEntry* e = g_slots.Lookup(texture);
    if (!e) {
        LogStaleHandle("Dispose");
        return;
    }
    e->dispose.store(true, std::memory_order_release);
}

bool TexturesDraw(void* texture, float w, float h) {
    ComPtr<ID3D11ShaderResourceView> srv;
    {
        std::lock_guard<std::mutex> lock(g_texMtx);
        TexEntry* e = g_slots.Lookup(texture);
        if (!e) {
            LogStaleHandle("Draw");
            return false;
        }
        if (!e->ready || !e->srv) return false;
        srv = e->srv;   // AddRef: survives a reap racing on another thread
    }
    ImGui::Image((ImTextureID)(ImU64)(uintptr_t)srv.Get(), ImVec2(w, h));
    return true;
}

void TexturesProcessPending() {
    ID3D11Device* dev = OverlayDevice();
    if (!dev) return;

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(g_texMtx);
        g_shutdown = false;   // device is back: Loads registered from now on are live
        batch.swap(g_pendingLoad);
    }
    for (const Pending& p : batch) {
        TexEntry* e = p.entry;
        if (!e || e->dispose.load(std::memory_order_relaxed) || e->ready || e->failed) continue;

        std::vector<unsigned char> bytes = ReadFileBytes(e->path);
        if (bytes.empty()) {
            e->failed = true;
            Log("[textures] read failed or rejected: %ls", e->path.c_str());
            continue;
        }
        // Probe dimensions before the full decode so a huge image is rejected
        // without allocating its decoded buffer (review H4).
        int probeW = 0, probeH = 0, probeComp = 0;
        if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &probeW, &probeH, &probeComp) ||
            probeW <= 0 || probeH <= 0 || probeW > kMaxDim || probeH > kMaxDim ||
            (uint64_t)probeW * (uint64_t)probeH > kMaxPixels) {
            e->failed = true;
            Log("[textures] rejected image %dx%d: %ls", probeW, probeH, e->path.c_str());
            continue;
        }
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
        if (!px) {
            e->failed = true;
            Log("[textures] decode failed: %ls", e->path.c_str());
            continue;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ px, 4 * (SIZE_T)w, 0 };
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hr = dev->CreateTexture2D(&desc, &init, &tex);
        if (SUCCEEDED(hr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = desc.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            hr = dev->CreateShaderResourceView(tex.Get(), &sv, &srv);
        }
        stbi_image_free(px);
        // Publish under the lock only when the slot still belongs to this
        // entry (it may have been disposed meanwhile); otherwise the upload is
        // dropped (review H4).
        {
            std::lock_guard<std::mutex> lock(g_texMtx);
            const bool live = p.slot < g_slots.slots.size() &&
                              g_slots.slots[p.slot].generation == p.generation &&
                              g_slots.slots[p.slot].entry.get() == e;
            if (live && !e->dispose.load(std::memory_order_relaxed)) {
                if (SUCCEEDED(hr)) {
                    e->srv = srv;
                    e->w = w;
                    e->h = h;
                    e->ready = true;
                } else {
                    e->failed = true;
                }
            }
        }
        if (SUCCEEDED(hr)) {
            Log("[textures] loaded: %ls (%dx%d)", e->path.c_str(), w, h);
        } else {
            Log("[textures] upload failed hr=0x%08lX: %ls", (unsigned long)hr, e->path.c_str());
        }
    }

    // Reap disposed/failed entries (render thread owns entry lifetimes). Failed
    // slots are detached from the path map so a later Load of the same path
    // retries instead of returning a dead handle forever (review H4).
    std::lock_guard<std::mutex> lock(g_texMtx);
    for (unsigned i = 0; i < g_slots.slots.size(); ++i) {
        TexEntry* e = g_slots.slots[i].entry.get();
        if (!e) continue;
        if (e->dispose.load(std::memory_order_relaxed) || e->failed) g_slots.Release(i);
    }
}

void TexturesShutdown() {
    std::lock_guard<std::mutex> lock(g_texMtx);
    g_shutdown = true;   // stop new Loads until a device shows up again
    // Keep the slots (only their generations grow): clearing them would let a
    // recycled index re-validate an old handle (review H4).
    for (unsigned i = 0; i < g_slots.slots.size(); ++i) g_slots.Release(i);
    g_slots.byPath.clear();
    g_pendingLoad.clear();
}

} // namespace pbzfm

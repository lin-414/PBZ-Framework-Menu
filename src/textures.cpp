#include "textures.h"

#include "common.h"
#include "overlay.h"

#include <d3d11.h>
#include <imgui.h>
#include <map>
#include <mutex>
#include <atomic>
#include <vector>
#include <wrl/client.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using Microsoft::WRL::ComPtr;

namespace pbzfm {

namespace {

struct TexEntry {
    std::string pathKey;              // utf8 key handed to LoadTexture (dedupe)
    std::wstring path;                // resolved file path
    int w = 0, h = 0;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::atomic<bool> dispose{ false };   // flagged by DisposeTexture
    bool ready = false;               // srv valid (render thread only)
    bool failed = false;              // decode/upload failed; stays until disposed
};

std::mutex g_texMtx;
std::map<std::string, TexEntry*> g_byPath;
std::vector<TexEntry*> g_order;           // owns the entries
std::vector<TexEntry*> g_pendingLoad;     // waiting for the render thread

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

std::vector<unsigned char> ReadFileBytes(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::vector<unsigned char> out;
    unsigned char buf[65536];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + rd);
    fclose(f);
    return out;
}

} // namespace

void* TexturesLoad(const char* path_utf8) {
    if (!path_utf8 || !*path_utf8) return nullptr;
    const std::string key = path_utf8;
    std::lock_guard<std::mutex> lock(g_texMtx);
    auto found = g_byPath.find(key);
    if (found != g_byPath.end()) return found->second;

    TexEntry* e = new TexEntry();
    e->pathKey = key;
    e->path = IsAnchored(key) ? Utf8ToWide(key) : ModRoot() + L"\\" + Utf8ToWide(key);
    g_byPath[key] = e;
    g_order.push_back(e);
    g_pendingLoad.push_back(e);
    Log("[textures] load requested: %s", key.c_str());
    return e;
}

void TexturesDispose(void* texture) {
    TexEntry* e = (TexEntry*)texture;
    if (e) e->dispose.store(true, std::memory_order_release);
}

bool TexturesDraw(void* texture, float w, float h) {
    TexEntry* e = (TexEntry*)texture;
    if (!e || !e->ready || !e->srv) return false;
    ImGui::Image((ImTextureID)(ImU64)(uintptr_t)e->srv.Get(), ImVec2(w, h));
    return true;
}

void TexturesProcessPending() {
    ID3D11Device* dev = OverlayDevice();
    if (!dev) return;

    std::vector<TexEntry*> batch;
    {
        std::lock_guard<std::mutex> lock(g_texMtx);
        batch.swap(g_pendingLoad);
    }
    for (TexEntry* e : batch) {
        if (e->dispose.load(std::memory_order_relaxed) || e->ready || e->failed) continue;
        std::vector<unsigned char> bytes = ReadFileBytes(e->path);
        int w = 0, h = 0, comp = 0;
        unsigned char* px = bytes.empty() ? nullptr
                                          : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
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
        HRESULT hr = dev->CreateTexture2D(&desc, &init, &tex);
        if (SUCCEEDED(hr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = desc.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            hr = dev->CreateShaderResourceView(tex.Get(), &sv, &e->srv);
        }
        stbi_image_free(px);
        if (SUCCEEDED(hr)) {
            e->w = w;
            e->h = h;
            e->ready = true;
            Log("[textures] loaded: %ls (%dx%d)", e->path.c_str(), w, h);
        } else {
            e->failed = true;
            Log("[textures] upload failed hr=0x%08lX: %ls", (unsigned long)hr, e->path.c_str());
        }
    }

    // Reap disposed entries (render thread owns entry lifetimes).
    std::lock_guard<std::mutex> lock(g_texMtx);
    for (auto it = g_order.begin(); it != g_order.end();) {
        TexEntry* e = *it;
        if (e->dispose.load(std::memory_order_relaxed)) {
            g_byPath.erase(e->pathKey);
            e->srv.Reset();
            delete e;
            it = g_order.erase(it);
        } else {
            ++it;
        }
    }
}

void TexturesShutdown() {
    std::lock_guard<std::mutex> lock(g_texMtx);
    for (TexEntry* e : g_order) {
        e->srv.Reset();
        delete e;
    }
    g_order.clear();
    g_byPath.clear();
    g_pendingLoad.clear();
}

} // namespace pbzfm

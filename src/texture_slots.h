#pragma once
// H4: the texture cache's handle packing and slot table are device-free. The
// D3D11 upload/decode stays in textures.cpp; this header is shared with
// framework_test so stale-handle and failed-load-retry behavior can be tested
// without a GPU, a swapchain or a game window.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pbzfm {

// (index, generation) packed into the void* the C API promises. Generation 0 is
// reserved for "never handed out", so a slot's first handle is generation 1.
inline uintptr_t MakeTextureHandle(unsigned slot, unsigned generation) {
    return ((uintptr_t)generation << 32) | ((uintptr_t)slot + 1u);
}

inline bool ParseTextureHandle(void* texture, unsigned& slot, unsigned& generation) {
    const uintptr_t v = (uintptr_t)texture;
    const uintptr_t low = v & 0xFFFFFFFFu;
    if (!low) return false;
    slot = (unsigned)(low - 1u);
    generation = (unsigned)(v >> 32);
    return true;
}

template <typename Entry>
struct TextureSlot {
    std::unique_ptr<Entry> entry;
    unsigned generation = 0;          // bumped every time the slot is released
};

// Caller holds the owning mutex (textures.cpp's g_texMtx). Entry must expose a
// `pathKey` member (the dedupe key erased on release).
template <typename Entry>
class TextureSlotTable {
public:
    static_assert(sizeof(uintptr_t) == 8, "texture handles pack slot+generation (x64 build)");

    std::vector<TextureSlot<Entry>> slots;
    std::map<std::string, unsigned> byPath;   // path key -> slot index

    // The live entry for a handle, or null for a stale/unknown/released one.
    Entry* Lookup(void* texture) {
        unsigned slot = 0, generation = 0;
        if (!ParseTextureHandle(texture, slot, generation)) return nullptr;
        if (slot >= slots.size() || slots[slot].generation != generation) return nullptr;
        return slots[slot].entry.get();
    }

    // Reuse a free slot or append one; the generation bump invalidates every
    // handle handed out for this index. The caller owns the entry contents
    // (pathKey / path) and the byPath insert.
    unsigned Acquire(Entry*& created) {
        unsigned idx = 0;
        for (; idx < slots.size(); ++idx) {
            if (!slots[idx].entry) break;
        }
        if (idx == slots.size()) slots.emplace_back();
        TextureSlot<Entry>& s = slots[idx];
        if (++s.generation == 0) s.generation = 1;   // 0 = "never handed out"
        s.entry = std::make_unique<Entry>();
        created = s.entry.get();
        return idx;
    }

    // Detach the entry from its path, destroy the payload (the entry destructor
    // releases the SRV in textures.cpp) and bump the generation so all handles
    // for this slot go stale. A failed entry is removed the same way, which is
    // what lets a later Load of the same path retry with a fresh slot.
    void Release(unsigned idx) {
        if (idx >= slots.size() || !slots[idx].entry) return;
        const Entry* e = slots[idx].entry.get();
        auto it = byPath.find(e->pathKey);
        if (it != byPath.end() && it->second == idx) byPath.erase(it);
        slots[idx].entry.reset();
        if (++slots[idx].generation == 0) slots[idx].generation = 1;
    }
};

} // namespace pbzfm

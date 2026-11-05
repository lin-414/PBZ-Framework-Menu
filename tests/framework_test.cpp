// framework_test: device-free unit coverage for the framework's pure logic.
// Everything here runs without the overlay, D3D11 or a game window: api.cpp /
// registry.cpp / schema.cpp / config.cpp / input.cpp / files.cpp are compiled
// in, and the DLL entry's paths, logging and subsystems are stubbed by
// framework_test_stubs.cpp. Each test names the review item it guards.
#include <windows.h>

#include <imgui.h>

#include "common.h"
#include "config.h"
#include "engine_port.h"
#include "input.h"
#include "pbzfm.h"
#include "registry.h"
#include "texture_slots.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Run(const char* name, void (*fn)());

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("[fail] %s:%d: %s\n", __FUNCTION__, __LINE__, #cond);     \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

std::wstring StorePath(const char* store) {
    const size_t n = std::strlen(store);
    return pbzfm::RootFile(
        (std::wstring(L"data\\") + std::wstring(store, store + n) + L".json").c_str());
}

// Best-effort cleanup only: tests start by writing a known store body, so a
// leftover from an earlier run can never leak into the assertions. (Some
// sandboxed/redirected file systems report delete success while keeping the
// name around, so we do not assert on the delete itself.)
void RemoveStore(const char* store) {
    DeleteFileW(StorePath(store).c_str());
}

bool WriteUtf8(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = text.empty() || fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

std::string ReadUtf8(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::string out;
    char buf[512];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, rd);
    fclose(f);
    return out;
}

// ---- H4: texture slot table (handle packing / generation / retry) ----
struct StubTex {
    std::string pathKey;
    bool failed = false;   // textures.cpp marks failed loads and releases the slot
};

// H4: a stale handle is ignored instead of dereferenced, and releasing a failed
// entry drops it from the path map so the next Load retries with a fresh
// generation (what a device-backed run would do after an upload failure).
void TestTextureSlots() {
    using pbzfm::MakeTextureHandle;
    using pbzfm::TextureSlotTable;

    TextureSlotTable<StubTex> t;
    CHECK(t.Lookup(nullptr) == nullptr);
    CHECK(t.Lookup((void*)MakeTextureHandle(0, 7)) == nullptr);   // no slot allocated yet

    StubTex* a = nullptr;
    const unsigned ia = t.Acquire(a);
    a->pathKey = "tex/a";
    t.byPath["tex/a"] = ia;
    void* ha = (void*)MakeTextureHandle(ia, t.slots[ia].generation);
    CHECK(ia == 0);
    CHECK(t.Lookup(ha) == a);
    CHECK(t.Lookup((void*)MakeTextureHandle(ia, t.slots[ia].generation + 100)) == nullptr);

    StubTex* b = nullptr;
    const unsigned ib = t.Acquire(b);
    b->pathKey = "tex/b";
    t.byPath["tex/b"] = ib;
    void* hb = (void*)MakeTextureHandle(ib, t.slots[ib].generation);
    CHECK(ib == 1);
    CHECK(t.Lookup(hb) == b);

    // Dispose/failure path: the handle goes stale, the path entry disappears.
    t.Release(ia);
    CHECK(t.Lookup(ha) == nullptr);
    CHECK(t.byPath.find("tex/a") == t.byPath.end());
    CHECK(t.Lookup(hb) == b);   // other slots untouched

    // Retry: the freed slot is reused, the generation bump keeps old handles dead.
    StubTex* a2 = nullptr;
    const unsigned ia2 = t.Acquire(a2);
    a2->pathKey = "tex/a";
    t.byPath["tex/a"] = ia2;
    void* ha2 = (void*)MakeTextureHandle(ia2, t.slots[ia2].generation);
    CHECK(ia2 == ia);
    CHECK(ha2 != ha);
    CHECK(t.Lookup(ha) == nullptr);
    CHECK(t.Lookup(ha2) == a2);
}

// ---- H8 / SplitPath: '/'-separated escaped paths ----
// H8: "A\/B" keeps a literal slash in one segment, "C\\D" a literal backslash;
// RegistryPathOf must round-trip the escaped form and FindPath must find it.
void TestPathEscaping() {
    using namespace pbzfm;

    Section* slash = RegistryGetOrCreatePath("Esc/A\\/B");
    CHECK(slash && slash->name == "A/B");
    CHECK(slash && RegistryPathOf(slash) == "Esc/A\\/B");
    CHECK(RegistryFindPath("Esc/A\\/B") == slash);

    Section* backslash = RegistryGetOrCreatePath("Esc/C\\\\D");
    CHECK(backslash && backslash->name == "C\\D");
    CHECK(backslash && RegistryPathOf(backslash) == "Esc/C\\\\D");
    CHECK(RegistryFindPath("Esc/C\\\\D") == backslash);

    Section* leaf = RegistryGetOrCreatePath("Esc/Deep/Leaf");
    CHECK(leaf && RegistryPathOf(leaf) == "Esc/Deep/Leaf");
    CHECK(RegistryFindPath("Esc/Deep/Leaf") == leaf);
    CHECK(RegistryFindPath("Esc/Missing") == nullptr);
}

// ---- H8: node lifetime across concurrent delete ----
// H8: RegistryWithPath must keep the node alive for the whole work callback
// while another thread deletes/recreates the same path. The single recursive
// lock is the guard; a UAF here would crash or corrupt the name we read.
void TestRegistryConcurrency() {
    using namespace pbzfm;

    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::atomic<int> workRuns{ 0 };
    constexpr int kIter = 1500;

    std::thread worker([&] {
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < kIter; ++i) {
            RegistryWithPath("Stress/Live", [&](Section* s) {
                Page p;
                p.label = "row";
                RegistryAddPage(s, p);          // touches the node under the lock
                (void)s->name.size();           // reads the node that may be deleted
                ++workRuns;
            });
        }
    });
    std::thread deleter([&] {
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < kIter; ++i) RegistryDeleteSection("Stress/Live");
    });
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    worker.join();
    deleter.join();

    CHECK(workRuns == kIter);
    Section* s = RegistryFindPath("Stress/Live");
    CHECK(!s || RegistryPathOf(s) == "Stress/Live");
}

// ---- H6 / L1 / L5: hotkey queue ----
std::vector<unsigned> g_fired;
void RecordFired(void* ud) { g_fired.push_back(*(const unsigned*)ud); }

// H6: the hook-side queue caps at 32 and drops NEW presses (oldest first);
// L5: unregistering a chord clears its held bit, so re-registering it fires on
// the very next press instead of swallowing it.
void TestHotkeyQueue() {
    using namespace pbzfm;

    RegistryClearModCallbacks();
    RegistryFlushQueuedHotkeys();
    g_fired.clear();

    unsigned vks[40];
    for (int i = 0; i < 26; ++i) vks[i] = (unsigned)('A' + i);
    for (int i = 0; i < 10; ++i) vks[26 + i] = (unsigned)('0' + i);
    for (int i = 0; i < 4; ++i) vks[36 + i] = (unsigned)(VK_F1 + i);
    int ids[40];
    for (int i = 0; i < 40; ++i) ids[i] = RegistryAddHotkey(vks[i], 0, RecordFired, &vks[i]);
    for (int i = 0; i < 40; ++i) CHECK(ids[i] != 0 && RegistryQueueHotkey(vks[i], false, 0));
    RegistryFlushQueuedHotkeys();
    CHECK(g_fired.size() == 32);                    // cap
    for (size_t i = 0; i < g_fired.size(); ++i) CHECK(g_fired[i] == vks[i]);   // drop-newest keeps the first 32
    for (int i = 0; i < 40; ++i) RegistryQueueHotkey(vks[i], true, 0);         // release held chords
    RegistryFlushQueuedHotkeys();
    for (int i = 0; i < 40; ++i) CHECK(RegistryRemoveHotkey(ids[i]));

    // L5 queue flavor: remove -> re-register, first press fires again.
    g_fired.clear();
    const unsigned vk = VK_F10;
    const int id = RegistryAddHotkey(vk, 0, RecordFired, (void*)&vk);
    CHECK(id != 0);
    CHECK(RegistryQueueHotkey(vk, false, 0));
    RegistryFlushQueuedHotkeys();
    CHECK(g_fired.size() == 1);
    CHECK(RegistryRemoveHotkey(id));
    CHECK(!RegistryQueueHotkey(vk, false, 0));       // no owner: dropped and down bit cleared
    const int id2 = RegistryAddHotkey(vk, 0, RecordFired, (void*)&vk);
    CHECK(id2 != 0 && RegistryQueueHotkey(vk, false, 0));
    RegistryFlushQueuedHotkeys();
    CHECK(g_fired.size() == 2);
    RegistryRemoveHotkey(id2);
}

// ---- H5: module-alive probe ----
int g_liveHotkeyCalls = 0;
void LiveRender(void*) {}
void LiveHotkey(void*) { ++g_liveHotkeyCalls; }

// H5: a page/hotkey entry point in a mapped module is kept, a fake address that
// belongs to no module is cleared. No FreeLibrary is involved - the probe only
// asks the loader which module (if any) owns the address.
void TestPurgeOrphaned() {
    using namespace pbzfm;

    PBZFM_RenderFn deadRender = (PBZFM_RenderFn)(UINT_PTR)0x1000;   // in no module
    Section* s = RegistryGetOrCreatePath("Purge/Pages");
    Page keep;
    keep.label = "keep";
    keep.fn = &LiveRender;
    Page dead;
    dead.label = "dead";
    dead.fn = deadRender;
    RegistryAddPage(s, keep);
    RegistryAddPage(s, dead);

    const int deadId = RegistryAddHotkey(VK_F5, 0, (PBZFM_HotkeyFn)deadRender, nullptr);
    g_liveHotkeyCalls = 0;
    const int liveId = RegistryAddHotkey(VK_F6, 0, &LiveHotkey, nullptr);

    RegistryPurgeOrphanedCallbacks();

    CHECK(s->pages.size() == 2);
    CHECK(s->pages[0].fn == &LiveRender);
    CHECK(s->pages[1].fn == nullptr);
    CHECK(!RegistryDispatchHotkey(VK_F5, false, 0));   // dead hotkey erased
    CHECK(RegistryDispatchHotkey(VK_F6, false, 0));    // live hotkey retained
    CHECK(g_liveHotkeyCalls == 1);
    CHECK(RegistryDispatchHotkey(VK_F6, true, 0));

    RegistryRemoveHotkey(deadId);   // already purged: returns false, harmless
    RegistryRemoveHotkey(liveId);
}

// ---- B1: two-phase persistence ----
// B1: at control creation the value already on disk must win over the declared
// default (the store was loaded before any item existed).
void TestSchemaB1() {
    RemoveStore("b1_store");
    CHECK(WriteUtf8(StorePath("b1_store"), "{\"fov\": 90, \"freeze\": true}"));

    PBZFM_SetSection("TestApi/B1");
    PBZFM_Config cfg = PBZFM_ConfigCreate("b1_store", "B1 page");
    CHECK(cfg != nullptr);
    if (!cfg) return;
    PBZFM_ConfigAddFloat(cfg, "fov", "FOV", 70.0f, 30.0f, 120.0f, 1.0f, "%.1f");
    PBZFM_ConfigAddBool(cfg, "freeze", "Freeze", 0);
    CHECK(PBZFM_GetFloat(cfg, "fov") == 90.0f);
    CHECK(PBZFM_GetBool(cfg, "freeze") == 1);

    PBZFM_SetFloat(cfg, "fov", 100.0f);
    PBZFM_SetBool(cfg, "freeze", 0);
    PBZFM_ConfigSave(cfg);

    PBZFM_SetSection("TestApi/B1b");
    PBZFM_Config again = PBZFM_ConfigCreate("b1_store", "B1 reload");
    CHECK(again != nullptr);
    if (!again) return;
    PBZFM_ConfigAddFloat(again, "fov", "FOV", 70.0f, 30.0f, 120.0f, 1.0f, "%.1f");
    PBZFM_ConfigAddBool(again, "freeze", "Freeze", 0);
    CHECK(PBZFM_GetFloat(again, "fov") == 100.0f);
    CHECK(PBZFM_GetBool(again, "freeze") == 0);
    RemoveStore("b1_store");
}

// ---- M2: JSON key escaping ----
// M2: keys containing a quote, a backslash, control bytes or newlines must be
// escaped on write so the store stays parseable and Load returns them intact.
void TestSchemaKeyEscaping() {
    RemoveStore("esc_store");
    CHECK(WriteUtf8(StorePath("esc_store"), "{}"));   // known-empty store, ignores leftovers
    const char* keys[] = { "q\"uote", "back\\slash", "line\nbreak", "tab\tkey", "ctrl\x01key" };
    const int n = (int)(sizeof(keys) / sizeof(keys[0]));

    PBZFM_SetSection("TestApi/Esc");
    PBZFM_Config cfg = PBZFM_ConfigCreate("esc_store", "Escape");
    CHECK(cfg != nullptr);
    if (!cfg) return;
    for (int i = 0; i < n; ++i) {
        PBZFM_ConfigAddInt(cfg, keys[i], "k", i, 0, 10);
        PBZFM_SetInt(cfg, keys[i], i + 1);
    }
    PBZFM_ConfigSave(cfg);

    const std::string text = ReadUtf8(StorePath("esc_store"));
    CHECK(text.find("q\\\"uote") != std::string::npos);
    CHECK(text.find("back\\\\slash") != std::string::npos);
    CHECK(text.find("line\\nbreak") != std::string::npos);
    CHECK(text.find("tab\\tkey") != std::string::npos);
    CHECK(text.find("ctrl\\u0001key") != std::string::npos);
    CHECK(text.find('\n') != std::string::npos);   // the record separator survives

    PBZFM_SetSection("TestApi/Esc2");
    PBZFM_Config again = PBZFM_ConfigCreate("esc_store", "Escape reload");
    CHECK(again != nullptr);
    if (!again) return;
    for (int i = 0; i < n; ++i) PBZFM_ConfigAddInt(again, keys[i], "k", i, 0, 10);
    for (int i = 0; i < n; ++i) CHECK(PBZFM_GetInt(again, keys[i]) == i + 1);
    RemoveStore("esc_store");
}

// ---- key combo codec: vk | (mods << 8) ----
// vk | mods<<8: the packed value stored for key combos must give back the exact
// vk and PBZFM_MOD_* bits, including the full 8-bit vk range.
void TestKeyCodec() {
    PBZFM_SetSection("TestApi/Key");
    PBZFM_Config cfg = PBZFM_ConfigCreate("key_store", "Keys");
    CHECK(cfg != nullptr);
    if (!cfg) return;

    PBZFM_ConfigAddKeyCombo(cfg, "hk", "Hotkey", 'P', PBZFM_MOD_CTRL);
    CHECK(PBZFM_GetKey(cfg, "hk") == 'P');
    CHECK(PBZFM_GetKeyMods(cfg, "hk") == PBZFM_MOD_CTRL);

    const unsigned allMods = PBZFM_MOD_CTRL | PBZFM_MOD_SHIFT | PBZFM_MOD_ALT;
    PBZFM_SetKeyCombo(cfg, "hk", VK_F1, allMods);
    CHECK(PBZFM_GetKey(cfg, "hk") == VK_F1);
    CHECK(PBZFM_GetKeyMods(cfg, "hk") == allMods);

    // 0x9A | (0xF << 8) = 0xF9A: both halves survive.
    PBZFM_SetKeyCombo(cfg, "hk", 0x9A, allMods | PBZFM_MOD_WIN);
    CHECK(PBZFM_GetKey(cfg, "hk") == 0x9A);
    CHECK(PBZFM_GetKeyMods(cfg, "hk") == 0xF);

    CHECK(pbzfm::Config::KeyModsToString(VK_F1, allMods) == "Ctrl+Shift+Alt+F1");
    CHECK(pbzfm::Config::VkFromName(L"NUMPAD4") == VK_NUMPAD4);
    CHECK(std::string(pbzfm::Config::VkName(VK_F8)) == "F8");
    CHECK(pbzfm::Config::VkFromName(L"0x9A") == 0x9A);
}

// ---- L1: input queue eviction policy ----
// L1: a full input queue must shed Move motion before any key edge, and a
// key-up edge must never be dropped (ImGui would otherwise keep the key held).
void TestInputQueueEviction() {
    using namespace pbzfm;

    InputEvent e{};
    while (InputPop(&e)) {}
    InputResetDropStats();

    // Ten key-downs, then a Move in the middle, then the rest: the queue fills.
    // A plain drop-oldest would lose key 0; the L1 policy must lose the Move.
    for (int i = 0; i < 10; ++i) {
        InputEvent k{};
        k.type = InputEvent::Key;
        k.key = (ImGuiKey)i;
        k.down = true;
        InputPushEvent(k);
    }
    InputEvent mv{};
    mv.type = InputEvent::Move;
    mv.wx = 10.0f;
    mv.wy = 20.0f;
    InputPushEvent(mv);
    for (int i = 0; i < 245; ++i) {
        InputEvent k{};
        k.type = InputEvent::Key;
        k.key = (ImGuiKey)(100 + i);
        k.down = true;
        InputPushEvent(k);
    }
    {
        InputEvent k{};
        k.type = InputEvent::Key;
        k.key = (ImGuiKey)999;
        k.down = true;
        InputPushEvent(k);   // full: evicts the Move, keeps every key edge
    }
    int moves = 0, keys = 0, lowKeys = 0;
    while (InputPop(&e)) {
        if (e.type == InputEvent::Move) ++moves;
        else if (e.type == InputEvent::Key) {
            ++keys;
            if ((int)e.key >= 0 && (int)e.key < 10) ++lowKeys;
        }
    }
    CHECK(moves == 0);
    CHECK(keys == 256);
    CHECK(lowKeys == 10);   // drop-oldest would have eaten key 0

    // 256 key-up edges, then one more up: no Move/Wheel/down candidate exists,
    // so the oldest up-edge is dropped and the newest one is kept.
    for (int i = 0; i < 256; ++i) {
        InputEvent k{};
        k.type = InputEvent::Key;
        k.key = i;
        k.down = false;
        InputPushEvent(k);
    }
    {
        InputEvent k{};
        k.type = InputEvent::Key;
        k.key = 999;
        k.down = false;
        InputPushEvent(k);
    }
    bool sawNewest = false, sawOldest = false;
    int ups = 0;
    while (InputPop(&e)) {
        if (e.type != InputEvent::Key || e.down) continue;
        ++ups;
        if (e.key == 999) sawNewest = true;
        if (e.key == 0) sawOldest = true;
    }
    CHECK(ups == 256);
    CHECK(sawNewest);
    CHECK(!sawOldest);
    InputResetDropStats();
}

// 方案 C：后端策略是纯判定，必须在单测里锁死——它决定「装不装全局钩子」，
// 判错一次就会把系统输入流重新污染回去。
void TestInputBackendPolicy() {
    using namespace pbzfm;

    CHECK(InputModeFromString(nullptr) == InputMode::Auto);
    CHECK(InputModeFromString("") == InputMode::Auto);
    CHECK(InputModeFromString("auto") == InputMode::Auto);
    CHECK(InputModeFromString("AUTO") == InputMode::Auto);
    CHECK(InputModeFromString("engine") == InputMode::Engine);
    CHECK(InputModeFromString("ue4ss") == InputMode::Engine);
    CHECK(InputModeFromString("os") == InputMode::Os);
    CHECK(InputModeFromString("hook") == InputMode::Os);
    CHECK(InputModeFromString("garbage") == InputMode::Auto);   // 未知值不能静默变成 os

    CHECK(std::string(InputModeName(InputMode::Auto)) == "auto");
    CHECK(std::string(InputModeName(InputMode::Engine)) == "engine");
    CHECK(std::string(InputModeName(InputMode::Os)) == "os");

    // auto：引擎就绪才用引擎；显式 os 永远走钩子；显式 engine 不就绪时回退 os。
    CHECK(ResolveInputBackend(InputMode::Auto, true) == InputBackend::Engine);
    CHECK(ResolveInputBackend(InputMode::Auto, false) == InputBackend::Os);
    CHECK(ResolveInputBackend(InputMode::Engine, true) == InputBackend::Engine);
    CHECK(ResolveInputBackend(InputMode::Engine, false) == InputBackend::Os);
    CHECK(ResolveInputBackend(InputMode::Os, true) == InputBackend::Os);
    CHECK(ResolveInputBackend(InputMode::Os, false) == InputBackend::Os);

    // 本测试构建没有引擎后端（PBZFM_ENGINE_PORT=OFF）：auto/os 必须落回 OS 钩子。
    CHECK(!EnginePortActive());
    CHECK(!EnginePortInit(InputMode::Auto));
    CHECK(!EnginePortActive());
    CHECK(!EnginePortInit(InputMode::Engine));   // 显式 engine 也不许假装成功
    CHECK(!EnginePortActive());
    CHECK(EnginePortUnavailableReason() != nullptr && EnginePortUnavailableReason()[0] != 0);
    EnginePortShutdown();
    CHECK(!EnginePortActive());
    // 未激活时转发是空操作，不能崩。
    EnginePortSetMenuOpen(true);
    EnginePortSetMenuOpen(false);
}

// input_mode 必须能从 ini 读回、并被写回（否则玩家改不了后端）。
void TestInputModeConfig() {
    const std::wstring path = pbzfm::RootFile(L"data/input_mode_test.ini");
    DeleteFileW(path.c_str());

    pbzfm::Config def;
    CHECK(def.inputMode == "auto");
    CHECK(def.Save(path));
    {
        pbzfm::Config back;
        CHECK(back.Load(path));
        CHECK(back.inputMode == "auto");
    }

    // 磁盘上写成 engine 后必须读回 engine。
    const std::string text = "; cfg\n\n[framework]\ninput_mode = engine\ntoggle_key = F8\n";
    CHECK(pbzfm::WriteFileAtomic(path, text));
    {
        pbzfm::Config back;
        CHECK(back.Load(path));
        CHECK(back.inputMode == "engine");
    }
    DeleteFileW(path.c_str());
}

void Run(const char* name, void (*fn)()) {
    const int before = g_failures;
    fn();
    std::printf("[test] %-26s %s\n", name, g_failures == before ? "PASS" : "FAIL");
}

} // namespace

int main() {
    CreateDirectoryW(pbzfm::ModRoot().c_str(), nullptr);
    CreateDirectoryW(pbzfm::RootFile(L"data").c_str(), nullptr);

    Run("H4 texture slots", TestTextureSlots);
    Run("H8 path escaping", TestPathEscaping);
    Run("H8 registry concurrency", TestRegistryConcurrency);
    Run("H6/L1/L5 hotkey queue", TestHotkeyQueue);
    Run("H5 orphaned callbacks", TestPurgeOrphaned);
    Run("B1 schema persistence", TestSchemaB1);
    Run("M2 schema key escaping", TestSchemaKeyEscaping);
    Run("key combo codec", TestKeyCodec);
    Run("L1 input queue eviction", TestInputQueueEviction);
    Run("C input backend policy", TestInputBackendPolicy);
    Run("C input_mode config", TestInputModeConfig);

    // Leave the TLS current section outside the subtrees we delete (deleting a
    // node that is still "current" would leave a dangling thread-local pointer).
    pbzfm::RegistryGetOrCreateSection("framework_test_root");
    pbzfm::RegistryDeleteSection("Stress/Live");
    pbzfm::RegistryDeleteSection("Purge/Pages");
    pbzfm::RegistryDeleteSection("Esc");

    std::printf("[framework_test] %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}

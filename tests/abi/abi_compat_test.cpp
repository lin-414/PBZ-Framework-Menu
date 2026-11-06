// ABI compile-time guard: include the shipped baseline header and the current
// SDK header in one translation unit, then assert that every type, struct
// layout, member offset and export signature that existed in the baseline is
// still byte-for-byte compatible.
//
// This target is compile-only (an OBJECT library): it produces no executable
// and is therefore never registered with CTest. A failing static_assert breaks
// build_all.bat / tools/pack.ps1 instead of only emitting a warning.
//
// The baseline lives at tests/abi/pbzfm_<version>.h (an exact byte copy of the
// released sdk/pbzfm.h). When PROJECT_VERSION changes, refresh the snapshot -
// CMake refuses to configure while the file for the current version is missing.

#include <windows.h>

#include <cstddef>
#include <type_traits>

// Current header first, at global scope. Its include guard is removed below so
// the baseline copy can be included a second time inside a namespace.
#include "pbzfm.h"

constexpr int kCurrentAbi = PBZFM_ABI_VERSION;
constexpr int kCurrentApi = PBZFM_API_VERSION;

#undef PBZFM_SDK_H
#undef PBZFM_ABI_VERSION
#undef PBZFM_API_VERSION

// Baseline copy in its own namespace: same names, distinct types. <windows.h>
// is already included above, so the header's own include is a no-op inside the
// namespace. The inline loader is renamed because it is an extern "C"-linked
// function (MSVC rejects a second definition of that name even in a namespace).
#define PBZFM_LoadApi PBZFM_LoadApi_baseline
namespace abi_base {
#include PBZFM_ABI_BASELINE_HEADER
}
#undef PBZFM_LoadApi

constexpr int kBaseAbi = PBZFM_ABI_VERSION;
constexpr int kBaseApi = PBZFM_API_VERSION;

// Version contract: the ABI version is pinned by the snapshot (bumping it
// requires refreshing the snapshot, which restarts these asserts against the
// new baseline). The API version may only move forward.
static_assert(kBaseAbi == 1, "baseline snapshot must record ABI version 1");
static_assert(kBaseApi == 3, "baseline snapshot must record API version 3");
static_assert(kCurrentAbi == kBaseAbi,
              "PBZFM_ABI_VERSION changed: breaking ABI change or stale baseline snapshot");
static_assert(kCurrentApi >= kBaseApi,
              "PBZFM_API_VERSION went backwards");

// Fixed-width C types and callback typedefs must not change.
static_assert(std::is_same_v<abi_base::PBZFM_Config, PBZFM_Config>,
              "PBZFM_Config handle type changed");
static_assert(std::is_same_v<abi_base::PBZFM_RenderFn, PBZFM_RenderFn>,
              "PBZFM_RenderFn signature changed");
static_assert(std::is_same_v<abi_base::PBZFM_ChangeFn, PBZFM_ChangeFn>,
              "PBZFM_ChangeFn signature changed");
static_assert(std::is_same_v<abi_base::PBZFM_HotkeyFn, PBZFM_HotkeyFn>,
              "PBZFM_HotkeyFn signature changed");
static_assert(std::is_same_v<abi_base::PBZFM_MenuEventFn, PBZFM_MenuEventFn>,
              "PBZFM_MenuEventFn signature changed");

// PBZFM_Api: the DLL never sees this struct (clients fill it via the inline
// loader), but a client built against the baseline header must keep working
// against a framework built with the current header. Existing members may
// never move or change type; new members may only be appended (which leaves
// the baseline offsets and all earlier sizes untouched).
using BaseApi = abi_base::PBZFM_Api;
using CurrentApi = PBZFM_Api;

static_assert(sizeof(CurrentApi) >= sizeof(BaseApi),
              "PBZFM_Api shrank: an existing member was removed or replaced");
static_assert(alignof(CurrentApi) == alignof(BaseApi),
              "PBZFM_Api alignment changed");

#define ABI_MEMBER(name)                                                                     \
    static_assert(offsetof(BaseApi, name) == offsetof(CurrentApi, name),                    \
                  #name " moved inside PBZFM_Api (append new members at the end only)");    \
    static_assert(std::is_same_v<decltype(BaseApi::name), decltype(CurrentApi::name)>,      \
                  #name " changed type inside PBZFM_Api")

/* required core (ABI v1) */
ABI_MEMBER(GetAbiVersion);
ABI_MEMBER(IsMenuOpen);
ABI_MEMBER(SetSection);
ABI_MEMBER(AddSectionItem);
ABI_MEMBER(ConfigCreate);
ABI_MEMBER(ConfigAddGroup);
ABI_MEMBER(ConfigAddBool);
ABI_MEMBER(ConfigAddInt);
ABI_MEMBER(ConfigAddFloat);
ABI_MEMBER(ConfigAddEnum);
ABI_MEMBER(ConfigAddKey);
ABI_MEMBER(ConfigOnChange);
ABI_MEMBER(ConfigSave);
ABI_MEMBER(GetBool);
ABI_MEMBER(GetFloat);
ABI_MEMBER(GetInt);
ABI_MEMBER(GetEnum);
ABI_MEMBER(GetKey);
ABI_MEMBER(SetBool);
ABI_MEMBER(SetFloat);
ABI_MEMBER(SetInt);
ABI_MEMBER(SetEnum);
ABI_MEMBER(SetKey);
ABI_MEMBER(AddHotkey);
ABI_MEMBER(Text);
ABI_MEMBER(TextDisabled);
ABI_MEMBER(Separator);
ABI_MEMBER(SameLine);
ABI_MEMBER(Button);
ABI_MEMBER(Checkbox);
ABI_MEMBER(SliderFloat);
ABI_MEMBER(SliderInt);
ABI_MEMBER(Combo);
ABI_MEMBER(LastItemRect);
ABI_MEMBER(GetImGuiContext);
/* v2 additions */
ABI_MEMBER(RenameSection);
ABI_MEMBER(DeleteSection);
ABI_MEMBER(RemoveSectionItem);
ABI_MEMBER(AddHotkeyEx);
ABI_MEMBER(RemoveHotkey);
ABI_MEMBER(OnMenuEvent);
ABI_MEMBER(RemoveMenuEvent);
ABI_MEMBER(ConfigAddKeyCombo);
ABI_MEMBER(GetKeyMods);
ABI_MEMBER(SetKeyCombo);
ABI_MEMBER(KeyName);
ABI_MEMBER(LoadTexture);
ABI_MEMBER(DisposeTexture);
ABI_MEMBER(Image);
ABI_MEMBER(InputText);
ABI_MEMBER(CollapsingHeader);
ABI_MEMBER(Selectable);
ABI_MEMBER(ProgressBar);
ABI_MEMBER(ColorEdit3);
ABI_MEMBER(VersionString);
/* v3 additions */
ABI_MEMBER(SetSectionPath);
ABI_MEMBER(AddSectionItemPath);
ABI_MEMBER(ConfigCreatePath);
ABI_MEMBER(GetApiVersion);

#undef ABI_MEMBER

// Exported free functions: a baseline declaration and the current declaration
// must yield the same function type (name, parameters and calling convention).
// Assigning the addresses both ways is the compile-time proof that a client
// built against the baseline header can bind the current exports.
#define ABI_FN(name)                                                                          \
    static_assert(std::is_same_v<decltype(&abi_base::name), decltype(&::name)>,               \
                  #name " export signature changed")

ABI_FN(PBZFM_GetAbiVersion);
ABI_FN(PBZFM_GetApiVersion);
ABI_FN(PBZFM_IsMenuOpen);
ABI_FN(PBZFM_SetSection);
ABI_FN(PBZFM_AddSectionItem);
ABI_FN(PBZFM_RenameSection);
ABI_FN(PBZFM_DeleteSection);
ABI_FN(PBZFM_RemoveSectionItem);
ABI_FN(PBZFM_SetSectionPath);
ABI_FN(PBZFM_AddSectionItemPath);
ABI_FN(PBZFM_ConfigCreatePath);
ABI_FN(PBZFM_ConfigCreate);
ABI_FN(PBZFM_ConfigAddGroup);
ABI_FN(PBZFM_ConfigAddBool);
ABI_FN(PBZFM_ConfigAddInt);
ABI_FN(PBZFM_ConfigAddFloat);
ABI_FN(PBZFM_ConfigAddEnum);
ABI_FN(PBZFM_ConfigAddKey);
ABI_FN(PBZFM_ConfigAddKeyCombo);
ABI_FN(PBZFM_ConfigOnChange);
ABI_FN(PBZFM_ConfigSave);
ABI_FN(PBZFM_GetBool);
ABI_FN(PBZFM_GetFloat);
ABI_FN(PBZFM_GetInt);
ABI_FN(PBZFM_GetEnum);
ABI_FN(PBZFM_GetKey);
ABI_FN(PBZFM_GetKeyMods);
ABI_FN(PBZFM_SetBool);
ABI_FN(PBZFM_SetFloat);
ABI_FN(PBZFM_SetInt);
ABI_FN(PBZFM_SetEnum);
ABI_FN(PBZFM_SetKey);
ABI_FN(PBZFM_SetKeyCombo);
ABI_FN(PBZFM_KeyName);
ABI_FN(PBZFM_AddHotkey);
ABI_FN(PBZFM_AddHotkeyEx);
ABI_FN(PBZFM_RemoveHotkey);
ABI_FN(PBZFM_OnMenuEvent);
ABI_FN(PBZFM_RemoveMenuEvent);
ABI_FN(PBZFM_Text);
ABI_FN(PBZFM_TextDisabled);
ABI_FN(PBZFM_Separator);
ABI_FN(PBZFM_SameLine);
ABI_FN(PBZFM_Button);
ABI_FN(PBZFM_Checkbox);
ABI_FN(PBZFM_SliderFloat);
ABI_FN(PBZFM_SliderInt);
ABI_FN(PBZFM_Combo);
ABI_FN(PBZFM_LastItemRect);
ABI_FN(PBZFM_InputText);
ABI_FN(PBZFM_CollapsingHeader);
ABI_FN(PBZFM_Selectable);
ABI_FN(PBZFM_ProgressBar);
ABI_FN(PBZFM_ColorEdit3);
ABI_FN(PBZFM_LoadTexture);
ABI_FN(PBZFM_DisposeTexture);
ABI_FN(PBZFM_Image);
ABI_FN(PBZFM_GetImGuiContext);
ABI_FN(PBZFM_GetSectionCount);
ABI_FN(PBZFM_GetSectionNameUtf8);
ABI_FN(PBZFM_GetSectionRect);
ABI_FN(PBZFM_GetLastSchemaItemRect);

#undef ABI_FN

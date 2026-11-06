cmake_minimum_required(VERSION 3.20)
project(PBZFM VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# 方案 C 的开关必须在建 target 之前声明：引擎后端要与 UE4SS 共用同一个 CRT
# 版本（UE4SS 用 /MD），否则跨 DLL 传 std::function/tick 回调会在堆上踩坏。
option(PBZFM_ENGINE_PORT "Build the UE4SS-backed engine input port (方案 C)" OFF)
option(PBZFM_UE4SS_SOURCE_BUILD "add_subdirectory(UE4SS) instead of using a prebuilt import lib" OFF)
set(PBZFM_UE4SS_DIR "" CACHE PATH "RE-UE4SS source root (contains UE4SS/ and deps/)")
set(PBZFM_UE4SS_LIBDIR "" CACHE PATH "directory holding the prebuilt UE4SS.lib import library")
set(PBZFM_UE4SS_GENERATED_DIR "" CACHE PATH "UE4SS generated/include dir (SDKGenerator output)")

if(PBZFM_ENGINE_PORT)
    # 与 UE4SS 一致：动态 CRT。默认的 /MT 会让 std::function 跨模块释放时崩。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(CMAKE_CXX_STANDARD 20)
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# M8: the automation/injection exports (PBZFM_Test*, PBZFM_GetDemoButton) are
# development-only. Default OFF so a release DLL exposes just the documented C
# API; the host regression configures with -DPBZFM_TEST_HOOKS=ON.
option(PBZFM_TEST_HOOKS "Expose automation-only test exports (host regression)" OFF)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE /utf-8 /EHsc)

# DLL version resource: right-click properties / crash logs can match the
# running binary to a dist zip (PROJECT_VERSION is the single source).
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

add_library(PBZFM SHARED
    src/pbzfm.cpp
    src/config.cpp
    src/engine_port.cpp
    src/i18n.cpp
    src/game_window.cpp
    src/input.cpp
    src/overlay.cpp
    src/menu.cpp
    src/theme.cpp
    src/registry.cpp
    src/schema.cpp
    src/api.cpp
    src/textures.cpp
    src/files.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(PBZFM PRIVATE src sdk third_party)
target_link_libraries(PBZFM PRIVATE imgui user32 gdi32 shell32)
target_compile_definitions(PBZFM PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                       PBZFM_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(PBZFM PRIVATE /utf-8 /EHsc /W3)
set_target_properties(PBZFM PROPERTIES PREFIX "")
if(PBZFM_TEST_HOOKS)
    target_compile_definitions(PBZFM PRIVATE PBZFM_TEST_HOOKS=1)
endif()

# ---- 方案 C：引擎内输入后端（可选） ----------------------------------------
# 默认 OFF：发布包仍是「零 UE4SS 依赖的普通 DLL」，输入走 OS 钩子（0.5.x 行为）。
# ON：输入改由引擎反射冻结（bBlockInput / SetIgnoreMoveInput / SetIgnoreLookInput），
# 模块不再安装任何全局 LL 钩子，因此系统的录屏/截图/游戏栏热键完全不受影响。
#
# 两种接入方式（二选一）：
#   A) 预编译 import lib（推荐，几分钟就能编完）：
#      -DPBZFM_ENGINE_PORT=ON
#      -DPBZFM_UE4SS_DIR=<RE-UE4SS 源码根>          # 只要头文件
#      -DPBZFM_UE4SS_LIBDIR=<含 UE4SS.lib 的 lib 目录>
#      例：D:/Documents/DSH/PBZ/_ref/ue4ss-src +
#          D:/Documents/DSH/PBZ/PBZPhotoModeCpp/build-dll/Game__Shipping__Win64/lib
#   B) 源码树构建（重：C++23 + Rust/patternsleuth + FetchContent）：
#      -DPBZFM_ENGINE_PORT=ON -DPBZFM_UE4SS_SOURCE_BUILD=ON -DPBZFM_UE4SS_DIR=...
if(PBZFM_ENGINE_PORT)
    if(NOT PBZFM_UE4SS_DIR OR NOT EXISTS "${PBZFM_UE4SS_DIR}/deps/first/Unreal/include/Unreal/UObject.hpp")
        message(FATAL_ERROR
            "PBZFM_ENGINE_PORT=ON requires -DPBZFM_UE4SS_DIR=<RE-UE4SS source root>,\n"
            "e.g. -DPBZFM_UE4SS_DIR=D:/Documents/DSH/PBZ/_ref/ue4ss-src")
    endif()

    target_sources(PBZFM PRIVATE src/engine_port_ue4ss.cpp)
    # UE4SS/UEPseudo 头是 C++23 + 严格一致性模式编出来的；用 c++20 会在 fmt/chrono
    # 之类的位置炸出 C4582/C4583。照抄 UE4SS 自身的编译选项。
    set_target_properties(PBZFM PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    target_compile_options(PBZFM PRIVATE
        /permissive- /Zc:preprocessor /Zc:lambda /Zc:inline /Zc:strictStrings
        /wd4005 /wd4251 /wd4068 /wd4201 /bigobj)
    target_compile_definitions(PBZFM PRIVATE
        PBZFM_ENGINE_PORT_UE4SS=1
        _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
        # 与 UE4SS 自身构建保持一致的 ABI/平台宏（少了任何一个都会在 Unreal 头里走错分支）
        RC_STRING_BUILD_STATIC RC_LUA_RAW_BUILD_STATIC
        UE_BUILD_SHIPPING UE_GAME
        PLATFORM_WINDOWS PLATFORM_MICROSOFT UBT_COMPILED_PLATFORM=Win64
        OVERRIDE_PLATFORM_HEADER_NAME=Windows
        ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD ASMJIT_STATIC ASMTK_STATIC
        HAS_INPUT DISABLE_PROFILER _WIN32_WINNT=0x0A00 WINVER=0x0A00)

    set(PBZFM_UE4SS_INCLUDES
        ${PBZFM_UE4SS_DIR}/UE4SS/include
        ${PBZFM_UE4SS_DIR}/UE4SS/generated_include
        ${PBZFM_UE4SS_DIR}/deps/first/Unreal/include
        ${PBZFM_UE4SS_DIR}/deps/first/Unreal/generated_include
        # UEPseudo 的 HAL 头用 "GenericPlatform/..." 相对 Core/HAL 寻址（见 UE4SS 自身编译行）
        ${PBZFM_UE4SS_DIR}/deps/first/Unreal/include/Unreal
        ${PBZFM_UE4SS_DIR}/deps/first/Unreal/include/Unreal/Core
        ${PBZFM_UE4SS_DIR}/deps/first/String/include
        ${PBZFM_UE4SS_DIR}/deps/first/File/include
        ${PBZFM_UE4SS_DIR}/deps/first/DynamicOutput/include
        ${PBZFM_UE4SS_DIR}/deps/first/Helpers/include
        ${PBZFM_UE4SS_DIR}/deps/first/Constructs/include
        ${PBZFM_UE4SS_DIR}/deps/first/Function/include
        ${PBZFM_UE4SS_DIR}/deps/first/ASMHelper/include
        ${PBZFM_UE4SS_DIR}/deps/first/SinglePassSigScanner/include)
    # UE4SS 生成的配置头（add_subdirectory 时由 SDKGenerator 产出；预编译路线需要现成的一份）
    if(PBZFM_UE4SS_GENERATED_DIR)
        list(APPEND PBZFM_UE4SS_INCLUDES ${PBZFM_UE4SS_GENERATED_DIR})
    endif()
    # UEPseudo 的 DynamicOutput/Headers 直接 include <fmt/core.h> 等第三方头；
    # 预编译路线下它们来自 UE4SS 构建目录的 _deps（FetchContent 产物）。
    if(PBZFM_UE4SS_LIBDIR)
        get_filename_component(_pbzfm_ue4ss_build "${PBZFM_UE4SS_LIBDIR}/../.." ABSOLUTE)
        foreach(_dep fmt glaze polyhook2 zydis)
            if(EXISTS "${_pbzfm_ue4ss_build}/_deps/${_dep}-src/include")
                list(APPEND PBZFM_UE4SS_INCLUDES "${_pbzfm_ue4ss_build}/_deps/${_dep}-src/include")
            endif()
        endforeach()
        if(EXISTS "${_pbzfm_ue4ss_build}/_deps/polyhook2-src")
            list(APPEND PBZFM_UE4SS_INCLUDES "${_pbzfm_ue4ss_build}/_deps/polyhook2-src")
        endif()
        if(EXISTS "${_pbzfm_ue4ss_build}/_deps/zydis-build")
            list(APPEND PBZFM_UE4SS_INCLUDES "${_pbzfm_ue4ss_build}/_deps/zydis-build")
        endif()
        # Zydis/Zycore 的头在 zydis-src/dependencies/zycore 下（与 UE4SS 自身编译行一致）
        foreach(_zin "${_pbzfm_ue4ss_build}/_deps/zydis-src/include"
                     "${_pbzfm_ue4ss_build}/_deps/zydis-src/dependencies/zycore/include"
                     "${_pbzfm_ue4ss_build}/_deps/zydis-build/zycore"
                     "${_pbzfm_ue4ss_build}/_deps/polyhook2-src/asmjit/src"
                     "${_pbzfm_ue4ss_build}/_deps/polyhook2-src/asmtk/src")
            if(EXISTS "${_zin}")
                list(APPEND PBZFM_UE4SS_INCLUDES "${_zin}")
            endif()
        endforeach()
    endif()
    target_include_directories(PBZFM PRIVATE ${PBZFM_UE4SS_INCLUDES})

    if(PBZFM_UE4SS_SOURCE_BUILD)
        add_subdirectory(${PBZFM_UE4SS_DIR} ${CMAKE_BINARY_DIR}/ue4ss)
        # include 目录通过 INTERFACE 传播，PUBLIC 才拿得到 <Mod/CppUserModBase.hpp>；
        # imgui 必须排在前面，保证符号解析用我们这份。
        target_link_libraries(PBZFM PUBLIC UE4SS)
    else()
        if(NOT PBZFM_UE4SS_LIBDIR OR NOT EXISTS "${PBZFM_UE4SS_LIBDIR}/UE4SS.lib")
            message(FATAL_ERROR
                "PBZFM_ENGINE_PORT=ON without PBZFM_UE4SS_SOURCE_BUILD needs\n"
                "-DPBZFM_UE4SS_LIBDIR=<directory containing UE4SS.lib>")
        endif()
        # UE4SS.lib 是 import lib（__imp_ 符号直接指向 UE4SS.dll 里已初始化的实例，
        # 不能改用 Unreal.lib 静态链接：那会得到第二份未初始化的 GUObjectArray）。
        target_link_libraries(PBZFM PRIVATE "${PBZFM_UE4SS_LIBDIR}/UE4SS.lib")
    endif()
endif()

# Demo client mod: zero dependencies, uses only the PBZFM C API.
add_library(demo_mod SHARED host/demo_mod.cpp)
target_include_directories(demo_mod PRIVATE sdk)
target_compile_definitions(demo_mod PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(demo_mod PRIVATE /utf-8 /EHsc /W3)
set_target_properties(demo_mod PROPERTIES PREFIX "")

add_executable(pbzfm_host host/main.cpp)
target_include_directories(pbzfm_host PRIVATE src sdk third_party)
target_link_libraries(pbzfm_host PRIVATE user32 gdi32 shell32 dwmapi)
target_compile_definitions(pbzfm_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pbzfm_host PRIVATE /utf-8 /EHsc /W3)
add_dependencies(pbzfm_host PBZFM demo_mod)

# i18n unit test (tests/i18n_test.cpp includes src/i18n.cpp directly).
add_executable(i18n_test tests/i18n_test.cpp)
target_compile_definitions(i18n_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(i18n_test PRIVATE /utf-8 /EHsc /W3)

# Framework unit test: device-free coverage of the registry (H8), the schema/
# C-API persistence + key codec (B1/M2), the hotkey and input queues (H5/H6/L1)
# and the texture slot table (H4). The production TUs are compiled in; the test
# stubs the DLL entry's paths/logging/subsystems (tests/framework_test_stubs.cpp)
# so no overlay, D3D11 device or game window is needed.
add_executable(framework_test
    tests/framework_test.cpp
    tests/framework_test_stubs.cpp
    src/files.cpp
    src/api.cpp
    src/schema.cpp
    src/config.cpp
    src/engine_port.cpp
    src/registry.cpp
    src/input.cpp)
target_include_directories(framework_test PRIVATE src sdk third_party)
target_link_libraries(framework_test PRIVATE imgui user32 gdi32 shell32)
target_compile_definitions(framework_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                            PBZFM_EXPORTS
                                            PBZFM_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(framework_test PRIVATE /utf-8 /EHsc /W3)

# ABI compatibility guard: compile the released SDK header snapshot next to the
# current header and static_assert that no struct layout, member offset or
# export signature changed. OBJECT library on purpose - compile-only, no
# executable, not registered with CTest; an ABI break fails the normal build so
# it cannot reach packaging.
# The baseline is the LAST RELEASED SDK, intentionally independent of
# PROJECT_VERSION: a new release must stay compatible with the previous one
# (the current 0.6.0 work still compares against the 0.5.2 snapshot).
set(PBZFM_ABI_BASELINE_TAG "0_5_2")
set(PBZFM_ABI_BASELINE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/pbzfm_${PBZFM_ABI_BASELINE_TAG}.h")
if(NOT EXISTS "${PBZFM_ABI_BASELINE_FILE}")
    message(FATAL_ERROR
        "ABI baseline header not found: ${PBZFM_ABI_BASELINE_FILE}\n"
        "Copy the released sdk/pbzfm.h byte-for-byte to that path before bumping PROJECT_VERSION.")
endif()
add_library(abi_compat_test OBJECT tests/abi/abi_compat_test.cpp)
target_include_directories(abi_compat_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
target_compile_definitions(abi_compat_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                              PBZFM_ABI_BASELINE_HEADER="pbzfm_${PBZFM_ABI_BASELINE_TAG}.h")
target_compile_options(abi_compat_test PRIVATE /utf-8 /EHsc /W3)

enable_testing()
add_test(NAME i18n_test COMMAND i18n_test)
add_test(NAME framework_test COMMAND framework_test)

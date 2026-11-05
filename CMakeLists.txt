cmake_minimum_required(VERSION 3.20)
project(PBZFM VERSION 0.5.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
# it cannot reach packaging. The snapshot name follows PROJECT_VERSION: bumping
# the version requires copying the newly released sdk/pbzfm.h to that exact path.
set(PBZFM_ABI_BASELINE_TAG "${PROJECT_VERSION}")
string(REPLACE "." "_" PBZFM_ABI_BASELINE_TAG "${PBZFM_ABI_BASELINE_TAG}")
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

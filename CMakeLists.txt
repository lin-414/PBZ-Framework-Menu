cmake_minimum_required(VERSION 3.20)
project(PBZFM VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(PBZFM PRIVATE src sdk third_party)
target_link_libraries(PBZFM PRIVATE imgui user32 gdi32 shell32)
target_compile_definitions(PBZFM PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                       PBZFM_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(PBZFM PRIVATE /utf-8 /EHsc /W3)
set_target_properties(PBZFM PROPERTIES PREFIX "")

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

enable_testing()
add_test(NAME i18n_test COMMAND i18n_test)

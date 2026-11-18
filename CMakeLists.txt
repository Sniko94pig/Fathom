cmake_minimum_required(VERSION 3.24)
project(fathom LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/utf-8 /MP /W3)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
function(fathom_dep name repo tag)
  string(TOUPPER ${name} uname)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/${name})
    set(FETCHCONTENT_SOURCE_DIR_${uname} ${CMAKE_SOURCE_DIR}/third_party/${name} CACHE PATH "" FORCE)
  endif()
  FetchContent_Declare(${name} GIT_REPOSITORY ${repo} GIT_TAG ${tag} GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
  FetchContent_MakeAvailable(${name})
  set(${name}_SOURCE_DIR ${${name}_SOURCE_DIR} PARENT_SCOPE)
endfunction()

fathom_dep(imgui https://github.com/ocornut/imgui v1.92.9b)
fathom_dep(json https://github.com/nlohmann/json v3.11.3)
fathom_dep(stb https://github.com/nothings/stb master)
fathom_dep(minhook https://github.com/TsudaKageyu/minhook master)
fathom_dep(kiero https://github.com/Rebzzel/kiero master)

set(GEN ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN})
function(fathom_embed out name file)
  add_custom_command(
    OUTPUT ${GEN}/${name}.cpp
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/assets/${file} -DOUTPUT=${GEN}/${name}.cpp -DNAME=${name} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/${file} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  set(${out} ${${out}} ${GEN}/${name}.cpp PARENT_SCOPE)
endfunction()
fathom_embed(ASSET_SRC font_inter_regular Inter-Regular.ttf)
fathom_embed(ASSET_SRC font_inter_medium Inter-Medium.ttf)
fathom_embed(ASSET_SRC font_inter_semibold Inter-SemiBold.ttf)
fathom_embed(ASSET_SRC font_lucide lucide.ttf)
fathom_embed(ASSET_SRC logo_png logo.png)

add_library(fathom_ui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${ASSET_SRC}
  src/core/model.cpp
  src/core/settings.cpp
  src/ui/ui.cpp
  src/ui/menu.cpp
  src/ui/pages.cpp
  src/ui/atlas.cpp)
target_include_directories(fathom_ui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp ${imgui_SOURCE_DIR}/backends
  ${json_SOURCE_DIR}/single_include ${stb_SOURCE_DIR} src)
target_compile_definitions(fathom_ui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS)

add_executable(fathom_shots src/preview/shots.cpp src/preview/sample.cpp)
target_include_directories(fathom_shots PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(fathom_shots PRIVATE fathom_ui)
if(WIN32)
  target_sources(fathom_shots PRIVATE src/launcher/home.cpp src/launcher/inject.cpp src/launcher/games.cpp src/launcher/art.cpp)
  target_link_libraries(fathom_shots PRIVATE urlmon ole32)
  target_compile_definitions(fathom_shots PRIVATE FATHOM_LAUNCHER_SHOTS NOMINMAX _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()

if(WIN32)
  add_executable(fathom_preview WIN32
    src/preview/preview_win.cpp src/preview/sample.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_link_libraries(fathom_preview PRIVATE fathom_ui d3d12 dxgi d3dcompiler dwmapi)
  if(MINGW)
    target_link_options(fathom_preview PRIVATE -static)
  endif()

  add_executable(fathom_launcher WIN32
    src/launcher/launcher_win.cpp
    src/launcher/home.cpp
    src/launcher/inject.cpp
    src/launcher/games.cpp
    src/launcher/art.cpp
    src/launcher/app.rc
    src/common/dx12_texture.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  set_target_properties(fathom_launcher PROPERTIES OUTPUT_NAME Fathom)
  target_compile_definitions(fathom_launcher PRIVATE NOMINMAX _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
  target_link_libraries(fathom_launcher PRIVATE fathom_ui d3d12 dxgi d3dcompiler dwmapi urlmon ole32)

  add_executable(fathom_setup WIN32 src/setup/setup.cpp src/setup/setup.rc)
  set_target_properties(fathom_setup PROPERTIES OUTPUT_NAME FathomSetup)
  target_compile_definitions(fathom_setup PRIVATE NOMINMAX UNICODE _UNICODE)
  target_link_libraries(fathom_setup PRIVATE urlmon wininet shell32 ole32 uuid advapi32 dwmapi)
  if(MSVC)
    set_property(TARGET fathom_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()

  add_library(fathom_loader SHARED src/loader/version.cpp src/loader/version.def)
  set_target_properties(fathom_loader PROPERTIES OUTPUT_NAME version)

  add_library(fathom SHARED
    src/game/main.cpp
    src/game/game.cpp
    src/game/ue.cpp
    src/game/overlay.cpp
    src/game/log.cpp
    src/common/dx12_texture.cpp
    ${kiero_SOURCE_DIR}/kiero.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_include_directories(fathom PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${minhook_SOURCE_DIR}/include)
  target_compile_definitions(fathom PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(fathom PRIVATE fathom_ui d3d12 dxgi)
  target_compile_options(fathom PRIVATE /Zi)
  target_link_options(fathom PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

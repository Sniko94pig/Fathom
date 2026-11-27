cmake_minimum_required(VERSION 3.24)
project(fathom LANGUAGES C CXX)
set(FATHOM_VERSION 1.15.4)

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

set(SN2 "${CMAKE_SOURCE_DIR}/games/Subnautica 2/src")
set(SN1 "${CMAKE_SOURCE_DIR}/games/Subnautica/src")
set(SOTF "${CMAKE_SOURCE_DIR}/games/Sons of the Forest/src")
set(MC "${CMAKE_SOURCE_DIR}/games/Minecraft Java/src")
set(UK "${CMAKE_SOURCE_DIR}/games/ULTRAKILL/src")
set(GD "${CMAKE_SOURCE_DIR}/games/Geometry Dash/src")
set(BL3 "${CMAKE_SOURCE_DIR}/games/Borderlands 3/src")
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
  ${SN2}/core/model.cpp
  ${SN2}/core/settings.cpp
  src/ui/ui.cpp
  ${SN2}/ui/menu.cpp
  ${SN2}/ui/pages.cpp
  src/ui/loader_page.cpp
  src/ui/atlas.cpp)
target_include_directories(fathom_ui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp ${imgui_SOURCE_DIR}/backends
  ${json_SOURCE_DIR}/single_include ${stb_SOURCE_DIR} src ${SN2})
target_compile_definitions(fathom_ui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS)

add_executable(fathom_shots src/preview/shots.cpp src/launcher/map_places.cpp src/launcher/map_info.cpp ${SN2}/preview/sample.cpp ${SN1}/core/sn1_features.cpp ${SOTF}/core/sotf_features.cpp ${MC}/core/mc_features.cpp ${MC}/core/mc_live.cpp ${UK}/core/ultrakill_features.cpp ${GD}/core/gd_features.cpp ${BL3}/core/bl3_features.cpp)
target_include_directories(fathom_shots PRIVATE ${stb_SOURCE_DIR} ${SN1} ${SOTF} ${MC} ${UK} ${GD} ${BL3})
target_link_libraries(fathom_shots PRIVATE fathom_ui)
if(WIN32)
  target_sources(fathom_shots PRIVATE src/launcher/logview.cpp src/launcher/home.cpp ${SN2}/launcher/inject.cpp src/launcher/games.cpp src/launcher/art.cpp)
  target_link_libraries(fathom_shots PRIVATE urlmon ole32 wininet)
  target_compile_definitions(fathom_shots PRIVATE FATHOM_VERSION="${FATHOM_VERSION}" FATHOM_LAUNCHER_SHOTS NOMINMAX _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()

if(WIN32)
  add_executable(fathom_preview WIN32
    src/preview/preview_win.cpp ${SN2}/preview/sample.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_link_libraries(fathom_preview PRIVATE fathom_ui d3d12 dxgi d3dcompiler dwmapi)
  if(MINGW)
    target_link_options(fathom_preview PRIVATE -static)
  endif()

  add_executable(fathom_launcher WIN32
    src/launcher/launcher_win.cpp
    src/launcher/home.cpp
    ${SN2}/launcher/inject.cpp
    src/launcher/game_inject.cpp
    src/launcher/logview.cpp
    src/launcher/games.cpp
    src/launcher/art.cpp
    src/launcher/mc_link.cpp
    src/launcher/maps.cpp
    src/launcher/map_places.cpp
    src/launcher/map_info.cpp
    src/launcher/prefs.cpp
    ${SN1}/core/sn1_features.cpp
    ${SOTF}/core/sotf_features.cpp
    ${MC}/core/mc_features.cpp
    ${MC}/core/mc_live.cpp
    ${UK}/core/ultrakill_features.cpp
    ${GD}/core/gd_features.cpp
    ${BL3}/core/bl3_features.cpp
    src/launcher/backups.cpp
    src/launcher/app.rc
    src/common/dx12_texture.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  set_target_properties(fathom_launcher PROPERTIES OUTPUT_NAME Fathom)
  target_include_directories(fathom_launcher PRIVATE ${SN1} ${SOTF} ${MC} ${UK} ${GD} ${BL3})
  target_compile_definitions(fathom_launcher PRIVATE NOMINMAX _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING FATHOM_VERSION="${FATHOM_VERSION}")
  target_link_libraries(fathom_launcher PRIVATE fathom_ui d3d12 dxgi d3dcompiler dwmapi urlmon ole32 wininet advapi32 bcrypt)

  add_executable(fathom_setup WIN32 src/setup/setup.cpp src/setup/setup.rc)
  set_target_properties(fathom_setup PROPERTIES OUTPUT_NAME FathomSetup)
  target_compile_definitions(fathom_setup PRIVATE NOMINMAX UNICODE _UNICODE)
  target_link_libraries(fathom_setup PRIVATE urlmon wininet shell32 ole32 uuid advapi32 dwmapi gdiplus)
  if(MSVC)
    set_property(TARGET fathom_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()

  add_library(fathom_loader SHARED ${SN2}/loader/version.cpp ${SN2}/loader/version.def)
  set_target_properties(fathom_loader PROPERTIES OUTPUT_NAME version)

  set(DEPS ${GEN}/deps)
  file(READ ${kiero_SOURCE_DIR}/kiero.h KIERO_H)
  string(REGEX REPLACE "#define KIERO_INCLUDE_D3D12 +0" "#define KIERO_INCLUDE_D3D12  1" KIERO_H "${KIERO_H}")
  string(REGEX REPLACE "#define KIERO_USE_MINHOOK +0" "#define KIERO_USE_MINHOOK    1" KIERO_H "${KIERO_H}")
  file(WRITE ${DEPS}/kiero/kiero.h.in "${KIERO_H}")
  configure_file(${DEPS}/kiero/kiero.h.in ${DEPS}/kiero/kiero.h COPYONLY)
  file(READ ${kiero_SOURCE_DIR}/kiero.cpp KIERO_CPP)
  string(REPLACE "#include <assert.h>" "#include <assert.h>\n#include <stdlib.h>" KIERO_CPP "${KIERO_CPP}")
  file(WRITE ${DEPS}/kiero/kiero.cpp.in "${KIERO_CPP}")
  configure_file(${DEPS}/kiero/kiero.cpp.in ${DEPS}/kiero/kiero.cpp COPYONLY)
  configure_file(${minhook_SOURCE_DIR}/include/MinHook.h ${DEPS}/minhook/include/MinHook.h COPYONLY)

  add_library(fathom SHARED
    ${SN2}/game/main.cpp
    ${SN2}/game/game.cpp
    ${SN2}/game/ue.cpp
    ${SN2}/game/overlay.cpp
    ${SN2}/game/log.cpp
    src/common/dx12_texture.cpp
    ${DEPS}/kiero/kiero.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_include_directories(fathom PRIVATE ${DEPS} ${minhook_SOURCE_DIR}/include)
  target_compile_definitions(fathom PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(fathom PRIVATE fathom_ui d3d12 dxgi)
  target_compile_options(fathom PRIVATE /Zi)
  target_link_options(fathom PRIVATE /DEBUG /PDBALTPATH:fathom.pdb /OPT:REF /OPT:ICF)

  if(EXISTS "${CMAKE_SOURCE_DIR}/games/Subnautica/src/CMakeLists.txt")
    add_subdirectory(games/Subnautica/src)
  endif()
  if(EXISTS "${SOTF}/CMakeLists.txt")
    add_subdirectory("${SOTF}")
  endif()
  if(EXISTS "${UK}/CMakeLists.txt")
    add_subdirectory("${UK}")
  endif()
  if(EXISTS "${GD}/CMakeLists.txt")
    add_subdirectory("${GD}")
  endif()
  if(EXISTS "${BL3}/CMakeLists.txt")
    add_subdirectory("${BL3}")
  endif()

  find_package(Java 17 COMPONENTS Development Runtime)
  if(EXISTS "${MC}/CMakeLists.txt")
    add_subdirectory("${MC}")
  endif()
  if(Java_JAVAC_EXECUTABLE AND Java_JAR_EXECUTABLE)
    set(MCMOD "${CMAKE_SOURCE_DIR}/games/Minecraft Java/mod")
    set(MCOUT ${GEN}/mcmod)
    file(GLOB_RECURSE MCMOD_SRC CONFIGURE_DEPENDS "${MCMOD}/src/*.java")
    set(MCMOD_STEPS)
    set(MCMOD_DEPS)
    if(TARGET fathom_mcnative)
      list(APPEND MCMOD_STEPS COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:fathom_mcnative> ${MCOUT}/classes/fathom/native/fathom_mc.dll)
      list(APPEND MCMOD_DEPS fathom_mcnative)
    endif()
    set(NIGHT_JAR ${CMAKE_SOURCE_DIR}/third_party/night/Night-Client.jar)
    set(FATHOM_MC_JARS "")
    if(EXISTS ${NIGHT_JAR} AND Java_JAVA_EXECUTABLE)
      list(APPEND MCMOD_STEPS
        COMMAND ${Java_JAVAC_EXECUTABLE} --release 17 -encoding UTF-8 -nowarn -d ${MCOUT}/tools "${MCMOD}/tools/Rebrand.java"
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MCOUT}/classes/META-INF/jars
        COMMAND ${Java_JAVA_EXECUTABLE} -cp ${MCOUT}/tools Rebrand ${NIGHT_JAR} ${MCOUT}/classes/META-INF/jars/fathom-engine.jar)
      list(APPEND MCMOD_DEPS ${NIGHT_JAR} "${MCMOD}/tools/Rebrand.java")
      set(FATHOM_MC_JARS "{\"file\": \"META-INF/jars/fathom-engine.jar\"}")
    endif()
    configure_file("${MCMOD}/fabric.mod.json.in" ${MCOUT}/fabric.mod.json @ONLY)
    add_custom_command(
      OUTPUT ${MCOUT}/fathom_mc.jar
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${MCOUT}/stub ${MCOUT}/classes
      COMMAND ${Java_JAVAC_EXECUTABLE} --release 17 -encoding UTF-8 -nowarn -d ${MCOUT}/stub "${MCMOD}/stub/net/fabricmc/api/ClientModInitializer.java"
      COMMAND ${Java_JAVAC_EXECUTABLE} --release 17 -encoding UTF-8 -Xlint:all -Werror -cp ${MCOUT}/stub -d ${MCOUT}/classes ${MCMOD_SRC}
      COMMAND ${CMAKE_COMMAND} -E copy ${MCOUT}/fabric.mod.json ${MCOUT}/classes/fabric.mod.json
      COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/logo.png ${MCOUT}/classes/assets/fathom/icon.png
      ${MCMOD_STEPS}
      COMMAND ${Java_JAR_EXECUTABLE} --create --file ${MCOUT}/fathom_mc.jar --date=2024-01-01T00:00:00Z -C ${MCOUT}/classes .
      DEPENDS ${MCMOD_SRC} "${MCMOD}/stub/net/fabricmc/api/ClientModInitializer.java" ${MCOUT}/fabric.mod.json ${CMAKE_SOURCE_DIR}/assets/logo.png ${MCMOD_DEPS}
      VERBATIM)
    add_custom_target(fathom_mc ALL
      DEPENDS ${MCOUT}/fathom_mc.jar
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MCOUT}/fathom_mc.jar $<TARGET_FILE_DIR:fathom_launcher>/fathom_mc.jar
      VERBATIM)
  endif()
endif()

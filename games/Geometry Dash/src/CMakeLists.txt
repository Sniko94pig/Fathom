if(TARGET fathom_gd OR NOT WIN32)
  return()
endif()

set(GD_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_gd SHARED
  ${GD_SRC}/ingame/main.cpp
  ${GD_SRC}/ingame/log.cpp
  ${GD_SRC}/ingame/overlay.cpp
  ${GD_SRC}/ingame/gd.cpp
  ${GD_SRC}/ingame/game.cpp
  ${GD_SRC}/ingame/safe.cpp
  ${GD_SRC}/ingame/speed.cpp
  ${GD_SRC}/ingame/player.cpp
  ${GD_SRC}/ingame/icons.cpp
  ${GD_SRC}/ingame/macro.cpp
  ${GD_SRC}/ingame/rgb.cpp
  ${GD_SRC}/ingame/scare.cpp
  ${GD_SRC}/ingame/scare_art.cpp
  ${GD_SRC}/ingame/audio.cpp
  ${GD_SRC}/core/gd_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)
target_include_directories(fathom_gd PRIVATE ${GD_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_gd PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_gd PRIVATE fathom_ui opengl32)
set_target_properties(fathom_gd PROPERTIES
  OUTPUT_NAME fathom_gd
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_gd PRIVATE /Zi /EHa)
  target_link_options(fathom_gd PRIVATE /DEBUG /PDBALTPATH:fathom_gd.pdb /OPT:REF /OPT:ICF)
endif()

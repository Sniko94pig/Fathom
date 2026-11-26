if(TARGET fathom_bl3 OR NOT WIN32)
  return()
endif()

set(BL3_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_bl3 SHARED
  ${BL3_SRC}/ingame/main.cpp
  ${BL3_SRC}/ingame/log.cpp
  ${BL3_SRC}/ingame/overlay.cpp
  ${BL3_SRC}/ingame/esp.cpp
  ${BL3_SRC}/game/ue.cpp
  ${BL3_SRC}/game/game.cpp
  ${BL3_SRC}/game/player.cpp
  ${BL3_SRC}/game/weapons.cpp
  ${BL3_SRC}/game/skills.cpp
  ${BL3_SRC}/game/movement.cpp
  ${BL3_SRC}/game/aim.cpp
  ${BL3_SRC}/game/loot.cpp
  ${BL3_SRC}/game/giver.cpp
  ${BL3_SRC}/game/giver_icons.cpp
  ${BL3_SRC}/game/world.cpp
  ${BL3_SRC}/game/mapshot.cpp
  ${BL3_SRC}/game/vehicle.cpp
  ${BL3_SRC}/core/bl3_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(fathom_bl3 PRIVATE ${BL3_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_bl3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_bl3 PRIVATE fathom_ui d3d11 d3d12 dxgi)
set_target_properties(fathom_bl3 PROPERTIES
  OUTPUT_NAME fathom_bl3
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_bl3 PRIVATE /Zi /EHa)
  target_link_options(fathom_bl3 PRIVATE /DEBUG /PDBALTPATH:fathom_bl3.pdb /OPT:REF /OPT:ICF)
endif()

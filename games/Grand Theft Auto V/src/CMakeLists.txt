if(TARGET fathom_gtav OR NOT WIN32)
  return()
endif()

set(GTAV_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_gtav SHARED
  ${GTAV_SRC}/ingame/main.cpp
  ${GTAV_SRC}/ingame/log.cpp
  ${GTAV_SRC}/ingame/guard.cpp
  ${GTAV_SRC}/ingame/overlay.cpp
  ${GTAV_SRC}/game/shv.cpp
  ${GTAV_SRC}/game/game.cpp
  ${GTAV_SRC}/game/mem.cpp
  ${GTAV_SRC}/game/player.cpp
  ${GTAV_SRC}/game/movement.cpp
  ${GTAV_SRC}/game/weapons.cpp
  ${GTAV_SRC}/game/aim.cpp
  ${GTAV_SRC}/game/giver.cpp
  ${GTAV_SRC}/game/vehicles.cpp
  ${GTAV_SRC}/game/world.cpp
  ${GTAV_SRC}/game/camera.cpp
  ${GTAV_SRC}/game/troll.cpp
  ${GTAV_SRC}/game/mapshot.cpp
  ${GTAV_SRC}/game/lists.cpp
  ${GTAV_SRC}/core/gtav_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(fathom_gtav PRIVATE ${GTAV_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_gtav PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_gtav PRIVATE fathom_ui d3d11 d3d12 dxgi version)
set_target_properties(fathom_gtav PROPERTIES
  OUTPUT_NAME fathom_gtav
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_gtav PRIVATE /Zi /EHa)
  target_link_options(fathom_gtav PRIVATE /DEBUG /PDBALTPATH:fathom_gtav.pdb /OPT:REF /OPT:ICF)
endif()

if(TARGET fathom_sotf OR NOT WIN32)
  return()
endif()

set(SOTF_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_sotf SHARED
  ${SOTF_SRC}/ingame/main.cpp
  ${SOTF_SRC}/ingame/il2cpp.cpp
  ${SOTF_SRC}/ingame/unity.cpp
  ${SOTF_SRC}/ingame/game.cpp
  ${SOTF_SRC}/ingame/vitals.cpp
  ${SOTF_SRC}/ingame/movement.cpp
  ${SOTF_SRC}/ingame/weapons.cpp
  ${SOTF_SRC}/ingame/enemies.cpp
  ${SOTF_SRC}/ingame/building.cpp
  ${SOTF_SRC}/ingame/world.cpp
  ${SOTF_SRC}/ingame/companions.cpp
  ${SOTF_SRC}/ingame/spawn.cpp
  ${SOTF_SRC}/ingame/map.cpp
  ${SOTF_SRC}/ingame/overlay.cpp
  ${SOTF_SRC}/ingame/log.cpp
  ${SOTF_SRC}/core/sotf_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(fathom_sotf PRIVATE ${SOTF_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_sotf PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_sotf PRIVATE fathom_ui d3d11 dxgi)
set_target_properties(fathom_sotf PROPERTIES
  OUTPUT_NAME fathom_sotf
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_sotf PRIVATE /Zi /EHa)
  target_link_options(fathom_sotf PRIVATE /DEBUG /PDBALTPATH:fathom_sotf.pdb /OPT:REF /OPT:ICF)
endif()

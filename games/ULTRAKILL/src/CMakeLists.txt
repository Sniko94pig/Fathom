if(TARGET fathom_ultrakill OR NOT WIN32)
  return()
endif()

set(UK_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_ultrakill SHARED
  ${UK_SRC}/ingame/main.cpp
  ${UK_SRC}/ingame/mono.cpp
  ${UK_SRC}/ingame/unity.cpp
  ${UK_SRC}/ingame/game.cpp
  ${UK_SRC}/ingame/player.cpp
  ${UK_SRC}/ingame/movement.cpp
  ${UK_SRC}/ingame/weapons_a.cpp
  ${UK_SRC}/ingame/weapons_b.cpp
  ${UK_SRC}/ingame/shop.cpp
  ${UK_SRC}/ingame/aim.cpp
  ${UK_SRC}/ingame/orbs.cpp
  ${UK_SRC}/ingame/level.cpp
  ${UK_SRC}/ingame/overlay.cpp
  ${UK_SRC}/ingame/log.cpp
  ${UK_SRC}/core/ultrakill_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(fathom_ultrakill PRIVATE ${UK_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_ultrakill PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_ultrakill PRIVATE fathom_ui d3d11 dxgi)
set_target_properties(fathom_ultrakill PROPERTIES
  OUTPUT_NAME fathom_ultrakill
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_ultrakill PRIVATE /Zi /EHa)
  target_link_options(fathom_ultrakill PRIVATE /DEBUG /PDBALTPATH:fathom_ultrakill.pdb /OPT:REF /OPT:ICF)
endif()

set(UK_SOLO_MENU ${CMAKE_CURRENT_BINARY_DIR}/solo_menu.dll)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/solo.rc CONTENT "1 RCDATA \"${UK_SOLO_MENU}\"\n")
add_library(fathom_ultrakill_solo SHARED EXCLUDE_FROM_ALL
  ${UK_SRC}/solo/solo.cpp
  ${UK_SRC}/solo/solo.def
  ${CMAKE_CURRENT_BINARY_DIR}/solo.rc)
add_dependencies(fathom_ultrakill_solo fathom_ultrakill)
add_custom_command(TARGET fathom_ultrakill_solo PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fathom_ultrakill> ${UK_SOLO_MENU})
target_compile_definitions(fathom_ultrakill_solo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(fathom_ultrakill_solo PROPERTIES
  OUTPUT_NAME version
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ultrakill_solo/$<CONFIG>)

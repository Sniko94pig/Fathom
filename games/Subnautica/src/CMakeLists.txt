if(TARGET fathom_sn1 OR NOT WIN32)
  return()
endif()

set(SN1_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_sn1 SHARED
  ${SN1_SRC}/ingame/main.cpp
  ${SN1_SRC}/ingame/mono.cpp
  ${SN1_SRC}/ingame/unity.cpp
  ${SN1_SRC}/ingame/game.cpp
  ${SN1_SRC}/ingame/esp.cpp
  ${SN1_SRC}/ingame/overlay.cpp
  ${SN1_SRC}/ingame/log.cpp
  ${SN1_SRC}/core/sn1_features.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(fathom_sn1 PRIVATE ${SN1_SRC} ${minhook_SOURCE_DIR}/include)
target_compile_definitions(fathom_sn1 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_sn1 PRIVATE fathom_ui d3d11 dxgi)
set_target_properties(fathom_sn1 PROPERTIES
  OUTPUT_NAME fathom_sn1
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_sn1 PRIVATE /Zi /EHa)
  target_link_options(fathom_sn1 PRIVATE /DEBUG /PDBALTPATH:fathom_sn1.pdb /OPT:REF /OPT:ICF)
endif()

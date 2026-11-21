if(TARGET fathom_mcnative OR NOT WIN32 OR NOT Java_JAVAC_EXECUTABLE)
  return()
endif()

get_filename_component(MC_JDK "${Java_JAVAC_EXECUTABLE}" DIRECTORY)
get_filename_component(MC_JDK "${MC_JDK}" DIRECTORY)
if(NOT EXISTS "${MC_JDK}/include/jni.h")
  return()
endif()

set(MC_SRC ${CMAKE_CURRENT_LIST_DIR})

add_library(fathom_mcnative SHARED
  ${MC_SRC}/ingame/main.cpp
  ${MC_SRC}/ingame/overlay.cpp
  ${MC_SRC}/ingame/log.cpp
  ${MC_SRC}/core/mc_features.cpp
  ${MC_SRC}/core/mc_live.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(fathom_mcnative PRIVATE ${MC_SRC} ${minhook_SOURCE_DIR}/include "${MC_JDK}/include" "${MC_JDK}/include/win32")
target_compile_definitions(fathom_mcnative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fathom_mcnative PRIVATE fathom_ui opengl32)
set_target_properties(fathom_mcnative PROPERTIES
  OUTPUT_NAME fathom_mc
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)
if(MSVC)
  target_compile_options(fathom_mcnative PRIVATE /Zi /EHa)
  target_link_options(fathom_mcnative PRIVATE /DEBUG /PDBALTPATH:fathom_mc.pdb /OPT:REF /OPT:ICF)
endif()

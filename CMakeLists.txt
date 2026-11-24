cmake_minimum_required(VERSION 3.21)
project(JC2_Recomp LANGUAGES CXX)

# Jade Cocoon 2 (NTSC-U, SLUS-20309): a native PC port on BozoEngine (TheBozos/BozoEngine, the submodule). The game's
# EE code is recompiled offline from the player's own disc files (game/), at build time, into build/; nothing from the
# disc is committed. No PS2 hardware at any level: the game's calls are answered by native libraries and JC2's own
# port code (runtime/).

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(JC2_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "User-owned extracted disc files")
set(JC2_ISO "${CMAKE_SOURCE_DIR}/Jade Cocoon 2 (USA).iso" CACHE FILEPATH "User-owned NTSC-U disc image")
set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "Pinned native engine source")
option(JC2_WINDOW "Build the SDL3 game window" OFF)
set(BOZO_APP ${JC2_WINDOW} CACHE BOOL "" FORCE)
set(BOZO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" BozoEngine)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(jc2_gen ${CMAKE_BINARY_DIR}/jc2_gen)
file(MAKE_DIRECTORY ${jc2_gen})
set(jc2_symbols ${CMAKE_SOURCE_DIR}/recomp/jc2_symbols.txt)

# The executable with its code spans described (the VU1 microcode and the data left out) and the offline entry points.
add_custom_command(OUTPUT ${jc2_gen}/jc2.elf ${jc2_gen}/jc2.entries
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/jc2_bozo_prepare.py ${CMAKE_SOURCE_DIR} ${jc2_gen} ${JC2_GAME_DIR}
  DEPENDS tools/jc2_bozo_prepare.py ${JC2_GAME_DIR}/SLUS_203.09 recomp/ghidra/jc2_functions.csv
  COMMENT "Preparing JC2's executable offline")

# What BozoEngine answers (named by recomp/jc2_symbols.txt) and what the port overrides (runtime/jc2_*.cpp).
file(GLOB jc2_port_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/jc2_*.cpp)
add_custom_command(OUTPUT ${jc2_gen}/jc2_externals.txt
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/jc2_bozo_overrides.py ${CMAKE_SOURCE_DIR} ${jc2_gen}/jc2_overrides.txt
  COMMAND bozo-externals ${jc2_gen}/jc2.elf ${jc2_gen}/jc2_externals.new.txt --overrides ${jc2_gen}/jc2_overrides.txt --symbols ${jc2_symbols}
  # Rewritten only when it changes: an edit to a runtime file that adds no override recompiles nothing (Ninja restat).
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${jc2_gen}/jc2_externals.new.txt ${jc2_gen}/jc2_externals.txt
  DEPENDS bozo-externals tools/jc2_bozo_overrides.py ${jc2_gen}/jc2.elf ${jc2_symbols} ${jc2_port_sources})

set(jc2_chunks 32)
set(jc2_sources ${jc2_gen}/jc2_functions.h ${jc2_gen}/jc2_table.cpp)
math(EXPR last "${jc2_chunks} - 1")
foreach(i RANGE ${last})
  string(LENGTH "${i}" len)
  if(len EQUAL 1)
    set(n "00${i}")
  else()
    set(n "0${i}")
  endif()
  list(APPEND jc2_sources ${jc2_gen}/jc2_${n}.cpp)
endforeach()
add_custom_command(OUTPUT ${jc2_sources}
  COMMAND bozo-recomp ${jc2_gen}/jc2.elf ${jc2_gen} --prefix jc2 --symbol-prefix jc2_ --files ${jc2_chunks} --no-comments
          --externals ${jc2_gen}/jc2_externals.txt --symbols ${jc2_symbols} --entries ${jc2_gen}/jc2.entries
          --report ${jc2_gen}/jc2_report.txt
  DEPENDS bozo-recomp ${jc2_gen}/jc2.elf ${jc2_gen}/jc2.entries ${jc2_gen}/jc2_externals.txt ${jc2_symbols}
  COMMENT "Recompiling JC2's EE code (bozo-recomp)")
add_custom_target(jc2_generate DEPENDS ${jc2_sources})

add_library(jc2_game STATIC ${jc2_sources})
target_link_libraries(jc2_game PUBLIC bozo_runtime_headers)
target_include_directories(jc2_game PUBLIC ${jc2_gen})
target_compile_options(jc2_game PRIVATE -O1 -w -ffp-contract=off)

# The code overlays (1,383 CodeWarrior modules loaded from CDDATA.000 at run time), each compiled as an image of its own,
# in 32 batches; jc2_overlays.cpp registers them by their 64-byte header (tools/jc2_bozo_overlays.py).
set(jc2_overlay_gen ${CMAKE_BINARY_DIR}/jc2_overlays)
set(jc2_overlay_sources ${jc2_overlay_gen}/jc2_overlays.cpp)
foreach(i RANGE 31)
  if(i LESS 10)
    list(APPEND jc2_overlay_sources ${jc2_overlay_gen}/jc2_overlays_0${i}.cpp)
  else()
    list(APPEND jc2_overlay_sources ${jc2_overlay_gen}/jc2_overlays_${i}.cpp)
  endif()
endforeach()
add_custom_command(OUTPUT ${jc2_overlay_sources}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/jc2_bozo_overlays.py ${CMAKE_SOURCE_DIR} ${jc2_overlay_gen}
          ${JC2_GAME_DIR} $<TARGET_FILE:bozo-recomp> ${jc2_gen}/jc2_externals.txt 32
  DEPENDS bozo-recomp tools/jc2_bozo_overlays.py tools/find_overlays.py tools/wrap_overlay.py tools/overlay_functions.py
          ${jc2_gen}/jc2_externals.txt ${JC2_GAME_DIR}/CDDATA.000
  COMMENT "Compiling JC2's code overlays (bozo-recomp, one image per module)")
add_library(jc2_overlays STATIC ${jc2_overlay_sources})
target_link_libraries(jc2_overlays PUBLIC bozo_runtime_headers)
target_include_directories(jc2_overlays PRIVATE ${jc2_overlay_gen}/gen)
target_compile_options(jc2_overlays PRIVATE -O1 -w -ffp-contract=off)

# The sound driver's IOP modules compiled offline to native C++ (strict: every import a native host service).
set(jc2_audio_gen ${CMAKE_BINARY_DIR}/jc2_audio_gen)
set(jc2_audio_sources)
set(jc2_audio_inputs)
foreach(module LIBSD MODMIDI MODHSYN UAMAN IMODEMAN M2IOPSYS M2SOUND M2IMODE)
  string(TOLOWER "${module}" lower)
  list(APPEND jc2_audio_sources ${jc2_audio_gen}/${lower}_iop.cpp)
  list(APPEND jc2_audio_inputs ${JC2_GAME_DIR}/IRX/${module}.IRX)
endforeach()
add_custom_command(OUTPUT ${jc2_audio_sources} ${jc2_audio_gen}/jc2_audio_modules.h
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/jc2_bozo_audio.py ${CMAKE_SOURCE_DIR} ${jc2_audio_gen} ${JC2_GAME_DIR} ${BOZO_ENGINE_DIR}
  DEPENDS ${jc2_audio_inputs} tools/jc2_bozo_audio.py
          ${BOZO_ENGINE_DIR}/tools/native_irx/relocate.py ${BOZO_ENGINE_DIR}/tools/native_irx/recompile.py
  COMMENT "Compiling JC2's sound driver modules offline (strict mode)")
add_library(jc2_audio_code OBJECT ${jc2_audio_sources})
target_link_libraries(jc2_audio_code PRIVATE bozo_runtime_headers)
target_compile_options(jc2_audio_code PRIVATE -O1 -w)

# The 3D models, converted offline from the player's disc into BozoEngine's mesh pack (game-derived: stays in build/).
set(jc2_mesh_pack ${CMAKE_BINARY_DIR}/jc2_meshes.pack)
add_custom_command(OUTPUT ${jc2_mesh_pack}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/jc2_mesh_pack.py ${JC2_GAME_DIR} ${jc2_mesh_pack}
  DEPENDS tools/jc2_mesh_pack.py tools/jc2_models.py ${BOZO_ENGINE_DIR}/tools/native_pack/mesh.py
          ${JC2_GAME_DIR}/CDDATA.000 ${JC2_GAME_DIR}/CDDATA.LOC
  COMMENT "Converting JC2's 3D models into the mesh pack")
add_custom_target(jc2_meshes_pack ALL DEPENDS ${jc2_mesh_pack})

set(jc2_runtime runtime/jc2_main.cpp runtime/jc2_port.cpp runtime/jc2_audio.cpp runtime/jc2_draw.cpp runtime/jc2_2d.cpp runtime/jc2_meshes.cpp runtime/jc2_debug.cpp runtime/jc2_ipu.cpp)
add_executable(jc2-headless ${jc2_runtime} $<TARGET_OBJECTS:jc2_audio_code>)
target_include_directories(jc2-headless PRIVATE ${jc2_audio_gen})
target_link_libraries(jc2-headless PRIVATE jc2_game jc2_overlays bozo_host)
target_compile_definitions(jc2-headless PRIVATE
  JC2_DEFAULT_ELF="${JC2_GAME_DIR}/SLUS_203.09" JC2_GAME_DIR="${JC2_GAME_DIR}" JC2_DEFAULT_ISO="${JC2_ISO}" JC2_SYMBOL_MAP="${jc2_symbols}" JC2_MESH_PACK="${jc2_mesh_pack}")
if(MINGW)
  target_link_options(jc2-headless PRIVATE -static -Wl,--stack,268435456)
endif()

# The game window (SDL3, OpenGL 3.3): BozoEngine's platform layer. SDL3 and Dear ImGui are fetched at configure unless
# FETCHCONTENT_SOURCE_DIR_SDL3 / FETCHCONTENT_SOURCE_DIR_IMGUI name local copies of the pinned versions.
if(JC2_WINDOW)
  add_executable(jc2 ${jc2_runtime} $<TARGET_OBJECTS:jc2_audio_code>)
  target_include_directories(jc2 PRIVATE ${jc2_audio_gen})
  target_link_libraries(jc2 PRIVATE jc2_game jc2_overlays bozo_host bozo_app)
  target_compile_definitions(jc2 PRIVATE JC2_WITH_WINDOW
    JC2_DEFAULT_ELF="${JC2_GAME_DIR}/SLUS_203.09" JC2_GAME_DIR="${JC2_GAME_DIR}" JC2_DEFAULT_ISO="${JC2_ISO}" JC2_SYMBOL_MAP="${jc2_symbols}" JC2_MESH_PACK="${jc2_mesh_pack}")
  if(MINGW)
    target_link_options(jc2 PRIVATE -static -Wl,--stack,268435456)
  endif()
  if(WIN32)
    # The window's icon and version information (runtime/app/jc2.rc; the icon from docs/branding/make_logo.py).
    enable_language(RC)
    set_source_files_properties(runtime/app/jc2.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/app/jc2.ico")
    target_sources(jc2 PRIVATE runtime/app/jc2.rc)
  endif()
endif()

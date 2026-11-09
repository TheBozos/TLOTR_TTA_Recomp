cmake_minimum_required(VERSION 3.21)
project(TLOTR_TTA_Recomp LANGUAGES CXX)

# The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027) on BozoEngine: the game's EE code recompiled by
# bozo-recomp, every library and kernel call answered natively by the engine's runtime. No PS2 hardware model.
# The ELF comes from the player's own disc (scripts/extract_iso.sh puts it in game/).

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(TTA_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "User-owned extracted disc files")
set(TTA_ISO "${CMAKE_SOURCE_DIR}/Lord of the Rings, The - The Third Age (USA).iso" CACHE FILEPATH
    "User-owned NTSC-U disc image")
set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "Pinned native engine source")
option(TTA_WINDOW "Build the SDL3 native game window" OFF)
set(BOZO_APP ${TTA_WINDOW} CACHE BOOL "" FORCE)
set(BOZO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" BozoEngine)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(tta_elf ${TTA_GAME_DIR}/SLUS_210.27)
set(tta_symbols ${CMAKE_SOURCE_DIR}/recomp/tta_symbols.txt)
set(tta_gen ${CMAKE_BINARY_DIR}/tta_gen)
file(MAKE_DIRECTORY ${tta_gen})
file(GLOB tta_port_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/tta_*.cpp)

# The externals list: library and kernel functions named by the symbol map become stubs the runtime answers; every
# address the port registers (tools/tta_overrides_list.py scans runtime/) becomes an override.
set(tta_externals ${tta_gen}/tta_externals.txt)
add_custom_command(OUTPUT ${tta_externals}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/tta_overrides_list.py ${CMAKE_SOURCE_DIR}/runtime ${tta_gen}/tta_overrides.txt
  COMMAND bozo-externals ${tta_elf} ${tta_externals} --symbols ${tta_symbols} --overrides ${tta_gen}/tta_overrides.txt
  DEPENDS bozo-externals ${tta_elf} ${tta_symbols} ${CMAKE_SOURCE_DIR}/tools/tta_overrides_list.py ${tta_port_sources}
  COMMENT "Listing the externals of SLUS_210.27")

set(tta_chunks 32)
set(tta_sources ${tta_gen}/tta_functions.h ${tta_gen}/tta_table.cpp)
math(EXPR tta_last "${tta_chunks} - 1")
foreach(i RANGE ${tta_last})
  if(i LESS 10)
    list(APPEND tta_sources ${tta_gen}/tta_00${i}.cpp)
  else()
    list(APPEND tta_sources ${tta_gen}/tta_0${i}.cpp)
  endif()
endforeach()
add_custom_command(OUTPUT ${tta_sources}
  COMMAND bozo-recomp ${tta_elf} ${tta_gen} --prefix tta --files ${tta_chunks} --no-comments --externals ${tta_externals}
          --symbols ${tta_symbols} --report ${tta_gen}/tta_report.txt
  DEPENDS bozo-recomp ${tta_elf} ${tta_externals} ${tta_symbols}
  COMMENT "Recompiling SLUS_210.27")
add_custom_target(tta_generate DEPENDS ${tta_sources})

add_library(tta_game STATIC ${tta_sources})
target_link_libraries(tta_game PUBLIC bozo_runtime_headers)
target_include_directories(tta_game PUBLIC ${tta_gen})
target_compile_options(tta_game PRIVATE -O1 -w -ffp-contract=off)

add_executable(tta-headless ${tta_port_sources})
target_link_libraries(tta-headless PRIVATE tta_game bozo_host)
target_compile_definitions(tta-headless PRIVATE TTA_DEFAULT_ELF="${tta_elf}" TTA_DEFAULT_ISO="${TTA_ISO}"
                                                TTA_SYMBOL_MAP="${tta_symbols}")
if(MINGW)
  target_link_options(tta-headless PRIVATE -static)
endif()

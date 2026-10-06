include_guard(GLOBAL)

option(DAL_PPU_WERROR "Treat warnings in generated PPU code as errors" ON)

# Options for every target that compiles generated PPU code or code that mirrors it. Target
# COMPILE_OPTIONS follow CMAKE_C_FLAGS_<CONFIG>, so -O1 overrides Debug's -O0.
function(dal_ppu_codegen_options target)
  target_compile_options(${target} PRIVATE
    -O1
    -march=x86-64-v3
    -ffp-contract=off
    -fpatchable-function-entry=8
    -Wall
    -Wextra)
  if(DAL_PPU_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
endfunction()

# Adds the generated PPU tree and its link test when DAL_PPU_GENERATED_DIR is set. Call after
# dal_ppu_stub is defined.
set(DAL_PPU_GENERATED_DIR "" CACHE PATH "Output directory of 'ps3tool emit' (empty: no generated code)")

function(dal_ppu_add_generated_tree)
  if(DAL_PPU_GENERATED_DIR STREQUAL "")
    message(STATUS "DAL: generated PPU code not configured (DAL_PPU_GENERATED_DIR is empty)")
    return()
  endif()

  cmake_path(ABSOLUTE_PATH DAL_PPU_GENERATED_DIR BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
             OUTPUT_VARIABLE generated_dir)
  set(sources_file "${generated_dir}/sources.cmake")
  if(NOT EXISTS "${sources_file}")
    message(FATAL_ERROR
      "DAL: ${sources_file} not found. Generate it with: "
      "python tools/ps3tool.py emit <EBOOT.elf> --out ${DAL_PPU_GENERATED_DIR}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sources_file}")
  include("${sources_file}")
  message(STATUS "DAL: generated PPU code from ${generated_dir}")

  add_library(dal_ppu_generated STATIC ${DAL_PPU_GENERATED_SOURCES})
  dal_ppu_codegen_options(dal_ppu_generated)
  target_include_directories(dal_ppu_generated PUBLIC "${generated_dir}")
  target_link_libraries(dal_ppu_generated PUBLIC dal_ppu_stub)

  add_executable(dal_ppu_link_test runtime/tests/ppu_link_test.cpp)
  target_compile_options(dal_ppu_link_test PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_ppu_link_test PRIVATE dal_ppu_generated dal_ppu_stub)

  add_test(NAME ppu_link COMMAND dal_ppu_link_test)
  set_tests_properties(ppu_link PROPERTIES PASS_REGULAR_EXPRESSION "ppu link ok")
endfunction()

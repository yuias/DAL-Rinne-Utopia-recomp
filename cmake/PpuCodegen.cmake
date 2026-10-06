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

# Toolchain for macOS with Homebrew LLVM (brew install llvm). Apple's own clang is rejected by
# cmake/CompilerChecks.cmake, so the compilers come from the LLVM prefix.
#
# The prefix is DAL_LLVM_PREFIX (cache variable or environment), else `brew --prefix llvm`.

if(NOT DAL_LLVM_PREFIX AND DEFINED ENV{DAL_LLVM_PREFIX})
  set(DAL_LLVM_PREFIX "$ENV{DAL_LLVM_PREFIX}")
endif()

if(NOT DAL_LLVM_PREFIX)
  execute_process(
    COMMAND brew --prefix llvm
    OUTPUT_VARIABLE dal_brew_llvm_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE dal_brew_result)
  if(dal_brew_result EQUAL 0)
    set(DAL_LLVM_PREFIX "${dal_brew_llvm_prefix}")
  endif()
endif()

if(NOT DAL_LLVM_PREFIX OR NOT EXISTS "${DAL_LLVM_PREFIX}/bin/clang++")
  message(FATAL_ERROR
    "DAL: LLVM clang not found (prefix '${DAL_LLVM_PREFIX}'). Install it with `brew install llvm` or "
    "set DAL_LLVM_PREFIX to a directory that contains bin/clang++.")
endif()

# Cached, and mirrored into the environment so the try_compile projects (which re-read this file)
# do not run brew again.
set(DAL_LLVM_PREFIX "${DAL_LLVM_PREFIX}" CACHE PATH "LLVM installation prefix" FORCE)
set(ENV{DAL_LLVM_PREFIX} "${DAL_LLVM_PREFIX}")

set(CMAKE_C_COMPILER "${DAL_LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${DAL_LLVM_PREFIX}/bin/clang++")
set(CMAKE_OBJCXX_COMPILER "${DAL_LLVM_PREFIX}/bin/clang++")

# Homebrew binutils can put a GNU ar first on PATH; use the LLVM archiver that matches the compiler.
set(CMAKE_AR "${DAL_LLVM_PREFIX}/bin/llvm-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${DAL_LLVM_PREFIX}/bin/llvm-ranlib" CACHE FILEPATH "Archive indexer")

# The C++26 library headers of this clang need the matching Homebrew libc++, not the one in the SDK
# (Homebrew's documented link flags for LLVM).
set(dal_llvm_link_flags
  "-L${DAL_LLVM_PREFIX}/lib/c++ -Wl,-rpath,${DAL_LLVM_PREFIX}/lib/c++ -L${DAL_LLVM_PREFIX}/lib/unwind -lunwind")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${dal_llvm_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${dal_llvm_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${dal_llvm_link_flags}")

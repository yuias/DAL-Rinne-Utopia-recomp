include_guard(GLOBAL)

include(CMakePushCheckState)
include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "DAL: clang is required (C: ${CMAKE_C_COMPILER_ID}, C++: ${CMAKE_CXX_COMPILER_ID})")
endif()

# clang-cl would accept the sources but later phases need GNU-style flags.
if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  message(FATAL_ERROR
    "DAL: clang GNU-style driver required, got frontend '${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}'")
endif()

cmake_push_check_state()
# Without -Werror an unknown attribute is only a warning and the check passes falsely.
set(CMAKE_REQUIRED_FLAGS -Werror)

check_c_source_compiles("
static int g(int x) { return x; }
int f(int x) { __attribute__((musttail)) return g(x); }
int main(void) { return f(0); }
" DAL_HAVE_MUSTTAIL_C)

check_cxx_source_compiles("
static int g(int x) { return x; }
int f(int x) { [[clang::musttail]] return g(x); }
int main() { return f(0); }
" DAL_HAVE_MUSTTAIL_CXX)
cmake_pop_check_state()

if(NOT DAL_HAVE_MUSTTAIL_C OR NOT DAL_HAVE_MUSTTAIL_CXX)
  message(FATAL_ERROR
    "DAL: musttail unsupported (C: ${DAL_HAVE_MUSTTAIL_C}, C++: ${DAL_HAVE_MUSTTAIL_CXX})")
endif()

message(STATUS "DAL: musttail supported (C, C++)")

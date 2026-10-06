include_guard(GLOBAL)

# Runtime core: everything that does not reference generated symbols, so it builds and its tests
# run without a generated tree. Call after enable_testing().
function(dal_runtime_add_core)
  add_library(dal_runtime_core STATIC
    runtime/src/core/elf_loader.cpp
    runtime/src/core/fatal.cpp
    runtime/src/core/guest_memory.cpp
    runtime/src/core/log.cpp)
  target_compile_options(dal_runtime_core PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_runtime_core PUBLIC
    ${CMAKE_SOURCE_DIR}/runtime/src
    ${CMAKE_SOURCE_DIR}/runtime/include)
  target_link_libraries(dal_runtime_core PUBLIC bcrypt)

  add_executable(dal_runtime_tests runtime/tests/runtime_core_test.cpp)
  target_compile_options(dal_runtime_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_runtime_tests PRIVATE dal_runtime_core)

  add_test(NAME runtime_core COMMAND dal_runtime_tests)
  set_tests_properties(runtime_core PROPERTIES
    PASS_REGULAR_EXPRESSION "runtime core tests: [0-9]+ passed, 0 failed")
endfunction()

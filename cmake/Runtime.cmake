include_guard(GLOBAL)

# spdlog is built as a static library rather than header-only: its sources then compile with the
# project's default flags (no -Werror) and keep <windows.h> out of runtime translation units.
# SPDLOG_USE_STD_FORMAT reuses the std::format the runtime already depends on instead of bundled fmt.
function(dal_runtime_add_spdlog)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/spdlog/CMakeLists.txt)
    message(FATAL_ERROR
      "DAL: externals/spdlog is missing or empty. Run: git submodule update --init")
  endif()
  set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
  set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/externals/spdlog
    ${CMAKE_BINARY_DIR}/externals/spdlog EXCLUDE_FROM_ALL)
endfunction()

# Runtime core: everything that does not reference generated symbols, so it builds and its tests
# run without a generated tree. Call after enable_testing().
function(dal_runtime_add_core)
  dal_runtime_add_spdlog()

  add_library(dal_runtime_core STATIC
    runtime/src/core/boot.cpp
    runtime/src/core/census.cpp
    runtime/src/core/cpu_hooks.cpp
    runtime/src/core/dispatch.cpp
    runtime/src/core/elf_loader.cpp
    runtime/src/core/fatal.cpp
    runtime/src/core/guest_memory.cpp
    runtime/src/core/log.cpp
    runtime/src/core/threads.cpp
    runtime/src/hle/hle.cpp
    runtime/src/lv2/syscalls.cpp)
  target_compile_options(dal_runtime_core PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_runtime_core PUBLIC
    ${CMAKE_SOURCE_DIR}/runtime/src
    ${CMAKE_SOURCE_DIR}/runtime/include)
  target_link_libraries(dal_runtime_core PUBLIC bcrypt)
  # PRIVATE: spdlog types stay out of core/log.h, so consumers never compile against its headers.
  target_link_libraries(dal_runtime_core PRIVATE spdlog::spdlog)

  add_executable(dal_runtime_tests runtime/tests/runtime_core_test.cpp)
  target_compile_options(dal_runtime_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_runtime_tests PRIVATE dal_runtime_core)

  add_test(NAME runtime_core COMMAND dal_runtime_tests)
  set_tests_properties(runtime_core PROPERTIES
    PASS_REGULAR_EXPRESSION "runtime core tests: [0-9]+ passed, 0 failed")
endfunction()

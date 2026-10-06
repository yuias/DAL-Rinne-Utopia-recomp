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
    runtime/src/core/param_sfo.cpp
    runtime/src/core/threads.cpp
    runtime/src/hle/cell_fs.cpp
    runtime/src/hle/cell_game.cpp
    runtime/src/hle/cell_gcm_sys.cpp
    runtime/src/hle/cell_spurs.cpp
    runtime/src/hle/cell_sysmodule.cpp
    runtime/src/hle/hle.cpp
    runtime/src/hle/sys_prx_for_user.cpp
    runtime/src/lv2/sys_event.cpp
    runtime/src/lv2/sys_fs.cpp
    runtime/src/lv2/sys_misc.cpp
    runtime/src/lv2/sys_mutex_cond.cpp
    runtime/src/lv2/sys_ppu_thread.cpp
    runtime/src/lv2/sys_spu.cpp
    runtime/src/lv2/syscalls.cpp
    runtime/src/lv2/vfs.cpp
    runtime/src/rsx/fifo_parser.cpp
    runtime/src/rsx/gcm_interrupts.cpp
    runtime/src/rsx/rsx_state.cpp
    runtime/src/rsx/rsx_thread.cpp)
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

  dal_sdat_add_tool()
endfunction()

# SDAT decryption tool and its tests. The AES keys are read from a local RPCS3 checkout when the
# tool runs, so nothing here (or in any test) contains key material.
function(dal_sdat_add_tool)
  add_library(dal_sdat_lib STATIC tools/dal_sdat/sdat.cpp tools/dal_sdat/sdat_lz.cpp)
  target_compile_options(dal_sdat_lib PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_sdat_lib PUBLIC ${CMAKE_SOURCE_DIR}/tools/dal_sdat)
  target_link_libraries(dal_sdat_lib PUBLIC bcrypt)

  add_executable(dal_sdat tools/dal_sdat/main.cpp)
  target_compile_options(dal_sdat PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_sdat PRIVATE dal_sdat_lib)

  add_executable(dal_sdat_tests tools/dal_sdat/sdat_test.cpp)
  target_compile_options(dal_sdat_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_sdat_tests PRIVATE dal_sdat_lib)
  add_test(NAME sdat_tests COMMAND dal_sdat_tests)
  set_tests_properties(sdat_tests PROPERTIES PASS_REGULAR_EXPRESSION "sdat tests: [0-9]+ passed, 0 failed")
endfunction()

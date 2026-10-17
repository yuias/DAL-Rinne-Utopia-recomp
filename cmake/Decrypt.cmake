include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/rpcs3_keys.cmake")

set(DAL_EBOOT_BIN "" CACHE FILEPATH
  "Disc PS3_GAME/USRDIR/EBOOT.BIN to decrypt into generated/inputs/EBOOT.elf (empty: not decrypted)")

# Shared by both presets: the decrypted content is identical.
set(DAL_INPUTS_DIR "${CMAKE_SOURCE_DIR}/generated/inputs")

# dal_decrypt: the offline decryption tool for EBOOT.BIN, its synthetic unit tests, and the build step
# that produces generated/inputs/EBOOT.elf. Call after dal_runtime_add_core() (for zlibstatic) and
# enable_testing().
function(dal_decrypt_add)
  set(decrypt_options ${DAL_ARCH_FLAGS} -Wall -Wextra)

  add_library(dal_decrypt_lib STATIC
    tools/dal_decrypt/cli.cpp
    tools/dal_decrypt/crypto.cpp
    tools/dal_decrypt/keys.cpp
    tools/dal_decrypt/sce.cpp
    tools/dal_decrypt/self.cpp)
  target_compile_options(dal_decrypt_lib PRIVATE ${decrypt_options})
  target_include_directories(dal_decrypt_lib PUBLIC tools/dal_decrypt)
  target_link_libraries(dal_decrypt_lib PUBLIC bcrypt zlibstatic)

  add_executable(dal_decrypt tools/dal_decrypt/main.cpp)
  target_compile_options(dal_decrypt PRIVATE ${decrypt_options})
  target_link_libraries(dal_decrypt PRIVATE dal_decrypt_lib)

  add_executable(dal_decrypt_tests
    tools/dal_decrypt/tests/test_main.cpp
    tools/dal_decrypt/tests/cli_test.cpp
    tools/dal_decrypt/tests/crypto_test.cpp
    tools/dal_decrypt/tests/keys_test.cpp
    tools/dal_decrypt/tests/sce_test.cpp
    tools/dal_decrypt/tests/self_test.cpp)
  target_compile_options(dal_decrypt_tests PRIVATE ${decrypt_options})
  target_link_libraries(dal_decrypt_tests PRIVATE dal_decrypt_lib)
  add_test(NAME decrypt_core COMMAND dal_decrypt_tests)
  set_tests_properties(decrypt_core PROPERTIES
    PASS_REGULAR_EXPRESSION "decrypt core tests: [0-9]+ passed, 0 failed")

  if(DAL_EBOOT_BIN STREQUAL "")
    message(STATUS "DAL: EBOOT.BIN decryption not configured (DAL_EBOOT_BIN is empty)")
    return()
  endif()

  dal_resolve_rpcs3_keys(REQUIRED)

  # The pins are read by CMake and passed as arguments, so the tool needs no JSON parser.
  set(target_json "${CMAKE_SOURCE_DIR}/config/target.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${target_json}")
  file(READ "${target_json}" target_text)
  string(JSON eboot_bin_sha256 GET "${target_text}" eboot_bin sha256)
  string(JSON eboot_elf_size GET "${target_text}" eboot_elf size)
  string(JSON eboot_elf_sha256 GET "${target_text}" eboot_elf sha256)

  set(key_files "${DAL_RPCS3_KEYS_DIR}/key_vault.h" "${DAL_RPCS3_KEYS_DIR}/key_vault.cpp")
  set(eboot_args self --keys "${DAL_RPCS3_KEYS_DIR}" --in "${DAL_EBOOT_BIN}"
    --expect-size ${eboot_elf_size} --expect-sha256 ${eboot_elf_sha256}
    --expect-input-sha256 ${eboot_bin_sha256})
  add_custom_command(OUTPUT "${DAL_INPUTS_DIR}/EBOOT.elf"
    COMMAND dal_decrypt ${eboot_args} --out "${DAL_INPUTS_DIR}/EBOOT.elf"
    DEPENDS dal_decrypt "${DAL_EBOOT_BIN}" ${key_files}
    COMMENT "Decrypting EBOOT.BIN to generated/inputs/EBOOT.elf"
    VERBATIM)
  add_custom_target(dal_inputs ALL DEPENDS "${DAL_INPUTS_DIR}/EBOOT.elf")

  add_test(NAME decrypt_eboot COMMAND dal_decrypt ${eboot_args} --out "${CMAKE_BINARY_DIR}/decrypt_test/EBOOT.elf")
  set_tests_properties(decrypt_eboot PROPERTIES
    PASS_REGULAR_EXPRESSION "sha256 ${eboot_elf_sha256}: matches the pin"
    TIMEOUT 120)
endfunction()

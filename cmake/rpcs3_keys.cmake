include_guard(GLOBAL)

# The keys for the .sdat files and EBOOT.BIN are read from RPCS3's key_vault.h and key_vault.cpp. They
# are not committed here: configuration downloads them from a pinned RPCS3 commit and checks their
# SHA-256. dal_sdat and dal_decrypt only parse key constants from these GPL-2.0 files, as data; no
# RPCS3 code is compiled or linked.
set(DAL_RPCS3_KEYS_COMMIT "f9b8c209a3800c1865bcfeade611f65f5237138c")
set(DAL_RPCS3_KEY_VAULT_H_SHA256 "d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf")
set(DAL_RPCS3_KEY_VAULT_CPP_SHA256 "a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7")
set(DAL_RPCS3_KEYS_URL_BASE "https://raw.githubusercontent.com/RPCS3/rpcs3/${DAL_RPCS3_KEYS_COMMIT}/rpcs3/Crypto")

set(DAL_RPCS3_CRYPTO_DIR "" CACHE PATH
  "Directory with key_vault.h and key_vault.cpp of an RPCS3 checkout (rpcs3/Crypto); empty: download the pinned copies")

# Sets DAL_RPCS3_KEYS_DIR in the caller to the directory holding key_vault.h and key_vault.cpp. With
# REQUIRED every failure is fatal. Without it a failure is a warning and DAL_RPCS3_KEYS_DIR is "", so
# the .sdat cache can be skipped while the rest of the build goes on.
function(dal_resolve_rpcs3_keys)
  cmake_parse_arguments(PARSE_ARGV 0 arg "REQUIRED" "" "")
  if(arg_REQUIRED)
    set(failure FATAL_ERROR)
  else()
    set(failure WARNING)
  endif()
  set(key_files key_vault.h key_vault.cpp)
  set(key_pins "${DAL_RPCS3_KEY_VAULT_H_SHA256}" "${DAL_RPCS3_KEY_VAULT_CPP_SHA256}")
  set(hint "Set DAL_RPCS3_CRYPTO_DIR (build.ps1 -Rpcs3CryptoDir) to rpcs3/Crypto of a local RPCS3 checkout to build without the download.")
  set(DAL_RPCS3_KEYS_DIR "" PARENT_SCOPE)

  if(NOT DAL_RPCS3_CRYPTO_DIR STREQUAL "")
    if(NOT IS_DIRECTORY "${DAL_RPCS3_CRYPTO_DIR}")
      message(${failure} "DAL: DAL_RPCS3_CRYPTO_DIR (${DAL_RPCS3_CRYPTO_DIR}) is not a directory; "
        "it must be the directory that holds key_vault.h and key_vault.cpp.")
      return()
    endif()
    foreach(name pin IN ZIP_LISTS key_files key_pins)
      set(file "${DAL_RPCS3_CRYPTO_DIR}/${name}")
      if(NOT EXISTS "${file}")
        message(${failure} "DAL: ${file} not found (DAL_RPCS3_CRYPTO_DIR must hold key_vault.h and key_vault.cpp).")
        return()
      endif()
      # A different RPCS3 commit normally has the same entries; the output pins catch a real problem.
      file(SHA256 "${file}" hash)
      if(NOT hash STREQUAL pin)
        message(STATUS "DAL: ${name} in DAL_RPCS3_CRYPTO_DIR differs from the pinned RPCS3 ${DAL_RPCS3_KEYS_COMMIT}")
      endif()
    endforeach()
    set(DAL_RPCS3_KEYS_DIR "${DAL_RPCS3_CRYPTO_DIR}" PARENT_SCOPE)
    return()
  endif()

  set(keys_dir "${CMAKE_SOURCE_DIR}/generated/deps/rpcs3/${DAL_RPCS3_KEYS_COMMIT}")
  foreach(name pin IN ZIP_LISTS key_files key_pins)
    set(file "${keys_dir}/${name}")
    set(hash "")
    if(EXISTS "${file}")
      file(SHA256 "${file}" hash)
    endif()
    if(NOT hash STREQUAL pin)
      message(STATUS "DAL: downloading ${name} from RPCS3 ${DAL_RPCS3_KEYS_COMMIT}")
      file(DOWNLOAD "${DAL_RPCS3_KEYS_URL_BASE}/${name}" "${file}.part"
        EXPECTED_HASH SHA256=${pin}
        STATUS status TLS_VERIFY ON)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        file(REMOVE "${file}.part")
        list(GET status 1 reason)
        message(${failure} "DAL: ${name} download failed (${reason}). ${hint}")
        return()
      endif()
      file(RENAME "${file}.part" "${file}")
    endif()
  endforeach()
  set(DAL_RPCS3_KEYS_DIR "${keys_dir}" PARENT_SCOPE)
endfunction()

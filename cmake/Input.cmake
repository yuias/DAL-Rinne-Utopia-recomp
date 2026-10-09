include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/gameinput.cmake")

# The Microsoft.GameInput package is fetched and verified by tools/fetch-gameinput.ps1 into
# generated/deps/gameinput. Needs a successful fetch, which leaves a marker holding the SHA-256 of
# the pinned .nupkg.
#
# Sets in the caller's scope:
#   DAL_GAMEINPUT_INCLUDE_DIR  the v3 header directory. Targets must add it as a SYSTEM include:
#                              the Windows SDK ships an older GameInput.h on the default include path.
#   DAL_GAMEINPUT_LIB          GameInput.lib, a static loader that resolves the runtime with
#                              LoadLibrary (it imports nothing from a GameInput DLL). Link it by this
#                              full path: the SDK's own gameinput.lib is an import library.
#   DAL_GAMEINPUT_SXS_DLLS     side-by-side runtime DLLs to copy next to the executable. The package
#                              ships its runtime only inside redist/GameInputRedist.msi;
#                              tools/fetch-gameinput.ps1 extracts the x64 GameInputRedist.dll from it
#                              into sxs/x64 and checks it against DAL_GAMEINPUT_REDIST_SHA256.
#   DAL_GAMEINPUT_LICENCES     licence texts that travel with the build
function(dal_input_find_gameinput)
  set(DAL_GAMEINPUT_DIR "" CACHE PATH
    "Extracted Microsoft.GameInput package (default: generated/deps/gameinput/<pinned version>)")
  if("${DAL_GAMEINPUT_DIR}" STREQUAL "")
    set(gameinput_dir "${CMAKE_SOURCE_DIR}/generated/deps/gameinput/${DAL_GAMEINPUT_VERSION}")
  else()
    set(gameinput_dir "${DAL_GAMEINPUT_DIR}")
  endif()

  set(marker "${gameinput_dir}/.verified-sha256")
  # A fetch after this configure must re-run it.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
  set(fetch_hint "Run: pwsh -NoProfile -File tools/fetch-gameinput.ps1")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "DAL: GameInput is not fetched (${marker} not found). ${fetch_hint}")
  endif()
  file(READ "${marker}" marker_hash)
  string(STRIP "${marker_hash}" marker_hash)
  string(TOLOWER "${marker_hash}" marker_hash)
  string(TOLOWER "${DAL_GAMEINPUT_SHA256}" pinned_hash)
  if(NOT marker_hash STREQUAL pinned_hash)
    message(FATAL_ERROR
      "DAL: GameInput in ${gameinput_dir} does not match the pin in cmake/gameinput.cmake. ${fetch_hint}")
  endif()
  foreach(path native/include/GameInput.h native/lib/x64/GameInput.lib LICENSE.txt NOTICE.txt
      sxs/x64/GameInputRedist.dll)
    if(NOT EXISTS "${gameinput_dir}/${path}")
      message(FATAL_ERROR "DAL: ${gameinput_dir}/${path} is missing. ${fetch_hint} -Force")
    endif()
  endforeach()
  # The marker only vouches for the package as fetched; the DLL is copied into every build, so check
  # it again, and re-run configure (and this check) whenever it changes.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gameinput_dir}/sxs/x64/GameInputRedist.dll")
  file(SHA256 "${gameinput_dir}/sxs/x64/GameInputRedist.dll" redist_hash)
  string(TOLOWER "${DAL_GAMEINPUT_REDIST_SHA256}" pinned_redist_hash)
  if(NOT redist_hash STREQUAL pinned_redist_hash)
    message(FATAL_ERROR
      "DAL: ${gameinput_dir}/sxs/x64/GameInputRedist.dll does not match the pin in cmake/gameinput.cmake. ${fetch_hint} -Force")
  endif()

  set(DAL_GAMEINPUT_INCLUDE_DIR "${gameinput_dir}/native/include" PARENT_SCOPE)
  set(DAL_GAMEINPUT_LIB "${gameinput_dir}/native/lib/x64/GameInput.lib" PARENT_SCOPE)
  set(DAL_GAMEINPUT_SXS_DLLS "${gameinput_dir}/sxs/x64/GameInputRedist.dll" PARENT_SCOPE)
  set(DAL_GAMEINPUT_LICENCES "${gameinput_dir}/LICENSE.txt;${gameinput_dir}/NOTICE.txt" PARENT_SCOPE)

  # Relative to the source tree so the log carries no machine-specific prefix.
  file(RELATIVE_PATH gameinput_rel "${CMAKE_SOURCE_DIR}" "${gameinput_dir}")
  message(STATUS "DAL: GameInput: ${gameinput_rel}")
endfunction()

# The input libraries and tools. Call after dal_runtime_add_core() and enable_testing().
function(dal_input_add)
  dal_input_find_gameinput()

  # GameInput.lib finds GameInputRedist.dll in the directory of the executable, so the DLL goes next
  # to dal_game; its licence texts travel with it.
  set(bin_dir "${CMAKE_BINARY_DIR}/bin")
  set(licence_dir "${bin_dir}/licenses/gameinput")
  set(copied "")
  foreach(dll ${DAL_GAMEINPUT_SXS_DLLS})
    get_filename_component(dll_name "${dll}" NAME)
    add_custom_command(OUTPUT "${bin_dir}/${dll_name}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${bin_dir}/${dll_name}"
      DEPENDS "${dll}"
      VERBATIM)
    list(APPEND copied "${bin_dir}/${dll_name}")
  endforeach()
  foreach(licence ${DAL_GAMEINPUT_LICENCES})
    get_filename_component(licence_name "${licence}" NAME)
    add_custom_command(OUTPUT "${licence_dir}/${licence_name}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${licence}" "${licence_dir}/${licence_name}"
      DEPENDS "${licence}"
      VERBATIM)
    list(APPEND copied "${licence_dir}/${licence_name}")
  endforeach()
  add_custom_target(dal_gameinput_runtime ALL DEPENDS ${copied})
endfunction()

include_guard(GLOBAL)

# Vulkan-Headers, volk and VulkanMemoryAllocator come from pinned submodules, so the headers do not
# depend on the SDK installed on a machine. volk is built from its single C file with the project's
# default flags (no -Werror); SYSTEM keeps the third-party headers out of our warnings. Nothing is
# fetched or generated at configure time.
function(dal_render_add_libraries)
  foreach(dir Vulkan-Headers volk VulkanMemoryAllocator)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.md AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE.txt AND
       NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/${dir}/LICENSE)
      message(FATAL_ERROR
        "DAL: externals/${dir} is missing or empty. Run: git submodule update --init")
    endif()
  endforeach()

  add_library(volk STATIC ${CMAKE_SOURCE_DIR}/externals/volk/volk.c)
  target_include_directories(volk SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/externals/volk
    ${CMAKE_SOURCE_DIR}/externals/Vulkan-Headers/include)
  # No link against the Vulkan loader import library: volk loads it at run time.
  target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
  target_compile_options(volk PRIVATE -march=x86-64-v3)
endfunction()

# Informational: the run-time shader compiler comes from the SDK (static shaderc), which is
# located through the environment and not linked yet.
function(dal_render_report_sdk)
  find_package(Vulkan QUIET OPTIONAL_COMPONENTS shaderc_combined)
  if(Vulkan_FOUND)
    message(STATUS "DAL: Vulkan SDK found (shaderc_combined: "
      "${Vulkan_shaderc_combined_FOUND})")
  else()
    message(STATUS "DAL: Vulkan SDK not found; shader compilation is unavailable")
  endif()
endfunction()

# The renderer library. dal_runtime_core stays GPU-free: only targets that link dal_render see
# Vulkan. Call after dal_runtime_add_core() and enable_testing().
function(dal_render_add)
  dal_render_add_libraries()
  dal_render_report_sdk()

  add_library(dal_render STATIC runtime/src/render/vk_context.cpp)
  target_compile_options(dal_render PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_render PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  target_include_directories(dal_render SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/externals/VulkanMemoryAllocator/include)
  # The allocator resolves its Vulkan entry points through volk's vkGet*ProcAddr; it must see the
  # same settings in every translation unit that includes its header.
  target_compile_definitions(dal_render PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1003000)
  target_link_libraries(dal_render PUBLIC volk dal_runtime_core)

  add_executable(dal_render_tests runtime/tests/render_test.cpp)
  target_compile_options(dal_render_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_render_tests PRIVATE dal_render)

  add_test(NAME render_core COMMAND dal_render_tests)
  set_tests_properties(render_core PROPERTIES
    PASS_REGULAR_EXPRESSION
      "render tests: [0-9]+ passed, 0 failed|render tests: skipped: no Vulkan device")

  # Replays a capture without the game; for now it prints the command list from the frame builder.
  add_executable(dal_rsxreplay tools/dal_rsxreplay/main.cpp)
  target_compile_options(dal_rsxreplay PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_rsxreplay PRIVATE dal_runtime_core)

  include("${CMAKE_SOURCE_DIR}/cmake/rsx_replay.cmake")
  set(replay_capture
    "${CMAKE_SOURCE_DIR}/generated/rsx/captures/${DAL_RSX_REPLAY_CAPTURE}.dalcap")
  if(EXISTS "${replay_capture}")
    add_test(NAME rsx_replay
      COMMAND dal_rsxreplay "${replay_capture}" --out "${CMAKE_BINARY_DIR}/replay")
    set_tests_properties(rsx_replay PROPERTIES
      PASS_REGULAR_EXPRESSION "replay: [0-9]+ commands, [0-9]+ draws, [0-9]+ skipped")
  else()
    message(STATUS "DAL: rsx_replay test not registered "
      "(generated/rsx/captures/${DAL_RSX_REPLAY_CAPTURE}.dalcap not found)")
  endif()
endfunction()

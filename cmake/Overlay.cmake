include_guard(GLOBAL)

# Dear ImGui comes from a pinned submodule and is built from its core files plus the Win32 and Vulkan
# backends (no demo window). Vulkan entry points resolve through volk, so the backend must not
# include <vulkan/vulkan.h> prototypes. Nothing is fetched or generated at configure time.
function(dal_overlay_add_imgui)
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/externals/imgui/LICENSE.txt)
    message(FATAL_ERROR
      "DAL: externals/imgui is missing or empty. Run: git submodule update --init")
  endif()

  set(imgui_dir ${CMAKE_SOURCE_DIR}/externals/imgui)
  add_library(imgui STATIC
    ${imgui_dir}/imgui.cpp
    ${imgui_dir}/imgui_draw.cpp
    ${imgui_dir}/imgui_tables.cpp
    ${imgui_dir}/imgui_widgets.cpp
    ${imgui_dir}/backends/imgui_impl_win32.cpp
    ${imgui_dir}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_dir} ${imgui_dir}/backends)
  # The game window has no IME and the overlay takes no gamepad input.
  target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)
  target_compile_options(imgui PRIVATE -march=x86-64-v3)
  target_link_libraries(imgui PUBLIC volk PRIVATE dwmapi gdi32)
endfunction()

# The overlay libraries. Call after dal_render_add(), which defines volk and dal_render and enables
# testing. dal_render stays free of ImGui; only this library will link it.
function(dal_overlay_add)
  dal_overlay_add_imgui()

  add_library(dal_overlay STATIC
    runtime/src/overlay/overlay_model.cpp)
  target_compile_options(dal_overlay PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_include_directories(dal_overlay PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  target_link_libraries(dal_overlay PUBLIC dal_render)

  # Needs no GPU for the model tests; the Vulkan parts skip without a device.
  add_executable(dal_overlay_tests runtime/tests/overlay_test.cpp)
  target_compile_options(dal_overlay_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_overlay_tests PRIVATE dal_overlay)

  add_test(NAME overlay_core COMMAND dal_overlay_tests)
  set_tests_properties(overlay_core PROPERTIES
    PASS_REGULAR_EXPRESSION "overlay tests: [0-9]+ passed, 0 failed")
endfunction()

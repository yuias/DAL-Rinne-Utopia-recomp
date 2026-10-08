include_guard(GLOBAL)

# Audio tools and tests. The audio library code itself lives in dal_runtime_core, which stays free
# of Windows audio and FFmpeg headers. Call after dal_runtime_add_core() and enable_testing().
function(dal_audio_add)
  add_executable(dal_audio_core_tests runtime/tests/audio_core_test.cpp)
  target_compile_options(dal_audio_core_tests PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_audio_core_tests PRIVATE dal_runtime_core)

  add_test(NAME audio_core COMMAND dal_audio_core_tests)
  set_tests_properties(audio_core PROPERTIES
    PASS_REGULAR_EXPRESSION "audio core tests: [0-9]+ passed, 0 failed")

  add_executable(dal_audiotool tools/dal_audiotool/main.cpp)
  target_compile_options(dal_audiotool PRIVATE -march=x86-64-v3 -Wall -Wextra)
  target_link_libraries(dal_audiotool PRIVATE dal_runtime_core)
endfunction()

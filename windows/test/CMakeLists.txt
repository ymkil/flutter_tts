cmake_minimum_required(VERSION 3.14)
project(flutter_tts_windows_tests LANGUAGES CXX)

if(NOT FLUTTER_EPHEMERAL_DIR OR NOT CPPWINRT_INCLUDE_DIR)
  message(FATAL_ERROR "Set FLUTTER_EPHEMERAL_DIR and CPPWINRT_INCLUDE_DIR from a Windows Flutter build")
endif()

set(WRAPPER_DIR "${FLUTTER_EPHEMERAL_DIR}/cpp_client_wrapper")
add_executable(flutter_tts_startup_test
  flutter_tts_startup_test.cpp
  "${WRAPPER_DIR}/core_implementations.cc"
  "${WRAPPER_DIR}/standard_codec.cc"
  "${WRAPPER_DIR}/plugin_registrar.cc"
)
target_compile_features(flutter_tts_startup_test PRIVATE cxx_std_20)
target_compile_definitions(flutter_tts_startup_test PRIVATE
  FLUTTER_PLUGIN_IMPL _HAS_EXCEPTIONS=0 UNICODE _UNICODE)
target_compile_options(flutter_tts_startup_test PRIVATE /W4 /WX /wd4100 /EHsc /utf-8)
target_include_directories(flutter_tts_startup_test PRIVATE
  "${FLUTTER_EPHEMERAL_DIR}" "${WRAPPER_DIR}/include" "${CPPWINRT_INCLUDE_DIR}")
target_link_libraries(flutter_tts_startup_test PRIVATE
  "${FLUTTER_EPHEMERAL_DIR}/flutter_windows.dll.lib" windowsapp)
add_custom_command(TARGET flutter_tts_startup_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FLUTTER_EPHEMERAL_DIR}/flutter_windows.dll"
    "$<TARGET_FILE_DIR:flutter_tts_startup_test>")

enable_testing()
set(SCENARIOS startup controls voice_query synthesis speech_failure media_failure player_stop)
foreach(scenario ${SCENARIOS})
  add_test(NAME flutter_tts_${scenario} COMMAND flutter_tts_startup_test ${scenario})
endforeach()

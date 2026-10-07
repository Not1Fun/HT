# @brief 真实面板服务与真实DAC请求队列的主机集成测试。
cmake_minimum_required(VERSION 3.20)
if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(screen_panel_tests LANGUAGES C)
enable_testing()
add_executable(screen_panel_tests test_screen_panel.c ../../src/app/dds_bench.c
    ../../src/ui/panel.c ../../src/ui/encoder.c ../../src/ui/dgus.c
    ../../src/ui/view.c ../../src/core/event_log.c)
target_include_directories(screen_panel_tests PRIVATE stubs ../../src)
target_compile_definitions(screen_panel_tests PRIVATE CONFIG_HT_SCREEN_TEMPERATURE=1 CONFIG_HT_DDS_BENCH=1)
set_target_properties(screen_panel_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(screen_panel_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS startup three_fields start_parameters stop_confirm frequency_confirm
    power_confirm power_cancel power_bounds
    stop_navigation_right stop_navigation_left permission_io permission_inputs permission_oc permission_ov
    permission_screen permission_deadline permission_temperature_inactive permission_temperature_unseen
    permission_temperature_stale display_error configure_failure false_running expiry)
  add_test(NAME screen_panel_${case} COMMAND screen_panel_tests ${case})
endforeach()

add_executable(screen_output_tests test_screen_output.c
    ../../src/ui/panel.c ../../src/ui/encoder.c ../../src/ui/dgus.c
    ../../src/ui/view.c ../../src/core/event_log.c)
target_include_directories(screen_output_tests PRIVATE stubs ../../src)
target_compile_definitions(screen_output_tests PRIVATE CONFIG_HT_OUTPUT=1)
set_target_properties(screen_output_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(screen_output_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS standby measurement_age output_states)
  add_test(NAME screen_output_${case} COMMAND screen_output_tests ${case})
endforeach()

add_executable(screen_debug_tests test_screen_output.c
    ../../src/ui/panel.c ../../src/ui/encoder.c ../../src/ui/dgus.c
    ../../src/ui/view.c ../../src/core/event_log.c)
target_include_directories(screen_debug_tests PRIVATE stubs ../../src)
target_compile_definitions(screen_debug_tests PRIVATE CONFIG_HT_OUTPUT=1 CONFIG_HT_OUTPUT_BENCH=1)
set_target_properties(screen_debug_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(screen_debug_tests PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME screen_debug_keys COMMAND screen_debug_tests debug_keys)

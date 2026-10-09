cmake_minimum_required(VERSION 3.20)
project(output_tests C)
enable_testing()
foreach(header IN ITEMS zephyr/drivers/gpio.h zephyr/drivers/watchdog.h zephyr/kernel.h zephyr/sys/util.h)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}" CONTENT "#include \"mock.h\"\n")
endforeach()
foreach(mode IN ITEMS output output_bench)
  add_executable(${mode}_tests test_output.c ../../src/core/power.c ../../src/core/bench.c)
  target_include_directories(${mode}_tests PRIVATE . ../../src "${CMAKE_CURRENT_BINARY_DIR}/stubs")
  target_compile_definitions(${mode}_tests PRIVATE CONFIG_HT_OUTPUT=1)
  set_target_properties(${mode}_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${mode}_tests PRIVATE -Wall -Wextra -Werror -pedantic)
  endif()
  foreach(case IN ITEMS manual_range blocking_reasons startup_latch latch_failure latch_relay_failure stop_priority interlocks stale_ui sample_fault standby_sampling standby_board_fault init_failure
      sample_permission pending_sample_invalid first_window_grace
      protection_race stop_in_flight relay_fault shutdown_output enclosure_interlocks
      running_oc running_ov running_sensor_loss running_temperature running_ntc_failure
      running_enable_release protection_irq)
    add_test(NAME ${mode}_${case} COMMAND ${mode}_tests ${case})
  endforeach()
endforeach()
target_compile_definitions(output_bench_tests PRIVATE CONFIG_HT_OUTPUT_BENCH=1)

foreach(case IN ITEMS debug_cycle debug_fault debug_disconnect dac_open_ntc dac_guards dac_relay_guard)
  add_test(NAME output_bench_${case} COMMAND output_bench_tests ${case})
endforeach()

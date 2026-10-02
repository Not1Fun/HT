# @brief 真实DDS驱动的寄存器边界与故障注入，不代替示波器验收。
cmake_minimum_required(VERSION 3.20)
if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(dds_io_tests LANGUAGES C)
enable_testing()
foreach(header IN ITEMS soc.h stm32_ll_bus.h stm32_ll_dac.h stm32_ll_dma.h
    stm32_ll_gpio.h stm32_ll_rcc.h stm32_ll_system.h stm32_ll_tim.h
    zephyr/devicetree.h zephyr/irq.h zephyr/kernel.h zephyr/sys/util.h)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}" CONTENT "#include \"mock.h\"\n")
endforeach()

add_executable(dds_output_tests test_dds_io.c ../../src/core/dds.c)
target_compile_definitions(dds_output_tests PRIVATE CONFIG_HT_OUTPUT=1)
target_include_directories(dds_output_tests PRIVATE . ../../src "${CMAKE_CURRENT_BINARY_DIR}/stubs")
set_target_properties(dds_output_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dds_output_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS dynamic_amplitude dynamic_deadline dynamic_fault start_stop startup_cancel)
  add_test(NAME dds_output_${case} COMMAND dds_output_tests ${case})
endforeach()
add_executable(dds_io_tests test_dds_io.c ../../src/core/dds.c)
target_include_directories(dds_io_tests PRIVATE . ../../src "${CMAKE_CURRENT_BINARY_DIR}/stubs")
set_target_properties(dds_io_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dds_io_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS init_idle input_guards resource_conflicts reference_guards
    start_stop frequency_restart dma_fault dac_fault startup_error stop_timeout
    resource_loss clock_loss external_fault startup_cancel adc_unchanged)
  add_test(NAME dds_io_${case} COMMAND dds_io_tests ${case})
endforeach()

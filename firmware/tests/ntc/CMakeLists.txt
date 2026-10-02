cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(ntc_tests LANGUAGES C)
enable_testing()

add_executable(ntc_tests test_ntc.c ../../src/core/ntc.c)
target_include_directories(ntc_tests PRIVATE ../../src)
set_target_properties(ntc_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ntc_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS anchors interpolation limits nominal_adc reference_config
        sensor_errors adc_sweep invalid_args wide_arithmetic)
  add_test(NAME ntc_${case} COMMAND ntc_tests ${case})
endforeach()

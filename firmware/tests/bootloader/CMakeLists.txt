# @brief 单独验证引导安全钩子的故障锁定和跳转前检查。
cmake_minimum_required(VERSION 3.20)
if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(boot_safe_tests LANGUAGES C)
enable_testing()
add_executable(boot_safe_tests test_safe.c ../../bootloader/src/safe.c)
target_include_directories(boot_safe_tests PRIVATE stubs ../../src)
target_compile_definitions(boot_safe_tests PRIVATE CONFIG_HT_SCREEN_BRINGUP=1)
set_target_properties(boot_safe_tests PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(boot_safe_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS startup board_failure relay_failure periodic_failure
    final_failure serial_failure unready_final)
  add_test(NAME boot_safe_${case} COMMAND boot_safe_tests ${case})
endforeach()

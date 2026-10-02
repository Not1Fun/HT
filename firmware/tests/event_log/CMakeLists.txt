# @brief 构建事件日志的独立主机测试。
cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(event_log_tests LANGUAGES C)
enable_testing()

add_executable(event_log_tests test_event_log.c ../../src/core/event_log.c)
target_include_directories(event_log_tests PRIVATE ../../src/core)
set_target_properties(event_log_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(event_log_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS initialization null_arguments not_ready bounds all_kinds
        value_limits capacity_and_wrap timestamp_order independent_instances
        failure_unchanged duplicate_events)
    add_test(NAME event_log_${case} COMMAND event_log_tests ${case})
endforeach()

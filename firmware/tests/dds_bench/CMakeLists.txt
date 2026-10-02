# @brief 真实台架服务的请求、许可、限时及并发停止故障注入。
cmake_minimum_required(VERSION 3.20)
if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(dds_bench_tests LANGUAGES C)
enable_testing()
add_executable(dds_bench_tests test_dds_bench.c ../../src/app/dds_bench.c)
target_include_directories(dds_bench_tests PRIVATE stubs ../../src)
set_target_properties(dds_bench_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dds_bench_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(case IN ITEMS defaults invalid_requests shared_queue actual_start expiry
    permission_loss stop_dequeue stop_configure stop_start stop_snapshot stop_commit stop_barrier
    init_failure configure_failure start_failure check_failure snapshot_failure
    unexpected_state running_drift stop_failure shell_input)
  add_test(NAME dds_bench_${case} COMMAND dds_bench_tests ${case})
endforeach()

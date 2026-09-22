cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(tca9539_tests LANGUAGES C)
enable_testing()

add_executable(tca9539_tests
    test_tca9539.c
    ../../src/drivers/tca9539_safe.c
)
target_include_directories(tca9539_tests PRIVATE ../../src/drivers)
set_target_properties(tca9539_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tca9539_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS init_order init_io_failure init_verify_failure
        verify_off verify_io_failure verify_mismatch panel invalid_arguments)
    add_test(NAME tca9539_${case} COMMAND tca9539_tests ${case})
endforeach()

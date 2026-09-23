cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(view_tests LANGUAGES C)
enable_testing()

add_executable(view_tests test_view.c ../../src/ui/view.c ../../src/ui/dgus.c)
target_include_directories(view_tests PRIVATE ../../src/ui)
set_target_properties(view_tests PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(view_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS mapping_sequence formatting field_widths invalid_readings states_animations
        crc send_failure invalid_args full_refresh continuous_refresh transitions
        incremental_failure reset_instances request_frequency startup_steps)
    add_test(NAME view_${case} COMMAND view_tests ${case})
endforeach()

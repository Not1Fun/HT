cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(panel_tests LANGUAGES C)
enable_testing()

add_executable(panel_tests test_panel.c ../../src/ui/panel.c)
target_include_directories(panel_tests PRIVATE ../../src/ui)
set_target_properties(panel_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(panel_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS defaults selection rotation presets range
        startup_enabled zero_target fault_rearm stop_priority
        mode_change_stop running_zero_stop invalid_config invalid_inputs extremes)
    add_test(NAME panel_${case} COMMAND panel_tests ${case})
endforeach()

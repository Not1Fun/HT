cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(encoder_tests LANGUAGES C)
enable_testing()

add_executable(encoder_tests test_encoder.c ../../src/ui/encoder.c)
target_include_directories(encoder_tests PRIVATE ../../src/ui)
set_target_properties(encoder_tests PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(encoder_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS directions initial_states partials bounce reversal invalid_transition
        resync instances invalid_args)
    add_test(NAME encoder_${case} COMMAND encoder_tests ${case})
endforeach()

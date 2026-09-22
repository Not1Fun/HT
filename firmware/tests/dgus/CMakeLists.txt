cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()

project(dgus_tests LANGUAGES C)
enable_testing()

add_executable(dgus_tests test_dgus.c ../../src/ui/dgus.c)
target_include_directories(dgus_tests PRIVATE ../../src/ui)
set_target_properties(dgus_tests PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dgus_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

foreach(case IN ITEMS golden crc_vectors boundaries malformed split_and_join
        resync timeout corruption bounded_noise)
    add_test(NAME dgus_${case} COMMAND dgus_tests ${case})
endforeach()

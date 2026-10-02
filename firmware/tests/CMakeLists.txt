cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{HOST_CC} AND NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "$ENV{HOST_CC}" CACHE FILEPATH "Host C compiler")
endif()
project(ht_host_tests LANGUAGES C)
enable_testing()

add_subdirectory(tca9539)
add_subdirectory(panel)
add_subdirectory(dgus)
add_subdirectory(view)
add_subdirectory(encoder)
add_subdirectory(event_log)
add_subdirectory(ntc)
add_subdirectory(bootloader)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME code_graph_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
  -s ${CMAKE_CURRENT_SOURCE_DIR}/code_graph -v)
add_test(NAME code_graph_sync COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../tools/code_graph.py --check)
add_test(NAME dwin_download_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
  -s ${CMAKE_CURRENT_SOURCE_DIR}/../../hmi/dwin/tests -v)
add_test(NAME uart_update_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
  -s ${CMAKE_CURRENT_SOURCE_DIR}/uart_update -v)

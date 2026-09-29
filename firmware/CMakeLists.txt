cmake_minimum_required(VERSION 3.28.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_firmware LANGUAGES C)

add_custom_target(code_graph_check ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/code_graph.py --check
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
  COMMENT "Checking source graph synchronization"
)
add_dependencies(app code_graph_check)

target_sources(app PRIVATE
  src/main.c
  src/platform/board_io.c
  src/platform/relay_io.c
  src/drivers/tca9539_safe.c
  src/ui/panel.c
  src/ui/dgus.c
  src/ui/view.c
  src/ui/encoder.c
)
target_include_directories(app PRIVATE src)
target_compile_options(app PRIVATE -Wall -Wextra -Werror)

target_sources_ifdef(CONFIG_HT_SCREEN_BRINGUP app PRIVATE src/platform/screen_uart.c)
target_sources_ifdef(CONFIG_HT_SCREEN_BRIDGE app PRIVATE src/app/screen_bridge.c)
target_sources_ifdef(CONFIG_HT_SCREEN_PANEL app PRIVATE src/app/screen_panel.c)

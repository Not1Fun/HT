if(CONFIG_MCUBOOT)
  target_sources(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/safe.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/platform/board_io.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/platform/relay_io.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/drivers/tca9539_safe.c
  )
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
  target_compile_definitions(app PRIVATE CONFIG_HT_SCREEN_BRINGUP=1)
endif()

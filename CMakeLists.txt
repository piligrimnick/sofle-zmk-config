# Sofle-specific runtime additions for ZMK.

if(CONFIG_ZMK_RGB_UNDERGLOW)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/sofle_rgb_status.c)

  # Redirect ZMK application writes to the LED strip through the status module.
  # Zephyr's strip driver itself is a separate target and is not intercepted.
  target_compile_options(
    app PRIVATE
    "-include" "${CMAKE_CURRENT_LIST_DIR}/include/sofle_rgb_status_intercept.h"
  )
endif()

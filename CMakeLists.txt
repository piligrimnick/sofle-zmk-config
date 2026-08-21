# Sofle-specific runtime additions for ZMK.

if(CONFIG_ZMK_RGB_UNDERGLOW)
  target_sources(
    app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/sofle_rgb_profiles.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sofle_rgb_status.c
  )

  # Redirect ZMK application writes to the LED strip through the status module.
  # Zephyr's strip driver itself is a separate target and is not intercepted.
  target_compile_options(
    app PRIVATE
    "-include" "${CMAKE_CURRENT_LIST_DIR}/include/sofle_rgb_status_intercept.h"
  )

  # Observe every user-facing RGB change without replacing ZMK's renderer.
  # Calls made internally by rgb_underglow.c (for example idle auto-off) stay
  # untouched and therefore do not overwrite the selected profile's setting.
  zephyr_ld_options(
    "-Wl,--wrap=zmk_rgb_underglow_on"
    "-Wl,--wrap=zmk_rgb_underglow_off"
    "-Wl,--wrap=zmk_rgb_underglow_select_effect"
    "-Wl,--wrap=zmk_rgb_underglow_set_hsb"
    "-Wl,--wrap=zmk_rgb_underglow_change_spd"
  )
endif()

if(CONFIG_ZMK_BATTERY_REPORTING)
  target_sources(
    app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/sofle_battery_filter.c
  )

  # Smooth the voltage before ZMK converts it to a reported percentage.
  zephyr_ld_options("-Wl,--wrap=lithium_ion_mv_to_pct")
endif()

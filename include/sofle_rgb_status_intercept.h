/*
 * Compile-time interception for LED-strip writes made by the ZMK app target.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Load Zephyr's inline implementation under a private name first. */
#define led_strip_update_rgb sofle_rgb_status_original_update_rgb
#include <zephyr/drivers/led_strip.h>
#undef led_strip_update_rgb

int sofle_rgb_status_intercept(const struct device *dev, struct led_rgb *pixels,
                               size_t num_pixels);

/* Redirect ordinary application writes; the status module uses the driver's
 * update_rgb function directly for its own frames. */
#define led_strip_update_rgb sofle_rgb_status_intercept

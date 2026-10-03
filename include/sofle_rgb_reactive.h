/*
 * Reactive per-key RGB effect for Sofle Choc V3.5.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

#define SOFLE_RGB_REACTIVE_EFFECT 4
#define SOFLE_RGB_EFFECT_COUNT 5

int sofle_rgb_reactive_select_effect(int effect);
bool sofle_rgb_reactive_render(struct led_rgb *pixels, size_t num_pixels);

/* Index in this half's LED chain for a key position, or -1 if the key has no
 * LED on this half. */
int sofle_rgb_position_to_led(uint32_t position);

uint8_t sofle_rgb_profile_brightness(void);

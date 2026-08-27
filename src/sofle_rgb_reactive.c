/*
 * Reactive per-key RGB effect for the Sofle Choc V3.5 LED chain.
 *
 * A pressed key keeps a random, fully saturated color for 500 ms, then fades
 * to black over 750 ms. Each half handles only its own physical key events,
 * so the animation adds no split-Bluetooth traffic.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/rgb_underglow.h>

#include "sofle_rgb_reactive.h"

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "Sofle reactive RGB requires a zmk,underglow chosen node"
#endif

#define SOFLE_RGB_STRIP DT_CHOSEN(zmk_underglow)
#define SOFLE_RGB_PIXEL_COUNT DT_PROP(SOFLE_RGB_STRIP, chain_length)

#define REACTIVE_HOLD_MS 500
#define REACTIVE_FADE_MS 750
#define REACTIVE_TOTAL_MS (REACTIVE_HOLD_MS + REACTIVE_FADE_MS)

BUILD_ASSERT(SOFLE_RGB_PIXEL_COUNT == 29,
             "The reactive key map expects exactly 29 LEDs per half");

/* LED order follows the PCB's daisy chain: it starts at the inner top key,
 * runs down that column and then snakes outward. The encoder-switch position
 * has no LED and is intentionally absent. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const uint8_t local_key_positions[SOFLE_RGB_PIXEL_COUNT] = {
    6, 18, 30, 44, 55, 56, 57, 45, 31, 19, 7,  8,  20, 32, 46,
    58, 59, 47, 33, 21, 9,  10, 22, 34, 48, 49, 35, 23, 11,
};
#else
static const uint8_t local_key_positions[SOFLE_RGB_PIXEL_COUNT] = {
    5, 17, 29, 41, 54, 53, 52, 40, 28, 16, 4,  3,  15, 27, 39,
    51, 50, 38, 26, 14, 2,  1,  13, 25, 37, 36, 24, 12, 0,
};
#endif

struct reactive_pixel {
    int64_t started_at;
    uint16_t hue;
    bool active;
};

static struct reactive_pixel reactive_pixels[SOFLE_RGB_PIXEL_COUNT];
static struct k_spinlock reactive_lock;
static atomic_t reactive_active = ATOMIC_INIT(false);

int __real_zmk_rgb_underglow_calc_effect(int direction);
int __real_zmk_rgb_underglow_select_effect(int effect);

static int position_to_led(uint32_t position) {
    for (int i = 0; i < ARRAY_SIZE(local_key_positions); i++) {
        if (local_key_positions[i] == position) {
            return i;
        }
    }

    return -1;
}

static struct led_rgb hue_to_rgb(uint16_t hue, uint8_t value) {
    uint8_t region = (hue / 60U) % 6U;
    uint8_t remainder = (hue % 60U) * 255U / 60U;
    uint8_t rising = (uint16_t)value * remainder / 255U;
    uint8_t falling = (uint16_t)value * (255U - remainder) / 255U;

    switch (region) {
    case 0:
        return (struct led_rgb){.r = value, .g = rising, .b = 0};
    case 1:
        return (struct led_rgb){.r = falling, .g = value, .b = 0};
    case 2:
        return (struct led_rgb){.r = 0, .g = value, .b = rising};
    case 3:
        return (struct led_rgb){.r = 0, .g = falling, .b = value};
    case 4:
        return (struct led_rgb){.r = rising, .g = 0, .b = value};
    default:
        return (struct led_rgb){.r = value, .g = 0, .b = falling};
    }
}

int sofle_rgb_reactive_select_effect(int effect) {
    if (effect < 0 || effect >= SOFLE_RGB_EFFECT_COUNT) {
        return -EINVAL;
    }

    if (effect == SOFLE_RGB_REACTIVE_EFFECT) {
        k_spinlock_key_t key = k_spin_lock(&reactive_lock);

        memset(reactive_pixels, 0, sizeof(reactive_pixels));
        atomic_set(&reactive_active, true);
        k_spin_unlock(&reactive_lock, key);
        return 0;
    }

    atomic_set(&reactive_active, false);
    return __real_zmk_rgb_underglow_select_effect(effect);
}

int __wrap_zmk_rgb_underglow_calc_effect(int direction) {
    int current = atomic_get(&reactive_active)
                      ? SOFLE_RGB_REACTIVE_EFFECT
                      : __real_zmk_rgb_underglow_calc_effect(0);

    return (current + SOFLE_RGB_EFFECT_COUNT + direction) % SOFLE_RGB_EFFECT_COUNT;
}

bool sofle_rgb_reactive_render(struct led_rgb *pixels, size_t num_pixels) {
    int64_t now;
    uint8_t maximum;
    k_spinlock_key_t key;

    if (!atomic_get(&reactive_active)) {
        return false;
    }

    now = k_uptime_get();
    maximum = (uint32_t)255U * sofle_rgb_profile_brightness() *
              CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / 10000U;
    key = k_spin_lock(&reactive_lock);

    for (int i = 0; i < num_pixels; i++) {
        struct reactive_pixel *pixel = &reactive_pixels[i];
        int64_t age;
        uint8_t level;

        pixels[i] = (struct led_rgb){0};
        if (!pixel->active) {
            continue;
        }

        age = now - pixel->started_at;
        if (age >= REACTIVE_TOTAL_MS) {
            pixel->active = false;
            continue;
        }

        if (age <= REACTIVE_HOLD_MS) {
            level = maximum;
        } else {
            level = (uint32_t)maximum * (REACTIVE_TOTAL_MS - age) /
                    REACTIVE_FADE_MS;
        }

        pixels[i] = hue_to_rgb(pixel->hue, level);
    }

    k_spin_unlock(&reactive_lock, key);
    return true;
}

static int reactive_position_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *changed =
        as_zmk_position_state_changed(event);
    bool rgb_on;
    int led;

    if (!changed || !changed->state ||
        changed->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL ||
        !atomic_get(&reactive_active) ||
        zmk_rgb_underglow_get_state(&rgb_on) != 0 || !rgb_on) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    led = position_to_led(changed->position);
    if (led >= 0) {
        uint16_t hue = sys_rand32_get() % 360U;
        k_spinlock_key_t key = k_spin_lock(&reactive_lock);

        reactive_pixels[led] = (struct reactive_pixel){
            .started_at = k_uptime_get(),
            .hue = hue,
            .active = true,
        };
        k_spin_unlock(&reactive_lock, key);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sofle_rgb_reactive, reactive_position_listener);
ZMK_SUBSCRIPTION(sofle_rgb_reactive, zmk_position_state_changed);

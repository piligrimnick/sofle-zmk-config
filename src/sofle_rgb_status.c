/*
 * Status animations for the Sofle Choc V3.5 per-key RGB chain.
 *
 * The normal ZMK underglow renderer remains responsible for user-selected
 * effects and persistence. While a status animation is active its writes are
 * suppressed, then allowed through again without modifying the saved state.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <drivers/ext_power.h>

#include <zmk/event_manager.h>
#include <zmk/rgb_underglow.h>

#if IS_ENABLED(CONFIG_ZMK_BLE) &&                                                        \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define SOFLE_RGB_HAS_HOST_BLE 1
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#else
#define SOFLE_RGB_HAS_HOST_BLE 0
#endif

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "Sofle RGB status animations require a zmk,underglow chosen node"
#endif

#define SOFLE_RGB_STRIP DT_CHOSEN(zmk_underglow)
#define SOFLE_RGB_PIXEL_COUNT DT_PROP(SOFLE_RGB_STRIP, chain_length)

#define STARTUP_DELAY_MS 200
#define STARTUP_FRAME_MS 100
#define STARTUP_FADE_STEPS 30
#define STARTUP_WHITE_10_PERCENT 26

#define BLE_BLUE_20_PERCENT 51
#define BLE_PAIR_FRAME_MS 120
#define BLE_CONNECTED_ON_MS 4000
#define BLE_CONNECTED_OFF_MS 80

enum sofle_rgb_status_mode {
    SOFLE_RGB_STATUS_NONE,
    SOFLE_RGB_STATUS_STARTUP,
    SOFLE_RGB_STATUS_PAIRING,
    SOFLE_RGB_STATUS_CONNECTED,
};

static const struct device *const status_strip = DEVICE_DT_GET(SOFLE_RGB_STRIP);
static struct led_rgb status_pixels[SOFLE_RGB_PIXEL_COUNT];
static atomic_t status_mode = ATOMIC_INIT(SOFLE_RGB_STATUS_STARTUP);
static uint8_t status_frame;
static bool restore_power;
static bool restore_power_valid;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const status_ext_power =
    DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

#if SOFLE_RGB_HAS_HOST_BLE
static bool previous_connected;
#endif

static void status_work_handler(struct k_work *work);
static struct k_work_delayable status_work;

static int status_raw_update(const struct device *dev, struct led_rgb *pixels,
                             size_t num_pixels) {
    const struct led_strip_driver_api *api =
        (const struct led_strip_driver_api *)dev->api;

    return api->update_rgb(dev, pixels, num_pixels);
}

int sofle_rgb_status_intercept(const struct device *dev, struct led_rgb *pixels,
                               size_t num_pixels) {
    if (atomic_get(&status_mode) != SOFLE_RGB_STATUS_NONE) {
        return 0;
    }

    return status_raw_update(dev, pixels, num_pixels);
}

static void status_fill(uint8_t red, uint8_t green, uint8_t blue) {
    for (int i = 0; i < SOFLE_RGB_PIXEL_COUNT; i++) {
        status_pixels[i] = (struct led_rgb){.r = red, .g = green, .b = blue};
    }
}

static void status_show(uint8_t red, uint8_t green, uint8_t blue) {
    status_fill(red, green, blue);
    status_raw_update(status_strip, status_pixels, SOFLE_RGB_PIXEL_COUNT);
}

static void status_capture_and_enable_power(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    int power_state = ext_power_get(status_ext_power);

    restore_power_valid = power_state >= 0;
    restore_power = power_state > 0;
    ext_power_enable(status_ext_power);
#else
    restore_power_valid = false;
    restore_power = true;
#endif
}

static void status_keep_power_enabled(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    /* ext_power_enable() schedules a settings save after 60 seconds. Pairing
     * can last longer, so keep rescheduling it until the original state is
     * restored instead of persisting this temporary power state. */
    ext_power_enable(status_ext_power);
#endif
}

static void status_restore_power(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    bool rgb_on;

    /* A Bluetooth profile can change while its status animation is running.
     * Restore the newly selected profile's live state, falling back to the
     * power state captured at animation start only if RGB is unavailable. */
    if (zmk_rgb_underglow_get_state(&rgb_on) != 0) {
        rgb_on = restore_power_valid ? restore_power : true;
    }

    if (rgb_on) {
        ext_power_enable(status_ext_power);
    } else {
        status_show(0, 0, 0);
        ext_power_disable(status_ext_power);
    }
#endif
}

static void status_finish(void) {
    atomic_set(&status_mode, SOFLE_RGB_STATUS_NONE);
    status_restore_power();
}

#if SOFLE_RGB_HAS_HOST_BLE
static void status_switch(enum sofle_rgb_status_mode mode) {
    enum sofle_rgb_status_mode current = atomic_get(&status_mode);

    /* Bluetooth events during the power-on animation are evaluated when the
     * fade finishes, so every power cycle always shows the complete fade. */
    if (current == SOFLE_RGB_STATUS_STARTUP) {
        return;
    }

    if (current == SOFLE_RGB_STATUS_NONE) {
        status_capture_and_enable_power();
    }

    atomic_set(&status_mode, mode);
    status_frame = 0;
    k_work_reschedule(&status_work, K_NO_WAIT);
}

static enum sofle_rgb_status_mode current_ble_status(void) {
    bool connected = zmk_ble_active_profile_is_connected();
    bool open = zmk_ble_active_profile_is_open();

    previous_connected = connected;

    if (connected) {
        return SOFLE_RGB_STATUS_CONNECTED;
    }

    if (open) {
        return SOFLE_RGB_STATUS_PAIRING;
    }

    return SOFLE_RGB_STATUS_NONE;
}
#endif

static void status_finish_startup(void) {
#if SOFLE_RGB_HAS_HOST_BLE
    enum sofle_rgb_status_mode next = current_ble_status();

    if (next != SOFLE_RGB_STATUS_NONE) {
        atomic_set(&status_mode, next);
        status_frame = 0;
        k_work_reschedule(&status_work, K_NO_WAIT);
        return;
    }
#endif

    status_finish();
}

static void status_work_handler(struct k_work *work) {
    enum sofle_rgb_status_mode mode = atomic_get(&status_mode);

    switch (mode) {
    case SOFLE_RGB_STATUS_STARTUP: {
        uint8_t level;

        if (status_frame == 0) {
            status_capture_and_enable_power();
        }

        if (status_frame <= STARTUP_FADE_STEPS) {
            level = STARTUP_WHITE_10_PERCENT * status_frame / STARTUP_FADE_STEPS;
        } else {
            level = STARTUP_WHITE_10_PERCENT *
                    (STARTUP_FADE_STEPS * 2 - status_frame) / STARTUP_FADE_STEPS;
        }

        status_show(level, level, level);

        if (status_frame++ < STARTUP_FADE_STEPS * 2) {
            k_work_reschedule(&status_work, K_MSEC(STARTUP_FRAME_MS));
        } else {
            status_finish_startup();
        }
        break;
    }

    case SOFLE_RGB_STATUS_PAIRING:
#if SOFLE_RGB_HAS_HOST_BLE
        if (zmk_ble_active_profile_is_connected()) {
            atomic_set(&status_mode, SOFLE_RGB_STATUS_CONNECTED);
            previous_connected = true;
            status_frame = 0;
            k_work_reschedule(&status_work, K_NO_WAIT);
            break;
        }

        if (!zmk_ble_active_profile_is_open()) {
            status_finish();
            break;
        }
#endif

        status_keep_power_enabled();
        if ((status_frame++ & 1U) == 0U) {
            status_show(0, 0, BLE_BLUE_20_PERCENT);
        } else {
            status_show(0, 0, 0);
        }
        k_work_reschedule(&status_work, K_MSEC(BLE_PAIR_FRAME_MS));
        break;

    case SOFLE_RGB_STATUS_CONNECTED:
        if (status_frame == 0) {
            status_show(0, 0, BLE_BLUE_20_PERCENT);
            status_frame++;
            k_work_reschedule(&status_work, K_MSEC(BLE_CONNECTED_ON_MS));
        } else if (status_frame == 1) {
            status_show(0, 0, 0);
            status_frame++;
            k_work_reschedule(&status_work, K_MSEC(BLE_CONNECTED_OFF_MS));
        } else {
            status_finish();
        }
        break;

    case SOFLE_RGB_STATUS_NONE:
        break;
    }
}

#if SOFLE_RGB_HAS_HOST_BLE
static int status_ble_listener(const zmk_event_t *event) {
    bool connected;
    bool open;
    bool newly_connected;

    if (!as_zmk_ble_active_profile_changed(event)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    connected = zmk_ble_active_profile_is_connected();
    open = zmk_ble_active_profile_is_open();
    newly_connected = connected && !previous_connected;
    previous_connected = connected;

    if (atomic_get(&status_mode) == SOFLE_RGB_STATUS_STARTUP) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (newly_connected) {
        status_switch(SOFLE_RGB_STATUS_CONNECTED);
    } else if (open) {
        status_switch(SOFLE_RGB_STATUS_PAIRING);
    } else if (atomic_get(&status_mode) == SOFLE_RGB_STATUS_PAIRING) {
        status_finish();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sofle_rgb_status, status_ble_listener);
ZMK_SUBSCRIPTION(sofle_rgb_status, zmk_ble_active_profile_changed);
#endif

static int sofle_rgb_status_init(void) {
    if (!device_is_ready(status_strip)) {
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(status_ext_power)) {
        return -ENODEV;
    }
#endif

    k_work_init_delayable(&status_work, status_work_handler);
    k_work_schedule(&status_work, K_MSEC(STARTUP_DELAY_MS));

    return 0;
}

/* Run immediately after ZMK's stock RGB initialization (priority 90). */
SYS_INIT(sofle_rgb_status_init, APPLICATION, 91);

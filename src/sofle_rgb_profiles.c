/*
 * Per-Bluetooth-profile RGB settings for Sofle Choc V3.5.
 *
 * ZMK v0.3 persists one global underglow state. This module keeps five small
 * profile records on each half and applies the matching record whenever the
 * central half changes Bluetooth profile. RGB commands are already global in
 * ZMK, so the same change is recorded independently on both halves.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_sofle_rgb_profile

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/rgb_underglow.h>

#if IS_ENABLED(CONFIG_ZMK_BLE) &&                                                        \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define SOFLE_RGB_PROFILE_CENTRAL 1
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#else
#define SOFLE_RGB_PROFILE_CENTRAL 0
#endif

#if !DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#error "Sofle RGB profiles require the rgb_prof behavior node"
#endif

#define SOFLE_RGB_PROFILE_COUNT 5
#define SOFLE_RGB_PROFILE_MAGIC 0x53465247U /* "SFRG" */
#define SOFLE_RGB_PROFILE_VERSION 1
#define SOFLE_RGB_EFFECT_COUNT 4
#define SOFLE_RGB_SPEED_MIN 1
#define SOFLE_RGB_SPEED_MAX 5
#define SOFLE_RGB_INIT_DELAY_MS 100
#define SOFLE_RGB_SYNC_RETRY_MS 750
#define SOFLE_RGB_SYNC_ATTEMPTS 8

#define SOFLE_RGB_PACK_HUE_SHIFT 0
#define SOFLE_RGB_PACK_SAT_SHIFT 9
#define SOFLE_RGB_PACK_BRT_SHIFT 16
#define SOFLE_RGB_PACK_SPEED_SHIFT 23
#define SOFLE_RGB_PACK_EFFECT_SHIFT 26
#define SOFLE_RGB_PACK_ON_SHIFT 28

struct sofle_rgb_profile {
    struct zmk_led_hsb color;
    uint8_t speed;
    uint8_t effect;
    uint8_t on;
    uint8_t reserved;
};

struct sofle_rgb_profile_store {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint8_t last_profile;
    uint8_t reserved;
    struct sofle_rgb_profile profiles[SOFLE_RGB_PROFILE_COUNT];
};

/* Layout of the stock ZMK v0.3 rgb/underglow/state setting. It is read only
 * during the one-time migration so the current global speed is not lost. */
struct zmk_v03_rgb_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

struct legacy_rgb_read_context {
    struct zmk_v03_rgb_state state;
    bool found;
};

static struct sofle_rgb_profile_store profile_store;
static uint8_t active_profile;
static uint8_t runtime_speed = CONFIG_ZMK_RGB_UNDERGLOW_SPD_START;
static bool profile_store_loaded;
static bool profiles_ready;
static bool applying_profile;

static void profile_save_work_handler(struct k_work *work);
static void profile_init_work_handler(struct k_work *work);
static struct k_work_delayable profile_save_work;
static struct k_work_delayable profile_init_work;

#if SOFLE_RGB_PROFILE_CENTRAL
static void profile_sync_work_handler(struct k_work *work);
static struct k_work_delayable profile_sync_work;
static uint8_t sync_attempts_remaining;
#endif

int __real_zmk_rgb_underglow_on(void);
int __real_zmk_rgb_underglow_off(void);
int __real_zmk_rgb_underglow_select_effect(int effect);
int __real_zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color);
int __real_zmk_rgb_underglow_change_spd(int direction);

static bool profile_is_valid(const struct sofle_rgb_profile *profile) {
    return profile->color.h <= 360 && profile->color.s <= 100 &&
           profile->color.b <= 100 && profile->speed >= SOFLE_RGB_SPEED_MIN &&
           profile->speed <= SOFLE_RGB_SPEED_MAX &&
           profile->effect < SOFLE_RGB_EFFECT_COUNT && profile->on <= 1;
}

#if SOFLE_RGB_PROFILE_CENTRAL
static uint32_t pack_profile(const struct sofle_rgb_profile *profile) {
    return ((uint32_t)profile->color.h << SOFLE_RGB_PACK_HUE_SHIFT) |
           ((uint32_t)profile->color.s << SOFLE_RGB_PACK_SAT_SHIFT) |
           ((uint32_t)profile->color.b << SOFLE_RGB_PACK_BRT_SHIFT) |
           ((uint32_t)profile->speed << SOFLE_RGB_PACK_SPEED_SHIFT) |
           ((uint32_t)profile->effect << SOFLE_RGB_PACK_EFFECT_SHIFT) |
           ((uint32_t)profile->on << SOFLE_RGB_PACK_ON_SHIFT);
}
#endif

static struct sofle_rgb_profile unpack_profile(uint32_t packed) {
    return (struct sofle_rgb_profile){
        .color = {
            .h = (packed >> SOFLE_RGB_PACK_HUE_SHIFT) & 0x1FF,
            .s = (packed >> SOFLE_RGB_PACK_SAT_SHIFT) & 0x7F,
            .b = (packed >> SOFLE_RGB_PACK_BRT_SHIFT) & 0x7F,
        },
        .speed = (packed >> SOFLE_RGB_PACK_SPEED_SHIFT) & 0x07,
        .effect = (packed >> SOFLE_RGB_PACK_EFFECT_SHIFT) & 0x03,
        .on = (packed >> SOFLE_RGB_PACK_ON_SHIFT) & 0x01,
    };
}

static bool store_is_valid(const struct sofle_rgb_profile_store *store) {
    if (store->magic != SOFLE_RGB_PROFILE_MAGIC ||
        store->version != SOFLE_RGB_PROFILE_VERSION ||
        store->count != SOFLE_RGB_PROFILE_COUNT ||
        store->last_profile >= SOFLE_RGB_PROFILE_COUNT) {
        return false;
    }

    for (int i = 0; i < SOFLE_RGB_PROFILE_COUNT; i++) {
        if (!profile_is_valid(&store->profiles[i])) {
            return false;
        }
    }

    return true;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int profile_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    const char *next;
    struct sofle_rgb_profile_store candidate;
    int rc;

    if (!settings_name_steq(name, "slots", &next) || next || len != sizeof(candidate)) {
        return -ENOENT;
    }

    rc = read_cb(cb_arg, &candidate, sizeof(candidate));
    if (rc != sizeof(candidate)) {
        return rc < 0 ? rc : -EIO;
    }

    if (!store_is_valid(&candidate)) {
        return -EINVAL;
    }

    profile_store = candidate;
    profile_store_loaded = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sofle_rgb_profiles, "sofle/rgb", NULL,
                               profile_settings_set, NULL, NULL);

static void schedule_profile_save(void) {
    if (profiles_ready && !applying_profile) {
        k_work_reschedule(&profile_save_work,
                          K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    }
}

static void profile_save_work_handler(struct k_work *work) {
    settings_save_one("sofle/rgb/slots", &profile_store, sizeof(profile_store));
}
#else
static void schedule_profile_save(void) {}
static void profile_save_work_handler(struct k_work *work) {}
#endif

static int legacy_rgb_state_read(const char *key, size_t len, settings_read_cb read_cb,
                                 void *cb_arg, void *param) {
    struct legacy_rgb_read_context *context = param;
    int rc;

    if (strcmp(key, "state") != 0 || len != sizeof(context->state)) {
        return 0;
    }

    rc = read_cb(cb_arg, &context->state, sizeof(context->state));
    if (rc != sizeof(context->state)) {
        return rc < 0 ? rc : -EIO;
    }

    context->found = true;
    return 1;
}

static struct sofle_rgb_profile capture_current_profile(void) {
    struct sofle_rgb_profile profile = {
        .color = zmk_rgb_underglow_calc_hue(0),
        .speed = CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        .effect = zmk_rgb_underglow_calc_effect(0),
        .on = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START),
    };
    bool on;

    if (zmk_rgb_underglow_get_state(&on) == 0) {
        profile.on = on;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    struct legacy_rgb_read_context legacy = {0};

    settings_load_subtree_direct("rgb/underglow", legacy_rgb_state_read, &legacy);
    if (legacy.found && legacy.state.animation_speed >= SOFLE_RGB_SPEED_MIN &&
        legacy.state.animation_speed <= SOFLE_RGB_SPEED_MAX) {
        profile.speed = legacy.state.animation_speed;
    }
#endif

    runtime_speed = profile.speed;
    return profile;
}

static int apply_profile(uint8_t index) {
    const struct sofle_rgb_profile *profile;
    int rc = 0;

    if (!profiles_ready || index >= SOFLE_RGB_PROFILE_COUNT) {
        return -EINVAL;
    }

    if (active_profile == index && profile_store.last_profile == index) {
        return 0;
    }

    active_profile = index;
    profile_store.last_profile = index;
    profile = &profile_store.profiles[index];
    applying_profile = true;

    rc = __real_zmk_rgb_underglow_set_hsb(profile->color);
    if (rc == 0) {
        rc = __real_zmk_rgb_underglow_select_effect(profile->effect);
    }

    while (rc == 0 && runtime_speed < profile->speed) {
        rc = __real_zmk_rgb_underglow_change_spd(1);
        if (rc == 0) {
            runtime_speed++;
        }
    }
    while (rc == 0 && runtime_speed > profile->speed) {
        rc = __real_zmk_rgb_underglow_change_spd(-1);
        if (rc == 0) {
            runtime_speed--;
        }
    }

    if (rc == 0) {
        rc = profile->on ? __real_zmk_rgb_underglow_on()
                         : __real_zmk_rgb_underglow_off();
    }

    applying_profile = false;
    schedule_profile_save();
    return rc;
}

static void migrate_or_activate_profiles(void) {
    struct sofle_rgb_profile current = capture_current_profile();
    uint8_t target;

    if (!profile_store_loaded) {
        profile_store = (struct sofle_rgb_profile_store){
            .magic = SOFLE_RGB_PROFILE_MAGIC,
            .version = SOFLE_RGB_PROFILE_VERSION,
            .count = SOFLE_RGB_PROFILE_COUNT,
            .last_profile = 0,
        };

        for (int i = 0; i < SOFLE_RGB_PROFILE_COUNT; i++) {
            profile_store.profiles[i] = current;
        }
    }

#if SOFLE_RGB_PROFILE_CENTRAL
    target = CLAMP(zmk_ble_active_profile_index(), 0, SOFLE_RGB_PROFILE_COUNT - 1);
#else
    target = profile_store.last_profile;
#endif

    /* Force the first application even when the selected slot is zero. */
    active_profile = UINT8_MAX;
    profiles_ready = true;
    apply_profile(target);

    if (!profile_store_loaded) {
        schedule_profile_save();
    }
}

static void profile_init_work_handler(struct k_work *work) {
    migrate_or_activate_profiles();

#if SOFLE_RGB_PROFILE_CENTRAL
    sync_attempts_remaining = SOFLE_RGB_SYNC_ATTEMPTS;
    k_work_reschedule(&profile_sync_work, K_NO_WAIT);
#endif
}

int __wrap_zmk_rgb_underglow_on(void) {
    int rc = __real_zmk_rgb_underglow_on();

    if (rc == 0 && profiles_ready && !applying_profile) {
        profile_store.profiles[active_profile].on = true;
        schedule_profile_save();
    }
    return rc;
}

int __wrap_zmk_rgb_underglow_off(void) {
    int rc = __real_zmk_rgb_underglow_off();

    if (rc == 0 && profiles_ready && !applying_profile) {
        profile_store.profiles[active_profile].on = false;
        schedule_profile_save();
    }
    return rc;
}

int __wrap_zmk_rgb_underglow_select_effect(int effect) {
    int rc = __real_zmk_rgb_underglow_select_effect(effect);

    if (rc == 0 && profiles_ready && !applying_profile) {
        profile_store.profiles[active_profile].effect = effect;
        schedule_profile_save();
    }
    return rc;
}

int __wrap_zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    int rc = __real_zmk_rgb_underglow_set_hsb(color);

    if (rc == 0 && profiles_ready && !applying_profile) {
        profile_store.profiles[active_profile].color = color;
        schedule_profile_save();
    }
    return rc;
}

int __wrap_zmk_rgb_underglow_change_spd(int direction) {
    int rc = __real_zmk_rgb_underglow_change_spd(direction);

    if (rc == 0 && profiles_ready && !applying_profile) {
        runtime_speed = CLAMP(runtime_speed + direction, SOFLE_RGB_SPEED_MIN,
                              SOFLE_RGB_SPEED_MAX);
        profile_store.profiles[active_profile].speed = runtime_speed;
        schedule_profile_save();
    }
    return rc;
}

static int profile_behavior_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    struct sofle_rgb_profile received;
    bool changed;

    if (binding->param1 >= SOFLE_RGB_PROFILE_COUNT) {
        return -EINVAL;
    }

    received = unpack_profile(binding->param2);
    if (!profile_is_valid(&received) || !profiles_ready) {
        return -EINVAL;
    }

    /* Carry the complete slot, not just its index. This lets a half that was
     * powered off during earlier RGB changes catch up when it reconnects. */
    changed = memcmp(&profile_store.profiles[binding->param1], &received,
                     sizeof(received)) != 0;
    profile_store.profiles[binding->param1] = received;
    schedule_profile_save();

    if (changed && active_profile == binding->param1) {
        /* The index did not change, but its remote value did. Force the new
         * color/effect/brightness into the live renderer. */
        active_profile = UINT8_MAX;
    }
    return apply_profile(binding->param1);
}

static int profile_behavior_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api profile_behavior_driver_api = {
    .binding_pressed = profile_behavior_pressed,
    .binding_released = profile_behavior_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &profile_behavior_driver_api);

#if SOFLE_RGB_PROFILE_CENTRAL
static void schedule_profile_sync(void) {
    sync_attempts_remaining = SOFLE_RGB_SYNC_ATTEMPTS;
    k_work_reschedule(&profile_sync_work, K_NO_WAIT);
}

static void profile_sync_work_handler(struct k_work *work) {
    const struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(rgb_prof)),
        .param1 = active_profile,
        .param2 = pack_profile(&profile_store.profiles[active_profile]),
    };
    const struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    if (!profiles_ready) {
        return;
    }

    zmk_behavior_invoke_binding(&binding, event, true);

    if (sync_attempts_remaining > 1) {
        sync_attempts_remaining--;
        k_work_reschedule(&profile_sync_work, K_MSEC(SOFLE_RGB_SYNC_RETRY_MS));
    } else {
        sync_attempts_remaining = 0;
    }
}

static int profile_ble_listener(const zmk_event_t *event) {
    const struct zmk_ble_active_profile_changed *changed =
        as_zmk_ble_active_profile_changed(event);

    if (!changed || changed->index >= SOFLE_RGB_PROFILE_COUNT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (profiles_ready) {
        apply_profile(changed->index);
        schedule_profile_sync();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sofle_rgb_profiles, profile_ble_listener);
ZMK_SUBSCRIPTION(sofle_rgb_profiles, zmk_ble_active_profile_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void profile_split_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err == 0 && bt_conn_get_info(conn, &info) == 0 &&
        info.role == BT_CONN_ROLE_CENTRAL && profiles_ready) {
        schedule_profile_sync();
    }
}

BT_CONN_CB_DEFINE(sofle_rgb_profile_conn_callbacks) = {
    .connected = profile_split_connected,
};
#endif
#endif

static int sofle_rgb_profiles_init(void) {
    k_work_init_delayable(&profile_save_work, profile_save_work_handler);
    k_work_init_delayable(&profile_init_work, profile_init_work_handler);
#if SOFLE_RGB_PROFILE_CENTRAL
    k_work_init_delayable(&profile_sync_work, profile_sync_work_handler);
#endif
    k_work_schedule(&profile_init_work, K_MSEC(SOFLE_RGB_INIT_DELAY_MS));
    return 0;
}

SYS_INIT(sofle_rgb_profiles_init, APPLICATION, 91);

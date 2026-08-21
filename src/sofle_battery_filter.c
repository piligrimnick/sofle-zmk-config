/*
 * Battery voltage smoothing for the Sofle Choc V3.5.
 *
 * A rolling median rejects one-sample voltage spikes caused by RGB load or
 * USB charging while retaining ZMK's normal battery percentage conversion.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

/* The wrapped function is provided by ZMK's battery_common driver helper. */
uint8_t __real_lithium_ion_mv_to_pct(int16_t battery_mv);

static int16_t samples_mv[3];
static uint8_t sample_count;
static uint8_t next_sample;

static int16_t median_of_three(int16_t a, int16_t b, int16_t c) {
    if (a > b) {
        int16_t tmp = a;
        a = b;
        b = tmp;
    }

    if (b > c) {
        b = c;
    }

    return a > b ? a : b;
}

uint8_t __wrap_lithium_ion_mv_to_pct(int16_t battery_mv) {
    samples_mv[next_sample] = battery_mv;
    next_sample = (next_sample + 1) % 3;

    if (sample_count < 3) {
        sample_count++;
    }

    /* Report boot immediately, then wait for two samples to confirm a change. */
    if (sample_count < 3) {
        return __real_lithium_ion_mv_to_pct(samples_mv[0]);
    }

    return __real_lithium_ion_mv_to_pct(
        median_of_three(samples_mv[0], samples_mv[1], samples_mv[2]));
}

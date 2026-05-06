/*
 * Minimal no-FEM MPSL shim for using Nordic ESB in ZMK without pulling all of NCS.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <nrf.h>

typedef enum {
    MPSL_FEM_PA = 1 << 0,
    MPSL_FEM_LNA = 1 << 1,
    MPSL_FEM_ALL = MPSL_FEM_PA | MPSL_FEM_LNA,
} mpsl_fem_functionality_t;

typedef enum {
    MPSL_FEM_EVENT_TYPE_TIMER,
    MPSL_FEM_EVENT_TYPE_GENERIC,
} mpsl_fem_event_type_t;

typedef uint32_t mpsl_subscribable_hw_event_t;
typedef int8_t mpsl_tx_power_t;

typedef struct {
    int8_t gain_db;
    uint8_t private_setting;
} mpsl_fem_gain_t;

typedef struct {
    mpsl_tx_power_t radio_tx_power;
    mpsl_fem_gain_t fem;
} mpsl_tx_power_split_t;

typedef struct {
    mpsl_fem_event_type_t type;
    union {
        struct {
            NRF_TIMER_Type *p_timer_instance;
            struct {
                uint32_t start;
                uint32_t end;
            } counter_period;
            uint8_t compare_channel_mask;
        } timer;
        struct {
            mpsl_subscribable_hw_event_t event;
        } generic;
    } event;
#if defined(NRF52_SERIES)
    bool override_ppi;
    uint8_t ppi_ch_id;
#endif
} mpsl_fem_event_t;

static inline int32_t mpsl_fem_disable(void) { return 0; }
static inline int32_t mpsl_fem_pa_configuration_set(const mpsl_fem_event_t *activate,
                                                    const mpsl_fem_event_t *deactivate) {
    ARG_UNUSED(activate);
    ARG_UNUSED(deactivate);
    return -1;
}
static inline int32_t mpsl_fem_pa_configuration_clear(void) { return 0; }
static inline int32_t mpsl_fem_lna_configuration_set(const mpsl_fem_event_t *activate,
                                                     const mpsl_fem_event_t *deactivate) {
    ARG_UNUSED(activate);
    ARG_UNUSED(deactivate);
    return -1;
}
static inline int32_t mpsl_fem_lna_configuration_clear(void) { return 0; }
static inline void mpsl_fem_deactivate_now(mpsl_fem_functionality_t type) { ARG_UNUSED(type); }
static inline void mpsl_fem_cleanup(void) {}
static inline void mpsl_fem_tx_power_split(const mpsl_tx_power_t power,
                                           mpsl_tx_power_split_t *const split, uint32_t frequency,
                                           bool is_high_voltage) {
    ARG_UNUSED(frequency);
    ARG_UNUSED(is_high_voltage);
    split->radio_tx_power = power;
    split->fem.gain_db = 0;
    split->fem.private_setting = 0;
}
static inline int32_t mpsl_fem_pa_gain_set(const mpsl_fem_gain_t *gain) {
    ARG_UNUSED(gain);
    return 0;
}
static inline void mpsl_fem_pa_is_configured(int8_t *const gain) { *gain = 0; }
static inline bool mpsl_fem_prepare_powerdown(NRF_TIMER_Type *timer, uint32_t compare_channel,
                                              uint32_t ppi_id, uint32_t event_addr) {
    ARG_UNUSED(timer);
    ARG_UNUSED(compare_channel);
    ARG_UNUSED(ppi_id);
    ARG_UNUSED(event_addr);
    return false;
}

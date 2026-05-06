/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <errno.h>

#include <esb.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/onoff.h>

static inline int zmk_split_esb_start_hf_clock(void) {
    int err;
    int res;
    struct onoff_manager *clk_mgr;
    struct onoff_client clk_cli;

    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!clk_mgr) {
        return -ENXIO;
    }

    sys_notify_init_spinwait(&clk_cli.notify);

    err = onoff_request(clk_mgr, &clk_cli);
    if (err < 0) {
        return err;
    }

    do {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res) {
            return res;
        }
    } while (err);

    return 0;
}

static inline int zmk_split_esb_configure_radio(enum esb_mode mode, esb_event_handler handler) {
    uint8_t base_addr_0[4] = {0x45, 0x53, 0x42, 0x5a};
    uint8_t base_addr_1[4] = {0x52, 0x4f, 0x42, 0x41};
    uint8_t prefixes[8] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};
    struct esb_config config = ESB_DEFAULT_CONFIG;
    int err;

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.mode = mode;
    config.event_handler = handler;
    config.bitrate = ESB_BITRATE_2MBPS;
    config.retransmit_delay = 600;
    config.retransmit_count = 3;
    config.selective_auto_ack = false;
    config.use_fast_ramp_up = true;

    err = esb_init(&config);
    if (err) {
        return err;
    }

    err = esb_set_base_address_0(base_addr_0);
    if (err) {
        return err;
    }

    err = esb_set_base_address_1(base_addr_1);
    if (err) {
        return err;
    }

    err = esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
    if (err) {
        return err;
    }

    return esb_set_rf_channel(CONFIG_ZMK_SPLIT_ESB_RF_CHANNEL);
}

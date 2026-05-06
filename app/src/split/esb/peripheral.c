/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include "common.h"
#include "packet.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= ZMK_SPLIT_ESB_PACKET_DATA_MAX);
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= ZMK_SPLIT_ESB_PACKET_DATA_MAX);

extern struct zmk_split_transport_peripheral esb_peripheral;

static bool enabled;
static bool initialized;
static bool tx_ready = true;
static int64_t last_success;

K_MSGQ_DEFINE(tx_msgq, sizeof(struct zmk_split_esb_packet), CONFIG_ZMK_SPLIT_ESB_EVENT_QUEUE_SIZE,
              4);
K_MSGQ_DEFINE(cmd_msgq, sizeof(struct zmk_split_transport_central_command),
              CONFIG_ZMK_SPLIT_ESB_COMMAND_QUEUE_SIZE, 4);

static void tx_work_cb(struct k_work *work);
static void cmd_work_cb(struct k_work *work);
static void poll_timer_cb(struct k_timer *timer);

static K_WORK_DEFINE(tx_work, tx_work_cb);
static K_WORK_DEFINE(cmd_work, cmd_work_cb);
static K_TIMER_DEFINE(poll_timer, poll_timer_cb, NULL);

static zmk_split_transport_peripheral_status_changed_cb_t transport_status_cb;

static void process_ack_payloads(void) {
    struct esb_payload payload;

    while (esb_read_rx_payload(&payload) == 0) {
        if (payload.length != sizeof(struct zmk_split_esb_packet)) {
            continue;
        }

        struct zmk_split_esb_packet packet;
        memcpy(&packet, payload.data, sizeof(packet));

        if (packet.magic != ZMK_SPLIT_ESB_MAGIC || packet.version != ZMK_SPLIT_ESB_VERSION ||
            packet.type != ZMK_SPLIT_ESB_PACKET_TYPE_COMMAND ||
            packet.source != CONFIG_ZMK_SPLIT_ESB_SOURCE_ID ||
            packet.len != sizeof(struct zmk_split_transport_central_command)) {
            continue;
        }

        struct zmk_split_transport_central_command cmd;
        memcpy(&cmd, packet.data, sizeof(cmd));
        if (k_msgq_put(&cmd_msgq, &cmd, K_NO_WAIT) == 0) {
            k_work_submit(&cmd_work);
        }
    }
}

static void esb_handler(const struct esb_evt *event) {
    switch (event->evt_id) {
    case ESB_EVENT_TX_SUCCESS:
        last_success = k_uptime_get();
        tx_ready = true;
        k_work_submit(&tx_work);
        break;
    case ESB_EVENT_TX_FAILED:
        tx_ready = true;
        k_work_submit(&tx_work);
        break;
    case ESB_EVENT_RX_RECEIVED:
        process_ack_payloads();
        break;
    }
}

static void tx_work_cb(struct k_work *work) {
    if (!enabled || !initialized || !tx_ready) {
        return;
    }

    struct zmk_split_esb_packet packet;
    if (k_msgq_get(&tx_msgq, &packet, K_NO_WAIT) != 0) {
        return;
    }

    struct esb_payload payload = {
        .pipe = CONFIG_ZMK_SPLIT_ESB_SOURCE_ID,
        .length = sizeof(packet),
        .noack = false,
    };
    memcpy(payload.data, &packet, sizeof(packet));

    tx_ready = false;
    esb_flush_tx();
    int err = esb_write_payload(&payload);
    if (err) {
        tx_ready = true;
        LOG_WRN("ESB payload write failed: %d", err);
        k_work_submit(&tx_work);
    }
}

static void cmd_work_cb(struct k_work *work) {
    struct zmk_split_transport_central_command cmd;

    while (k_msgq_get(&cmd_msgq, &cmd, K_NO_WAIT) == 0) {
        zmk_split_transport_peripheral_command_handler(&esb_peripheral, cmd);
    }
}

static void poll_timer_cb(struct k_timer *timer) {
    if (!enabled) {
        return;
    }

    struct zmk_split_esb_packet packet = {
        .magic = ZMK_SPLIT_ESB_MAGIC,
        .version = ZMK_SPLIT_ESB_VERSION,
        .type = ZMK_SPLIT_ESB_PACKET_TYPE_POLL,
        .source = CONFIG_ZMK_SPLIT_ESB_SOURCE_ID,
        .len = 0,
    };

    if (k_msgq_put(&tx_msgq, &packet, K_NO_WAIT) == 0) {
        k_work_submit(&tx_work);
    }
}

static int
esb_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    struct zmk_split_esb_packet packet = {
        .magic = ZMK_SPLIT_ESB_MAGIC,
        .version = ZMK_SPLIT_ESB_VERSION,
        .type = ZMK_SPLIT_ESB_PACKET_TYPE_EVENT,
        .source = CONFIG_ZMK_SPLIT_ESB_SOURCE_ID,
        .len = sizeof(*event),
    };
    memcpy(packet.data, event, sizeof(*event));

    int err = k_msgq_put(&tx_msgq, &packet, K_NO_WAIT);
    if (err) {
        return err;
    }

    k_work_submit(&tx_work);
    return 0;
}

static int esb_peripheral_set_enabled(bool en) {
    enabled = en;

    if (enabled) {
        k_timer_start(&poll_timer, K_NO_WAIT, K_MSEC(CONFIG_ZMK_SPLIT_ESB_POLL_INTERVAL_MS));
        k_work_submit(&tx_work);
    } else {
        k_timer_stop(&poll_timer);
    }

    return 0;
}

static struct zmk_split_transport_status esb_peripheral_get_status(void) {
    bool connected = last_success > 0 &&
                     k_uptime_get() - last_success < CONFIG_ZMK_SPLIT_ESB_CONNECTION_TIMEOUT_MS;

    return (struct zmk_split_transport_status){
        .available = initialized,
        .enabled = enabled,
        .connections = connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                 : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static int
esb_peripheral_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = esb_peripheral_report_event,
    .set_enabled = esb_peripheral_set_enabled,
    .get_status = esb_peripheral_get_status,
    .set_status_callback = esb_peripheral_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &peripheral_api,
                                        CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static int esb_peripheral_init(void) {
    int err = zmk_split_esb_start_hf_clock();
    if (err) {
        LOG_ERR("ESB HF clock start failed: %d", err);
        return err;
    }

    err = zmk_split_esb_configure_radio(ESB_MODE_PTX, esb_handler);
    if (err) {
        LOG_ERR("ESB peripheral init failed: %d", err);
        return err;
    }

    initialized = true;
    if (transport_status_cb) {
        transport_status_cb(&esb_peripheral, esb_peripheral_get_status());
    }

    return 0;
}

SYS_INIT(esb_peripheral_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

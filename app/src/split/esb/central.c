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
#include <zephyr/sys/util.h>

#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>

#include "common.h"
#include "packet.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct zmk_split_transport_peripheral_event) <= ZMK_SPLIT_ESB_PACKET_DATA_MAX);
BUILD_ASSERT(sizeof(struct zmk_split_transport_central_command) <= ZMK_SPLIT_ESB_PACKET_DATA_MAX);

extern struct zmk_split_transport_central esb_central;

static bool enabled;
static bool initialized;
static int64_t last_seen[CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS];
static struct zmk_split_transport_central_command
    pending_cmds[CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS];
static bool has_pending_cmd[CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS];

struct central_rx_item {
    uint8_t source;
    struct zmk_split_transport_peripheral_event event;
};

K_MSGQ_DEFINE(rx_msgq, sizeof(struct central_rx_item), CONFIG_ZMK_SPLIT_ESB_EVENT_QUEUE_SIZE, 4);

static void rx_work_cb(struct k_work *work);
static K_WORK_DEFINE(rx_work, rx_work_cb);

static zmk_split_transport_central_status_changed_cb_t transport_status_cb;

static int queue_ack_payload(uint8_t source) {
    if (source >= ARRAY_SIZE(pending_cmds) || !has_pending_cmd[source]) {
        return 0;
    }

    struct zmk_split_esb_packet packet = {
        .magic = ZMK_SPLIT_ESB_MAGIC,
        .version = ZMK_SPLIT_ESB_VERSION,
        .type = ZMK_SPLIT_ESB_PACKET_TYPE_COMMAND,
        .source = source,
        .len = sizeof(pending_cmds[source]),
    };
    memcpy(packet.data, &pending_cmds[source], packet.len);

    struct esb_payload payload = {
        .pipe = source,
        .length = sizeof(packet),
        .noack = false,
    };
    memcpy(payload.data, &packet, sizeof(packet));

    int err = esb_write_payload(&payload);
    if (err == 0) {
        has_pending_cmd[source] = false;
    }

    return err;
}

static void esb_handler(const struct esb_evt *event) {
    if (event->evt_id != ESB_EVENT_RX_RECEIVED) {
        return;
    }

    struct esb_payload payload;
    while (esb_read_rx_payload(&payload) == 0) {
        if (payload.length != sizeof(struct zmk_split_esb_packet)) {
            continue;
        }

        struct zmk_split_esb_packet packet;
        memcpy(&packet, payload.data, sizeof(packet));

        if (packet.magic != ZMK_SPLIT_ESB_MAGIC || packet.version != ZMK_SPLIT_ESB_VERSION ||
            packet.source >= ARRAY_SIZE(last_seen) || payload.pipe != packet.source) {
            continue;
        }

        last_seen[packet.source] = k_uptime_get();
        queue_ack_payload(packet.source);

        if (packet.type != ZMK_SPLIT_ESB_PACKET_TYPE_EVENT ||
            packet.len != sizeof(struct zmk_split_transport_peripheral_event)) {
            continue;
        }

        struct central_rx_item item = {.source = packet.source};
        memcpy(&item.event, packet.data, sizeof(item.event));
        if (k_msgq_put(&rx_msgq, &item, K_NO_WAIT) == 0) {
            k_work_submit(&rx_work);
        }
    }
}

static void rx_work_cb(struct k_work *work) {
    struct central_rx_item item;
    while (k_msgq_get(&rx_msgq, &item, K_NO_WAIT) == 0) {
        zmk_split_transport_central_peripheral_event_handler(&esb_central, item.source,
                                                             item.event);
    }
}

static int esb_central_send_command(uint8_t source,
                                    struct zmk_split_transport_central_command cmd) {
    if (source >= ARRAY_SIZE(pending_cmds)) {
        return -EINVAL;
    }

    pending_cmds[source] = cmd;
    has_pending_cmd[source] = true;
    return queue_ack_payload(source);
}

static int esb_central_get_available_source_ids(uint8_t *sources) {
    for (uint8_t i = 0; i < ARRAY_SIZE(last_seen); i++) {
        sources[i] = i;
    }

    return ARRAY_SIZE(last_seen);
}

static int esb_central_set_enabled(bool en) {
    enabled = en;

    if (!initialized) {
        return -ENODEV;
    }

    if (enabled) {
        return esb_start_rx();
    }

    esb_stop_rx();
    return 0;
}

static struct zmk_split_transport_status esb_central_get_status(void) {
    int connected = 0;
    int64_t now = k_uptime_get();

    for (size_t i = 0; i < ARRAY_SIZE(last_seen); i++) {
        if (last_seen[i] > 0 &&
            now - last_seen[i] < CONFIG_ZMK_SPLIT_ESB_CONNECTION_TIMEOUT_MS) {
            connected++;
        }
    }

    return (struct zmk_split_transport_status){
        .available = initialized,
        .enabled = enabled,
        .connections = connected == ARRAY_SIZE(last_seen)
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                       : connected > 0
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED
                           : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static int esb_central_set_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static const struct zmk_split_transport_central_api central_api = {
    .send_command = esb_central_send_command,
    .get_available_source_ids = esb_central_get_available_source_ids,
    .set_enabled = esb_central_set_enabled,
    .get_status = esb_central_get_status,
    .set_status_callback = esb_central_set_status_callback,
};

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(esb_central, &central_api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static int esb_central_init(void) {
    int err = zmk_split_esb_start_hf_clock();
    if (err) {
        LOG_ERR("ESB HF clock start failed: %d", err);
        return err;
    }

    err = zmk_split_esb_configure_radio(ESB_MODE_PRX, esb_handler);
    if (err) {
        LOG_ERR("ESB central init failed: %d", err);
        return err;
    }

    err = esb_enable_pipes(BIT_MASK(CONFIG_ZMK_SPLIT_ESB_CENTRAL_PERIPHERALS));
    if (err) {
        LOG_ERR("ESB pipe enable failed: %d", err);
        return err;
    }

    initialized = true;
    if (transport_status_cb) {
        transport_status_cb(&esb_central, esb_central_get_status());
    }

    return 0;
}

SYS_INIT(esb_central_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#define ZMK_SPLIT_ESB_MAGIC 0x5a
#define ZMK_SPLIT_ESB_VERSION 1

enum zmk_split_esb_packet_type {
    ZMK_SPLIT_ESB_PACKET_TYPE_POLL = 0,
    ZMK_SPLIT_ESB_PACKET_TYPE_EVENT = 1,
    ZMK_SPLIT_ESB_PACKET_TYPE_COMMAND = 2,
} __packed;

struct zmk_split_esb_packet {
    uint8_t magic;
    uint8_t version;
    enum zmk_split_esb_packet_type type;
    uint8_t source;
    uint8_t len;
    uint8_t data[CONFIG_ZMK_SPLIT_ESB_PACKET_DATA_SIZE - 5];
} __packed;

#define ZMK_SPLIT_ESB_PACKET_DATA_MAX sizeof(((struct zmk_split_esb_packet *)0)->data)

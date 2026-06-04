/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>

/* Fired on ESB peripherals when the central notifies them of a USB/BLE transport change.
 * Sensor drivers subscribe to this to adjust their hardware polling rates. */
struct zmk_peripheral_transport_changed {
    enum zmk_transport transport;
};

ZMK_EVENT_DECLARE(zmk_peripheral_transport_changed);

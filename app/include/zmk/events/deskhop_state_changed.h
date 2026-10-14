/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

// DeskHop's own enum values, carried verbatim on the wire.
#define ZMK_DESKHOP_OS_LINUX 1
#define ZMK_DESKHOP_OS_MACOS 2
#define ZMK_DESKHOP_OS_WINDOWS 3
#define ZMK_DESKHOP_OS_ANDROID 4
#define ZMK_DESKHOP_OS_OTHER 255

#define ZMK_DESKHOP_POS_LEFT 1
#define ZMK_DESKHOP_POS_RIGHT 2

struct zmk_deskhop_output_state {
    uint8_t os;
    uint8_t pos;
    uint8_t screen_count; // monitors on this output, >= 1
    uint8_t screen_index; // 1 = the screen at the border with the other output
};

// Raised for every v2 DeskHop sync report. DeskHop sends one whenever any of
// these fields changes, and once when the sync interface mounts.
struct zmk_deskhop_state_changed {
    uint8_t active_output; // 0 = A, 1 = B
    bool gaming;           // gaming mode: relative pointer and switching locked
    bool relative;         // pointer is relative, as DeskHop computed it
    bool switch_lock;
    struct zmk_deskhop_output_state outputs[2];
};

ZMK_EVENT_DECLARE(zmk_deskhop_state_changed);

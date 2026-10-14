/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)
#include <zmk/events/deskhop_state_changed.h>
#endif // IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *hid_dev;

static K_SEM_DEFINE(hid_sem, 1, 1);

static void in_ready_cb(const struct device *dev) { k_sem_give(&hid_sem); }

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_OUTPUT 0x200
#define HID_REPORT_TYPE_FEATURE 0x300

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
static uint8_t hid_protocol = HID_PROTOCOL_REPORT;

static void set_proto_cb(const struct device *dev, uint8_t protocol) { hid_protocol = protocol; }

void zmk_usb_hid_set_protocol(uint8_t protocol) { hid_protocol = protocol; }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

static uint8_t *get_keyboard_report(size_t *len) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol != HID_PROTOCOL_REPORT) {
        zmk_hid_boot_report_t *boot_report = zmk_hid_get_boot_report();
        *len = sizeof(*boot_report);
        return (uint8_t *)boot_report;
    }
#endif
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    *len = sizeof(*report);
    return (uint8_t *)report;
}

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            static struct zmk_hid_mouse_resolution_feature_report res_feature_report;

            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            *len = sizeof(struct zmk_hid_mouse_resolution_feature_report);
            struct zmk_pointing_resolution_multipliers mult =
                zmk_pointing_resolution_multipliers_get_profile(endpoint);

            res_feature_report.body.wheel_res = mult.wheel;
            res_feature_report.body.hwheel_res = mult.hor_wheel;
            *data = (uint8_t *)&res_feature_report;
            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;
    case HID_REPORT_TYPE_INPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
        case ZMK_HID_REPORT_ID_KEYBOARD: {
            size_t size;
            *data = get_keyboard_report(&size);
            *len = (int32_t)size;
            break;
        }
        case ZMK_HID_REPORT_ID_CONSUMER: {
            struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
            *data = (uint8_t *)report;
            *len = sizeof(*report);
            break;
        }
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        /*
         * 7.2.1 of the HID v1.11 spec is unclear about handling requests for reports that do not
         * exist For requested reports that aren't input reports, return -ENOTSUP like the Zephyr
         * subsys does
         */
        LOG_ERR("Unsupported report type %d requested", (setup->wValue & HID_GET_REPORT_TYPE_MASK)
                                                            << 8);
        return -ENOTSUP;
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)

#define DESKHOP_SYNC_MAGIC 0xA5
#define DESKHOP_SYNC_V1_LEN sizeof(struct zmk_hid_deskhop_sync_report)
#define DESKHOP_SYNC_V2_LEN sizeof(struct zmk_hid_deskhop_sync_report_v2)

BUILD_ASSERT(sizeof(struct zmk_hid_deskhop_sync_report_body_v2) == ZMK_HID_DESKHOP_SYNC_BODY_LEN,
             "DeskHop sync descriptor REPORT_COUNT must match the v2 body");

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void deskhop_sync_apply_output(uint8_t output) {
    // Clear every base layer except the one being switched to, then
    // activate it. Not zmk_keymap_layer_to(): that tears down the
    // whole stack and would drop any momentary layer the user is
    // holding (mouse/scroll/snipe) mid-switch. Layers outside the
    // base mask are deliberately left alone for that reason.
    //
    // Flipping only LAYER_A/LAYER_B is not enough: key resolution
    // walks layers from the highest index down, so any other base
    // layer that is still on -- a gaming layer above both of them --
    // shadows the target and the switch appears to do nothing.
    //
    // The deactivate is forcing (locking = true) because &to and &tog
    // both set the lock bit, and a non-forcing disable refuses to turn
    // a locked layer off.
    const bool want_b = output != 0;
    const uint8_t target_layer =
        want_b ? CONFIG_ZMK_DESKHOP_SYNC_LAYER_B : CONFIG_ZMK_DESKHOP_SYNC_LAYER_A;
    for (uint8_t layer = 0; layer < ZMK_KEYMAP_LAYERS_LEN; layer++) {
        if (layer != target_layer && (CONFIG_ZMK_DESKHOP_SYNC_BASE_LAYER_MASK & BIT(layer))) {
            zmk_keymap_layer_deactivate(layer, true);
        }
    }
    zmk_keymap_layer_activate(target_layer, false);
    LOG_INF("DeskHop sync output=%d -> layer=%d", output, target_layer);
}

static void deskhop_sync_raise_state(const struct zmk_hid_deskhop_sync_report_body_v2 *body) {
    struct zmk_deskhop_state_changed ev = {
        .active_output = body->active_output,
        .gaming = (body->flags & ZMK_HID_DESKHOP_SYNC_FLAG_GAMING) != 0,
        .relative = (body->flags & ZMK_HID_DESKHOP_SYNC_FLAG_RELATIVE) != 0,
        .switch_lock = (body->flags & ZMK_HID_DESKHOP_SYNC_FLAG_SWITCH_LOCK) != 0,
    };

    for (uint8_t i = 0; i < ARRAY_SIZE(ev.outputs); i++) {
        ev.outputs[i] = (struct zmk_deskhop_output_state){
            .os = body->outputs[i].os,
            .pos = body->outputs[i].pos,
            .screen_count = body->outputs[i].screen_count,
            .screen_index = body->outputs[i].screen_index,
        };
    }

    raise_zmk_deskhop_state_changed(ev);
}
#endif

static int deskhop_sync_set_report(const uint8_t *data, int32_t len) {
    // v1 is exactly magic + output. v2 and anything later appends to it, so a
    // longer report is read up to the v2 fields and the rest is ignored.
    if (len != DESKHOP_SYNC_V1_LEN && len < DESKHOP_SYNC_V2_LEN) {
        LOG_ERR("DeskHop sync report malformed: length=%d", len);
        return -EINVAL;
    }

    const struct zmk_hid_deskhop_sync_report *dh = (const struct zmk_hid_deskhop_sync_report *)data;
    if (dh->body.magic != DESKHOP_SYNC_MAGIC) {
        LOG_WRN("DeskHop sync magic mismatch: 0x%02x", dh->body.magic);
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return 0;
#else
    if (len == DESKHOP_SYNC_V1_LEN) {
        // A v1 sender only reports on an output change, so every report is a
        // switch.
        deskhop_sync_apply_output(dh->body.output);
        return 0;
    }

    // A v2 sender reports on every state change -- screen, gaming mode,
    // monitor layout -- so most reports are not a switch at all. Applying the
    // output every time would re-clear the base mask, which includes the
    // gaming layer, and undo a layer toggled since the last real switch.
    static int16_t applied_output = -1;
    const struct zmk_hid_deskhop_sync_report_v2 *v2 =
        (const struct zmk_hid_deskhop_sync_report_v2 *)data;

    if (v2->body.output != applied_output) {
        applied_output = v2->body.output;
        deskhop_sync_apply_output(v2->body.output);
    }

    deskhop_sync_raise_state(&v2->body);
    return 0;
#endif
}

#endif // IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_FEATURE:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            if (*len != sizeof(struct zmk_hid_mouse_resolution_feature_report)) {
                return -EINVAL;
            }

            struct zmk_hid_mouse_resolution_feature_report *report =
                (struct zmk_hid_mouse_resolution_feature_report *)*data;
            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            zmk_pointing_resolution_multipliers_process_report(&report->body, endpoint);

            break;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#if IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)
        case ZMK_HID_REPORT_ID_DESKHOP_SYNC: {
            int err = deskhop_sync_set_report(*data, *len);
            if (err < 0) {
                return err;
            }
            break;
        }
#endif // IS_ENABLED(CONFIG_ZMK_DESKHOP_SYNC_REPORT)
        default:
            return -ENOTSUP;
        }
        break;

    case HID_REPORT_TYPE_OUTPUT:
        switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        case ZMK_HID_REPORT_ID_LEDS:
            if (*len != sizeof(struct zmk_hid_led_report)) {
                LOG_ERR("LED set report is malformed: length=%d", *len);
                return -EINVAL;
            } else {
                struct zmk_hid_led_report *report = (struct zmk_hid_led_report *)*data;
                struct zmk_endpoint_instance endpoint = {
                    .transport = ZMK_TRANSPORT_USB,
                };
                zmk_hid_indicators_process_report(&report->body, endpoint);
            }
            break;
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        default:
            LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
            return -EINVAL;
        }
        break;
    default:
        LOG_ERR("Unsupported report type %d requested",
                (setup->wValue & HID_GET_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }

    return 0;
}

static const struct hid_ops ops = {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    .protocol_change = set_proto_cb,
#endif
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

static int zmk_usb_hid_send_report(const uint8_t *report, size_t len) {
    switch (zmk_usb_get_status()) {
    case USB_DC_SUSPEND:
        return usb_wakeup_request();
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return -ENODEV;
    default:
        k_sem_take(&hid_sem, K_MSEC(30));
        int err = hid_int_ep_write(hid_dev, report, len, NULL);

        if (err) {
            k_sem_give(&hid_sem);
        }

        return err;
    }
}

int zmk_usb_hid_send_keyboard_report(void) {
    size_t len;
    uint8_t *report = get_keyboard_report(&len);
    return zmk_usb_hid_send_report(report, len);
}

int zmk_usb_hid_send_consumer_report(void) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report() {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static int zmk_usb_hid_init(void) {
    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    usb_hid_set_proto_code(hid_dev, HID_BOOT_IFACE_CODE_KEYBOARD);
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    usb_hid_init(hid_dev);

    return 0;
}

SYS_INIT(zmk_usb_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);

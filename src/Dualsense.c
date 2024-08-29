// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Sony DualSense(TM) controller.
 *
 *  Copyright (c) 2020 Sony Interactive Entertainment
 */

#include <dualsensectl/Dualsense.h>

static void dualsense_init_output_report(struct dualsense *ds, struct dualsense_output_report *rp, void *buf)
{
    if (ds->bt) {
        struct dualsense_output_report_bt *bt = buf;

        memset(bt, 0, sizeof(*bt));
        bt->report_id = DS_OUTPUT_REPORT_BT;
        bt->tag = DS_OUTPUT_TAG; /* Tag must be set. Exact meaning is unclear. */

        /*
         * Highest 4-bit is a sequence number, which needs to be increased
         * every report. Lowest 4-bit is tag and can be zero for now.
         */
        bt->seq_tag = (ds->output_seq << 4) | 0x0;
        if (++ds->output_seq == 16)
            ds->output_seq = 0;

        rp->data = buf;
        rp->len = sizeof(*bt);
        rp->bt = bt;
        rp->usb = NULL;
        rp->common = &bt->common;
    } else { /* USB */
        struct dualsense_output_report_usb *usb = buf;

        memset(usb, 0, sizeof(*usb));
        usb->report_id = DS_OUTPUT_REPORT_USB;

        rp->data = buf;
        rp->len = sizeof(*usb);
        rp->bt = NULL;
        rp->usb = usb;
        rp->common = &usb->common;
    }
}

static void dualsense_send_output_report(struct dualsense *ds, struct dualsense_output_report *report)
{
    /* Bluetooth packets need to be signed with a CRC in the last 4 bytes. */
    if (report->bt) {
        uint32_t crc;
        uint8_t seed = PS_OUTPUT_CRC32_SEED;

        crc = crc32_le(0xFFFFFFFF, &seed, 1);
        crc = ~crc32_le(crc, report->data, report->len - 4);

        report->bt->crc32 = crc;
    }

    int res = hid_write(ds->dev, report->data, report->len);
    if (res < 0) {
        fprintf(stderr, "Error: %ls\n", hid_error(ds->dev));
    }
}

static bool compare_serial(const char *s, const wchar_t *dev)
{
    if (!s) {
        return true;
    }
    const size_t len = wcslen(dev);
    if (strlen(s) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (s[i] != dev[i]) {
            return false;
        }
    }
    return true;
}

static struct hid_device_info *dualsense_hid_enumerate(void)
{
    struct hid_device_info *devs;
    struct hid_device_info **end = &devs;
    *end = hid_enumerate(DS_VENDOR_ID, DS_PRODUCT_ID);
    while (*end) {
        end = &(*end)->next;
    }
    *end = hid_enumerate(DS_VENDOR_ID, DS_EDGE_PRODUCT_ID);
    return devs;
}

bool dualsense_init(struct dualsense *ds, const char *serial)
{
    bool ret = false;

    memset(ds, 0, sizeof(*ds));

    bool found = false;
    struct hid_device_info *devs = dualsense_hid_enumerate();
    struct hid_device_info *dev = devs;
    while (dev) {
        if (compare_serial(serial, dev->serial_number)) {
            found = true;
            break;
        }
        dev = dev->next;
    }

    if (!found) {
        if (serial) {
            fprintf(stderr, "Device '%s' not found\n", serial);
        } else {
            fprintf(stderr, "No device found\n");
        }
        ret = false;
        goto out;
    }

    ds->dev = hid_open(DS_VENDOR_ID, dev->product_id, dev->serial_number);
    if (!ds->dev) {
        fprintf(stderr, "Failed to open device: %ls\n", hid_error(NULL));
        ret = false;
        goto out;
    }

    wchar_t *serial_number = dev->serial_number;

    if (wcslen(serial_number) != 17) {
        fprintf(stderr, "Invalid device serial number: %ls\n", serial_number);
        // Let's just fake serial number as everything except disconnecting will still work
        serial_number = L"00:00:00:00:00:00";
    }

    for (int i = 0; i < 18; ++i) {
        char c = serial_number[i];
        if (c && (i + 1) % 3) {
            c = toupper(c);
        }
        ds->mac_address[i] = c;
    }

    ds->bt = dev->interface_number == -1;

    ret = true;

out:
    if (devs) {
        hid_free_enumeration(devs);
    }
    return ret;
}

void dualsense_destroy(struct dualsense *ds)
{
    hid_close(ds->dev);
}

bool dualsense_bt_disconnect(struct dualsense *ds)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Failed to connect to DBus daemon: %s %s\n", err.name, err.message);
        return false;
    }
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Failed to enumerate BT devices: %s %s\n", err.name, err.message);
        return false;
    }
    DBusMessageIter dict;
    dbus_message_iter_init(reply, &dict);
    int objects_count = dbus_message_iter_get_element_count(&dict);
    DBusMessageIter dict_entry;
    dbus_message_iter_recurse(&dict, &dict_entry);
    DBusMessageIter dict_kv;
    char *ds_path = NULL;
    char *path, *iface, *prop;
    while (objects_count-- && !ds_path) {
        dbus_message_iter_recurse(&dict_entry, &dict_kv);
        dbus_message_iter_get_basic(&dict_kv, &path);
        dbus_message_iter_next(&dict_kv);
        int ifaces_count = dbus_message_iter_get_element_count(&dict_kv);
        DBusMessageIter ifacedict_entry, ifacedict_kv;
        dbus_message_iter_recurse(&dict_kv, &ifacedict_entry);
        while (ifaces_count-- && !ds_path) {
            dbus_message_iter_recurse(&ifacedict_entry, &ifacedict_kv);
            dbus_message_iter_get_basic(&ifacedict_kv, &iface);
            if (!strcmp(iface, "org.bluez.Device1")) {
                dbus_message_iter_next(&ifacedict_kv);
                int props_count = dbus_message_iter_get_element_count(&ifacedict_kv);
                DBusMessageIter propdict_entry, propdict_kv;
                dbus_message_iter_recurse(&ifacedict_kv, &propdict_entry);
                while (props_count-- && !ds_path) {
                    dbus_message_iter_recurse(&propdict_entry, &propdict_kv);
                    dbus_message_iter_get_basic(&propdict_kv, &prop);
                    DBusMessageIter variant;
                    if (!strcmp(prop, "Address")) {
                        dbus_message_iter_next(&propdict_kv);
                        dbus_message_iter_recurse(&propdict_kv, &variant);
                        char *address = NULL;
                        dbus_message_iter_get_basic(&variant, &address);
                        if (!strcmp(address, ds->mac_address) && !ds_path) {
                            ds_path = path;
                        }
                    }
                    dbus_message_iter_next(&propdict_entry);
                }
            }
            dbus_message_iter_next(&ifacedict_entry);
        }
        dbus_message_iter_next(&dict_entry);
    }
    dbus_message_unref(reply);
    if (!ds_path) {
        fprintf(stderr, "Failed to find BT device\n");
        return false;
    }
    msg = dbus_message_new_method_call("org.bluez", ds_path, "org.bluez.Device1", "Disconnect");
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Failed to disconnect BT device: %s %s\n", err.name, err.message);
        return false;
    }
    dbus_message_unref(reply);
    dbus_connection_unref(conn);
    return true;
}

int command_power_off(struct dualsense *ds)
{
    if (!ds->bt) {
        fprintf(stderr, "Controller is not connected via BT\n");
        return 1;
    }
    if (!dualsense_bt_disconnect(ds)) {
        return 2;
    }
    return 0;
}

int command_battery(struct dualsense *ds)
{
    uint8_t data[DS_INPUT_REPORT_BT_SIZE];
    int res = hid_read_timeout(ds->dev, data, sizeof(data), 1000);
    if (res <= 0) {
        if (res == 0) {
            fprintf(stderr, "Timeout waiting for report\n");
        } else {
            fprintf(stderr, "Failed to read report %ls\n", hid_error(ds->dev));
        }
        return 2;
    }

    struct dualsense_input_report *ds_report;

    if (!ds->bt && data[0] == DS_INPUT_REPORT_USB && res == DS_INPUT_REPORT_USB_SIZE) {
        ds_report = (struct dualsense_input_report *)&data[1];
    } else if (ds->bt && data[0] == DS_INPUT_REPORT_BT && res == DS_INPUT_REPORT_BT_SIZE) {
        /* Last 4 bytes of input report contain crc32 */
        /* uint32_t report_crc = *(uint32_t*)&data[res - 4]; */
        ds_report = (struct dualsense_input_report *)&data[2];
    } else {
        fprintf(stderr, "Unhandled report ID %d\n", (int)data[0]);
        return 3;
    }

    const char *battery_status;
    uint8_t battery_capacity;
    uint8_t battery_data = ds_report->status & DS_STATUS_BATTERY_CAPACITY;
    uint8_t charging_status = (ds_report->status & DS_STATUS_CHARGING) >> DS_STATUS_CHARGING_SHIFT;

#define min(a, b) ((a) < (b) ? (a) : (b))
    switch (charging_status) {
    case 0x0:
        /*
         * Each unit of battery data corresponds to 10%
         * 0 = 0-9%, 1 = 10-19%, .. and 10 = 100%
         */
        battery_capacity = min(battery_data * 10 + 5, 100);
        battery_status = "discharging";
        break;
    case 0x1:
        battery_capacity = min(battery_data * 10 + 5, 100);
        battery_status = "charging";
        break;
    case 0x2:
        battery_capacity = 100;
        battery_status = "full";
        break;
    case 0xa: /* voltage or temperature out of range */
    case 0xb: /* temperature error */
        battery_capacity = 0;
        battery_status = "not-charging";
        break;
    case 0xf: /* charging error */
    default:
        battery_capacity = 0;
        battery_status = "unknown";
    }
#undef min

    printf("%d %s\n", (int)battery_capacity, battery_status);
    return 0;
}

int command_info(struct dualsense *ds)
{
    uint8_t buf[DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = DS_FEATURE_REPORT_FIRMWARE_INFO;
    int res = hid_get_feature_report(ds->dev, buf, sizeof(buf));
    if (res != sizeof(buf)) {
        fprintf(stderr, "Invalid feature report\n");
        return false;
    }

    struct dualsense_feature_report_firmware *ds_report;
    ds_report = (struct dualsense_feature_report_firmware *)&buf;

    printf("Hardware: %x\n", ds_report->hardware_info);
    printf("Build date: %.11s %.8s\n", ds_report->build_date, ds_report->build_time);
    printf("Firmware: %x (type %i)\n", ds_report->firmware_version, ds_report->fw_type);
    printf("Fw version: %i %i %i\n", ds_report->fw_version_1, ds_report->fw_version_2, ds_report->fw_version_3);
    printf("Sw series: %i\n", ds_report->sw_series);
    printf("Update version: %04x\n", ds_report->update_version);
    /* printf("Device info: %.12s\n", ds_report->device_info); */
    /* printf("Update image info: %c\n", ds_report->update_image_info); */

    return 0;
}

int command_lightbar1(struct dualsense *ds, char *state)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    rp.common->valid_flag2 = DS_OUTPUT_VALID_FLAG2_LIGHTBAR_SETUP_CONTROL_ENABLE;
    if (!strcmp(state, "on")) {
        rp.common->lightbar_setup = DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_ON;
    } else if (!strcmp(state, "off")) {
        rp.common->lightbar_setup = DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_OUT;
    } else {
        fprintf(stderr, "Invalid state\n");
        return 1;
    }

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_lightbar3(struct dualsense *ds, uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    uint8_t max_brightness = 255;

    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE;
    rp.common->lightbar_red = brightness * red / max_brightness;
    rp.common->lightbar_green = brightness * green / max_brightness;
    rp.common->lightbar_blue = brightness * blue / max_brightness;

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_player_leds(struct dualsense *ds, uint8_t number)
{
    if (number > 5) {
        fprintf(stderr, "Invalid player number\n");
        return 1;
    }

    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    static const int player_ids[6] = {
        0,
        BIT(2),
        BIT(3) | BIT(1),
        BIT(4) | BIT(2) | BIT(0),
        BIT(4) | BIT(3) | BIT(1) | BIT(0),
        BIT(4) | BIT(3) | BIT(2) | BIT(1) | BIT(0)
    };

    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE;
    rp.common->player_leds = player_ids[number];

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_microphone(struct dualsense *ds, char *state)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_POWER_SAVE_CONTROL_ENABLE;
    if (!strcmp(state, "on")) {
        rp.common->power_save_control &= ~DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE;
    } else if (!strcmp(state, "off")) {
        rp.common->power_save_control |= DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE;
    } else {
        fprintf(stderr, "Invalid state\n");
        return 1;
    }

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_microphone_led(struct dualsense *ds, char *state)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_MIC_MUTE_LED_CONTROL_ENABLE;
    if (!strcmp(state, "on")) {
        rp.common->mute_button_led = 1;
    } else if (!strcmp(state, "off")) {
        rp.common->mute_button_led = 0;
    } else {
        fprintf(stderr, "Invalid state\n");
        return 1;
    }

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_speaker(struct dualsense *ds, char *state)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_AUDIO_CONTROL_ENABLE;
    /* value
     * | /left headphone
     * | | / right headphone
     * | | | / internal speaker
     * 0 L_R_X
     * 1 L_L_X
     * 2 L_L_R
     * 3 X_X_R
     */
    if (!strcmp(state, "internal")) { /* right channel to speaker */
        rp.common->audio_flags = 3 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
    } else if (!strcmp(state, "headphone")) { /* stereo channel to headphone */
        rp.common->audio_flags = 0;
    } else if (!strcmp(state, "monoheadphone")) { /* left channel to headphone */
        rp.common->audio_flags = 1 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
    } else if (!strcmp(state, "both")) { /* left channel to headphone, right channel to speaker */
        rp.common->audio_flags = 2 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
    } else {
        fprintf(stderr, "Invalid state\n");
        return 1;
    }

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_volume(struct dualsense *ds, uint8_t volume)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    uint8_t max_volume = 255;

    /* TODO see if we can get old values of volumes to be able to set values independently */
    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_HEADPHONE_VOLUME_ENABLE;
    rp.common->headphone_audio_volume = volume * 0x7f / max_volume;

    rp.common->valid_flag0 |= DS_OUTPUT_VALID_FLAG0_SPEAKER_VOLUME_ENABLE;
    /* the PS5 use 0x3d-0x64 trying over 0x64 doesnt change but below 0x3d can still lower the volume */
    rp.common->speaker_audio_volume = volume * 0x64 / max_volume;

    /* if we want to set speaker pre gain */
    //rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_AUDIO_CONTROL2_ENABLE;
    //rp.common->audio_flags2 = 4;

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_vibration_attenuation(struct dualsense *ds, uint8_t rumble_attenuation, uint8_t trigger_attenuation)
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    /* need to store or get current values if we want to change motor/haptic and trigger separately */
    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_VIBRATION_ATTENUATION_ENABLE;
    rp.common->reduce_motor_power = (uint8_t)((rumble_attenuation & 0x07) | ((trigger_attenuation & 0x07) << 4 ));

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_trigger(struct dualsense *ds, char *trigger, uint8_t mode, uint8_t param1, uint8_t param2, uint8_t param3, uint8_t param4, uint8_t param5, uint8_t param6, uint8_t param7, uint8_t param8, uint8_t param9 )
{
    struct dualsense_output_report rp;
    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
    dualsense_init_output_report(ds, &rp, rbuf);

    if (!strcmp(trigger, "right") || !strcmp(trigger, "both")) {
        rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_RIGHT_TRIGGER_MOTOR_ENABLE;
    }
    if (!strcmp(trigger, "left") || !strcmp(trigger, "both")) {
        rp.common->valid_flag0 |= DS_OUTPUT_VALID_FLAG0_LEFT_TRIGGER_MOTOR_ENABLE;
    }

    rp.common->right_trigger_motor_mode = mode;
    rp.common->right_trigger_param[0] = param1;
    rp.common->right_trigger_param[1] = param2;
    rp.common->right_trigger_param[2] = param3;
    rp.common->right_trigger_param[3] = param4;
    rp.common->right_trigger_param[4] = param5;
    rp.common->right_trigger_param[5] = param6;
    rp.common->right_trigger_param[6] = param7;
    rp.common->right_trigger_param[7] = param8;
    rp.common->right_trigger_param[8] = param9;

    rp.common->left_trigger_motor_mode = mode;
    rp.common->left_trigger_param[0] = param1;
    rp.common->left_trigger_param[1] = param2;
    rp.common->left_trigger_param[2] = param3;
    rp.common->left_trigger_param[3] = param4;
    rp.common->left_trigger_param[4] = param5;
    rp.common->left_trigger_param[5] = param6;
    rp.common->left_trigger_param[6] = param7;
    rp.common->left_trigger_param[7] = param8;
    rp.common->left_trigger_param[8] = param9;

    dualsense_send_output_report(ds, &rp);

    return 0;
}

int command_trigger_off(struct dualsense *ds, char *trigger)
{
    return command_trigger(ds, trigger, DS_TRIGGER_EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

static int trigger_bitpacking_array(struct dualsense *ds, char *trigger, uint8_t mode, uint8_t strength[10], uint8_t frequency)
{
    uint32_t strength_zones = 0;
    uint16_t active_zones = 0;
    for (int i = 0; i < 10; i++) {
        if (strength[i] > 8) {
            fprintf(stderr, "strengths must be between 0 and 8\n");
            return 1;
        }
        if (strength[i] > 0) {
            uint8_t strength_value = (uint8_t)((strength[i] -1) & 0x07);
            strength_zones |= (uint32_t)(strength_value << (3 * i));
            active_zones |= (uint16_t)(1 << i);
        }
    }

    return command_trigger(ds, trigger, mode,
                           (uint8_t)((active_zones >> 0) & 0xff),
                           (uint8_t)((active_zones >> 8) & 0xff),
                           (uint8_t)((strength_zones >> 0) & 0xff),
                           (uint8_t)((strength_zones >> 8) & 0xff),
                           (uint8_t)((strength_zones >> 16) & 0xff),
                           (uint8_t)((strength_zones >> 24) & 0xff),
                           0, 0,
                           frequency);
}

int command_trigger_feedback(struct dualsense *ds, char *trigger, uint8_t position, uint8_t strength)
{
    if (position > 9) {
        fprintf(stderr, "position must be between 0 and 9\n");
        return 1;
    }
    if (strength > 8 || !(strength > 0)) {
        fprintf(stderr, "strength must be between 1 and 8\n");
        return 1;
    }
    uint8_t strength_array[10] = {0};
    for (int i = position; i < 10; i++) {
        strength_array[i] = strength;
    }

    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_FEEDBACK, strength_array, 0);
}

int command_trigger_weapon(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength)
{
    if (start_position > 7 || start_position < 2) {
        fprintf(stderr, "start position must be between 2 and 7\n");
        return 1;
    }
    if (end_position > 8 || end_position < start_position+1) {
        fprintf(stderr, "end position must be between start position+1 and 8\n");
        return 1;
    }
    if (strength > 8 || !(strength > 0)) {
        fprintf(stderr, "strength must be between 1 and 8\n");
        return 1;
    }

    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
    return command_trigger(ds, trigger, DS_TRIGGER_EFFECT_WEAPON,
                           (uint8_t)((start_stop_zones >> 0) & 0xff),
                           (uint8_t)((start_stop_zones >> 8) & 0xff),
                           strength-1,
                           0, 0, 0, 0, 0, 0);
}

int command_trigger_bow(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength, uint8_t snap_force)
{
    if (start_position > 8 || !(start_position > 0)) {
        fprintf(stderr, "start position must be between 0 and 8\n");
        return 1;
    }
    if (end_position > 8 || end_position < start_position+1) {
        fprintf(stderr, "end position must be between start position+1 and 8\n");
        return 1;
    }
    if (strength > 8 || !(strength > 0)) {
        fprintf(stderr, "strength must be between 1 and 8\n");
        return 1;
    }
    if (snap_force > 8 || !(snap_force > 0)) {
        fprintf(stderr, "snap_force must be between 1 and 8\n");
        return 1;
    }

    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
    uint32_t force_pair =  (uint16_t)(((strength -1) & 0x07) | (((snap_force -1 ) & 0x07) << 3 ));
    return command_trigger(ds, trigger, DS_TRIGGER_EFFECT_BOW,
                           (uint8_t)((start_stop_zones >> 0) & 0xff),
                           (uint8_t)((start_stop_zones >> 8) & 0xff),
                           (uint8_t)((force_pair >> 0) & 0xff),
                           0, 0, 0, 0, 0, 0);
}

int command_trigger_galloping(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t first_foot, uint8_t second_foot, uint8_t frequency)
{
    if (start_position > 8) {
        fprintf(stderr, "start position must be between 0 and 8\n");
        return 1;
    }
    if (end_position > 9 || end_position < start_position+1) {
        fprintf(stderr, "end position must be between start position+1 and 9\n");
        return 1;
    }
    if (first_foot > 6) {
        fprintf(stderr, "first_foot must be between 0 and 8\n");
        return 1;
    }
    if (second_foot > 7 || second_foot < first_foot+1) {
        fprintf(stderr, "second_foot must be between first_foot+1 and 8\n");
        return 1;
    }

    if (!(frequency > 0)) {
        fprintf(stderr, "frequency must be greater than 0\n");
        return 1;
    }
    if (frequency > 8) {
        fprintf(stdout, "frequency has a better effect when lower than 8\n");
    }
    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
    uint32_t ratio =  (uint16_t)((second_foot & 0x07) | ((first_foot & 0x07) << 3 ));
    return command_trigger(ds, trigger, DS_TRIGGER_EFFECT_GALLOPING,
                           (uint8_t)((start_stop_zones >> 0) & 0xff),
                           (uint8_t)((start_stop_zones >> 8) & 0xff),
                           (uint8_t)((ratio >> 0) & 0xff),
                           frequency,
                           0, 0, 0, 0, 0);
}

int command_trigger_machine(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength_a, uint8_t strength_b, uint8_t frequency, uint8_t period)
{
    // if start_position == 0 nothing happen
    if (start_position > 8 || !(start_position > 0)) {
        fprintf(stderr, "start position must be between 1 and 8\n");
        return 1;
    }
    if (end_position > 9 || end_position < start_position+1) {
        fprintf(stderr, "end position must be between start position+1 and 9\n");
        return 1;
    }
    if (strength_a > 7) {
        fprintf(stderr, "strength_a position must be between 0 and 7\n");
        return 1;
    }
    if (strength_b > 7) {
        fprintf(stderr, "strength_b position must be between 0 and 7\n");
        return 1;
    }
    if (!(frequency > 0)) {
        fprintf(stderr, "frequency must be greater than 0\n");
        return 1;
    }
    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
    uint32_t force_pair =  (uint16_t)((strength_a & 0x07) | ((strength_b & 0x07) << 3 ));
    return command_trigger(ds, trigger, DS_TRIGGER_EFFECT_MACHINE,
                           (uint8_t)((start_stop_zones >> 0) & 0xff),
                           (uint8_t)((start_stop_zones >> 8) & 0xff),
                           (uint8_t)((force_pair >> 0) & 0xff),
                           frequency,
                           period,
                           0, 0, 0, 0);
}

int command_trigger_vibration(struct dualsense *ds, char *trigger, uint8_t position, uint8_t amplitude, uint8_t frequency)
{
    if (position > 9) {
        fprintf(stderr, "position must be between 0 and 9\n");
        return 1;
    }
    if (amplitude > 8 || !(amplitude > 0)) {
        fprintf(stderr, "amplitude must be between 1 and 8\n");
        return 1;
    }
    if (!(frequency > 0)) {
        fprintf(stderr, "frequency must be greater than 0\n");
        return 1;
    }

    uint8_t strength_array[10] = {0};
    for (int i = position; i < 10; i++) {
        strength_array[i] = amplitude;
    }
    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_VIBRATION, strength_array, frequency);

}

int command_trigger_feedback_raw(struct dualsense *ds, char *trigger, uint8_t strength[10] )
{
    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_FEEDBACK, strength, 0);
}

int command_trigger_vibration_raw(struct dualsense *ds, char *trigger, uint8_t strength[10], uint8_t frequency)
{
    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_VIBRATION, strength, frequency);
}

static bool sh_command_wait = false;
static const char *sh_command_add = NULL;
static const char *sh_command_remove = NULL;

static void run_sh_command(const char *command, const char *serial_number)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (!sh_command_wait) {
            pid = fork();
        }
        if (pid == 0) {
            setenv("DS_DEV", serial_number, 1);
            if (system(command) < 0) {
                perror("system");
            }
            exit(1);
        } else if (pid < 0) {
            perror("fork");
            exit(1);
        }
        exit(0);
    } else if (pid < 0) {
        perror("fork");
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static uint32_t read_file_hex(const char *path)
{
    uint32_t out = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return out;
    }
    if (fscanf(f, "%x", &out) != 1) {
        fprintf(stderr, "Failed to read %s\n", path);
    }
    fclose(f);
    return out;
}

static void read_file_str(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    if (fread(buf, size, 1, f) != 1) {
        fprintf(stderr, "Failed to read %s\n", path);
    }
    buf[size - 1] = '\0';
    fclose(f);
}

static bool check_dualsense_device(struct udev_device *dev, char serial_number[18])
{
    const char *path = udev_device_get_syspath(dev);
    char *end = strrchr(path, '/');
    if (!end || strncmp(end, "/event", 6)) {
        return false;
    }

    const char *joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
    if (!joystick || strcmp(joystick, "1")) {
        return false;
    }

    size_t baselen = end - path + 1;
    char idpath[256];
    strncpy(idpath, path, baselen);
    idpath[baselen] = '\0';
    char *baseend = idpath + baselen;

    strcpy(baseend, "id/vendor");
    uint32_t vendor = read_file_hex(idpath);

    strcpy(baseend, "id/product");
    uint32_t product = read_file_hex(idpath);

    strcpy(baseend, "uniq");
    read_file_str(idpath, serial_number, 18);

    return vendor == DS_VENDOR_ID && (product == DS_PRODUCT_ID || product == DS_EDGE_PRODUCT_ID);
}

static void add_device(struct udev_device *dev)
{
    char serial_number[18] = "00:00:00:00:00:00";
    if (!check_dualsense_device(dev, serial_number)) {
        return;
    }
    if (sh_command_add) {
        run_sh_command(sh_command_add, serial_number);
    }
}

static void remove_device(struct udev_device *dev)
{
    char serial_number[18] = "00:00:00:00:00:00";
    if (!check_dualsense_device(dev, serial_number)) {
        return;
    }
    if (sh_command_remove) {
        run_sh_command(sh_command_remove, serial_number);
    }
}

int command_monitor(void)
{
    struct udev *u = udev_new();
    struct udev_enumerate *enumerate = udev_enumerate_new(u);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *dev_list_entry;
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *path = udev_list_entry_get_name(dev_list_entry);
        struct udev_device *dev = udev_device_new_from_syspath(u, path);
        add_device(dev);
        udev_device_unref(dev);
    }
    udev_enumerate_unref(enumerate);

    struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
    udev_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL);
    udev_monitor_enable_receiving(monitor);

    struct pollfd fd;
    fd.fd = udev_monitor_get_fd(monitor);
    fd.events = POLLIN;

    while (1) {
        int ret = poll(&fd, 1, -1);
        if (ret < 0) {
            perror("poll");
            break;
        }
        struct udev_device *dev = udev_monitor_receive_device(monitor);
        if (!dev) {
            continue;
        }
        if (!strcmp(udev_device_get_action(dev), "add")) {
            add_device(dev);
        } else if (!strcmp(udev_device_get_action(dev), "remove")) {
            remove_device(dev);
        }
        udev_device_unref(dev);
    }

    udev_monitor_unref(monitor);
    udev_unref(u);

    return 0;
}

void print_version(void)
{
    printf("%s\n", DUALSENSECTL_VERSION);
}

int list_devices(void)
{
    struct hid_device_info *devs = dualsense_hid_enumerate();
    if (!devs) {
        fprintf(stderr, "No devices found\n");
        return 1;
    }
    printf("Devices:\n");
    struct hid_device_info *dev = devs;
    while (dev) {
        printf(" %ls (%s)\n", dev->serial_number ? dev->serial_number : L"???", dev->interface_number == -1 ? "Bluetooth" : "USB");
        dev = dev->next;
    }
    return 0;
}

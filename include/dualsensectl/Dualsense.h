// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Sony DualSense(TM) controller.
 *
 *  Copyright (c) 2020 Sony Interactive Entertainment
 */

#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <poll.h>
#include <sys/wait.h>

#include <dbus/dbus.h>
#include <hidapi/hidapi.h>
#include <libudev.h>

#include <dualsensectl/crc32.h>

#define DS_VENDOR_ID 0x054c
#define DS_PRODUCT_ID 0x0ce6
#define DS_EDGE_PRODUCT_ID 0x0df2

/* Seed values for DualShock4 / DualSense CRC32 for different report types. */
#define PS_INPUT_CRC32_SEED 0xA1
#define PS_OUTPUT_CRC32_SEED 0xA2
#define PS_FEATURE_CRC32_SEED 0xA3

#define DS_INPUT_REPORT_USB 0x01
#define DS_INPUT_REPORT_USB_SIZE 64
#define DS_INPUT_REPORT_BT 0x31
#define DS_INPUT_REPORT_BT_SIZE 78
#define DS_OUTPUT_REPORT_USB 0x02
#define DS_OUTPUT_REPORT_USB_SIZE 63
#define DS_OUTPUT_REPORT_BT 0x31
#define DS_OUTPUT_REPORT_BT_SIZE 78

#define DS_FEATURE_REPORT_CALIBRATION 0x05
#define DS_FEATURE_REPORT_CALIBRATION_SIZE 41
#define DS_FEATURE_REPORT_PAIRING_INFO 0x09
#define DS_FEATURE_REPORT_PAIRING_INFO_SIZE 20
#define DS_FEATURE_REPORT_FIRMWARE_INFO 0x20
#define DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE 64

/* Magic value required in tag field of Bluetooth output report. */
#define DS_OUTPUT_TAG 0x10
/* Flags for DualSense output report. */
#define BIT(n) (1 << n)
#define DS_OUTPUT_VALID_FLAG0_COMPATIBLE_VIBRATION BIT(0)
#define DS_OUTPUT_VALID_FLAG0_HAPTICS_SELECT BIT(1)
#define DS_OUTPUT_VALID_FLAG0_RIGHT_TRIGGER_MOTOR_ENABLE BIT(2)
#define DS_OUTPUT_VALID_FLAG0_LEFT_TRIGGER_MOTOR_ENABLE BIT(3)
#define DS_OUTPUT_VALID_FLAG0_HEADPHONE_VOLUME_ENABLE BIT(4)
#define DS_OUTPUT_VALID_FLAG0_SPEAKER_VOLUME_ENABLE BIT(5)
#define DS_OUTPUT_VALID_FLAG0_MICROPHONE_VOLUME_ENABLE BIT(6)
#define DS_OUTPUT_VALID_FLAG0_AUDIO_CONTROL_ENABLE BIT(7)

#define DS_OUTPUT_VALID_FLAG1_MIC_MUTE_LED_CONTROL_ENABLE BIT(0)
#define DS_OUTPUT_VALID_FLAG1_POWER_SAVE_CONTROL_ENABLE BIT(1)
#define DS_OUTPUT_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE BIT(2)
#define DS_OUTPUT_VALID_FLAG1_RELEASE_LEDS BIT(3)
#define DS_OUTPUT_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE BIT(4)
#define DS_OUTPUT_VALID_FLAG1_VIBRATION_ATTENUATION_ENABLE BIT(6)
#define DS_OUTPUT_VALID_FLAG1_AUDIO_CONTROL2_ENABLE BIT(7)

#define DS_OUTPUT_VALID_FLAG2_LIGHTBAR_SETUP_CONTROL_ENABLE BIT(1)
#define DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE BIT(4)
#define DS_OUTPUT_POWER_SAVE_CONTROL_AUDIO_MUTE BIT(5)
#define DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_ON BIT(0)
#define DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_OUT BIT(1)

/* audio control flags */
#define DS_OUTPUT_AUDIO_FLAG_FORCE_INTERNAL_MIC BIT(0)
#define DS_OUTPUT_AUDIO_FLAG_FORCE_HEADSET_MIC BIT(1)
#define DS_OUTPUT_AUDIO_FLAG_ECHO_CANCEL BIT(2)
#define DS_OUTPUT_AUDIO_FLAG_NOISE_CANCEL BIT(3)
#define DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT 4
#define DS_OUTPUT_AUDIO_FLAG_DISABLE_HEADPHONE BIT(4)
#define DS_OUTPUT_AUDIO_FLAG_ENABLE_INTERNAL_SPEAKER BIT(5)

/* audio control2 flags */
#define DS_OUTPUT_AUDIO2_FLAG_BEAM_FORMING BIT(4)

/* Status field of DualSense input report. */
#define DS_STATUS_BATTERY_CAPACITY 0xF
#define DS_STATUS_CHARGING 0xF0
#define DS_STATUS_CHARGING_SHIFT 4

#define DS_TRIGGER_EFFECT_OFF 0x05
#define DS_TRIGGER_EFFECT_FEEDBACK 0x21
#define DS_TRIGGER_EFFECT_BOW 0x22
#define DS_TRIGGER_EFFECT_GALLOPING 0x23
#define DS_TRIGGER_EFFECT_WEAPON 0x25
#define DS_TRIGGER_EFFECT_VIBRATION 0x26
#define DS_TRIGGER_EFFECT_MACHINE 0x27

struct dualsense_touch_point {
    uint8_t contact;
    uint8_t x_lo;
    uint8_t x_hi:4, y_lo:4;
    uint8_t y_hi;
} __attribute__((packed));

/* Main DualSense input report excluding any BT/USB specific headers. */
struct dualsense_input_report {
    uint8_t x, y;
    uint8_t rx, ry;
    uint8_t z, rz;
    uint8_t seq_number;
    uint8_t buttons[4];
    uint8_t reserved[4];

    /* Motion sensors */
    uint16_t gyro[3]; /* x, y, z */
    uint16_t accel[3]; /* x, y, z */
    uint32_t sensor_timestamp;
    uint8_t reserved2;

    /* Touchpad */
    struct dualsense_touch_point points[2];

    uint8_t reserved3[12];
    uint8_t status;
    uint8_t reserved4[10];
} __attribute__((packed));

/* Common data between DualSense BT/USB main output report. */
struct dualsense_output_report_common {
    uint8_t valid_flag0;
    uint8_t valid_flag1;

    /* For DualShock 4 compatibility mode. */
    uint8_t motor_right;
    uint8_t motor_left;

    /* Audio controls */
    uint8_t headphone_audio_volume; /* 0-0x7f */
    uint8_t speaker_audio_volume;   /* 0-255 */
    uint8_t internal_microphone_volume; /* 0-0x40 */
    uint8_t audio_flags;
    uint8_t mute_button_led;

    uint8_t power_save_control;

    /* right trigger motor */
    uint8_t right_trigger_motor_mode;
    uint8_t right_trigger_param[10];

    /* right trigger motor */
    uint8_t left_trigger_motor_mode;
    uint8_t left_trigger_param[10];

    uint8_t reserved2[4];

    uint8_t reduce_motor_power;
    uint8_t audio_flags2; /* 3 first bits: speaker pre-gain */

    /* LEDs and lightbar */
    uint8_t valid_flag2;
    uint8_t reserved3[2];
    uint8_t lightbar_setup;
    uint8_t led_brightness;
    uint8_t player_leds;
    uint8_t lightbar_red;
    uint8_t lightbar_green;
    uint8_t lightbar_blue;
} __attribute__((packed));
_Static_assert(sizeof(struct dualsense_output_report_common) == 47, "Bad output report structure size");

struct dualsense_output_report_bt {
    uint8_t report_id; /* 0x31 */
    uint8_t seq_tag;
    uint8_t tag;
    struct dualsense_output_report_common common;
    uint8_t reserved[24];
    uint32_t crc32;
} __attribute__((packed));

struct dualsense_output_report_usb {
    uint8_t report_id; /* 0x02 */
    struct dualsense_output_report_common common;
    uint8_t reserved[15];
} __attribute__((packed));

/*
 * The DualSense has a main output report used to control most features. It is
 * largely the same between Bluetooth and USB except for different headers and CRC.
 * This structure hide the differences between the two to simplify sending output reports.
 */
struct dualsense_output_report {
    uint8_t *data; /* Start of data */
    uint8_t len; /* Size of output report */

    /* Points to Bluetooth data payload in case for a Bluetooth report else NULL. */
    struct dualsense_output_report_bt *bt;
    /* Points to USB data payload in case for a USB report else NULL. */
    struct dualsense_output_report_usb *usb;
    /* Points to common section of report, so past any headers. */
    struct dualsense_output_report_common *common;
};

struct dualsense_feature_report_firmware {
    uint8_t report_id; // 0x20
    char build_date[11]; // string
    char build_time[8]; // string
    uint16_t fw_type;
    uint16_t sw_series;
    uint32_t hardware_info; // 0x00FF0000 - Variation
                            // 0x0000FF00 - Generation
                            // 0x0000003F - Trial?
                            // ^ Values tied to enumerations
    uint32_t firmware_version; // 0xAABBCCCC AA.BB.CCCC
    char device_info[12];
    ////
    uint16_t update_version;
    char update_image_info;
    char update_unk;
    ////
    uint32_t fw_version_1; // AKA SblFwVersion
                           // 0xAABBCCCC AA.BB.CCCC
                           // Ignored for fw_type 0
                           // HardwareVersion used for fw_type 1
                           // Unknown behavior if HardwareVersion < 0.1.38 for fw_type 2 & 3
                           // If HardwareVersion >= 0.1.38 for fw_type 2 & 3
    uint32_t fw_version_2; // AKA VenomFwVersion
    uint32_t fw_version_3; // AKA SpiderDspFwVersion AKA BettyFwVer
                           // May be Memory Control Unit for Non Volatile Storage
    uint32_t crc32;
};
_Static_assert(sizeof(struct dualsense_feature_report_firmware) == DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE, "Bad feature report firmware structure size");

struct dualsense {
    bool bt;
    hid_device *dev;
    char mac_address[18];
    uint8_t output_seq;
};

static void dualsense_init_output_report(struct dualsense *ds, struct dualsense_output_report *rp, void *buf);
static void dualsense_send_output_report(struct dualsense *ds, struct dualsense_output_report *report);
static bool compare_serial(const char *s, const wchar_t *dev);
static struct hid_device_info *dualsense_hid_enumerate(void);
bool dualsense_init(struct dualsense *ds, const char *serial);
void dualsense_destroy(struct dualsense *ds);
bool dualsense_bt_disconnect(struct dualsense *ds);
int command_power_off(struct dualsense *ds);
int command_battery(struct dualsense *ds);
int command_info(struct dualsense *ds);
int command_lightbar1(struct dualsense *ds, char *state);
int command_lightbar3(struct dualsense *ds, uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness);
int command_player_leds(struct dualsense *ds, uint8_t number);
int command_microphone(struct dualsense *ds, char *state);
int command_microphone_led(struct dualsense *ds, char *state);
int command_speaker(struct dualsense *ds, char *state);
int command_volume(struct dualsense *ds, uint8_t volume);
int command_vibration_attenuation(struct dualsense *ds, uint8_t rumble_attenuation, uint8_t trigger_attenuation);
int command_trigger(struct dualsense *ds, char *trigger, uint8_t mode, uint8_t param1, uint8_t param2, uint8_t param3, uint8_t param4, uint8_t param5, uint8_t param6, uint8_t param7, uint8_t param8, uint8_t param9 );
int command_trigger_off(struct dualsense *ds, char *trigger);
static int trigger_bitpacking_array(struct dualsense *ds, char *trigger, uint8_t mode, uint8_t strength[10], uint8_t frequency);
int command_trigger_feedback(struct dualsense *ds, char *trigger, uint8_t position, uint8_t strength);
int command_trigger_weapon(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength);
int command_trigger_bow(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength, uint8_t snap_force);
int command_trigger_galloping(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t first_foot, uint8_t second_foot, uint8_t frequency);
int command_trigger_machine(struct dualsense *ds, char *trigger, uint8_t start_position, uint8_t end_position, uint8_t strength_a, uint8_t strength_b, uint8_t frequency, uint8_t period);
int command_trigger_vibration(struct dualsense *ds, char *trigger, uint8_t position, uint8_t amplitude, uint8_t frequency);
int command_trigger_feedback_raw(struct dualsense *ds, char *trigger, uint8_t strength[10] );
int command_trigger_vibration_raw(struct dualsense *ds, char *trigger, uint8_t strength[10], uint8_t frequency);

static bool sh_command_wait;
static const char *sh_command_add;
static const char *sh_command_remove;

static void run_sh_command(const char *command, const char *serial_number);
static uint32_t read_file_hex(const char *path);
static void read_file_str(const char *path, char *buf, size_t size);
static bool check_dualsense_device(struct udev_device *dev, char serial_number[18]);
static void add_device(struct udev_device *dev);
static void remove_device(struct udev_device *dev);
int command_monitor(void);
void print_version(void);
int list_devices(void);

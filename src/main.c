// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Sony DualSense(TM) controller.
 *
 *  Copyright (c) 2020 Sony Interactive Entertainment
 */

#include <dualsensectl/dualsense.h>

static void print_help(void)
{
  printf("Usage: dualsensectl [options] command [ARGS]\n");
  printf("\n");
  printf("Options:\n");
  printf("  -l                                       List available devices\n");
  printf("  -d DEVICE                                Specify which device to use\n");
  printf("  -w                                       Wait for shell command to complete (monitor only)\n");
  printf("  -h --help                                Show this help message\n");
  printf("  -v --version                             Show version\n");
  printf("Commands:\n");
  printf("  power-off                                Turn off the controller (BT only)\n");
  printf("  battery                                  Get the controller battery level\n");
  printf("  info                                     Get the controller firmware info\n");
  printf("  lightbar STATE                           Enable (on) or disable (off) lightbar\n");
  printf("  lightbar RED GREEN BLUE [BRIGHTNESS]     Set lightbar color and brightness (0-255)\n");
  printf("  player-leds NUMBER                       Set player LEDs (1-5) or disabled (0)\n");
  printf("  microphone STATE                         Enable (on) or disable (off) microphone\n");
  printf("  microphone-led STATE                     Enable (on) or disable (off) microphone LED\n");
  printf("  speaker STATE                            Toggle to 'internal' speaker, 'headphone' or both\n");
  printf("  volume VOLUME                            Set audio volume (0-255) of internal speaker and headphone\n");
  printf("  attenuation RUMBLE TRIGGER               Set the attenuation (0-7) of rumble/haptic motors and trigger vibration\n");
  printf("  trigger TRIGGER off                      remove all effects\n");
  printf("  trigger TRIGGER feedback POSITION STRENGTH\n\
                                           set a resistance starting at position with a defined strength\n");
  printf("  trigger TRIGGER weapon START STOP STRENGTH\n\
                                           Emulate weapon like gun trigger\n");
  printf("  trigger TRIGGER bow START STOP STRENGTH SNAPFORCE\n\
                                           Emulate weapon like bow\n");
  printf("  trigger TRIGGER galloping START STOP FIRST_FOOT SECOND_FOOT FREQUENCY\n\
                                           Emulate a galloping\n");
  printf("  trigger TRIGGER machine START STOP STRENGTH_A STRENGTH_B FREQUENCY PERIOD\n\
                                           Switch vibration between to strength at a specified period\n");
  printf("  trigger TRIGGER vibration POSITION AMPLITUDE FREQUENCY \n\
                                           Vibrates motor arm around specified position\n");
  printf("  trigger TRIGGER feedback-raw STRENGTH[10]\n\
                                           set a resistance starting using array of strength\n");
  printf("  trigger TRIGGER vibration-raw AMPLITUDE[10] FREQUENCY\n\
                                           Vibrates motor arm at position and strength specified by an array of amplitude\n");
  printf("  trigger TRIGGER MODE [PARAMS]            set the trigger (left, right or both) mode with parameters (up to 9)\n");
  printf("  monitor [add COMMAND] [remove COMMAND]   Run shell command COMMAND on add/remove events\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_help();
        return 1;
    }

    const char *dev_serial = NULL;

    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        print_help();
        return 0;
    } else if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
        print_version();
        return 0;
    } else if (!strcmp(argv[1], "-l")) {
        return list_devices();
    } else if (!strcmp(argv[1], "monitor")) {
        argc -= 2;
        argv += 2;
        while (argc) {
            if (!strcmp(argv[0], "-w")) {
                sh_command_wait = true;
            } else if (!strcmp(argv[0], "add")) {
                if (argc < 2) {
                    print_help();
                    return 1;
                }
                sh_command_add = argv[1];
                argc -= 1;
                argv += 1;
            } else if (!strcmp(argv[0], "remove")) {
                if (argc < 2) {
                    print_help();
                    return 1;
                }
                sh_command_remove = argv[1];
                argc -= 1;
                argv += 1;
            }
            argc -= 1;
            argv += 1;
        }
        return command_monitor();
    } else if (!strcmp(argv[1], "-d")) {
        if (argc < 3) {
            print_help();
            return 1;
        }
        dev_serial = argv[2];
        argc -= 2;
        argv += 2;
    }

    if (argc < 2) {
        print_help();
        return 1;
    }

    struct dualsense ds;
    if (!dualsense_init(&ds, dev_serial)) {
        return 1;
    }

    if (!strcmp(argv[1], "power-off")) {
        return command_power_off(&ds);
    } else if (!strcmp(argv[1], "battery")) {
        return command_battery(&ds);
    } else if (!strcmp(argv[1], "info")) {
        return command_info(&ds);
    } else if (!strcmp(argv[1], "lightbar")) {
        if (argc == 3) {
            return command_lightbar1(&ds, argv[2]);
        } else if (argc == 5 || argc == 6) {
            uint8_t brightness = argc == 6 ? atoi(argv[5]) : 255;
            return command_lightbar3(&ds, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), brightness);
        } else {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
    } else if (!strcmp(argv[1], "player-leds")) {
        if (argc != 3) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        return command_player_leds(&ds, atoi(argv[2]));
    } else if (!strcmp(argv[1], "microphone")) {
        if (argc != 3) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        return command_microphone(&ds, argv[2]);
    } else if (!strcmp(argv[1], "microphone-led")) {
        if (argc != 3) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        return command_microphone_led(&ds, argv[2]);
    } else if (!strcmp(argv[1], "speaker")) {
        if (argc != 3) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        return command_speaker(&ds, argv[2]);
    } else if (!strcmp(argv[1], "volume")) {
        if (argc != 3) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        if (atoi(argv[2]) > 255) {
            fprintf(stderr, "Invalid volume\n");
            return 1;
        }
        return command_volume(&ds, atoi(argv[2]));
    } else if (!strcmp(argv[1], "attenuation")) {
        if (argc != 4) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        if ((atoi(argv[2]) > 7) | (atoi(argv[3]) > 7)) {
            fprintf(stderr, "Invalid attenuation\n");
            return 1;
        }
        return command_vibration_attenuation(&ds, atoi(argv[2]), atoi(argv[3]));
    } else if (!strcmp(argv[1], "trigger")) {
        if (argc < 4) {
            fprintf(stderr, "Invalid arguments\n");
            return 2;
        }
        if (strcmp(argv[2], "left") && strcmp(argv[2], "right") && strcmp(argv[2], "both")) {
            fprintf(stderr, "Invalid argument: TRIGGER must be either \"left\", \"right\" or \"both\"\n");
            return 2;
        }
        if (!strcmp(argv[3], "off")) {
            return command_trigger_off(&ds, argv[2]);
        } else if (!strcmp(argv[3], "feedback")) {
            if (argc < 6) {
                fprintf(stderr, "feedback mode need two parameters\n");
                return 2;
            }
            return command_trigger_feedback(&ds, argv[2], atoi(argv[4]), atoi(argv[5]));
        } else if (!strcmp(argv[3], "weapon")) {
            if (argc < 7) {
                fprintf(stderr, "weapons mode need three parameters\n");
                return 2;
            }
            return command_trigger_weapon(&ds, argv[2], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
        } else if (!strcmp(argv[3], "bow")) {
            if (argc < 8) {
                fprintf(stderr, "bow mode need four parameters\n");
                return 2;
            }
            return command_trigger_bow(&ds, argv[2], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]));
        } else if (!strcmp(argv[3], "galloping")) {
            if (argc < 9) {
                fprintf(stderr, "galloping mode need five parameters\n");
                return 2;
            }
            return command_trigger_galloping(&ds, argv[2], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]));
        } else if (!strcmp(argv[3], "machine")) {
            if (argc < 10) {
                fprintf(stderr, "machine mode need six parameters\n");
                return 2;
            }
            return command_trigger_machine(&ds, argv[2], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]));
        } else if (!strcmp(argv[3], "vibration")) {
            if (argc < 7) {
                fprintf(stderr, "vibration mode need three parameters\n");
                return 2;
            }
            return command_trigger_vibration(&ds, argv[2], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
        } else if (!strcmp(argv[3], "feedback-raw")) {
            if (argc < 14) {
                fprintf(stderr, "feedback-raw mode need ten parameters\n");
                return 2;
            }
            uint8_t strengths[10] = { atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]), atoi(argv[10]), atoi(argv[11]), atoi(argv[12]), atoi(argv[13]) };
            return command_trigger_feedback_raw(&ds, argv[2], strengths);
        } else if (!strcmp(argv[3], "vibration-raw")) {
            if (argc < 15) {
                fprintf(stderr, "vibration-raw mode need eleven parameters\n");
                return 2;
            }
            uint8_t strengths[10] = { atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]), atoi(argv[10]), atoi(argv[11]), atoi(argv[12]), atoi(argv[13]) };
            return command_trigger_vibration_raw(&ds, argv[2], strengths, atoi(argv[14]));
        }

        /* mostly to test raw parameters without any kind of bitpacking or range check */
        uint8_t param1 = argc > 4 ? atoi(argv[4]) : 0;
        uint8_t param2 = argc > 5 ? atoi(argv[5]) : 0;
        uint8_t param3 = argc > 6 ? atoi(argv[6]) : 0;
        uint8_t param4 = argc > 7 ? atoi(argv[7]) : 0;
        uint8_t param5 = argc > 8 ? atoi(argv[8]) : 0;
        uint8_t param6 = argc > 9 ? atoi(argv[9]) : 0;
        uint8_t param7 = argc > 10 ? atoi(argv[10]) : 0;
        uint8_t param8 = argc > 11 ? atoi(argv[11]) : 0;
        uint8_t param9 = argc > 12 ? atoi(argv[12]) : 0;

        return command_trigger(&ds, argv[2], atoi(argv[3]), param1, param2, param3, param4, param5, param6, param7, param8, param9);
    } else {
        fprintf(stderr, "Invalid command\n");
        return 2;
    }

    dualsense_destroy(&ds);
    return 0;
}

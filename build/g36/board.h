/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Everything the G36 menu programs know about ONE particular handheld, in one place.
 *
 * To run the menu on a different handheld, this is the file to change (and the files listed at the end).
 * Nothing here is logic: the programs use these names exactly as they used to use the literal text. */
#ifndef G36_BOARD_H
#define G36_BOARD_H

#define BOARD_NAME            "G36"

/* storage: where the card's two data drives are mounted (the image's layout), and the card itself */
#define BOARD_GAMES_MOUNT     "/mnt/sdcard"                 /* the games drive: roms, saves, g36/, thumbnails ... */
#define BOARD_EMU_MOUNT       "/mnt/emu"                    /* the emulator drive: cores/ */
#define BOARD_CARD_SIZE       "/sys/block/mmcblk0/size"     /* the whole card's size, in 512-byte sectors */

/* power, charger, screen brightness */
#define BOARD_BATTERY_DIR     "/sys/class/power_supply/rkadc-battery"
#define BOARD_BATTERY_CAPACITY BOARD_BATTERY_DIR "/capacity"
#define BOARD_EXTCON_STATE    "/sys/class/extcon/extcon0/state"   /* cable state (USB data / charger / none) */
#define BOARD_UDC_DIR         "/sys/class/udc"                    /* the USB device controller */
#define BOARD_BACKLIGHT       "/sys/class/backlight/backlight/brightness"   /* written with a percent, 10 - 100 */

/* screen: the DRM device the menu draws on */
#define BOARD_DRM_DEVICE      "/dev/dri/card0"

/* sound: the ALSA device that plays, the one whose mixer is the volume, and that mixer's control */
#define BOARD_PCM_DEVICE      "default"
#define BOARD_MIXER_DEVICE    "default"
#define BOARD_MIXER_ELEMENT   "Master"

/* the menu's text font, from the handheld's own firmware */
#define BOARD_UI_FONT         "/oem/release/assets/default/raw/fonts/OpenSans_Semibold.ttf"

/* Not in this file, but also specific to a handheld:
 *  - the screen layout: the menu is laid out for an 854 x 480 landscape screen (the positions are in g36menu.c)
 *  - the buttons: the menu reads the standard Linux button codes (BTN_SOUTH, BTN_TL, KEY_POWER ...); a handheld
 *    whose buttons send other codes needs them translated where the keys are read (input.c)
 *  - system-scripts/ (USB gadget setup, file transfer, start-up) and the image itself (partitions, boot, splash) */
#endif

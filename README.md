# G36 firmware

A custom front end and system image for the "G36" retro handheld (Rockchip RK3506B, 256 MB of RAM,
480x854 screen). It replaces the stock menu with a faster one, adds a video and music player, sleep and
power handling, and bundles the RetroArch emulators the handheld runs. The code that is ours is MIT-licensed;
the rest is other people's work under their own licences (see [Licences](#licences-and-credits)).

**How it was made:** most of the code in this repository was written with AI (Anthropic's Claude). A human
(DezPanda) directed the work, made the decisions and tested every change on a real handheld.

This project is not affiliated with the handheld's maker. It was built and tested on one handheld, so treat it as
a hobby project: flashing erases the card you flash to, and you use it at your own risk.

## What it does

- **Home screen and game lists** for every console folder, with box-art thumbnails, favorites, recently played
  games, your own collections, search and an All Games list.
- **Video Player** (H.264, up to 1080p; 720p or less plays smoothly) with subtitles (embedded or `.srt`, `.ass`,
  `.vtt` files beside the video), several sound tracks, speed control, watch progress and a "sound only" mode.
- **Music Player** with album art.
- **Sleep and power:** tap Power twice to sleep (a running game is saved and resumed when you wake), hold it to save
  and power off, safe shutdown at low battery, and a prompt to load the saved state after a crash.
- **File transfer over USB** (the card shows up on your computer through the handheld) and optional ADB.
- **Get Artwork**, a small program for Windows, Mac and Linux that downloads box art for your games from the
  [libretro thumbnails](https://thumbnails.libretro.com) project.
- A small chatbot and an image generator that run on the handheld itself.

No games, BIOS files or box art are included. Bring your own.

## Installing

1. Download `g36-firmware-v1.0.img` from the [Releases](../../releases) page.
2. Flash it to a microSD card with [balenaEtcher](https://etcher.balena.io) (cards from 4 GB up to 2 TB; 64 GB and
   256 GB cards have been tested). This erases the card.
3. Put the card in the handheld and switch it on. The first start takes a few minutes: it creates and formats a games
   drive (`G36-GAMES`) in the rest of the card and copies the starter files onto it.
4. Copy your games to `roms/<console>/` on that drive (with a card reader, or with File Transfer on over USB).
   The `README.txt` on the drive explains the folders; run `Get Artwork` from the drive to fetch box art.

## What is in this repository

| Folder | What is in it |
|---|---|
| `build/g36/` | The menu (`menu/`), `g36d` (power button, battery, clock, volume), the chatbot and image generator, `g36-sleep`, the system scripts |
| `build/*.patch`, `build/src/` | Our changes to RetroArch and uMTP-Responder, and the FFmpeg build recipe (`build/ffmpeg-lgpl/`) |
| `patch/` | The files the first boot copies onto the games drive (`template/`), the theme (`theme/`), and the system scripts that are installed on the handheld |
| `tools/` | Get Artwork (`artwork-go/`, in Go) and the scripts used to work on the system images |
| `release/` | The scripts that build the public image from a working handheld |

## Building

The menu and system programs are cross-compiled for the handheld (32-bit ARM, hard-float, glibc). You need an
`armv7-unknown-linux-gnueabihf` cross compiler and the handheld's own libraries and headers (copy them from a running
handheld); `build/g36-env.sh` shows how they are laid out. Set `G36_BUILD` to the full path of `build/` (a path without
spaces), then `sh build/g36/menu/build.sh g36menu` builds the menu. `sh build/g36/g36d-build.sh` builds `g36d`.

Get Artwork needs Go 1.21 or newer: `cd tools/artwork-go && sh build.sh`.

The public image is assembled by the scripts in `release/` from a handheld that is already running this software
(`build_public_rootfs.py`, `make_public_image.py`, `verify_rootfs.py`).

## Licences and credits

- Our own code is under the [MIT licence](LICENSE), Copyright (c) 2026 DezPanda.
- Everything else keeps its own licence. RetroArch is GPL-3.0 (our changes to it are the patches here), FFmpeg is the
  LGPL build described in `build/ffmpeg-lgpl/`, and each emulator core has its own terms; several, including the
  arcade cores, allow non-commercial use only. The console logos are by Dan Patrick and may never be sold.
  [`patch/template/CREDITS.txt`](patch/template/CREDITS.txt) credits everyone, and
  [`patch/template/SOURCE.txt`](patch/template/SOURCE.txt) says where the source of every part is.
- Because of the logos and the emulators, please do not sell the card or the image.

Questions and problems: open an issue.

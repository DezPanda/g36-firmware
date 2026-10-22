G36 GAMES
=========
This drive (G36-GAMES) holds your games and everything that goes with them.
It's safe to open and edit on any computer.

roms/<console>/    Your games, one folder per console, using ES-DE folder names
                   (gba, gbc, gb, nes, snes, megadrive, mastersystem, gamegear,
                   pcengine, psx, ngp, neogeo, arcade, cps1, atari2600 ...).
                   Subfolders are fine; they show up as folders in the menu.
                   The menu notices new or removed games on its own.
                   CD games: use .chd files, not .zip. Zipped games are unpacked
                   into memory first, and there's only room for about 90 MB.
                   The CD consoles are:
                     psx          PlayStation
                     segacd       Sega CD
                     pcenginecd   PC Engine CD
                     neogeocd     Neo Geo CD

bios/              BIOS files that some consoles need, directly in this folder
                   (not in subfolders), e.g. neogeo.zip for Neo Geo. The menu
                   warns before starting a game whose BIOS is missing; the list
                   of what each console needs is in g36/bios.cfg.

saves/<console>/   In-game saves (.srm) and save states (.state).
                   Copy this folder off before reflashing the card.

thumbnails/        Game thumbnails, matched to games by ROM file name:
  ready/           images that are already the exact size the menu uses
  to-resize/       an inbox: drop images of any size here; the handheld resizes
                   them at boot, saves them to resized/ and empties the inbox
  resized/         the handheld's resized copies
  videos/          pictures for the Video Player, named after the videos (in the
                   same folders); replace one with your own JPG or PNG

Get Artwork/       A program that gets box art for your games. Plug this card into a
                   computer and double-click "Get Artwork - Windows.exe" (Windows) or
                   "Get Artwork - Mac.command" (Mac). It finds a picture for each game
                   (from thumbnails.libretro.com), saves it in thumbnails/to-resize/,
                   and the G36 turns them into thumbnails by itself. The computer needs
                   internet; the G36 doesn't. Run it again whenever you add games.
                   Games with no picture show a faint note about this in the menu.
                   More in the Read me.txt inside the folder.

collections/       Custom collections: one text file per collection, one game per
                   line, written as its path inside roms/, for example:
                   gba/Pokemon - Emerald Version (USA, Europe).gba
                   The file name is the collection's name: Pokemon.txt shows as
                   "Pokemon". Favorites (g36/favorites.txt) and Recently Played
                   (g36/recent.txt) are kept the same way.

videos/            Videos for the Video Player (on the Tools row). Folders are
                   fine. The screen is 854x480, so 480p MP4 (H.264) looks as sharp
                   as anything bigger and is the easiest on the battery. 720p is
                   tested to play at full speed, even at 2x; 1080p plays too but
                   takes more space and looks no sharper. H.265 (HEVC) may
                   stutter. Where each video
                   was stopped is kept in g36/watch-progress.txt; X in the Video
                   Player resets it.
                   Subtitles: the video's own tracks (MKV, MP4), or a file with
                   the same name beside it (Episode.srt, Episode.en.srt, also
                   .ass .ssa .vtt). The Menu button while a video plays opens
                   the options: subtitles on / off, their track and size, the
                   sound track, speed (0.5x to 2x), picture (fit, zoom, stretch),
                   playing on to the next, a sleep timer, and starting over.
                   They're kept in g36/player.cfg.

music/             Music for the Music Player (on the Tools row). A folder plays
                   through in order, with the album art from the file or a
                   cover.jpg beside the songs.

CREDITS.txt        Who made the logos, models, emulators and tools this uses, and
                   what they asked for in return (credit; never to be sold).

LICENSE.txt        The MIT licence the programs written for this project are under:
                   use, change and share them however you like.

SOURCE.txt         Where the source code of everything on the card is, and each part's
                   licence (the firmware is free software; you can have its source).

imagegen/          The Image Generator (on the Tools row): imagegen/models/ holds
                   its three models (about 230 MB), and each picture it draws is
                   saved in imagegen/pictures/ as a PNG, with its prompt, seed
                   and steps in prompts.txt. The Pictures card (also on the Tools
                   row) is a gallery of them: A views one, X deletes one. Drawing
                   a picture takes minutes (about 24 s a step: 20 steps is about
                   9 min). Select + X stops one that's drawing.

theme/logos/       The logos on the menu's cards: <console folder>.png (gba.png,
                   megadrive.png ...) or <collection name>.png ("All Games.png").
                   Replace them with your own, or delete one to show the name.
                   Console logos by Dan Patrick (his platform logo set, made for
                   LaunchBox), used with credit; never to be sold.
                   Collections without a logo show their name in Montserrat
                   (theme/fonts/, SIL Open Font License: see OFL.txt there).

g36/theme.cfg      The menu's colours, set with the Theme card on the Tools row
                   (one RRGGBB value per line; delete the file for the default).

g36/               Settings (settings.cfg), the console list (systems.cfg),
                   per-game emulator choices (game-cores.cfg) and logs.

The emulators live on the other drive, G36-EMU, in its cores/ folder.

USING THE HANDHELD
==================
Buttons: hold Start in the menu or in a game to see every button. In a game,
Select + B quits, Select + R and Select + L save and load a state, Select + X
fast-forwards, and Menu opens RetroArch's own menu.

Power and sleep
- Tap Power in the menu: the screen goes off or on. Hold Power for 3 seconds:
  save and power off (in a game it saves first). The screen turns itself off
  after 3 minutes and the handheld powers off after 10 (Settings changes both);
  at 20% battery it warns you, and at 10% it saves and powers off.
- Sleep: in the menu, Power > Sleep or Settings > Sleep; in a game, tap Power
  twice. Press Power to wake. In a menu it's a real sleep. In a game it isn't:
  the game saves a state and closes, the handheld sleeps, and on waking the
  same game starts again with that state loaded.
- Sleep is still new. Two things have happened while testing it: a game can
  crash on waking, and ADB (below) can stop working until you restart. If a
  game crashes on waking, the next time you start it from the menu you're asked
  "Load the sleep save state?": "Load state" picks up where you went to sleep,
  "Start normally" starts the game fresh. Your normal in-game saves are not
  touched either way.
- The first three times you start a game, a tip about sleeping and powering off
  appears; "Hide tips" stops them.

File transfer and USB
- Plug the handheld into a computer with a data cable. It asks "Computer
  connected: allow file transfer?"; Allow makes both drives show up on the
  computer, and you can keep playing while it's on.
- USB debugging (ADB) is for developers and is off by default. Settings > System
  > USB debugging (ADB) turns it on or off (USB is set up again within a couple
  of seconds). If the computer stops seeing the handheld after a sleep, restart
  it.

Settings (Tools row)
- Brightness and volume (and what they start at), the menu's click volume, when
  the screen turns off and the handheld powers off, the battery warning and
  save-and-power-off levels, the date and time (the handheld has no clock chip:
  the time is kept while it's on and falls behind while it's off, so set it
  when it's wrong), the clock's 12 or 24 hour format, rescanning the library, and
  Sleep, Restart (only on a charger) and Power off.
- Restore default settings puts the settings above back to how the handheld
  came, except USB debugging and the date and time. Your games, saves,
  favorites, theme and pictures are not touched.

SD cards
- Any microSD from 4 GB up to 2 TB works (SDHC and SDXC). The new 2 TB+ kind
  (SDUC) doesn't. Tested with 64 GB and 256 GB cards; the games drive fills the
  whole card either way.
- The handheld reads cards at up to about 23 MB/s (the older "high speed" mode),
  so a faster card (U3, V30, A2) is no quicker in it than any Class 10 card.
- The system uses the first 1.6 GB; G36-GAMES fills the rest.

Reflashing the card erases this drive, so copy saves/ (and anything else you
want to keep) off first. Power off from the menu (hold the power button) rather
than letting the battery run flat.

# Installing on a modded Nintendo Switch

This port is early. Read "How it runs" and "Not tested" first.

**Tested on one console only:** an original, unpatched Switch 1 on firmware 22.1.0 with Atmosphère 1.12.1, in handheld mode.

Your console must already be able to install and run NSP files that don't come from the eShop. This guide doesn't cover setting that up.

## What you need

- `svr2011.nsp` from this release. It holds the port's program and shaders, and no game assets or keys.
- The PC version of this port, already set up from your own copy of the game.
- About 6 GB free on the SD card. Our card is 64 GB, formatted FAT32.

## Copy the game files to the SD card

Create a folder called `SvR2011` at the top of the SD card, then copy these from your PC game folder:

```
SD card
└── SvR2011
    ├── default.xex
    ├── plist360.arc
    ├── plist360_4x3.arc   (if you have it)
    ├── pac
    ├── sound
    ├── movies
    ├── DLC
    ├── pad_icons
    ├── nxeart
    └── native_shaders
```

To reach the card from the PC, we used Hekate's USB mode (the card shows up as a drive). The game creates its settings, `Saves` and `logs` itself on first start.

## Install

Install `svr2011.nsp` with the NSP installer you normally use, then start it from the home menu.

## First start

The first start builds shaders, so it takes longer than later ones. Later loads are also not instant: expect a wait before Press Start.

## Settings

- The frame rate defaults to **30 fps**, and you can change it in OPTIONS → GRAPHICS → FRAME RATE. The limit applies in matches; menus run at 60.
- The game sets the console's clocks itself to CPU 1785 MHz, GPU 768 MHz and memory 1600 MHz, in handheld too, at either frame rate. This drains the battery faster and makes the console warmer than normal games.
- Don't run a separate overclocking tool at the same time. We haven't tested that.

## How it runs

Measured at the 60 fps setting, in handheld:

| Scene | Frame rate |
|---|---|
| Practice ring | 55 to 60 |
| Match | 45 to 50 |
| Entrances | about 30, and they lag |

**60 fps isn't reached in matches or entrances.** The graphics chip is the limit there.

## Updating

Delete the old copy from the console, then install the new NSP. Your files in `SvR2011`, including saves, stay on the SD card.

## If something goes wrong

Logs are in `SvR2011/logs` on the SD card:

- `svr2011_NNN.log`: the game's log. The highest number is the latest run.
- `nx_crash.txt`: added to after a crash. It can also hold older crashes.

To report a problem, send the latest log and `nx_crash.txt`. Say what you were doing, and give your console model, firmware and Atmosphère version.

## Not tested

- Docked play
- Switch Lite, OLED, or patched and later consoles
- Other firmware or Atmosphère versions
- exFAT SD cards
- Online play
- Mods
- Running with an overclocking tool

# Installing on a modded Nintendo Switch

This port is early. Read "How it runs" and "Not tested" first.

**Tested on one console only:** an original, unpatched Switch 1 on firmware 22.1.0 with Atmosphère 1.12.1, in handheld mode.

Your console must already be able to install and run NSP files that don't come from the eShop. This guide doesn't cover setting that up.

## What you need

- The PC version of this port, already set up from your own copy of the game.
- DBI on the console, to install the NSP.
- About 6 GB free on the SD card, plus anything you add. Our card is 64 GB, formatted FAT32.

## Make the SD card from the PC launcher

1. Connect the SD card to your PC. We used Hekate's USB mode, which shows the card as a drive.
2. In the launcher, open **Setup → Switch**.
3. Tick anything you want to add: your saves, mods, or music and custom movies.
4. Click **Make Switch files**, then choose the top of the SD card. You can also choose a folder and copy its contents to the top of the card later.

The card then has:

```
SD card
├── svr2011.nsp             the program and its shaders, with no game files or keys
├── Install on Switch.txt
└── SvR2011                 the game as loose files, with your saves, mods and music
```

Run it again whenever you like: it only copies what changed. If you tick your saves, they replace the ones on the card, and the card's old saves are kept in `SvR2011/SaveBackups`.

## Install

1. Put the card back in the console and start DBI.
2. Open **Browse SD card**, select `svr2011.nsp` and install it.
3. Start the game from the home menu.

You can delete `svr2011.nsp` from the card once it's installed.

## Saves, mods and music

Everything is in the `SvR2011` folder on the card, so you can also copy files there yourself: `Saves`, `Mods`, `Music` and `Custom Movies`.

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

Delete the old copy from the console, then install the new NSP with DBI. To get the new NSP and any changed files on the card, run **Setup → Switch** again. Your saves on the card stay unless you tick your saves.

## Report a problem

Use **MY WWE → OPTIONS → REPORT A PROBLEM**, or **REPORT A PROBLEM** in the pause menu during a match. The console needs an internet connection.

A report sends what you type, a screenshot if you choose, the game's logs and crash notes, your settings without any account details, and your main save and recorded match if you choose. IP addresses in the logs are blanked. Mods aren't sent.

You get a report number on screen. Keep it if you want to ask about that report.

If the console can't connect, the logs are in `SvR2011/logs` on the card: `svr2011_NNN.log` with the highest number is the latest run, and `nx_crash.txt` holds crash notes, which can include older crashes.

## Not tested

- Docked play
- Switch Lite, OLED, or patched and later consoles
- Other firmware or Atmosphère versions
- exFAT SD cards
- Online play
- Mods, custom music and custom movies on the console
- Running with an overclocking tool

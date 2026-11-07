# Building the Switch program

This builds `svr2011.nsp` from this repository and your own copy of the game. It's also the route for changing the LGPL parts (FFmpeg, libmspack) and relinking: edit them under `<work>/rex/thirdparty`, then run `build` and `nsp` again.

`port/switch/tools/build_switch.sh` runs every step. Status of each step: see "What has been checked".

## You need

| What | Where it comes from |
|---|---|
| MSYS2 and devkitPro | MSYS2 at `C:\msys64`, with `pacman -S switch-dev switch-mbedtls switch-zlib switch-libzstd switch-libexpat mingw-w64-x86_64-gcc cmake ninja git`. Python on the PATH |
| This repository | The branch that holds the Switch port, with `port/patches` |
| The graphics driver | mesa-switch commit `0ff52079ca9e` (MIT), built by that project's GitHub Actions. Unpack it so `lib/libvulkan.a` exists, and set `NX_DRIVER`. See `port/switch/tests/vktest/DRIVER.txt` |
| `rexglue.exe` | From the PC SDK build (`recomp/rexglue-sdk`, see `port/build.ps1`). It only generates code, and runs on Windows |
| `default.xex` | Your own copy of the game |
| The `.spvc` shaders | `port/tools/convert_shaders.py` output (`port/runs/shaders_native/spirv`), made from shaders captured from your own copy |
| `prod.keys` | Your own, dumped from your own console. Only the NSP step needs it |

## Folders

The devkitPro compiler is a Windows program. It reads `/work/...` from the root of the current drive, and it can't read MSYS paths such as `/e/...`. So everything lives under `/work`, with no spaces:

```bash
cmd //c "mklink /J C:\\work C:\\msys64\\work"
```

Clone this repository into `C:\msys64\work`, or link it there with another junction. Run the script through that `/work/...` path. The script stops if it's run from anywhere else. A work folder on another drive also works through a junction under `C:\msys64\work`.

## Build

Run these from MSYS2's bash, in `port/switch/tools`:

```bash
export NX_WORK=/work/nx NX_DRIVER=/work/nvk-sdk
```

```bash
bash build_switch.sh sources
```

```bash
NX_XEX=/path/to/default.xex NX_REXGLUE=/path/to/rexglue.exe bash build_switch.sh codegen
```

```bash
bash build_switch.sh build
```

```bash
NX_KEYS=/path/to/prod.keys NX_SPVC=/path/to/spirv bash build_switch.sh nsp
```

The result is `$NX_WORK/pk/nsp/0500565220110000.nsp`. The keys are read by hacBrewPack only, and never go in the NSP or the logs.

To test a change without packing an NSP, use `$NX_WORK/b/svr2011.nro`: copy it to `SvR2011/update/svr2011.nro` on the SD card, and the installed NSP starts it instead of its own copy.

## Test builds and release builds

Report a problem sends to the public server only when the build is made with `NX_LIVE=ON`. Without it, a build sends only to the `online_server` in `svr2011.toml`, refuses the public server, and can trust a private server's certificate from `SvR2011/nx_test_ca.pem`. A release build ignores that file.

## Sources this is pinned to

| Part | Source | Commit | Changes |
|---|---|---|---|
| rexglue SDK | github.com/rexglue/rexglue-sdk | `c94f5eb` (v0.10.0) | `rexglue-sdk-svr2011.patch`, then `rexglue-sdk-switch.patch` |
| FFmpeg (SDK submodule) | github.com/wmarti/FFmpeg | `0604b46` | `ffmpeg-switch.patch` |
| plume | github.com/renderbag/plume | `d723793` | `plume-svr2011.patch`, then `plume-switch.patch` |
| nx-hbloader | github.com/switchbrew/nx-hbloader | `82b95122c5ae` | `port/switch/tools/hbl_patch.sh` |
| hacBrewPack | github.com/XITRIX/hacBrewPack | `745b16e` | none |

## What has been checked

Run on 7 Nov 2026 from a fresh clone of this branch, on the PC that already had devkitPro installed:

- **`sources`:** fetched everything from the addresses above and applied every patch. The SDK and plume then matched the working copies, ignoring line endings.
- **`codegen`:** generated the code, using an existing `rexglue.exe`.
- **`build` and `nsp`:** built the whole SDK and game and made an NSP, using an existing set of `.spvc` shaders. No part of `prod.keys` is in the NSP, the program or the patch.

Not checked yet:

- This run's NSP on a console.
- A PC with nothing set up beforehand.
- Building `rexglue.exe` and the `.spvc` shaders from a clean checkout.


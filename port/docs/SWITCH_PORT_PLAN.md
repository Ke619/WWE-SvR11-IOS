# SvR 2011 on Switch 1: research and plan

Research only. Nothing has been built or run on a Switch yet. Each claim is marked:

- **[Measured]**: from our own code, logs or files.
- **[Source]**: read in another project's code or docs (links at the end).
- **[Estimate]**: my own arithmetic. Treat it as a guess until it's measured on a console.
- **[Memory]**: from what I remember, not checked in a source. Confirm it on a console.

## 1. The short answer

A native port is possible. It has already been done for a game built the same way, and most of our port carries over.

- **The closest example:** UnleashedRecomp-NX is Sonic Unleashed, also an Xbox 360 game recompiled to C++, running as a native Switch program on Vulkan. It reached version 1.0.0 in October 2026. **[Source]**
- **What we already have:** an ARM64 build (Android) and a Vulkan renderer, and we draw through plume, the same graphics layer that port patches for Switch. **[Measured]**
- **The main risk is CPU speed.** 60 fps at stock clocks isn't realistic at first.
  - First target: **30 fps in matches, docked, with an optional CPU boost**.
  - 60 fps would need the game's frame cost roughly halved, which the Mali work already found (`svr2011-mali-60fps`).
- **The main unknown is memory mapping.** Our SDK maps the same physical memory at several guest addresses, and the Sonic port doesn't appear to need that. This needs a small test on a real console before anything else (section 3).

## 2. The hardware

| | Switch 1 | Source |
|---|---|---|
| CPU | 4 Cortex-A57; 1020 MHz stock in games | **[Source]** |
| Cores for the game | 3 (the fourth is kept for the system) | **[Memory]** |
| CPU boost | 1785 MHz on older units, 1963 MHz on Mariko units; more heat and battery | **[Source]** |
| GPU | Maxwell GM20B; 768 MHz docked, 460 MHz handheld; 921 MHz or more with an overclock | **[Source]** |
| Memory for the game | **3285 MB** on a 4 GB console, in the Atmosphère kernel's table. Only a full title gets this, so an installed NSP is the right choice | **[Source]** |

## 3. The biggest unknown: guest memory

**How our SDK does it [Measured]** (`recomp/rexglue-sdk/src/system/xmemory.cpp`):
- It creates one 4.5 GB file mapping and maps views of it.
- 0xA0000000, 0xC0000000, 0xE0000000 and the raw 0x100000000 range all point at the **same physical memory**.

Horizon has no `mmap` and no file mappings, so this has to be rebuilt.

**What the Sonic port does [Source]** (`UnleashedRecomp/kernel/memory.cpp`):
- It reserves a 4 GB window in the address space and commits pages only when they're needed.
- Each commit runs `memalign`, then `svcMapProcessCodeMemory`, then `svcSetProcessMemoryPermission(RW)`, then `svcMapProcessMemory` into the window.
- It maps each range once, as far as I read the code. It doesn't need aliases.

**What we need to find out:**
1. Can `svcMapProcessMemory` map one source at several addresses, with writes visible through all of them? Switchbrew doesn't say. **This is test 1 in section 9.**
2. If not: does the game really touch the same page through two views? Xenia-style games usually keep to the view they allocated from, and the raw range is used by the runtime. We can count this on PC first. Then the SDK could route its own raw-range access through a lookup, with one mapping per page.
3. Commit on demand is required. We can't back the whole range with real memory.

**Other points:**
- Generated code already uses `REX_PHYS_HOST_OFFSET`, which is 0 on Android (`svr2011-phys-offset`). Switch would use 0 too, since its pages are 4 KB.
- The SDK registers an MMIO fault handler. Whether our native renderer still depends on it needs checking. libnx has a user exception handler, but I haven't confirmed that it can resume after a fault the way Android signals do.

## 4. Graphics

**Recommendation: Vulkan through mesa-switch (NVK), linked statically.**

- mesa-switch has a Horizon backend written against libnx, needs no Linux DRM, and links as `libvulkan.a` with no loader. It's MIT licensed and based on Mesa 26.2.4. **[Source]**
- Three native Switch projects use NVK: OpenTrickyNX, the Sonic port and nfsmw-nx. **[Source]** None of them is a proven long-term stable base: OpenTricky is still marked experimental.
- Our Vulkan backend is 278 lines on top of plume, plus the renderer. **[Measured]**

| Item | Finding | Action |
|---|---|---|
| plume | The Sonic port has a `PLUME_PLATFORM_SWITCH` patch | Write our own, using it as a reference (licence note in section 8) |
| Presenter | The SDK presenter has Android/XCB/Wayland/Metal surfaces **[Measured]** | Add an NWindow surface |
| Constants | Sonic's big GPU win was uniform buffers instead of 64-bit pointers **[Source]** | We already have this path: `native_compact_tables` (`.spvc`) |
| Shader compile | NVK compiles on the console, so the first use stutters **[Source]** | Keep a pipeline cache; build shaders on a "first start" screen |
| Device lost | Four Sonic renderer shortcuts lost the GPU in long play **[Source]** | Soak-test every renderer shortcut for 30+ minutes |

**Rejected: deko3d.** It's the mature homebrew API, but it isn't Vulkan. We'd need a new renderer backend and a new shader path. Keep it only as a fallback if NVK proves too slow or unstable.

## 5. CPU: what we can expect

**Our numbers [Measured]:**
- A Ryzen 5700X3D uses about 1.07 cores for 60 fps: render thread 25%, logic 25%, GPU Commands 16%, job thread 12%.
- Galaxy A51 (A73 cores, throttled): about 15 ms to record the render thread plus about 15 ms of game frame.
- The world always runs at 60 Hz (`svr2011-frame-rate`). At 30 fps, each frame still runs **2 updates**, so 30 fps doesn't halve the game cost. It mostly halves the drawing.

**Rough scale to the A57 [Estimate]**, using a guess of 1.25× slower per clock than the A73 at 2.3 GHz. These numbers are probably pessimistic, because the A51 was throttled:

| CPU clock | Game frame | Render thread | Result |
|---|---|---|---|
| 1020 MHz | about 42 ms | about 42 ms | Misses 30 fps as it stands |
| 1785 MHz | about 24 ms | about 24 ms | 30 fps fits; 60 doesn't |
| 1963 MHz | about 22 ms | about 22 ms | 30 fps fits; 60 doesn't |

Heavy scenes such as 6-man and Rumble will cost more than these numbers.

**What helped the Sonic port [Source]:**
- At 1020 MHz, its CPU-limited run went from 48 to 53.6 FPS at 858x482 across its rounds of work.
- At the GPU limit, it runs about 44 FPS at 1080p; its defaults are 864p docked and 648p handheld.
- Its gains came from these changes, and we can try the same on our generated code:
  - direct calls between recompiled functions
  - PGO + LTO, with `-O2`
  - non-volatile guest memory access
  - `-fno-math-errno -fno-trapping-math`
  - fast critical sections and events
  - a high-priority render thread

**Our own work to do:**
- Threads don't time-slice at high priority on Horizon (the Sonic doc says so for priority 0x2D). **[Source]** Guest spin-waits can stall or hang, so the kernel wait code needs a review.
- 4+ busy threads must share 3 cores, with affinity set by hand.
- The existing `process_priority`, Mali and `small_draws` options are starting points.

## 6. The port's parts

| Part | Today | On Switch |
|---|---|---|
| Compiler | The SDK **requires Clang**; 210 MB of generated C++ **[Measured]** | devkitA64 is GCC. Either clang targeting `aarch64-none-elf` against libnx (try first), or make the SDK build with GCC, as the Sonic port's toolchain does |
| Window, input, audio | SDL3 (`window_sdl`, `sdl_input_driver`, `sdl_audio_driver`) **[Measured]** | I found no SDL3 for Switch in devkitPro. Write a small platform layer: NWindow, `hid`, `audout`. Sonic moved audio to `audout` to fix silence under load **[Source]** |
| Threads, files | `threading_posix`, `filesystem_posix`, `fiber_posix` | Mostly reusable through libnx/newlib; needs testing |
| Memory | `memory_posix` + file mappings | New Horizon version (section 3) |
| Code size | Android `libmain.so` is 227 MB **[Measured]** | Horizon loads it all into memory **[Memory]**. `.text.hot`, `-O2` and removing the emulated GPU plugin all help |
| Plugins | `gpu_plugin_loader`, `dlopen` | Static only; the native renderer doesn't need the plugin |
| Movies | Guest Bink; HD movies through DXVA/MediaCodec | Bink stays. HD movies: FFmpeg `nvtegra` uses the hardware decoder **[Source]** |
| Music | NDK media | FFmpeg |
| Online | Sockets; on Android, `NetBridge` in Java | libnx BSD sockets, plus libcurl or mbedTLS for HTTPS |
| Keyboard | On-screen | `swkbd` applet |
| Rumble, touch | SDK / touch controls | `hid` vibration; touch in handheld |
| Sleep, dock | Android lifecycle | Applet messages, and rebuild the swap chain when docking changes |

**Memory budget [Estimate]:** 3285 MB minus about 230 MB of code, minus the driver and shader caches, leaves about 2.8 GB for guest memory and GPU copies. The stock game should fit. Large mods that use the big-memory fallback may not, so we'd need a limit and a clear message. Our 4 GB Android reports (`svr2011-android-reports-1104`) show this risk is real.

**Online warning:** online play locks to 1 update per frame at 60 fps (`SetLockstep`). A Switch that can't hold 60 would slow its opponent. Decide this after real numbers: Switch-only matches, or block online if it can't keep up.

## 7. Launcher, NSP and updates

### PC launcher: new "Switch" tab, next to the Android tab
Modelled on `launcher/apk_package.c` (838 lines).

1. The user selects their game folder and their **own `prod.keys`**, dumped from their console with Lockpick_RCM. **We never ship keys.**
2. The launcher builds the program (`main`, `main.npdm`), the icon and title data (NACP), and a romfs from the game files.
3. It runs hacBrewPack (GPL-2, shipped as a separate program) to make the `.nsp`. **[Source]**
4. It writes a how-to: Atmosphère with signature patches is required, and how to install.

Things to plan for:
- **Size:** about 5.5 GB (pac 3.1, sound 1.5, movies 0.8, shaders) **[Measured]**, not counting Windows files or Mods (4.3 GB here). That's over FAT32's 4 GB limit, so the guide needs exFAT, split files or USB install (for example DBI).
- **NPDM:** 64-bit, 39-bit address space, application type, all needed SVCs, and a `SystemResourceSize` if we use `svcMapPhysicalMemory`.
- **Signature patches break** after firmware or Atmosphère updates **[Source]**, so this goes in the guide and the error messages.
- **Game files:** the NSP contains the user's game data, so it's for their own console and must not be shared. The guide should say so.

### Files on the console
- **Read-only game files:** `romfs:/`, inside the NSP.
- **Everything that changes:** `sdmc:/SvR2011/` (Saves, UserData, Mods, logs, Reports, pipeline cache, updates).
- A file found on the SD card wins over the romfs copy, so mods and small updates need no reinstall.

### The launcher on the console
- It's a screen built into the same program, shown **before the game starts**, drawn with the SDK's ImGui: Play, Settings, Mods on/off, Online account, Updates, Report a problem.
- This is not a copy of Android's 6,303 lines of Java. Converters, Mod Maker and Paint stay on PC.
- The in-game OPTIONS and GRAPHICS pages carry over. They need a boost toggle and a handheld/docked resolution.

### Updates: both of the ways you asked for
1. **Reinstall an update NSP**, like a normal game: the launcher builds one with a higher version number. Whether the patch type (title ID + 0x800) works without a full romfs needs a test, and I haven't confirmed that hacBrewPack can make one.
2. **In-app update (recommended as the everyday path):** the installed `main` is a small loader, based on `nx-hbloader` (ISC). It starts the newest program, either `sdmc:/SvR2011/bin/svr2011.nro` or the copy in romfs.
   - The in-game updater then downloads only the new program and changed files from our GitHub releases (same source as `launcher/updater.c`).
   - No reinstall is needed, it keeps full title memory, and OpenTrickyNX uses the same loader for its NSP. **[Source]**
   - Installing an NSP from inside the game would require ncm and es services and is much riskier. I don't recommend it.

## 8. Licences and rules

- mesa-switch, plume: MIT. nx-hbloader: ISC. hacBrewPack: GPL-2 (separate program, fine).
- **UnleashedRecomp-NX and OpenTrickyNX are GPL-3.** Reading them for ideas is fine. Copying code would bring GPL-3 terms into what it's merged with, so anything copied must be a deliberate decision.
- Only libnx and open tools. **Never Nintendo's SDK.** No game data or keys in our releases.

## 9. Order of work

Each step has a pass or fail check, so a dead end shows up early.

| # | Step | Pass when |
|---|---|---|
| 1 | **Console test: memory aliases.** Tiny program maps one block at 3 addresses and commits 1.5 GB | Writes show through all views; memory stays stable |
| 2 | **Console test: NVK + NSP.** Triangle through plume from a hand-built NSP | Installs, starts from the home menu, reports 3285 MB |
| 3 | **PC count: alias use.** Log which views the game touches | Tells us how much step 1 matters |
| 4 | **Toolchain.** Clang or GCC builds generated code + SDK for Switch, without SDL3 | One `main` links |
| 5 | **Platform layer.** Memory, threads, files, exceptions, window, pad, audio | Reaches the title screen |
| 6 | **Native renderer.** Shader pack, swap chain, pipeline cache | Main menu, then a 1v1 match |
| 7 | **Measure, decide targets.** Real CPU and GPU per scene, stock and boosted | Target fps and resolution agreed with you |
| 8 | **Optimize.** Direct calls, PGO/LTO, sync, renderer rounds | One switch per change, before and after |
| 9 | **Launcher + NSP.** PC tab, console launcher, updates | Clean install from a fresh folder |
| 10 | **Online, reports, sleep, long play.** Soak tests | Long sessions without device-lost or leaks |

**Testing limit:** no PC tool can check this for us. NVK needs the real GPU, so every console test depends on the console. The fastest loop is Wi-Fi: upload builds over FTP or `nxlink` and read logs back. Scripted input (`SVR2011_INPUT_FILE`) should still work, but someone has to start each run.

**Build rule:** the toolchain needs devkitPro (MSYS2 or Docker) and Rust if we build Mesa ourselves, though prebuilt NVK packages exist. Builds follow the current low-priority 4-core rule.

## 10. Decisions from the user (6 Nov 2026)

- **Console:** original unpatched Switch, modded, on firmware 22.1.0 with Atmosphère 1.12.1. This is the older chip, so the limits are CPU 1785 MHz and GPU 921 MHz.
- **Target:** 30 fps first. The user expects 60 fps to be possible with the faster CPU, GPU and memory clocks, so 60 stays the goal. Real numbers come at step 7.
- **Boost:** off by default, and on when the player selects 60 fps.
- **Tools:** devkitPro and Docker approved. devkitPro is installed in `C:\msys64` (`/opt/devkitpro`). Docker isn't installed yet, because nothing needs it so far. Its likely first use is building mesa-switch from source.
- **Build note:** devkitPro's `make` fails on paths with spaces, so builds run from `/work` inside MSYS2.

## 11. Console results so far (6 Nov 2026)

Measured on the user's console (original unpatched, FW 22.1.0, AMS 1.12.1), from the Homebrew Menu in title mode.

| Check | Result |
|---|---|
| One block at several addresses (`svcMapProcessMemory`) | Works at 4 addresses plus a shifted one, writes visible through all |
| Memory held | 2816 MB in 16 MB pieces at 2 MB aligned addresses; 2 GB with the first 1 GB at 3 addresses |
| Separate mappings | About 10,000 at most (3334 x 3, `0xCE01`, repeats exactly over 3 rounds), so commit in big regions, not 64 KB pieces |
| `memalign` | Refuses 2 GB and up (`0x559`, libnx), 1792 MB is fine |
| Cores and threads | Cores 0-2 only; at priority 0x2C same-core threads don't share time, at 0x3B they do |
| Vulkan (mesa-switch, commit 0ff52079ca9e) | 1.4 on GM20B, triangle at 60.5 fps, worst frame 17.2 ms; BDA, int64, descriptor indexing, BC textures present; heap 2391 MB |

**Not measured yet:** driver memory use (the used-memory figure is flat because the loader reserves the heap up front), real game shaders, and the installed NSP route.

**Compiler:** devkitA64's GCC compiled one generated file (`svr2011_recomp.101.cpp`, 1 MB source, 420 KB code at `-O2`, `-std=gnu++23`) once the SDK headers had stand-ins for Linux, `sys/syscall.h` and `ucontext.h`. So GCC is the first choice, and the SDK's Clang-only check must go. `ucontext.h` doesn't exist on Horizon, so fibers need another way if the game uses them.

**Step 3 (PC count of alias use) is dropped:** aliases work on the console, so it no longer decides anything.

## Sources

- UnleashedRecomp-NX: https://github.com/ChanseyIsTheBest/UnleashedRecomp-NX (`docs/SWITCH-PERFORMANCE.md`, `docs/SWITCH-PERFORMANCE-AUDIT.md`, `docs/SWITCH-MESA.md`, `UnleashedRecomp/kernel/memory.cpp`, `toolchains/switch-devkitA64.cmake`)
- Earlier fork: https://github.com/NaGaa95/UnleashedRecomp-NX
- mesa-switch: https://github.com/danfromtico/mesa-switch
- OpenTrickyNX: https://github.com/DevCrashDay/OpenTrickyNX (`docs/switch.md`)
- Switchbrew: https://switchbrew.org/wiki/SVC and https://switchbrew.org/wiki/NPDM
- libnx JIT code: https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/jit.c
- Atmosphère kernel (`GetApplicationPoolSize`): https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/board/nintendo/nx/kern_k_system_control.cpp
- hacBrewPack: https://github.com/xitrix/hacbrewpack
- deko3d: https://github.com/devkitPro/deko3d
- FFmpeg nvtegra: https://ffmpeg.org/pipermail/ffmpeg-devel/2024-May/328549.html
- Clocks: https://www.gamebrew.org/wiki/Horizon_OC_Switch and https://wccftech.com/overclocked-nintendo-switch-tested-by-digital-foundry-tegra-x1-gets-significant-performance-boost/
- Forwarder and signature-patch notes: https://pypi.org/project/nton/

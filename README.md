# WWE SmackDown vs. Raw 2011 — iOS Port

This is an unofficial iPhone port of the 2010 Xbox 360 game *WWE SmackDown vs. Raw 2011*. It builds on KaikoClanworth1's [native PC port](https://github.com/KaikoClanworth1/wwe-svr2011-pc) and isn't an emulator. The game's code is statically recompiled to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and built into an iOS app. It runs on the SDK's Xenia-derived runtime and draws with the PC port's native renderer on Vulkan, which [MoltenVK](https://github.com/KhronosGroup/MoltenVK) translates to Apple's Metal.

> [!IMPORTANT]
> **This repository has no game in it.** It holds no disc image, XEX, game data, movies, music or recompiled game code. You need your **own copy of the Xbox 360 game**.

> [!NOTE]
> **This is a fork of the PC port.** The game-side work (the recompilation, the native renderer, the saves, the menus and most features) is KaikoClanworth1's. This fork adds the iOS layer: the memory fixes, the Apple build, the app and the touch setup. See [Credits](#credits). For anything the two ports share, the [original README](https://github.com/KaikoClanworth1/wwe-svr2011-pc#readme) is the full reference.

---

## Status

Tested on an **iPhone 15 Pro (A17 Pro)**, running inside LiveContainer:

- Menus, character select, entrances and matches all work, at 60 fps in testing.
- On-screen touch controls, which you can rearrange.
- Saves are plain files in the game folder.
- On the iPhone's wide screen, matches and entrances fill the whole screen.

Other iPhones and iPads haven't been tested. It's an early port, so expect rough edges. See [What carries over from the PC port](#what-carries-over-from-the-pc-port) for what is and isn't available.

<!-- TODO(author): add a tested-devices table here once more people have tried it. -->

---

## Screenshots

> **TODO (author):** add screenshots of the port running on an iPhone: title screen, a match with the touch controls, and the graphics page. Put them in `docs/screenshots/` and show them here, for example:
>
> ```html
> <table>
>   <tr>
>     <td width="50%"><img src="docs/screenshots/ios-title.jpg" alt="Title screen"></td>
>     <td width="50%"><img src="docs/screenshots/ios-match.jpg" alt="A match with touch controls"></td>
>   </tr>
> </table>
> ```

---

## Contents

- [Status](#status)
- [What you need](#what-you-need)
- [Install and play](#install-and-play)
- [Controls](#controls)
- [Settings](#settings)
- [What carries over from the PC port](#what-carries-over-from-the-pc-port)
- [Building from source](#building-from-source)
- [Troubleshooting](#troubleshooting)
- [Credits](#credits)
- [Legal](#legal)

---

## What you need

| | |
|---|---|
| 💿 **The game** | Your own copy of the *WWE SmackDown vs. Raw 2011* **Xbox 360** game, as a disc image (`.iso`). The PS3, Wii, PS2 and PSP versions won't work. |
| 📱 **iPhone** | Tested on an iPhone 15 Pro. The app is built for iOS 17 or later. Other devices are untested, and older or lower-memory ones may not run it. <!-- TODO(author): confirm the minimum iOS version. --> |
| 💾 **Storage** | About 6 GB: 5.3 GB for the game, plus room for saves and caches. |
| 🔧 **A way to sideload** | This can't go on the App Store. Use AltStore, SideStore, LiveContainer, or any tool that installs an IPA. It has been tested through LiveContainer. |
| 🎮 **Controller** | Optional. The on-screen touch controls work without one. <!-- TODO(author): say whether physical controllers work, and which. --> |

---

## Install and play

1. **Get the app.**
   - **Option A: download the IPA** from [**Releases**](https://github.com/Ke619/wwe-svr2011-ios/releases/latest). It holds no game data, but it does contain code generated from the game's executable, so download it only if you own the game.
   - **Option B: build it yourself.** See [Building from source](#building-from-source). This keeps all game-derived code on your own machine.

   <!-- TODO(author): if you decide not to publish an IPA, delete Option A. -->

2. **Install it with your sideloading tool.** Add the IPA in AltStore, SideStore or LiveContainer, and follow that tool's steps. Apps signed with a free Apple ID usually expire after 7 days, and need refreshing in your tool.

3. **Put the game on the phone.**

   > **TODO (author):** describe exactly how a user gets their game files into the app, step by step. For example: an in-app install from the `.iso`, or copying a game folder into the app's storage with the Files app. Say where the files must end up, and how big the folder should be afterwards.

4. **Start the app and play.**

**Updating:** install the new IPA over the old one. Your saves are in the game folder inside the app. <!-- TODO(author): confirm that saves survive an update with your sideloading tool, and say which tools you checked. -->

---

## Controls

**On-screen controls.** The menus get a simple pad: D-pad, A/B/X/Y, bumpers (LB/RB), triggers (LT/RT), and Back and Start. **MENU** (top left) and **EDIT** (top right) are always there. **EDIT** lets you move and resize the controls. Your layout is saved in `UserData/touch_layout.txt` in the game folder.

iOS uses the same touch-control system as the PC port's Android build, so the match controls and the layout editor work the same way. The original README describes them in its [Android section](https://github.com/KaikoClanworth1/wwe-svr2011-pc#android).

**Physical controller.** <!-- TODO(author): fill in once you've tested one. -->

The game's main menu has an **EXIT** entry at the bottom.

---

## Settings

Open **My WWE → Options → Graphics** in the game. It has two tabs (**LB / RB**). **Display** holds the basics. **Quality** trades looks for speed, and its changes apply at once.

| Quality setting | What it does |
|---|---|
| **Render resolution** | The most the game renders at. Lower is faster. |
| **Anti-aliasing** | Smoother edges, at a cost in speed. |
| **Textures** | **High**: as the game has them. **Medium** / **Low**: big textures use the game's own half / quarter-size versions, which saves memory and loading time. Menus and fonts stay sharp. |
| **Shadows & effects** | **High**: at the render resolution. **Normal**: at the Xbox 360's size, much less GPU work. |
| **Entrance frame rate** | **60 fps**, or **30 fps** as on the Xbox 360. Entrances and cutscenes then need half the work. |
| **Wide screens** | **Full width**: on wide screens, matches and entrances fill the screen with a wider view. The HUD and menus stay 16:9. **16:9** keeps the original picture with bars. |

**Renderer.** The iOS build uses the **Native** renderer only (Vulkan through MoltenVK). There's no Emulated fallback, as there is on the PC.

Settings are saved in `svr2011.toml` in the game folder. <!-- TODO(author): confirm the file name and location on iOS. -->

---

## What carries over from the PC port

The game itself is the same, so most of the PC port's features are in the code. What's different is that the PC port's **launcher** is a Windows program, so anything that needs it isn't available on the phone itself. This table is honest about what has and hasn't been checked on an iPhone.

| Feature | On iOS |
|---|---|
| Native renderer, 60 fps | ✅ Working |
| Touch controls | ✅ Working |
| Saves as plain files | ✅ Working |
| Wide-screen matches and entrances | ✅ Working |
| Up to 10 HD logos per superstar | ❓ Set up, not tested in play |
| Entrance movies | ❓ The game reads `Custom Movies/`, but the movie maker is a Windows launcher tool. Movies made on a PC may work if you copy them over. |
| DLC | ❓ Untested |
| Mods (`.svrmod`) | ❓ Untested. The Mod Maker is a PC tool. |
| Texture packs | ❓ Untested |
| Achievements | ❓ Untested |
| Languages (English, French, German, Spanish, Italian) | ❓ Untested |
| Entrance music from your own songs | ❌ Not available on iOS yet |
| Windows launcher tabs (Paint Tool, Saves, Movies, DLC) | ❌ PC only. You can use the PC launcher on a PC and copy the `Saves` folder across. |
| Online modes | ❌ Not available (same as the PC port) |
| Discord status, Steam Deck, Android | ➖ Not part of this port. See the original README. |

If you test one of the ❓ items, please [open an issue](https://github.com/Ke619/wwe-svr2011-ios/issues) and say whether it worked.

---

## Building from source

The recompiled game code is **generated on your machine from your own game files**. It isn't stored here. The exact commands are in the workflows in `.github/workflows/` (`ios-sdk.yml` and `plume-ios.yml`), which are the source of truth if anything below is out of date.

**You need:**

- A **Mac** with **Xcode** and its iOS SDK.
- **CMake 3.25** or newer, **Ninja** (`brew install ninja`), **Git** and **Python 3**.
- Your own **`default.xex`**, taken from your game files.

**Steps:**

1. Get the **ReXGlue SDK** source at the commit this port was built against (its v0.10.0 release commit):
   ```bash
   git clone https://github.com/rexglue/rexglue-sdk recomp/rexglue-sdk
   git -C recomp/rexglue-sdk checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
   git -C recomp/rexglue-sdk submodule update --init --recursive
   ```
2. Apply the PC port's runtime fixes, then the iOS ones:
   ```bash
   git -C recomp/rexglue-sdk apply ../../port/patches/rexglue-sdk-svr2011.patch
   git -C recomp/rexglue-sdk apply ../../port/patches/ios/0001-ios-memory.patch
   ```
   <!-- TODO(author): list any other iOS patches you add, in the order they apply. -->
3. Get **plume**, the graphics layer the native renderer draws through, and apply the PC port's additions and the iOS changes (which leave out plume's own Metal and AppKit files, so only the Vulkan backend is built):
   ```bash
   git clone --recursive https://github.com/renderbag/plume.git recomp/plume
   git -C recomp/plume checkout d72379344dacd3dbf9f810f92ddc87e6de1845b1
   git -C recomp/plume submodule update --init --recursive
   git -C recomp/plume apply ../../port/patches/plume-svr2011.patch
   ```
   <!-- TODO(author): add the command for your iOS plume patch once it's a patch file. -->
4. Build the SDK and plume for iOS arm64 (with MoltenVK and SDL3), using the commands in `ios-sdk.yml` and `plume-ios.yml`.
5. **Generate the game code** from your `default.xex`, on your own machine.

   > **TODO (author):** write the exact codegen command and say which codegen tool you used. The PC port provides a Windows build of the ReXGlue codegen tool, so say how you ran it for this build.

6. **Build the app and package the IPA.**

   > **TODO (author):** write the Xcode or command-line steps that produce the app, and the signing and packaging step.

**Repository layout:**

| Path | What's there |
|---|---|
| `port/` | The PC port, as it is in the original repo, except where noted. |
| `port/patches/ios/` | The iOS fixes to the ReXGlue SDK's runtime. |
| `ios/` | The iOS app: project, source, entitlements and touch setup. <!-- TODO(author): adjust to what's really in this folder. --> |
| `.github/workflows/` | The iOS build workflows. |
| `docs/` | Install guides and screenshots. |

The Windows build steps (LLVM, PowerShell, the launcher) are in the [original README](https://github.com/KaikoClanworth1/wwe-svr2011-pc#building-from-source).

---

## Troubleshooting

- **The game closed unexpectedly.** A crash report is saved in `UserData/crashes/` in the game folder. Please include it when you report a problem.
- **The app won't open after about a week.** Apps signed with a free Apple ID expire after 7 days. Refresh or re-sign it in your sideloading tool.
- **It's slow.** Open **My WWE → Options → Graphics → Quality**. Set **Shadows & effects** to **Normal**, lower **Render resolution**, turn **Anti-aliasing** off, and set **Textures** to **Medium**. If entrances still stutter, set **Entrance frame rate** to **30 fps**. The first time a scene is shown can hitch while its shaders are prepared. They're kept in `UserData/cache/` for next time.
- **Something is drawn wrong.** Report it with a screenshot and the crash report if there is one. The Emulated renderer isn't available on iOS, so there's no fallback to try.
- **My entrance song isn't in the list.** Entrance music from your own songs isn't available on iOS yet.
- **A superstar's logos 3 and up are blank.** Their images are in `Saves/.logos`. Copy that folder along with the superstar's save file.
- **The costume is missing in the entrance.** Edit the superstar's **Entrance Attire** as well as the match one.
- **Online modes** aren't available in this port.

---

## Credits

| | Who | What |
|---|---|---|
| 📱 | [**Ke619**](https://github.com/Ke619) | The iOS port: the Apple build, memory fixes and app. |
| 🤝 | **TODO (author)** | Your friend who helped. Add their name and what they did. |
| 🧑‍💻 | [**KaikoClanworth1**](https://github.com/KaikoClanworth1) | Author of the [PC port](https://github.com/KaikoClanworth1/wwe-svr2011-pc) this is built on: the recompilation setup, native renderer, saves, menus and features. |
| 🛠️ | [**ReXGlue**](https://github.com/rexglue/rexglue-sdk) | The static recompiler and runtime this port is built on. |
| 🛠️ | [**Xenia**](https://github.com/xenia-project/xenia) | The Xbox 360 emulator whose code the ReXGlue runtime is derived from. |
| 🛠️ | [**MoltenVK**](https://github.com/KhronosGroup/MoltenVK) | Runs Vulkan on top of Apple's Metal. |
| 🛠️ | [**SDL**](https://www.libsdl.org/) | Windowing and input. |
| 🛠️ | [**plume**](https://github.com/renderbag/plume) | The graphics layer the native renderer draws through (MIT license). |
| 🛠️ | [**XenosRecomp**](https://github.com/hedge-dev/XenosRecomp) | Converts the game's shaders. |

The original PC port says it was developed with Claude Code, and has its own AI disclosure. See its [README](https://github.com/KaikoClanworth1/wwe-svr2011-pc#contributors) and `CONTRIBUTORS.md` for the full list of contributors and credits, including the people who made the parts of the PC port that this one inherits.

---

## Legal

- This is an unofficial fan project, made for preservation and research. It isn't affiliated with or endorsed by WWE, 2K, Yuke's, THQ or Microsoft. *WWE SmackDown vs. Raw 2011* and all related names, characters and trademarks belong to their respective owners.
- **No game material is included:** no disc image, XEX, game data, audio, video or recompiled game code. You must own the game and supply your own copy.
- **The IPA contains code generated from the game's executable.** Download it only if you own the game. Building it yourself keeps that code on your own machine.
- The screenshots in `docs/screenshots/` were taken of the port running. They're used only to show the port, and they belong to the game's owners.
- **Recompiler and runtime:** the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which is derived from [Xenia](https://github.com/xenia-project/xenia). See their licenses, and the licenses of MoltenVK, SDL and plume.
- If you're a rights holder and want something removed, please [open an issue](https://github.com/Ke619/wwe-svr2011-ios/issues) or contact me, and I will take it down.

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.8.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.8.apk` on its own for Android. `SvR2011-Mods-v2.1.8.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.x? The launcher and the Android app update themselves - **close the Mod Maker before updating**.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

Another fix release from your **Report a problem** reports - thank you.

## 🛠️ Fixes
- **CREATE A MOVE-SET loaded forever** for some players - fixed (a failed file read left the game waiting). Damaged move-pack copies are now noticed and rebuilt.
- **Matches crashed at the start** with some big sets of ported superstars - a damaged move bank is now skipped instead.
- **The game closed half a second after starting** when game files were missing - it now tells you which files are missing and to reinstall from your disc image (then **Verify game files**).
- **Running out of game memory** with many mods and custom arenas now shows a clear "Out of game memory" message instead of a sudden crash.
- **An online match crashed on a top-rope dive** - fixed. After any crash, the controller no longer keeps vibrating.
- **Direct3D 11 on old NVIDIA cards** (e.g. Quadro 600 / GTX 400-500): textures turning white - the game now uses a safer path there 🧪. If you still see it, please send a report.
- **Every match crashed while loading** with many superstar mods installed (their crowd signs filled the match-load memory) - fixed.
- **CREATE A MOVE-SET froze** when a mod's move name had a stray character in it - fixed, and mod move names are cleaned.
- **Superstar Threads**: in 2.1.7, with a mod's Threads list in use, Threads edits didn't show on anyone - the disc superstars keep their own places again.
- **Sabu** (bundled mod, now 1.8): the camera no longer goes under the ring during his finisher.
- **Mod Maker**: moves ported from **SvR 2007, 2008 and 2009** come over much better - all their parts (cameras, ropes, turnbuckles), their own type of move, and holds and submissions that used to freeze (e.g. Ankle Lock, DDT) now play 🧪.
- **Mod Maker**: if it ever crashes, it now leaves a crash file that **Report a problem** sends, so we can find the cause.
- Reports now carry more detail for a few problems we couldn't reproduce yet (ladder-match briefcase, rare freezes, PlayStation button pictures) - if you hit one, please send a report again.

Everything new in 2.1.7 is in the [2.1.7 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.7).

Third-party: AMD FidelityFX Super Resolution 1 and 2 (MIT licence) and NVIDIA DLSS (NVIDIA's licence, in the zip). See "Third-party notices.txt".

☕ Enjoying the port? **Support me:** https://ko-fi.com/kaikoclanworth · https://www.paypal.com/donate/?hosted_button_id=MS4C2WC38N3W2

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

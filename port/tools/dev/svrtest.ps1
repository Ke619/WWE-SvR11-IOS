# svrtest - one command for a hidden, private test run (docs/DEBUG_SUITE_PLAN.md 2A). For us only: tools\dev is
# never packaged (tools/release_guard.ps1).
#
#   svrtest.ps1 match --p1 EDGE --p2 KANE [--p3 ..] [--attire 2,1] [--arena 9] [--rule 3C] [--entrances]
#                     [--shot 5s] [--shots every:2s:5] [--state 8s] [--fast 2] [--until "<log text>"] [--quit 15s]
#                     [--boot] [--cpu all]   (--boot, dbg only: straight into the match - no title, no menus)
#   svrtest.ps1 entrance --p1 EDGE [--attire 2] [--shots every:6s:20]        (until "entrances over")
#   svrtest.ps1 route <name> [--shot 3s] [--quit 10s]                       (script_input.cpp kRoutes)
#   svrtest.ps1 lan --p1 EDGE --p2 KANE [--arena 1] [--rule 3C] [--pad1 <file>] [--pad2 <file>] [--clock 600,1200]
#                   [--play 30s]                         (two games in a LAN match, lockstep check: svrtest_lan.ps1)
#   svrtest.ps1 open <page> [--shot 3s] [--quit 10s]    (dbg: straight to caw, threads, cae, options:<ROW>,
#                                                        match_creator, universe, menu:<A>B>C> - boot.cpp kPages)
#   svrtest.ps1 script <file> [--shot ..] [--quit ..]                        (script_input.h steps, one a line)
# common: --exe release|dbg|<path>  --backend d3d12|vulkan|d3d11  --mods none|<folder>[,<folder>...]
#         --userdata <dir>  --perf 0|1|2  --log-level info|debug  --name <run name>  --keep (leave it running)
#         --slot <n> (its own game folder runs\svrtest\game<n>: runs side by side; give each its own --userdata)
#         --fresh (the user data may have no save: a first-start test - else svrtest refuses one without
#         Saves\SaveData.dat, the game would sit in its first-time screens)
#         --arg "<game option>" (more of the game's --options, repeatable)
#
# Each run: refuses when the player's game runs or 4 test games already do (any chat); refreshes the private
# game folder runs\svrtest\game (the exe + DLLs copied; pac's own files private copies - the game patches some at
# start - and its sub-folders, sound, movies, native_shaders, pad_icons as read-only junctions to Game Files; its own Mods holding only --mods; private UserData, Music, Custom Movies);
# takes a free p2p port in 36500-36599; starts hidden, muted, behind every window; follows the cues; stops the
# game by its PID; writes runs\svrtest\<stamp>_<name>\ (game.log, shots, result.json) and prints result.json.
#
# Times: "5s", "1500ms" or a number of seconds; shots count from the match's start (the log's "replay recorder
# recording": the match world running - after the entrances when they play; the "match starts" line doesn't come in
# test matches), the entrances' start (entrance: the "test match: people" hand-off), or the route / script's end.
# With --exe dbg a match's cues are the game's own (debug_suite.h: dbg_shot / dbg_state / dbg_quit from the match's
# start): shots read back from the GPU (no window capture), state_<ms>.json dumps (--state), fast-forward (--fast N
# world updates a frame), and the game ends itself at --quit. Other commands keep the window shots.
param([Parameter(Position = 0)][string]$Command = "", [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest)
$ErrorActionPreference = "Stop"
if ($Command -eq "lan") { & (Join-Path $PSScriptRoot "svrtest_lan.ps1") @Rest; exit $LASTEXITCODE }  # (svrtest_lan.ps1)
$tools = Split-Path $PSScriptRoot -Parent
$port = Split-Path $tools -Parent
$gameFiles = if (Test-Path (Join-Path (Split-Path $port -Parent) "Game Files\pac")) { Join-Path (Split-Path $port -Parent) "Game Files" } `
             else { "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\Game Files" }
. (Join-Path $tools "test_guard.ps1")
Add-Type -AssemblyName System.Drawing
if (-not ("SvrTestWin" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SvrTestWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}
[void][SvrTestWin]::SetProcessDPIAware()

# ---- options
$o = @{ p = @{}; exe = "release"; backend = ""; mods = "none"; perf = ""; loglevel = "info"; name = ""; keep = $false; args = @()
        attire = ""; arena = ""; rule = ""; entrances = $false; shot = @(); every = ""; until = ""; quit = ""; userdata = ""; target = ""; state = @(); fast = ""; boot = $false; cpu = ""; fresh = $false; slot = "" }
for ($i = 0; $i -lt $Rest.Count; $i++) {
    $a = $Rest[$i]; $v = if ($i + 1 -lt $Rest.Count) { $Rest[$i + 1] } else { "" }
    switch -regex ($a) {
        '^--p([1-6])$' { $o.p[[int]$Matches[1]] = $v; $i++; continue }
        '^--attire$' { $o.attire = $v; $i++; continue }
        '^--arena$' { $o.arena = $v; $i++; continue }
        '^--rule$' { $o.rule = $v; $i++; continue }
        '^--entrances$' { $o.entrances = $true; continue }
        '^--shot$' { $o.shot += $v; $i++; continue }
        '^--shots$' { $o.every = $v; $i++; continue }
        '^--until$' { $o.until = $v; $i++; continue }
        '^--quit$' { $o.quit = $v; $i++; continue }
        '^--state$' { $o.state += $v; $i++; continue }
        '^--fast$' { $o.fast = $v; $i++; continue }
        '^--boot$' { $o.boot = $true; continue }
        '^--fresh$' { $o.fresh = $true; continue }
        '^--cpu$' { $o.cpu = $v; $i++; continue }
        '^--exe$' { $o.exe = $v; $i++; continue }
        '^--backend$' { $o.backend = $v; $i++; continue }
        '^--mods$' { $o.mods = $v; $i++; continue }
        '^--userdata$' { $o.userdata = $v; $i++; continue }
        '^--slot$' { $o.slot = $v; $i++; continue }  # (a game folder of its own: runs\svrtest\game<n> - runs side by side)
        '^--perf$' { $o.perf = $v; $i++; continue }
        '^--log-level$' { $o.loglevel = $v; $i++; continue }
        '^--name$' { $o.name = $v; $i++; continue }
        '^--keep$' { $o.keep = $true; continue }
        '^--arg$' { $o.args += $v; $i++; continue }
        default { if (-not $o.target -and $a -notlike "--*") { $o.target = $a } else { throw "svrtest: unknown option $a" } }
    }
}
if ($Command -notin "match", "entrance", "route", "script", "open", "prepare") {
    throw "svrtest: match | entrance | route <name> | script <file> | open <page> (see the top of tools\dev\svrtest.ps1)"
}
if ($Command -eq "open") { if (-not $o.target) { throw "svrtest open <page>" }; $o.exe = if ($o.exe -eq "release") { "dbg" } else { $o.exe } }
if ($Command -eq "entrance") { $o.entrances = $true; if (-not $o.until) { $o.until = "entrances over" } }
function Seconds([string]$t) {
    if (-not $t) { return 0 }
    if ($t -match '^(\d+(\.\d+)?)ms$') { return [double]$Matches[1] / 1000 }
    if ($t -match '^(\d+(\.\d+)?)s?$') { return [double]$Matches[1] }
    throw "svrtest: a time is 5s, 1500ms or a number, not $t"
}

# ---- guards: the player's game, the test-game cap (all chats), a free port
Assert-NoPlayerGame  # (and waits for a test-game slot: test_guard's limit, all chats)
$used = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalPort })
$p2p = 36500..36599 | Where-Object { $_ -notin $used } | Get-Random
if (-not $p2p) { throw "svrtest: no free UDP port in 36500-36599" }

# ---- the private game folder
$root = Join-Path $port "runs\svrtest"
$game = Join-Path $root "game$($o.slot)"
New-Item -ItemType Directory -Force $game, (Join-Path $game "Mods"), (Join-Path $root "music"), (Join-Path $root "movies") | Out-Null
# pac: a real folder. The game patches pac\string.pac and pac\menu\menu.pac in place at start (game_files.cpp
# PatchGameFiles: a .tmp renamed over the file) - through a junction that would rewrite the player's install. So
# the files in pac\ and pac\menu\ are private copies (refreshed when Game Files' copy is newer), and only the sub-folders the game never writes (bg, ch, edit, evt, evtdsc) are junctions.
function Remove-Junction([string]$path) {  # (the link only, never what it points at)
    if ((Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue).LinkType -eq "Junction") { cmd /c rmdir "`"$path`"" | Out-Null }
}
function Mirror-Pac([string]$src, [string]$dst, [string[]]$copyDirs) {
    Remove-Junction $dst
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($f in Get-ChildItem -LiteralPath $src -File) {
        $to = Join-Path $dst $f.Name
        if (-not (Test-Path -LiteralPath $to) -or ((Get-Item -LiteralPath $to).LastWriteTime -lt $f.LastWriteTime)) {
            Copy-Item -LiteralPath $f.FullName $to -Force
        }
    }
    foreach ($d in Get-ChildItem -LiteralPath $src -Directory) {
        $to = Join-Path $dst $d.Name
        if ($d.Name -in $copyDirs) { Mirror-Pac $d.FullName $to @() }
        elseif (-not (Test-Path -LiteralPath $to)) { New-Item -ItemType Junction -Path $to -Target $d.FullName | Out-Null }
    }
}
Mirror-Pac (Join-Path $gameFiles "pac") (Join-Path $game "pac") @("menu")
foreach ($d in "sound", "movies", "native_shaders", "nxeart", "pad_icons") {
    $link = Join-Path $game $d
    $src = Join-Path $gameFiles $d
    if ((Test-Path $link) -or -not (Test-Path $src)) { continue }
    if (Test-Path $src -PathType Container) { New-Item -ItemType Junction -Path $link -Target $src | Out-Null }
    else { Copy-Item $src $link }  # (nxeart is a file)
}
foreach ($f in "default.xex", "plist360.arc", "plist360_4x3.arc") {
    if (-not (Test-Path (Join-Path $game $f))) { Copy-Item (Join-Path $gameFiles $f) $game }
}
$build = switch ($o.exe) { "release" { Join-Path $port "out\build\SourceRelease" } "dbg" { Join-Path $port "out\build\DebugSuite" } default { Split-Path $o.exe -Parent } }
$exeName = switch ($o.exe) { "release" { "svr2011.exe" } "dbg" { "svr2011_dbg.exe" } default { Split-Path $o.exe -Leaf } }
foreach ($f in $exeName, "rexruntime.dll", "rexgpu-xenos.dll") {
    $src = Join-Path $build $f
    if (-not (Test-Path $src) -and $f -ne $exeName) { $src = Join-Path $port "out\build\SourceRelease\$f" }
    if (-not (Test-Path $src)) { throw "svrtest: no $f in $build - build it first" }
    # (only when the build changed; a game that just ended can hold the exe a moment)
    $to = Join-Path $game $f
    $same = (Test-Path $to) -and (Get-Item $to).Length -eq (Get-Item $src).Length -and (Get-Item $to).LastWriteTimeUtc -eq (Get-Item $src).LastWriteTimeUtc
    for ($k = 0; -not $same; $k++) {
        try { Copy-Item $src $game -Force -ErrorAction Stop; break } catch { if ($k -ge 20) { throw }; Start-Sleep -Milliseconds 500 }
    }
}
# its Mods: only --mods (a mod folder is placed by its manifest's type)
$kinds = @{ superstar = "Superstars"; attire = "Attires"; arena = "Arenas"; moves = "Moves"; backstage = "Backstage"; signs = "Signs"; media = "Media"; backgrounds = "Backgrounds"; matchtype = "MatchTypes"; weapon = "Weapons" }
Get-ChildItem (Join-Path $game "Mods") -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne ".convert" } | Remove-Item -Recurse -Force
if ($o.mods -and $o.mods -ne "none") {
    foreach ($m in $o.mods -split ',') {
        $man = Join-Path $m "manifest.txt"
        if (-not (Test-Path $man)) { throw "svrtest: --mods $m has no manifest.txt" }
        $type = ((Get-Content $man | Where-Object { $_ -match '^type=' }) -replace '^type=', '').Trim().ToLower()
        $kind = if ($kinds[$type]) { $kinds[$type] } else { "Superstars" }
        New-Item -ItemType Directory -Force (Join-Path $game "Mods\$kind") | Out-Null
        Copy-Item -Recurse $m (Join-Path $game "Mods\$kind\$(Split-Path $m -Leaf)")
    }
}
# (prepare: only the game folder, for svrtest_lan.ps1 - its last output line)
if ($Command -eq "prepare") { $game; exit 0 }
$userdata = if ($o.userdata) { $o.userdata } else { Join-Path $port "runs\univ_userdata" }  # (private: its own DLC copy)
# never the player's user data, nor one whose DLC is a link (the game patches the installed DLC's string.pac and
# catalog.dlc in place)
$udFull = [IO.Path]::GetFullPath($userdata).TrimEnd('\')
if ($udFull.StartsWith([IO.Path]::GetFullPath($gameFiles).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { throw "svrtest: --userdata is inside Game Files - use a private copy" }
$dlc = Join-Path $userdata "0000000000000000"
if ((Test-Path $dlc) -and (Get-Item -LiteralPath $dlc -Force).LinkType) { throw "svrtest: $dlc is a link - give the test user data a private DLC copy" }
# a user data with no save starts the game's first-time screens: no route gets past them ("fast start: gave up
# skipping") - a worktree's own runs\univ_userdata is empty until one is copied in
if (-not $o.fresh -and -not (Test-Path (Join-Path $userdata "Saves\SaveData.dat"))) {
    throw ("svrtest: $userdata has no Saves\SaveData.dat - the game would stop at its first-time screens. Copy a test " +
           "save in (real copies, not links): e.g. robocopy <main>\port\runs\test_userdata_thd\Saves and its " +
           "0000000000000000 (the DLC) into it, or pass --userdata <a test user data>; --fresh for a first-start test")
}
# a tripwire: what the game patches at start, in Game Files, before the run (compared in result.json)
$watched = @("pac\string.pac", "pac\menu\menu.pac") | ForEach-Object { Join-Path $gameFiles $_ } | Where-Object { Test-Path $_ }
$watchedTimes = @{}; foreach ($w in $watched) { $watchedTimes[$w] = (Get-Item $w).LastWriteTimeUtc }

# ---- the run's folder and the game's environment
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$name = if ($o.name) { $o.name } else { $Command + $(if ($o.target) { "_" + ($o.target -replace '[^A-Za-z0-9]', '') }) }
$out = Join-Path $root "$($stamp)_$name"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "game.log"
$inputFile = Join-Path $out "input.txt"
Set-Content $inputFile "" -NoNewline
$env:SVR2011_INPUT_FILE = $inputFile
$env:SVR2011_USER_DATA = $userdata
$env:SVR2011_MUSIC = Join-Path $root "music"
$env:SVR2011_AUDIO_MUTE = "1"  # (the game mutes even if its command line is lost: svr2011_app.cpp OnPreSetup)
$env:SVR2011_MOVIES = Join-Path $root "movies"
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
$env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
foreach ($v in "SVR2011_TEST_MATCH", "SVR2011_TEST_RULE", "SVR2011_NATIVE_SHADERS") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
$steps = @()
if ($Command -eq "script" -and $o.p[1]) {  # (a script's "route match" with these people: test_match.cpp)
    $env:SVR2011_TEST_MATCH = "people=" + (@(1..6 | Where-Object { $o.p[$_] } | ForEach-Object { $o.p[$_] }) -join ",") + " arena=" + $(if ($o.arena) { $o.arena } else { "1" })
    if ($o.rule) { $env:SVR2011_TEST_RULE = $o.rule -replace '^0x', '' }
}
if ($Command -in "match", "entrance") {
    $people = @(1..6 | Where-Object { $o.p[$_] } | ForEach-Object { $o.p[$_] })
    if ($people.Count -lt 2 -and $Command -eq "match") { throw "svrtest match: --p1 and --p2 at least" }
    if ($people.Count -lt 2) { $people += "EDGE" }
    $spec = "people=" + ($people -join ",")
    if ($o.arena) { $spec += " arena=$($o.arena)" } else { $spec += " arena=1" }
    if ($o.attire) { $spec += " attires=$($o.attire)" }
    if ($o.cpu -eq "all") { $spec += " cpu=all" }  # (every person a CPU: test_match.cpp)
    $env:SVR2011_TEST_MATCH = $spec
    if ($o.rule) { $env:SVR2011_TEST_RULE = $(if ($o.rule -match '^\d+$' -and $o.rule.Length -lt 3) { "{0:X2}" -f [int]$o.rule } else { $o.rule -replace '^0x', '' }) }
    $steps = if ($o.boot) { @() } elseif ($o.entrances) { @("route normal", "pressuntil A test match: people") } else { @("route match") }
} elseif ($Command -eq "open") {
    $steps = @()
} elseif ($Command -eq "route") {
    if (-not $o.target) { throw "svrtest route <name>" }
    $steps = @("route $($o.target)")
} else {
    if (-not $o.target -or -not (Test-Path $o.target)) { throw "svrtest script <file>" }
    $steps = @(Get-Content $o.target)
}
# the test folder's settings file: muted, no keyboard / mouse - so even a game whose --options were all dropped
# (a flag named twice) stays silent and leaves the player's mouse alone
$toml = Join-Path $game "svr2011.toml"
if (Test-Path $toml) {
    $lines = @(Get-Content $toml | Where-Object { $_ -notmatch '^\s*(audio_mute|mnk_mode)\s*=' })
    [IO.File]::WriteAllLines($toml, [string[]]($lines + @("audio_mute = true", "mnk_mode = false")))
}
$gameArgs = @("--log_file=`"$log`"", "--log_level=$($o.loglevel)", "--audio_mute=true", "--fullscreen=false", "--monitor=2",
              "--mnk_mode=false", "--p2p_port_mapping=false", "--p2p_port=$p2p")
if ($o.backend) { $gameArgs += "--gpu_backend=$($o.backend)" }
if ($o.perf) { $gameArgs += "--perf_overlay=$($o.perf)", "--perf_log_seconds=5" }
# the dbg build: the game's own cues for a match (debug_suite.h), fast-forward
$dbg = $exeName -like "*_dbg.exe"
if (($o.state -or $o.fast -or $o.boot) -and -not $dbg) { throw "svrtest: --state, --fast and --boot need --exe dbg" }
if ($o.boot) {
    if ($Command -ne "match") { throw "svrtest: --boot is for match" }
    $env:SVR2011_TEST_MATCH = $null; $env:SVR2011_TEST_RULE = $null  # (the game's own boot fills the match)
    $bm = "people=$($people -join ',') arena=$(if ($o.arena) { $o.arena } else { 1 })"
    if ($o.attire) { $bm += " attires=$($o.attire)" }
    if ($o.rule) { $bm += " rule=$(if ($o.rule -match '^\d+$' -and $o.rule.Length -lt 3) { '{0:X2}' -f [int]$o.rule } else { $o.rule -replace '^0x', '' })" }
    if ($o.cpu -eq "all") { $bm += " cpu=all" }
    $gameArgs += "--dbg_boot=match", "--dbg_match=`"$bm`""
    if ($o.entrances) { $gameArgs += "--dbg_entrances=true" }
}
$gameCues = $dbg -and $Command -eq "match"
$inv = [Globalization.CultureInfo]::InvariantCulture
if ($dbg) { $gameArgs += "--dbg_out=`"$out`"" }
if ($o.fast) { $gameArgs += "--dbg_speed=$([int]$o.fast)" }
$wantShots = 0; $wantStates = @($o.state).Count
if ($gameCues) {
    $list = @($o.shot | ForEach-Object { (Seconds $_).ToString($inv) })
    if ($o.every -match '^every:([^:]+)(:(\d+))?$') {
        $n = if ($Matches[3]) { [int]$Matches[3] } else { 10 }
        $list += "every:$((Seconds $Matches[1]).ToString($inv)):$n"
    }
    $wantShots = @($o.shot).Count + $(if ($o.every) { $n } else { 0 })
    if ($list) { $gameArgs += "--dbg_shot=$($list -join ',')" }
    if ($o.state) { $gameArgs += "--dbg_state=$((@($o.state | ForEach-Object { (Seconds $_).ToString($inv) })) -join ',')" }
    if ($o.quit) { $gameArgs += "--dbg_quit=$((Seconds $o.quit).ToString($inv))" }
}
if ($Command -eq "open") { $gameArgs += "--dbg_boot=`"$($o.target)`"" }
# (--arg: an option svrtest sets already is replaced - the game's parser drops every --option of a command line that
# names one twice, --audio_mute=true with them: 27 Oct a test game was heard; --audio_mute stays true)
foreach ($x in $o.args) {
    $nm = ($x -replace '^(--[^=]+).*$', '$1')
    if ($nm -eq "--audio_mute" -or $nm -eq "--no-audio_mute") { Write-Host "svrtest: --arg $x ignored - test games stay muted"; continue }
    $gameArgs = @($gameArgs | Where-Object { ($_ -replace '^(--[^=]+).*$', '$1') -ne $nm })
    $gameArgs += $x
}

# ---- run
$t0 = Get-Date
Use-SvrTestServer -GameDir $game -GameArgs $gameArgs | Out-Null
$proc = Start-Process (Join-Path $game $exeName) -WorkingDirectory $game -ArgumentList $gameArgs -PassThru -WindowStyle Minimized
Set-SvrTestPriority $proc
for ($t = 0; $t -lt 100 -and -not $proc.HasExited; $t++) { $proc.Refresh(); if ($proc.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
if ($proc.MainWindowHandle -ne 0) {
    [void][SvrTestWin]::SetWindowPos($proc.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)
    [void][SvrTestWin]::ShowWindow($proc.MainWindowHandle, 4)
}
Add-Content $inputFile ($steps -join "`n")
$shots = @(); $timings = [ordered]@{}; $ok = $true; $why = ""
function LogHas([string]$text) { (Test-Path $log) -and (Select-String -Path $log -Pattern $text -SimpleMatch -Quiet) }
function Wait-Log([string]$text, [double]$secs) {
    $until = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $until -and -not $proc.HasExited) { if (LogHas $text) { return $true }; Start-Sleep -Milliseconds 500 }
    return (LogHas $text)
}
function Shot([string]$label) {
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object SvrTestWin+RECT; [void][SvrTestWin]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][SvrTestWin]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
    $k = [math]::Min(1.0, 1280.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $k)), ([int]($hh * $k))
    $file = Join-Path $out "$label.png"
    $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $g.Dispose(); $bmp.Dispose()
    $script:shots += (Split-Path $file -Leaf)
}
try {
    # the start: the main menu, then the command's own cue (the match hand-off / the route's end)
    $menuCue = if ($o.boot -or $Command -eq "open") { "fast start: the main menu" } else { "title: main menu" }
    if (Wait-Log $menuCue 180) { $timings.main_menu_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
    # (the real line has a space after "people": not the script's own "pressuntil A test match: people")
    $cue = if ($o.boot) { "SVRDBG: boot: match -" } elseif ($Command -eq "open") { "SVRDBG: boot: page " } elseif ($Command -in "match", "entrance") { "test match: people " } else { "script input: all steps done" }
    $deadline = (Get-Date).AddSeconds(240)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited -and -not (LogHas $cue) -and -not (LogHas "script input: no route")) { Start-Sleep -Milliseconds 500 }
    if (LogHas "script input: no route") { $ok = $false; $why = (@(Select-String -Path $log -Pattern "script input: no route" -SimpleMatch)[0].Line -split '\] ', 2)[-1] -replace '^.*script input: ', '' }
    elseif (LogHas $cue) { $timings.cue_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
    else { $ok = $false; $why = "no `"$cue`" in the log (the game: $(if ($proc.HasExited) { 'exited' } else { 'still running' }))" }
    # (a match's shots count from its start: the replay recorder recording - after the entrances when they play)
    if ($ok -and $Command -eq "match") {
        if (Wait-Log "replay recorder recording" 300) { $timings.match_starts_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
        else { $ok = $false; $why = "the match didn't start (no `"replay recorder recording`" in the log)" }
    }
    $from = Get-Date
    if ($ok -and $gameCues) {
        # (the game's own cues: wait for its quit, or for every shot and state it was asked for)
        $limit = (Get-Date).AddSeconds(60 + [math]::Max((Seconds $o.quit), 0) + 3 * ($wantShots + $wantStates) +
                                       ((@($o.shot) + @($o.state) | ForEach-Object { Seconds $_ } | Measure-Object -Maximum).Maximum))
        while ((Get-Date) -lt $limit -and -not $proc.HasExited) {
            if (-not $o.quit -and @(Select-String -Path $log -Pattern "SVRDBG: (shot|state) " -ErrorAction SilentlyContinue).Count -ge $wantShots + $wantStates) { break }
            if ($o.until -and (LogHas $o.until)) { $timings.until_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1); break }
            Start-Sleep -Milliseconds 250
        }
        if (LogHas "SVRDBG: quit") { $timings.quit_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
        elseif ($o.quit) { $ok = $false; $why = "no `"SVRDBG: quit`" (the game: $(if ($proc.HasExited) { "exited, code $($proc.ExitCode)" } else { 'still running' }))" }
        $script:shots = @(Get-ChildItem $out -Filter "shot_*.png" | ForEach-Object { $_.Name })
        $o.shot = @(); $o.every = ""; $o.quit = ""
    }
    $plan = @()
    foreach ($s in $o.shot) { $plan += [pscustomobject]@{ at = (Seconds $s); label = "shot_$s" } }
    if ($o.every -match '^every:([^:]+)(:(\d+))?$') {
        $step = Seconds $Matches[1]; $n = if ($Matches[3]) { [int]$Matches[3] } else { 10 }
        for ($k = 1; $k -le $n; $k++) { $plan += [pscustomobject]@{ at = $step * $k; label = ("shot_{0:D2}" -f $k) } }
    }
    $quitAt = if ($o.quit) { Seconds $o.quit } else { 0 }
    foreach ($p in ($plan | Sort-Object at)) {
        while (((Get-Date) - $from).TotalSeconds -lt $p.at -and -not $proc.HasExited) {
            if ($o.until -and (LogHas $o.until)) { break }
            Start-Sleep -Milliseconds 200
        }
        if ($proc.HasExited) { break }
        Shot $p.label
        if ($o.until -and (LogHas $o.until)) { $timings.until_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1); break }
    }
    if ($o.until -and -not $timings.until_s) { if (Wait-Log $o.until 300) { $timings.until_s = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) } }
    if ($quitAt) { while (((Get-Date) - $from).TotalSeconds -lt $quitAt -and -not $proc.HasExited) { Start-Sleep -Milliseconds 250 } }
    if ($proc.HasExited -and $ok -and -not $timings.quit_s) { $ok = $false; $why = "the game exited (code $($proc.ExitCode))" }
} finally {
    if (-not $o.keep -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        [void]$proc.WaitForExit(15000)  # (gone before the next run copies the exe over it)
    }
    foreach ($v in "SVR2011_TEST_MATCH", "SVR2011_TEST_RULE", "SVR2011_INPUT_FILE") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
}

# ---- result.json
$lines = if (Test-Path $log) { Get-Content $log } else { @() }
$errors = @($lines | Where-Object { $_ -match '\[(error|critical)\]' -and $_ -notmatch 'BaseHeap|PhysicalHeap|ResolvePath' })
$fps = @($lines | Where-Object { $_ -match '\] fps: ' } | Select-Object -Last 3 | ForEach-Object { ($_ -split '\] fps: ')[1] })
# (crash reports always go to <game>\UserData\crashes, whatever SVR2011_USER_DATA says)
$crashes = @(Get-ChildItem (Join-Path $userdata "crashes"), (Join-Path $game "UserData\crashes") -Filter "crash_*.txt" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | ForEach-Object { $_.FullName })
if ($crashes) { $ok = $false; $why = "crashed: $($crashes -join ', ')" }
$touched = @($watched | Where-Object { (Get-Item $_).LastWriteTimeUtc -ne $watchedTimes[$_] })
if ($touched) { $ok = $false; $why = "GAME FILES CHANGED during the run: $($touched -join ', ')"; Write-Warning $why }
$result = [ordered]@{
    ok = $ok; why = $why; command = "$Command $($Rest -join ' ')"; exe = (Join-Path $game $exeName); pid = $proc.Id; p2p_port = $p2p
    timings = $timings; match = @($lines | Where-Object { $_ -match 'test match: people|\] match: ' } | Select-Object -Last 2 | ForEach-Object { $_.Substring([Math]::Min(40, $_.Length)) })
    fps = $fps; errors = $errors.Count; first_errors = @($errors | Select-Object -First 5 | ForEach-Object { $_.Substring([Math]::Min(40, $_.Length)) })
    crashes = $crashes; game_files_changed = $touched; shots = $shots; states = @(Get-ChildItem $out -Filter "state_*.json" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }); folder = $out; log = $log; kept_running = ($o.keep -and -not $proc.HasExited)
}
$json = $result | ConvertTo-Json -Depth 4
Set-Content (Join-Path $out "result.json") $json -Encoding UTF8
$json

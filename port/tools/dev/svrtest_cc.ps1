# svrtest cc - one hidden dbg game signed in to its OWN local test server, booted in the ONLINE menu, then a steps
# file (script_input.h steps; Community Creations uploads: cc_upload.txt). For us only: tools\dev is never packaged.
#
#   svrtest_cc.ps1 [--script <steps file>] [--mods <folder>[,...]] [--userdata <dir>] [--env NAME=value ...]
#                  [--name cc] [--secs 600]
#
# Beyond script_input.h's steps the file may hold (handled here, not by the game):
#   shot <label>     once every step before it is done (a "where" added, then the log's "script input: all steps
#                    done"; 5 minutes at most, then a "stuck_<label>" shot), a window shot
#                    <label>.png (PrintWindow, the window stays hidden behind / off the screens)
#   sleep <seconds>  here, between steps
#   waitlog <text>   until the game's log has a line with text (any time - script_input's "until" only sees lines
#                    logged after it starts), e.g. "online: game files clean" before ONLINE (else: online is off)
# Defaults: the steps tools\dev\cc_upload.txt (a Superstar upload: CAW slot 0, refused when it uses mods), the user
# data runs\test_userdata_wwe13b (4 CAWs) - always a PRIVATE COPY (runs\svrtest\cc_ud, made fresh each run) - and
# SVR2011_TEST_CAW_UPLOAD=1 (online.cpp: the CAWs' upload check logged once, at a menu).
# What it does: svrtest's private game folder (svrtest.ps1 prepare, the dbg exe, only --mods in its Mods); a test
# server (port\server\server.py on a copy of runs\ent_srv's accounts, a free port 8460-8499, log server.log); the
# EntDown test account's config (test_config_ent_down.toml) pointed at it; p2p port 365xx; the game started as
# usual (the steps go to the main menu: "title", then ONLINE once the game files' check is done). Stops the game and the server by their PIDs. Writes runs\svrtest\<stamp>_<name>\ (game.log,
# server.log, the shots, result.json with the CC lines of both logs).
#
# Example (the mod-move refusal: a jeff_hardy move planted into CAW slot 0's moveset, in memory):
#   svrtest_cc.ps1 --mods "...\Game Files\Mods\Superstars\jeff_hardy" --env SVR2011_TEST_CC_MODMOVE=0:3575
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
$tools = Split-Path $PSScriptRoot -Parent
$port = Split-Path $tools -Parent
. (Join-Path $tools "test_guard.ps1")
Add-Type -AssemblyName System.Drawing
if (-not ("SvrCcWin2" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SvrCcWin2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
}
"@
}
[void][SvrCcWin2]::SetProcessDPIAware()

$o = @{ script = (Join-Path $PSScriptRoot "cc_upload.txt"); mods = "none"; userdata = (Join-Path $port "runs\test_userdata_wwe13b")
        env = @("SVR2011_TEST_CAW_UPLOAD=1"); name = "cc"; secs = "600" }
for ($i = 0; $i -lt $Rest.Count; $i++) {
    $a = $Rest[$i]; $v = if ($i + 1 -lt $Rest.Count) { $Rest[$i + 1] } else { "" }
    switch -regex ($a) {
        '^--env$' { $o.env += $v; $i++; continue }
        '^--(script|mods|userdata|name|secs)$' { $o[$Matches[1]] = $v; $i++; continue }
        default { throw "svrtest_cc: unknown option $a" }
    }
}
if (-not (Test-Path $o.script)) { throw "svrtest_cc: no steps file $($o.script)" }

# ---- guards: the player's game, a test-game slot (test_guard's limit, all chats)
Assert-NoPlayerGame

# ---- the game folder, the run folder, private copies
$prep = @(& (Join-Path $PSScriptRoot "svrtest.ps1") prepare --exe dbg --mods $o.mods)
$game = [string]$prep[-1]
$exe = Join-Path $game "svr2011_dbg.exe"
$root = Join-Path $port "runs\svrtest"
$out = Join-Path $root ("{0}_{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $o.name)
New-Item -ItemType Directory -Force $out | Out-Null
$src = @((Join-Path $port "runs"), "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\runs") | Where-Object { Test-Path (Join-Path $_ "ent_srv\data") } | Select-Object -Last 1
if (-not $src) { throw "svrtest_cc: no runs\ent_srv\data (the test server's accounts) to copy" }
function Mirror([string]$from, [string]$to) {
    if (Test-Path $to) { Remove-Item -Recurse -Force $to }
    robocopy $from $to /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "svrtest_cc: copying $from failed" }
}
Mirror (Join-Path $src "ent_srv\data") (Join-Path $root "cc_srv\data")   # (fresh: no records from earlier runs)
$ud = Join-Path $root "cc_ud"
Mirror $o.userdata $ud
# (online_files.cpp CheckVersion: GitHub's newest release, cached 6 h here - an old cached "newest" keeps a dev
# build's version from turning online off: "a difference in game versions")
[IO.File]::WriteAllText((Join-Path $ud "online_version.txt"), "$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) 0.0.1`n")
$dlc = Join-Path $ud "0000000000000000"
if ((Test-Path $dlc) -and (Get-Item -LiteralPath $dlc -Force).LinkType) { throw "svrtest_cc: $dlc is a link - give the user data a private DLC copy" }
$listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalPort })
$sp = 8460..8499 | Where-Object { $_ -notin $listen } | Select-Object -Last 1
$udp = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalPort })
$p2p = 36520..36539 | Where-Object { $_ -notin $udp } | Get-Random
if (-not $sp -or -not $p2p) { throw "svrtest_cc: no free server / p2p port" }
$cfg = Join-Path $root "cc.toml"
$t = (Get-Content (Join-Path $src "test_config_ent_down.toml") -Raw) -replace '(?m)^online_server = .*$', "online_server = `"http://127.0.0.1:$sp`"" `
     -replace '(?m)^p2p_(port|address|peers) = .*\r?\n', ''
[IO.File]::WriteAllText($cfg, $t)  # (no BOM)

# ---- the server, the game
$py = "C:\Users\Ranger\AppData\Local\Programs\Python\Python311\python.exe"
$srvLog = Join-Path $out "server.log"
$srv = Start-Process $py -ArgumentList "`"$port\server\server.py`" --test-open-accounts --host 127.0.0.1 --port $sp --data `"$(Join-Path $root 'cc_srv\data')`" --log `"$srvLog`"" -PassThru -WindowStyle Hidden
$log = Join-Path $out "game.log"
$inputFile = Join-Path $out "input.txt"
Set-Content $inputFile "" -NoNewline
$proc = $null; $ok = $true; $why = ""; $shots = @(); $t0 = Get-Date
function LogCount([string]$text) { if (Test-Path $log) { @(Select-String -Path $log -Pattern $text -SimpleMatch -ErrorAction SilentlyContinue).Count } else { 0 } }
function Shot([string]$label) {
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    # (the window's real pixels whatever this host's DPI mode: a scaled screen gave 581x355 crops)
    [void][SvrCcWin2]::SetThreadDpiAwarenessContext([IntPtr](-4))
    $r = New-Object SvrCcWin2+RECT; [void][SvrCcWin2]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][SvrCcWin2]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
    $k = [math]::Min(1.0, 1280.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $k)), ([int]($hh * $k))
    $small.Save((Join-Path $out "$label.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $g.Dispose(); $bmp.Dispose()
    $script:shots += "$label.png"
}
try {
    for ($k = 0; $k -lt 40 -and -not (Get-NetTCPConnection -LocalPort $sp -State Listen -ErrorAction SilentlyContinue); $k++) { Start-Sleep -Milliseconds 500 }
    $env:SVR2011_CONFIG = $cfg
    $env:SVR2011_USER_DATA = $ud
    $env:SVR2011_INPUT_FILE = $inputFile
    $env:SVR2011_MUSIC = Join-Path $root "music"; $env:SVR2011_MOVIES = Join-Path $root "movies"
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
    foreach ($e in $o.env) { $n, $v = $e -split '=', 2; [Environment]::SetEnvironmentVariable($n, $v, "Process") }
    $gameArgs = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--mnk_mode=false",
                  "--p2p_port_mapping=false", "--p2p_relay=false", "--p2p_port=$p2p", "--dbg_out=`"$out`"")
    Use-SvrTestServer -GameDir $game -GameArgs $gameArgs | Out-Null
    $proc = Start-Process $exe -WorkingDirectory $game -ArgumentList $gameArgs -PassThru -WindowStyle Minimized
    foreach ($e in $o.env) { [Environment]::SetEnvironmentVariable(($e -split '=', 2)[0], $null, "Process") }
    foreach ($v in "SVR2011_INPUT_FILE", "SVR2011_CONFIG", "SVR2011_USER_DATA") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
    for ($k = 0; $k -lt 100 -and -not $proc.HasExited; $k++) { $proc.Refresh(); if ($proc.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
    if ($proc.MainWindowHandle -ne 0) {
        [void][SvrCcWin2]::SetWindowPos($proc.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)
        [void][SvrCcWin2]::ShowWindow($proc.MainWindowHandle, 4)
    }
    # the steps: a segment at a time up to each shot / sleep
    $deadline = $t0.AddSeconds([double]$o.secs)
    $segment = @()
    $lines = @(Get-Content $o.script | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }) + @("shot _end")
    foreach ($line in $lines) {
        $l = ($line -replace '\s+#.*$', '').Trim()
        if ($l -match '^(shot|sleep|waitlog)\s+(.+)$') {
            $verb = $Matches[1]; $arg = $Matches[2]
            if ($segment) {
                $before = LogCount "script input: all steps done"
                # (a "where" last: only a waiting step's end logs "all steps done", a press's / wait's doesn't)
                Add-Content $inputFile (($segment + "where") -join "`n"); $segment = @()
                $segEnd = (Get-Date).AddSeconds(300)
                while ((LogCount "script input: all steps done") -le $before) {
                    if ($proc.HasExited) { throw "the game exited (code $($proc.ExitCode))" }
                    if ((Get-Date) -gt $deadline) { throw "out of time (--secs $($o.secs))" }
                    # (a screen that doesn't read the pad holds the steps: shoot it anyway, once)
                    if ((Get-Date) -gt $segEnd) { Shot "stuck_$arg"; $segEnd = $deadline; Write-Warning "steps held before $verb $arg" }
                    Start-Sleep -Milliseconds 500
                }
            }
            if ($verb -eq "shot") { Shot $arg }
            elseif ($verb -eq "sleep") { Start-Sleep -Seconds ([double]$arg) }
            else {  # (waitlog: a log line seen any time - "until" only sees the ones after it starts)
                while ((LogCount $arg) -eq 0) {
                    if ($proc.HasExited) { throw "the game exited (code $($proc.ExitCode))" }
                    if ((Get-Date) -gt $deadline) { throw "out of time (--secs $($o.secs)) waiting for `"$arg`"" }
                    Start-Sleep -Milliseconds 500
                }
            }
        } else { $segment += $l }
    }
} catch {
    $ok = $false; $why = $_.Exception.Message
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; [void]$proc.WaitForExit(15000) }
    if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue; [void]$srv.WaitForExit(5000) }
}
function Lines([string]$file, [string]$pattern) {
    if (-not (Test-Path $file)) { return @() }
    @(Select-String -Path $file -Pattern $pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
}
$result = [ordered]@{
    ok = $ok; why = $why; ports = [ordered]@{ server = $sp; p2p = $p2p }; secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    caw_check = (Lines $log "online test: CAW|online test: \d+ CAWs"); cc = (Lines $log "online: CC upload")
    server = (Lines $srvLog "sake: .*(UserContent|record)"); shots = $shots; folder = $out
    game_left = [bool]($proc -and -not $proc.HasExited); server_left = -not $srv.HasExited
}
$json = $result | ConvertTo-Json -Depth 4
Set-Content (Join-Path $out "result.json") $json -Encoding UTF8
$json

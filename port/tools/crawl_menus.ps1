# Menu / mode crawler: for every leaf of the game's menu tree (runs\crawl_leaves.json,
# made from menu.pac), boot the game, walk the menus to it, probe it (match types:
# press through the setup into the match and let it run; other screens: a few
# presses), then record whether the game crashed or hung, with screenshots.
# Background only: muted, window off-screen, never focused; its own saves copy
# per worker (seeded from runs\test_userdata\Saves); waits while the player plays.
#   crawl_menus.ps1 [-Worker 0 -Workers 2] [-Only 3,7] [-MatchSeconds 40]
param([int]$Worker = 0, [int]$Workers = 1, [int[]]$Only = @(), [int]$MatchSeconds = 40)
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][CW]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")

$port  = Split-Path $PSScriptRoot -Parent
$game  = Join-Path (Split-Path $port -Parent) "Game Files"
$runs  = Join-Path $port "runs"
$out   = Join-Path $runs "crawl"
$wdir  = Join-Path $out "w$Worker"
New-Item -ItemType Directory -Force $out, $wdir | Out-Null
$ud    = Join-Path $wdir "userdata"
$inputFile = Join-Path $wdir "input.txt"
$results = Join-Path $out "results_w$Worker.jsonl"
$leaves = Get-Content (Join-Path $runs "crawl_leaves.json") -Raw | ConvertFrom-Json
$seedSaves = Join-Path $runs "test_userdata\Saves"
$dlc = Join-Path $game "UserData\0000000000000000"
$crashDir = Join-Path $game "UserData\crashes"

function Wait-NoPlayer {
    while ($true) {
        $player = @(Get-SvrGames | Where-Object { $_.CommandLine -and -not (Test-IsTestGame $_) })
        if (-not $player.Count) { return }
        Start-Sleep 60   # the player is playing: wait
    }
}

function Seed-Saves {
    if (-not (Test-Path $ud)) {
        New-Item -ItemType Directory $ud | Out-Null
        cmd /c mklink /J "$ud\0000000000000000" "$dlc" | Out-Null
    }
    $s = Join-Path $ud "Saves"
    if (Test-Path $s) { cmd /c rmdir /s /q "$s" | Out-Null }
    robocopy $seedSaves $s /E /XD .mount /NFL /NDL /NJH /NJS /NP | Out-Null
}

$script:proc = $null
function Start-Game($log) {
    Set-Content $inputFile "" -NoNewline
    $env:SVR2011_INPUT_FILE = $inputFile
    $env:SVR2011_USER_DATA = $ud
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
    $a = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--native_renderer=main")
    $p = Start-Process (Join-Path $game "svr2011.exe") -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
    for ($t = 0; $t -lt 100 -and -not $p.HasExited; $t++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
    if ($p.MainWindowHandle -ne 0) {
        [void][CW]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX + 120 * $Worker, 60 * $Worker, 0, 0, $SvrParkFlags)  # (screen 2, behind every window)
        [void][CW]::ShowWindow($p.MainWindowHandle, 4)
    }
    $script:proc = $p
}

function Alive { return $script:proc -and -not $script:proc.HasExited }

function Press($b, [double]$wait = 1.5) {
    if (-not (Alive)) { return }
    Add-Content $inputFile "press $b 150"
    Start-Sleep -Milliseconds ([int]($wait * 1000))
}

function Shot($file) {
    if (-not (Alive)) { return $null }
    try {
        $script:proc.Refresh()
        $h = $script:proc.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { return $null }
        $r = New-Object CW+RECT; [void][CW]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        if ($w -le 0 -or $hh -le 0) { return $null }
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [void][CW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
        $small = New-Object System.Drawing.Bitmap $bmp, 640, ([int](640 * $hh / $w))
        $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Jpeg)
        $small.Dispose(); $g.Dispose(); $bmp.Dispose()
        return (Split-Path $file -Leaf)
    } catch { return $null }
}

function Stop-Game {
    if (Alive) {
        Stop-Process -Id $script:proc.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $script:proc.Id -Timeout 20 -ErrorAction SilentlyContinue
    }
}

foreach ($leaf in $leaves) {
    if ($Only.Count -and $Only -notcontains $leaf.id) { continue }
    if (-not $Only.Count -and ($leaf.id % $Workers) -ne $Worker) { continue }
    Wait-NoPlayer
    Seed-Saves
    $tag = "{0:D3}" -f $leaf.id
    $log = Join-Path $out "$tag.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $t0 = Get-Date
    Start-Game $log
    $shots = @()
    # boot to the main menu
    Start-Sleep 45; Press START 40; Press A 5; Press START 10
    $shots += Shot (Join-Path $out "${tag}_0menu.jpg")
    # walk the path
    for ($i = 0; $i -lt $leaf.path.Count; $i++) {
        for ($d = 0; $d -lt $leaf.path[$i]; $d++) { Press DOWN 1.0 }
        Press A 3.5
    }
    $shots += Shot (Join-Path $out "${tag}_1leaf.jpg")
    # probe
    if ($leaf.kind -eq "match") {
        for ($k = 0; $k -lt 16 -and (Alive); $k++) {
            Press A 2.5
            if ($k -eq 7) { $shots += Shot (Join-Path $out "${tag}_2setup.jpg") }
        }
        $shots += Shot (Join-Path $out "${tag}_3start.jpg")
        for ($s = 0; $s -lt $MatchSeconds -and (Alive); $s += 5) { Start-Sleep 5 }
        $shots += Shot (Join-Path $out "${tag}_4match.jpg")
    } else {
        Press A 5; $shots += Shot (Join-Path $out "${tag}_2in.jpg")
        Press A 5; Press DOWN 1.5; Press A 5
        $shots += Shot (Join-Path $out "${tag}_3deeper.jpg")
        Press B 3; Press B 3; Press B 3
        $shots += Shot (Join-Path $out "${tag}_4back.jpg")
    }
    Start-Sleep 2
    $alive = Alive
    $exit = if ($alive) { $null } else { $script:proc.ExitCode }
    # what happened
    $lines = if (Test-Path $log) { Get-Content $log } else { @() }
    $crash = @($lines | Select-String -Pattern "Unhandled guest access violation|FatalError|abort\(\)|terminate|exception 0x" | ForEach-Object { $_.Line }) | Select-Object -First 3
    $sel = @($lines | Select-String -Pattern "menu select: group ([0-9A-F]+) row (\d+)" | ForEach-Object { "{0}:{1}" -f $_.Matches[0].Groups[1].Value, $_.Matches[0].Groups[2].Value })
    $want = @(for ($i = 0; $i -lt $leaf.path.Count; $i++) { "{0:X}:{1}" -f [int]$leaf.groups[$i], [int]$leaf.path[$i] })
    $navOk = (($sel | Select-Object -Skip 1 -First $want.Count) -join ",") -eq ($want -join ",")   # skip the title's select
    $fps = @($lines | Select-String -Pattern "fps: ([0-9.]+) avg")
    $lastFps = if ($fps.Count) { [double]$fps[-1].Matches[0].Groups[1].Value } else { -1 }
    $minFps = if ($fps.Count -gt 20) { ($fps | Select-Object -Skip 20 | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Minimum).Minimum } else { -1 }
    $lastFpsAge = if ($fps.Count) { ((Get-Date) - [datetime]::ParseExact($fps[-1].Line.Substring(1, 23), "yyyy-MM-dd HH:mm:ss.fff", $null)).TotalSeconds } else { -1 }
    $newCrash = @(Get-ChildItem $crashDir -Filter "*.txt" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | ForEach-Object { $_.FullName })
    Stop-Game
    $rec = [ordered]@{ id = $leaf.id; name = $leaf.name; kind = $leaf.kind; alive = $alive; exit = $exit
        crash = $crash; crashReports = $newCrash; navOk = $navOk; selects = ($sel -join " "); want = ($want -join " ")
        lastFps = $lastFps; minFps = $minFps; lastFpsAgeS = [int]$lastFpsAge; shots = $shots; seconds = [int]((Get-Date) - $t0).TotalSeconds }
    ($rec | ConvertTo-Json -Compress -Depth 4) | Add-Content $results
    "{0} {1,-60} alive={2} nav={3} fps={4}/{5} {6}" -f $tag, $leaf.name, $alive, $navOk, $lastFps, $minFps, ($crash -join " | ")
}
"worker $Worker done"

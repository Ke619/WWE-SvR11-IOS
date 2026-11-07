# arena_session.ps1 for the arenas branch: its own game folder (runs\opt_arena_game), state
# and input files, and it stops only its own game, so it runs beside other
# sessions' tests. Needs SVR2011_USER_DATA (its own test saves).
# Drive a background game session (muted, never focused, window off-screen).
#   .\session.ps1 start [-Name s1]         launch; log -> runs\<Name>.log
#   .\session.ps1 input "press START" "wait 2000" "press A"
#   .\session.ps1 shot  <file-stem>        screenshot -> runs\<stem>.png
#   .\session.ps1 fps                      last frame-rate lines from the log
#   .\session.ps1 status | stop
# Input goes through the scripted controller (src/script_input.h).
param([Parameter(Position = 0)][string]$Action, [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest,
      [string]$Name = "session", [string]$Exe = "svr2011.exe", [string]$LogLevel = "info", [string]$GameDir = "")
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
}
"@
[void][SW]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")
$Rest = @($Rest | Where-Object { $_ })

$port  = Split-Path $PSScriptRoot -Parent
$game  = if ($GameDir) { $GameDir } else { Join-Path $port "runs\opt_arena_game" }
$runs  = Join-Path $port "runs"; New-Item -ItemType Directory -Force $runs | Out-Null
$state = Join-Path $runs "arena_session.json"
$input = Join-Path $runs "arena_input.txt"

function Stop-Own {
    if (-not (Test-Path $state)) { return }
    $id = (Get-Content $state | ConvertFrom-Json).pid
    $p = Get-CimInstance Win32_Process -Filter "ProcessId = $id" -ErrorAction SilentlyContinue
    if ($p -and [string]$p.CommandLine -match [regex]::Escape("\runs\opt_arena_game\")) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $id -Timeout 15 -ErrorAction SilentlyContinue
    }
}

function Get-Session {
    if (-not (Test-Path $state)) { throw "no session - run: session.ps1 start" }
    $s = Get-Content $state | ConvertFrom-Json
    $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -notlike "svr2011*") { throw "game is not running (pid $($s.pid)); log: $($s.log)" }
    return @{ proc = $p; log = $s.log }
}

switch ($Action) {
    "start" {
        Assert-NoPlayerGame
        if (-not $env:SVR2011_USER_DATA) { throw "set SVR2011_USER_DATA to this session's test saves" }
        Stop-Own
        Set-Content $input "" -NoNewline
        $log = Join-Path $runs "$Name.log"; Remove-Item $log -ErrorAction SilentlyContinue
        $env:SVR2011_INPUT_FILE = $input
        $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
        $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"   # native renderer window, off-screen too
        $a = @("--log_file=`"$log`"", "--log_level=$LogLevel", "--audio_mute=true", "--fullscreen=false", "--monitor=2") + $Rest
        Use-SvrTestServer -GameDir $game -GameArgs $a | Out-Null
        $p = Start-Process (Join-Path $game $Exe) -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
        for ($t = 0; $t -lt 100 -and -not $p.HasExited; $t++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
        if ($p.MainWindowHandle -ne 0) {
            [void][SW]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)  # (screen 2, behind every window)
            [void][SW]::ShowWindow($p.MainWindowHandle, 4)
        }
        @{ pid = $p.Id; log = $log } | ConvertTo-Json | Set-Content $state
        "started pid $($p.Id), log $log"
    }
    "input" {
        $null = Get-Session
        # (the game may be reading the file this moment: try again)
        for ($try = 0; ; $try++) {
            try { Add-Content $input ($Rest -join "`n") -ErrorAction Stop; break }
            catch { if ($try -ge 20) { throw }; Start-Sleep -Milliseconds 50 }
        }
        "queued: $($Rest -join ' | ')"
    }
    { $_ -in "shot", "shotnative" } {
        $s = Get-Session
        # By title/class: with the native renderer on, the process has two windows.
        # The test's own window (never a player's game with the same title).
        $s.proc.Refresh()
        $h = if ($Action -eq "shotnative") { [SW]::FindWindowW("SvR2011NativeRenderer", [NullString]::Value) } else { $s.proc.MainWindowHandle }
        if ($h -eq [IntPtr]::Zero) { throw "window not found" }
        $r = New-Object SW+RECT; [void][SW]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [void][SW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
        $scale = [math]::Min(1.0, 1280.0 / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($hh * $scale))
        $file = Join-Path $runs (($Rest | Select-Object -First 1) + ".png")
        $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $small.Dispose(); $g.Dispose(); $bmp.Dispose()
        $file
    }
    "fps" {
        $s = Get-Session
        Get-Content $s.log | Select-String -Pattern "fps|FPS|frame time" | Select-Object -Last 5 | ForEach-Object { $_.Line }
    }
    "status" {
        $s = Get-Session
        "running pid $($s.proc.Id), cpu $([int]$s.proc.CPU)s, ws $([int]($s.proc.WorkingSet64/1MB)) MB"
        Get-Content $s.log | Select-String -Pattern "\[(error|critical)\]" | Where-Object { $_.Line -notmatch "BaseHeap|PhysicalHeap" } | Select-Object -Last 8 | ForEach-Object { $_.Line }
    }
    "stop" {
        Stop-Own
        "stopped"
    }
    default { "usage: session.ps1 start|input|shot|shotnative|fps|status|stop" }
}

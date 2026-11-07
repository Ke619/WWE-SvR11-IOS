# Drive a background game session (muted, never focused, window off-screen).
#   .\session.ps1 start [-Name s1]         launch; log -> runs\<Name>.log
#   .\session.ps1 input "press START" "wait 2000" "press A"
#   .\session.ps1 route <name>             go there (src/script_input.cpp kRoutes: main,
#                                          exhibition, normal, cage, match_creator, online, match,
#                                          options) and wait until the game is there
#                                          ("route match": into the match that
#                                          SVR2011_TEST_MATCH="people=EDGE,KANE arena=1" and
#                                          SVR2011_TEST_RULE=3C ask for - src/test_match.cpp)
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
$game  = if ($GameDir) { $GameDir } else { Join-Path (Split-Path $port -Parent) "Game Files" }
$runs  = Join-Path $port "runs"; New-Item -ItemType Directory -Force $runs | Out-Null
$state = Join-Path $runs "session.json"
$input = Join-Path $runs "input.txt"

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
        Stop-TestGames
        Set-Content $input "" -NoNewline
        $log = Join-Path $runs "$Name.log"; Remove-Item $log -ErrorAction SilentlyContinue
        $env:SVR2011_INPUT_FILE = $input
        $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
        $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"   # native renderer window, off-screen too
        # dev: converted shaders (not for -GameDir: test the install as players get it)
        if (-not $GameDir) { $env:SVR2011_NATIVE_SHADERS = Join-Path $runs "shaders_native\dxil" } else { Remove-Item Env:SVR2011_NATIVE_SHADERS -ErrorAction SilentlyContinue }
        $a = @("--log_file=`"$log`"", "--log_level=$LogLevel", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--mnk_mode=false", "--p2p_port_mapping=false") + $Rest
        Use-TestSaves
        Use-SvrTestServer -GameDir $game -GameArgs $a | Out-Null
        $p = Start-Process (Join-Path $game $Exe) -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
        Set-SvrTestPriority $p
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
        Add-Content $input ($Rest -join "`n")
        "queued: $($Rest -join ' | ')"
    }
    "route" {
        # (the script waits on the game itself: "title", "menu <LABEL>"; this waits for its end)
        $s = Get-Session
        $before = @(Select-String -Path $s.log -Pattern "script input: all steps done" -ErrorAction SilentlyContinue).Count
        Add-Content $input "route $($Rest[0])"
        $t0 = Get-Date
        while (((Get-Date) - $t0).TotalSeconds -lt 300) {
            Start-Sleep -Milliseconds 500
            if (@(Select-String -Path $s.log -Pattern "script input: all steps done" -ErrorAction SilentlyContinue).Count -gt $before) { break }
        }
        Select-String -Path $s.log -Pattern "script input: (title:|menu .+: |until .+: |no route)" | Where-Object { $_.Line -notmatch "menu group" } | Select-Object -Last 8 | ForEach-Object { $_.Line.Substring(26) }
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
        $scale = if ($env:SVR2011_SHOT_FULL) { 1.0 } else { [math]::Min(1.0, 1280.0 / $w) }  # (SVR2011_SHOT_FULL=1: full size)
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
        Stop-TestGames
        "stopped"
    }
    default { "usage: session.ps1 start|input|route|shot|shotnative|fps|status|stop" }
}

# netperf copy of online_session.ps1: runs\netperf_game from this worktree's out\build\SourceRelease.
# Online tests: a background game session (muted, never focused, off-screen)
# from the online build (out\build\SourceOnline) in its own game folder
# (runs\online_game: the program files copied, the game data linked from
# Game Files), with its own state, so it never touches another test's game.
#   .\online_session.ps1 start [-Name on1] [-Config test_config_online.toml] [-UserData <dir>]
#   .\online_session.ps1 input "press START" "wait 2000" "press A"
#   .\online_session.ps1 shot <file-stem>      (native renderer window: shotnative)
#   .\online_session.ps1 status | stop
# -Slot 2 (3, 4): a second game at the same time (its own session and input
# files; give it its own -UserData and a -Config with p2p_port = 36100).
param([Parameter(Position = 0)][string]$Action, [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest,
      [string]$Name = "online", [string]$Config = "test_config_online.toml", [string]$UserData = "", [string]$LogLevel = "info",
      [string]$Slot = "")
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class OW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
}
"@
[void][OW]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")
$Rest = @($Rest | Where-Object { $_ })

$port  = Split-Path $PSScriptRoot -Parent
$runs  = Join-Path $port "runs"
$game  = Join-Path $runs "netperf_game"
$build = Join-Path $port "out\build\SourceRelease"
$state = Join-Path $runs "netperf_session$Slot.json"
$input = Join-Path $runs "netperf_input$Slot.txt"

function Get-OnlineSession {
    if (-not (Test-Path $state)) { throw "no session - run: online_session.ps1 start" }
    $s = Get-Content $state | ConvertFrom-Json
    $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -notlike "svr2011*") { throw "game is not running (pid $($s.pid)); log: $($s.log)" }
    return @{ proc = $p; log = $s.log }
}

function Stop-OnlineSession {
    if (-not (Test-Path $state)) { return }
    $s = Get-Content $state | ConvertFrom-Json
    $p = Get-CimInstance Win32_Process -Filter "ProcessId = $($s.pid)" -ErrorAction SilentlyContinue
    if ($p -and $p.Name -eq "svr2011.exe" -and (Test-IsTestGame $p)) {
        Stop-Process -Id $s.pid -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $s.pid -Timeout 15 -ErrorAction SilentlyContinue
    }
}

switch ($Action) {
    "start" {
        Assert-NoPlayerGame
        Stop-OnlineSession
        # the program files of the online build
        # (the SDK's DLLs from where it builds them: the copies beside the exe
        # are only refreshed when the exe relinks)
        $sdkOut = Join-Path $PSScriptRoot "..\..\recomp\rexglue-sdk\out\win-amd64"  # (this worktree's own SDK)
        # Only what changed; the stopped game's files can stay locked a moment.
        function Copy-Changed($src) {
            $dst = Join-Path $game (Split-Path $src -Leaf)
            $a = Get-Item $src; $b = Get-Item $dst -ErrorAction SilentlyContinue
            if ($b -and $a.Length -eq $b.Length -and $a.LastWriteTimeUtc -eq $b.LastWriteTimeUtc) { return }
            for ($t = 0; $t -lt 30; $t++) {
                try { Copy-Item $src $dst -Force -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 500 }
            }
            throw "could not copy $src (in use)"
        }
        $sdkNames = "rexruntime.dll", "rexgpu-xenos.dll"
        # (a second game at the same time runs the first one's files)
        if (-not $Slot) {
            Get-ChildItem $build -Filter "*.dll" | Where-Object { $sdkNames -notcontains $_.Name } | ForEach-Object { Copy-Changed $_.FullName }
            Copy-Changed (Join-Path $build "svr2011.exe")
            foreach ($f in $sdkNames) { Copy-Changed (Join-Path $sdkOut $f) }
        }
        Set-Content $input "" -NoNewline
        $log = Join-Path $runs "$Name.log"; Remove-Item $log -ErrorAction SilentlyContinue
        $env:SVR2011_INPUT_FILE = $input
        $env:SVR2011_CONFIG = Join-Path $runs $Config
        $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
        $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
        # (NP_SHADERS: another shader folder - the D3D11 runs need the .dxbc ones)
        $env:SVR2011_NATIVE_SHADERS = if ($env:NP_SHADERS) { $env:NP_SHADERS } else { Join-Path $runs "shaders_native\dxil" }
        # a copy of the test saves (never the base test saves or the player's)
        if (-not $UserData) {
            $UserData = Join-Path $runs "test_userdata_online"
            if (-not (Test-Path $UserData)) {
                New-Item -ItemType Directory $UserData | Out-Null
                Get-ChildItem (Join-Path $runs "test_userdata") -Directory | Where-Object { $_.Name -ne '0000000000000000' } |
                    ForEach-Object { Copy-Item -Recurse $_.FullName $UserData }
                $dlc = Join-Path $runs "test_userdata\0000000000000000"
                if (Test-Path $dlc) { cmd /c mklink /J "$UserData\0000000000000000" "$((Get-Item $dlc).Target)" | Out-Null }
            }
        }
        $env:SVR2011_USER_DATA = $UserData
        $a = @("--log_file=`"$log`"", "--log_level=$LogLevel", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--mnk_mode=false", "--p2p_port_mapping=false") + $Rest
        Use-SvrTestServer -GameDir $game -GameArgs $a | Out-Null
        $p = Start-Process (Join-Path $game "svr2011.exe") -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
        for ($t = 0; $t -lt 100 -and -not $p.HasExited; $t++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
        if ($p.MainWindowHandle -ne 0) {
            [void][OW]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)  # (screen 2, behind every window)
            [void][OW]::ShowWindow($p.MainWindowHandle, 4)
        }
        @{ pid = $p.Id; log = $log } | ConvertTo-Json | Set-Content $state
        "started pid $($p.Id), log $log"
    }
    "input" {
        $null = Get-OnlineSession
        # (the game reads the file: retry a moment if it has it open)
        for ($t = 0; $t -lt 20; $t++) {
            try { Add-Content $input ($Rest -join "`n") -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 50 }
        }
        "queued: $($Rest -join ' | ')"
    }
    { $_ -in "shot", "shotnative" } {
        $s = Get-OnlineSession
        $s.proc.Refresh()
        $h = [IntPtr]::Zero
        if ($Action -eq "shotnative") {
            # (the native window of this process)
            $h = [OW]::FindWindowW("SvR2011NativeRenderer", [NullString]::Value)
        } else { $h = $s.proc.MainWindowHandle }
        if ($h -eq [IntPtr]::Zero) { throw "window not found" }
        $r = New-Object OW+RECT; [void][OW]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [void][OW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
        $scale = [math]::Min(1.0, 1280.0 / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($hh * $scale))
        $file = Join-Path $runs (($Rest | Select-Object -First 1) + ".png")
        $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $small.Dispose(); $g.Dispose(); $bmp.Dispose()
        $file
    }
    "status" {
        $s = Get-OnlineSession
        "running pid $($s.proc.Id), cpu $([int]$s.proc.CPU)s"
    }
    "stop" { Stop-OnlineSession; "stopped"; & (Join-Path $PSScriptRoot "test_server.ps1") stop | Out-Null }  # (the test server: when no test game is left)
    default { "usage: online_session.ps1 start|input|shot|shotnative|status|stop" }
}

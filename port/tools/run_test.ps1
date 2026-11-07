# Launch the game from "Game Files", screenshot its window every few seconds,
# then stop it and print the log's errors/tail.
# Runs in the background: audio muted, window never activated, opened on the
# second screen (--monitor=2) behind every window there (it still renders and
# PrintWindow captures it; parking it off-screen after --monitor=2 captured black).
#   .\run_test.ps1 [-Seconds 60] [-Shots 4] [-Name run] [-Extra "--flag=x"] [-Game <folder>]
#   (-Game: another game folder - a test build next to links to the game data)
param([int]$Seconds = 60, [int]$Shots = 4, [string]$Name = "run", [string[]]$Extra = @(), [string]$Game = "")
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@

[void][W]::SetProcessDPIAware()   # physical pixels, or high-DPI captures come out cropped
. (Join-Path $PSScriptRoot "test_guard.ps1")
Assert-NoPlayerGame

$game = if ($Game) { $Game } else { Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "Game Files" }
$out  = Join-Path $PSScriptRoot "..\runs" | ForEach-Object { New-Item -ItemType Directory -Force $_ } | Select-Object -ExpandProperty FullName
$log  = Join-Path $out "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue

$args_ = @("--log_file=`"$log`"", "--log_level=debug", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--mnk_mode=false", "--p2p_port_mapping=false") + $Extra
# SDL hints (read from the environment): don't take focus when the window opens.
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
Use-TestSaves
Use-SvrTestServer -GameDir $game -GameArgs $args_ | Out-Null
$p = Start-Process (Join-Path $game "svr2011.exe") -WorkingDirectory $game -ArgumentList $args_ -PassThru -WindowStyle Minimized
Set-SvrTestPriority $p

# Park the window off-screen without activating it as soon as it exists.
$SWP_NOSIZE = 0x1; $SWP_NOZORDER = 0x4; $SWP_NOACTIVATE = 0x10
for ($t = 0; $t -lt 100; $t++) {
    $p.Refresh()
    if ($p.HasExited -or $p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 100
}
if (-not $p.HasExited -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    [void][W]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)  # (screen 2, behind every window: HWND_BOTTOM)
    Add-Type -Name U -Namespace N -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);' -ErrorAction SilentlyContinue
    [void][N.U]::ShowWindow($p.MainWindowHandle, 4)   # SW_SHOWNOACTIVATE: restore without focus
}

$interval = [math]::Max(1, [int]($Seconds / [math]::Max(1, $Shots)))
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep $interval
    if ($p.HasExited) { break }
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { "shot $i : no window yet"; continue }
    $r = New-Object W+RECT
    [void][W]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $gfx.GetHdc()
    [void][W]::PrintWindow($h, $dc, 2)   # PW_RENDERFULLCONTENT
    $gfx.ReleaseHdc($dc)
    $png = Join-Path $out ("{0}_{1}.png" -f $Name, $i)
    # Save at most 1280 wide so captures stay small.
    $scale = [math]::Min(1.0, 1280.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($hh * $scale))
    $small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $gfx.Dispose(); $bmp.Dispose()
    "shot $i : $png"
}

if ($p.HasExited) { "EXITED early, code $($p.ExitCode)" } else { Stop-Process -Id $p.Id -Force; "stopped after ${Seconds}s" }
Start-Sleep 1
if (Test-Path $log) {
    $lines = Get-Content $log
    "log: $log ($($lines.Count) lines)"
    "--- errors/warnings (first 40) ---"
    $lines | Select-String -Pattern "\[(error|critical|warning)\]" | Select-Object -First 40 | ForEach-Object { $_.Line }
    "--- tail ---"
    $lines | Select-Object -Last 25
}

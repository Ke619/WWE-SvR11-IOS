# A scripted CPU-vs-CPU match for the limits work (runs\lim_game): one game,
# muted, screen 2 behind every window, below normal, test saves. The menus
# to ONE ON ONE (route normal) with SVR2011_TEST_MATCH's people, then -Secs of
# the match (from when its clock runs) with a shot every -ShotEvery s.
#   .\lim_cell.ps1 -Name cell1 -Rule 31 [-Env "SVR2011_TEST_CELL=1","SVR2011_TEST_CELL_ROOF=-80"] [-Secs 90]
param([string]$Name = "cell", [string]$Rule = "", [string[]]$Env = @(), [int]$Secs = 90, [int]$ShotEvery = 15,
      [string]$People = "JOHN CENA,RANDY ORTON", [string]$Arena = "1", [string[]]$Extra = @(), [string[]]$AfterStart = @(), [switch]$Human)
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("WC" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class WC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}
[void][WC]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")
Assert-NoPlayerGame
$port = Split-Path $PSScriptRoot -Parent
$game = Join-Path $port "runs\lim_game"
$runs = Join-Path $port "runs"

function Shot($p, $png) {
    $p.Refresh(); if ($p.HasExited) { return }; $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object WC+RECT; [void][WC]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][WC]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
    $s = [math]::Min(1.0, 960.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $s)), ([int]($hh * $s))
    $small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $g.Dispose(); $bmp.Dispose()
}

$set = @()
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
$env:SVR2011_WINDOW_BEHIND = "1"
$env:SVR2011_TEST_MATCH = "people=$People arena=$Arena" + $(if ($Human) { "" } else { " cpu=all" }); $set += "SVR2011_TEST_MATCH"
if ($Rule) { $env:SVR2011_TEST_RULE = $Rule; $set += "SVR2011_TEST_RULE" }
$env:SVR2011_FPS_PROBE = "1"; $set += "SVR2011_FPS_PROBE"
foreach ($e in $Env) { $k, $v = $e.Split("=", 2); [Environment]::SetEnvironmentVariable($k, $v); $set += $k }
$in = Join-Path $runs "lim_cell_input.txt"; Set-Content $in "route normal`npressuntil A test match: people" -Encoding ascii
$env:SVR2011_INPUT_FILE = $in; $env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"; $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
$set += "SVR2011_INPUT_FILE", "SVR2011_CONFIG", "SVR2011_USER_DATA", "SVR2011_NATIVE_WINDOW_POS", "SVR2011_WINDOW_BEHIND"
$log = Join-Path $runs "lim_$Name.log"; Remove-Item $log -ErrorAction SilentlyContinue
$a = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2") + $Extra
Use-SvrTestServer -GameDir $game -GameArgs $a | Out-Null
$p = Start-Process (Join-Path $game "svr2011.exe") -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
try {
    for ($t = 0; $t -lt 100; $t++) { $p.Refresh(); if ($p.HasExited -or $p.MainWindowHandle -ne [IntPtr]::Zero) { break }; Start-Sleep -Milliseconds 100 }
    if (-not $p.HasExited) {
        try { $p.PriorityClass = "Normal" } catch {}
        [void][WC]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)
        [void][WC]::ShowWindow($p.MainWindowHandle, 4)
    }
    $ok = $false
    for ($t = 0; $t -lt 600 -and -not $p.HasExited; $t++) {
        Start-Sleep 1
        if (-not (Test-Path $log)) { continue }
        $at = Select-String -Path $log -Pattern "\] test match: people" | Select-Object -First 1
        if (-not $at) { continue }
        $run = @(Select-String -Path $log -Pattern "fps probe: (\d+) game frames" | Where-Object { $_.LineNumber -gt $at.LineNumber } |
                 ForEach-Object { [long]$_.Matches[0].Groups[1].Value })
        if ($run.Count -ge 3 -and ($run[-3..-1] | Where-Object { $_ -gt 20 -and $_ -lt 200 }).Count -eq 3) { $ok = $true; break }
    }
    if (-not $ok) { "$Name : no match ($(if ($p.HasExited) { 'exited' } else { 'timeout' }))"; Shot $p (Join-Path $runs "lim_${Name}_stuck.png"); return }
    if ($AfterStart.Count) { Add-Content $in ($AfterStart -join "`n") -Encoding ascii }  # (the match's own inputs)
    $t0 = Get-Date; $i = 0
    while (((Get-Date) - $t0).TotalSeconds -lt $Secs -and -not $p.HasExited) {
        Start-Sleep $ShotEvery; $i++
        Shot $p (Join-Path $runs ("lim_{0}_{1:D2}.png" -f $Name, $i))
    }
    "$Name : ran $([int]((Get-Date) - $t0).TotalSeconds) s of the match, $i shots$(if ($p.HasExited) { ' - EXITED' })"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Wait-Process -Id $p.Id -Timeout 15 -ErrorAction SilentlyContinue }
    foreach ($k in $set) { [Environment]::SetEnvironmentVariable($k, $null) }
}

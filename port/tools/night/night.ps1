# Overnight runner (docs/OVERNIGHT_TESTS.md): runs runs\night\scenarios.jsonl
# (gen_scenarios.py), up to -Workers games at once, each in its own worker
# folder (runs\night\w<N>: own exe copy, UserData, Mods copy, saves; disc data
# linked from Game Files). Writes runs\night\results.jsonl (one line per
# scenario; a rerun skips those already there unless -Redo).
# Never on the player's screen: --monitor=2, behind every window before it
# shows (SVR2011_WINDOW_BEHIND), muted, never activated.
#   night.ps1 [-Workers 4] [-Only B,C] [-Ids B2B,C100_101] [-Redo] [-Setup]
param([int]$Workers = 4, [string[]]$Only = @(), [string[]]$Ids = @(), [switch]$Redo, [switch]$Setup,
      [string]$Scenarios = "")
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Continue"
# (powershell -File passes "a,b" as one string)
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Ids = @($Ids | ForEach-Object { $_ -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class NW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][NW]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "..\test_guard.ps1")

$port  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$top   = Split-Path $port -Parent
$game  = Join-Path $top "Game Files"
$night = Join-Path $port "runs\night"
$results = Join-Path $night "results.jsonl"
if (-not $Scenarios) { $Scenarios = Join-Path $night "scenarios.jsonl" }
New-Item -ItemType Directory -Force $night | Out-Null

# ---- worker folders ---------------------------------------------------------
function Sync-Worker([int]$w) {
    $d = Join-Path $night "w$w"
    New-Item -ItemType Directory -Force $d | Out-Null
    # program files: refreshed when Game Files has a newer build
    foreach ($f in "svr2011.exe", "rexruntime.dll", "rexgpu-xenos.dll", "rexffmpeg.dll", "rexmspack.dll", "default.xex", "nxeart", "plist360.arc", "plist360_4x3.arc") {
        $src = Join-Path $game $f; $dst = Join-Path $d $f
        if ((Test-Path $src) -and (-not (Test-Path $dst) -or (Get-Item $src).LastWriteTime -ne (Get-Item $dst).LastWriteTime)) {
            Copy-Item $src $dst -Force
        }
    }
    # read-only data: linked
    foreach ($j in "pac", "sound", "movies", "DLC", "`$SystemUpdate", "pad_icons", "Music", "Custom Movies") {
        $src = Join-Path $game $j; $dst = Join-Path $d $j
        if ((Test-Path $src) -and -not (Test-Path $dst)) { New-Item -ItemType Junction -Path $dst -Target $src | Out-Null }
    }
    # written at start (overlays, caches): copied
    robocopy (Join-Path $game "native_shaders") (Join-Path $d "native_shaders") /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    robocopy (Join-Path $game "Mods") (Join-Path $d "Mods") /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    # every mod on in the tests (bundled ones install switched off)
    Get-ChildItem (Join-Path $d "Mods") -Recurse -Filter "disabled" -File -ErrorAction SilentlyContinue | Remove-Item -Force
    # user data: its own; the installed DLC with its own catalogs (info\ copied, content linked)
    $ud = Join-Path $d "UserData"
    $src = Join-Path $game "UserData\0000000000000000\5451085D"
    $t = Join-Path $ud "0000000000000000\5451085D"
    if (-not (Test-Path $t)) {
        New-Item -ItemType Directory -Force (Join-Path $t "00000002") | Out-Null
        Copy-Item -Recurse (Join-Path $src "Headers") (Join-Path $t "Headers") -Force
        foreach ($pkg in Get-ChildItem (Join-Path $src "00000002") -Directory) {
            $pd = Join-Path $t "00000002\$($pkg.Name)"
            New-Item -ItemType Directory -Force $pd | Out-Null
            foreach ($sub in Get-ChildItem $pkg.FullName -Directory) {
                if ($sub.Name -eq "info") { Copy-Item -Recurse $sub.FullName (Join-Path $pd "info") -Force }
                else { New-Item -ItemType Junction -Path (Join-Path $pd $sub.Name) -Target $sub.FullName | Out-Null }
            }
        }
    }
    $saves = Join-Path $ud "Saves"   # (with SVR2011_USER_DATA the game reads <user data>\Saves)
    if (-not (Test-Path (Join-Path $saves "SaveData.dat"))) {
        New-Item -ItemType Directory -Force $saves | Out-Null
        Get-ChildItem (Join-Path $port "runs\test_userdata\Saves") -Force | Where-Object { $_.Name -ne ".online" } |
            ForEach-Object { Copy-Item $_.FullName $saves -Recurse -Force }
    }
    Copy-Item (Join-Path $port "runs\test_config.toml") (Join-Path $d "test_config.toml") -Force
    return $d
}

$wdirs = @()
for ($w = 0; $w -lt $Workers; $w++) { $wdirs += Sync-Worker $w }
if ($Setup) { "workers ready: $($wdirs -join ', ')"; return }

# ---- scenarios --------------------------------------------------------------
$done = @{}
if ((Test-Path $results) -and -not $Redo) {
    Get-Content $results | ForEach-Object { try { $done[($_ | ConvertFrom-Json).id] = 1 } catch {} }
}
$queue = New-Object System.Collections.Queue
Get-Content $Scenarios | ForEach-Object {
    $s = $_ | ConvertFrom-Json
    if ($Only.Count -and $Only -notcontains $s.suite) { return }
    if ($Ids.Count -and $Ids -notcontains $s.id) { return }
    if ($done.ContainsKey($s.id)) { return }
    $queue.Enqueue($s)
}
"$($queue.Count) scenarios to run, $Workers at a time"

# ---- helpers ----------------------------------------------------------------
function Shot($proc, [string]$file) {
    $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $null }
    $r = New-Object NW+RECT
    [void][NW]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc(); [void][NW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    # broken-picture check: brightness and spread over a 32x18 grid
    $vals = @()
    for ($y = 0; $y -lt 18; $y++) { for ($x = 0; $x -lt 32; $x++) {
        $c = $bmp.GetPixel([int](($x + 0.5) * $w / 32), [int](($y + 0.5) * $hh / 18))
        $vals += (0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B) } }
    $mean = ($vals | Measure-Object -Average).Average
    $sd = [math]::Sqrt((($vals | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Average).Average)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / 2)), ([int]($hh / 2))
    $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Jpeg)
    $small.Dispose(); $bmp.Dispose()
    return @{ file = (Split-Path $file -Leaf); mean = [math]::Round($mean, 1); sd = [math]::Round($sd, 1) }
}

$errPattern = 'SvR 2011 crash|\[FATAL\]|Unhandled guest access|device lost|DEVICE_REMOVED|can''t draw the game|not in its list|failed to load'
function Start-Scenario($s, [int]$w) {
    $d = $wdirs[$w]
    $log = Join-Path $night "logs\$($s.id).log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    Remove-Item $log -ErrorAction SilentlyContinue
    $input = Join-Path $d "input.txt"; Set-Content $input ""
    # this scenario's test aids only
    foreach ($k in @("SVR2011_ROUTE", "SVR2011_TEST_MATCH", "SVR2011_TEST_RULE", "SVR2011_TEST_ARENA_REDIRECT",
                     "SVR2011_NATIVE_DEPTH_D32", "SVR2011_TEST_HALF_30", "SVR2011_TEST_MODE")) { Set-Item "env:$k" "" }
    foreach ($p in $s.env.PSObject.Properties) { Set-Item "env:$($p.Name)" $p.Value }
    $env:SVR2011_INPUT_FILE = $input
    $env:SVR2011_CONFIG = Join-Path $d "test_config.toml"
    $env:SVR2011_USER_DATA = Join-Path $d "UserData"
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    $args_ = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2",
               "--show_fps=false") + @($s.args)
    Use-SvrTestServer -GameDir $d -GameArgs $args_ | Out-Null
    $p = Start-Process (Join-Path $d "svr2011.exe") -WorkingDirectory $d -ArgumentList $args_ -PassThru -WindowStyle Minimized
    return @{ proc = $p; s = $s; w = $w; log = $log; start = Get-Date; pos = 0; text = ""; tmStarted = $false;
              passed = $false; needsSeen = (-not $s.needs); err = @(); stuckMax = 0; fps = @(); worst = @(); shots = @(); parked = $false;
              nextShot = 0; lastFpsAt = Get-Date }
}

function Read-New($r) {
    if (-not (Test-Path $r.log)) { return }
    try {
        $fs = [System.IO.File]::Open($r.log, 'Open', 'Read', 'ReadWrite')
        if ($fs.Length -lt $r.pos) { $r.pos = 0 }
        [void]$fs.Seek($r.pos, 'Begin')
        $sr = New-Object System.IO.StreamReader($fs)
        $new = $sr.ReadToEnd(); $r.pos = $fs.Position
        $sr.Close(); $fs.Close()
    } catch { return }
    foreach ($line in ($new -split "`n")) {
        if (-not $line) { continue }
        if ($line -match 'test match: ') { $r.tmStarted = $true }
        if ($r.s.needs -and $line -match $r.s.needs) { $r.needsSeen = $true }
        if ($r.tmStarted -and $line -match $r.s.pass) { $r.passed = $true }
        # Tag rules skip the ending's highlights (no step 3): the test's own match
        # reaching "match end: step 0" and then the after-match menu also counts.
        if ($r.tmStarted -and $line -match '\[svr2011\] match: rule') { $r.inMatch = $true }
        if ($r.inMatch -and $line -match 'match end: step 0 \(') { $r.endAt = Get-Date }
        if ($line -match 'match end: step \d waiting') { $r.endWait = $true }
        if ($line -match $errPattern -and $r.err.Count -lt 12) { $r.err += $line.Substring(0, [Math]::Min(240, $line.Length)) }
        if ($line -match 'world update stuck (\d+) s') { $r.stuckMax = [Math]::Max($r.stuckMax, [int]$Matches[1])
            if ($r.err.Count -lt 12) { $r.err += $line.Substring(0, [Math]::Min(240, $line.Length)) } }
        if ($line -match 'test match: no superstar') { $r.err += $line.Substring(0, [Math]::Min(200, $line.Length)) }
        if ($line -match 'fps: ([\d.]+) avg, worst frame ([\d.]+) ms') {
            $r.fps += [double]$Matches[1]; $r.worst += [double]$Matches[2]; $r.lastFpsAt = Get-Date }
    }
}

function Finish($r, [string]$result) {
    if (-not $r.proc.HasExited) { Stop-Process -Id $r.proc.Id -Force -ErrorAction SilentlyContinue; Wait-Process -Id $r.proc.Id -Timeout 15 -ErrorAction SilentlyContinue }
    Read-New $r
    $secs = [int]((Get-Date) - $r.start).TotalSeconds
    $fpsAfter = if ($r.fps.Count -gt 3) { $r.fps[3..($r.fps.Count - 1)] } else { $r.fps }
    $row = [ordered]@{
        id = $r.s.id; suite = $r.s.suite; name = $r.s.name; result = $result; seconds = $secs; worker = $r.w
        errors = $r.err; stuckMax = $r.stuckMax
        fpsMinAvg = if ($fpsAfter.Count) { ($fpsAfter | Measure-Object -Minimum).Minimum } else { $null }
        fpsMeanAvg = if ($fpsAfter.Count) { [math]::Round(($fpsAfter | Measure-Object -Average).Average, 1) } else { $null }
        worstFrameMs = if ($r.worst.Count) { ($r.worst | Measure-Object -Maximum).Maximum } else { $null }
        shots = $r.shots; log = (Split-Path $r.log -Leaf); time = (Get-Date -Format s)
    }
    ($row | ConvertTo-Json -Compress -Depth 4) | Add-Content $results
    "{0} {1,-22} {2,-8} {3,4}s  {4}" -f (Get-Date -Format HH:mm:ss), $r.s.id, $result, $secs, $r.s.name
}

# ---- main loop --------------------------------------------------------------
$running = @{}
while ($queue.Count -or $running.Count) {
    # the player's own game: wait (never test beside it)
    try { Assert-NoPlayerGame } catch { Start-Sleep -Seconds 30; continue }
    for ($w = 0; $w -lt $Workers; $w++) {
        if (-not $running.ContainsKey($w) -and $queue.Count) { $running[$w] = Start-Scenario $queue.Dequeue() $w; Start-Sleep -Seconds 3 }
    }
    Start-Sleep -Seconds 2
    foreach ($w in @($running.Keys)) {
        $r = $running[$w]
        Read-New $r
        $age = ((Get-Date) - $r.start).TotalSeconds
        if (-not $r.parked) {
            $r.proc.Refresh()
            if ($r.proc.MainWindowHandle -ne [IntPtr]::Zero) {
                # screen 2, behind every window (HWND_BOTTOM), never activated
                [void][NW]::SetWindowPos($r.proc.MainWindowHandle, [IntPtr]1, $SvrParkX + 40 * $w, 40 * $w, 0, 0, $SvrParkFlags)
                $r.parked = $true
            }
        }
        if ($r.nextShot -lt $r.s.shots.Count -and $age -ge $r.s.shots[$r.nextShot]) {
            $f = Join-Path $night ("shots\{0}_{1}.jpg" -f $r.s.id, $r.nextShot)
            New-Item -ItemType Directory -Force (Split-Path $f) | Out-Null
            $sh = $null; try { $r.proc.Refresh(); $sh = Shot $r.proc $f } catch {}
            if ($sh) { $r.shots += $sh }
            $r.nextShot++
        }
        $result = $null
        if ($r.proc.HasExited) { $result = if ($r.passed) { "pass" } else { "crash" } }
        elseif ($r.err | Where-Object { $_ -match 'SvR 2011 crash|FATAL' }) { $result = "crash" }
        elseif ($r.err | Where-Object { $_ -match 'device lost|DEVICE_REMOVED|can''t draw' }) { $result = "gpu" }
        elseif ($r.stuckMax -ge 30) { $result = "hang" }
        elseif ($r.passed) { Start-Sleep -Seconds 1; $result = "pass" }
        elseif ($r.endAt -and -not $r.endWait -and ((Get-Date) - $r.endAt).TotalSeconds -gt 40 -and
                $r.lastFpsAt -and ((Get-Date) - $r.lastFpsAt).TotalSeconds -lt 15) { $result = "pass" }
        elseif ($r.err | Where-Object { $_ -match 'test match: no superstar' }) { $result = "invalid" }
        elseif ($age -gt $r.s.timeout) { $result = "timeout" }
        elseif ($r.fps.Count -gt 0 -and ((Get-Date) - $r.lastFpsAt).TotalSeconds -gt 45) { $result = "hang" }
        elseif ($r.fps.Count -eq 0 -and $age -gt 420) { $result = "hang" }   # (a first start merges the move packs: ~2 min)
        if ($result) {
            if ($result -eq "pass" -and ($r.shots | Where-Object { $_.sd -lt 3 -and $_.mean -lt 10 }).Count -ge 2) { $result = "black" }
            if ($result -eq "pass" -and -not $r.needsSeen) { $result = "mode-missing" }
            Finish $r $result
            $running.Remove($w)
        }
    }
}
"all done -> $results"

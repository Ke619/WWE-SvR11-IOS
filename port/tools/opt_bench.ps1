# The optimization benchmark (port/docs/PERF_LOG.md): one scripted run of
# the same route - boot and the title (its demo match), the menus to ONE ON
# ONE, a match with entrances, -MatchSecs of the match - in its own game
# folder (runs\opt_bench, a copy of the program files with the game data
# linked in), muted, off-screen, its own test saves and settings. Stops only
# its own game, so it runs beside other sessions' tests.
#   opt_bench.ps1 -Name base1                     normal
#   opt_bench.ps1 -Name base1w -Cond weak         2 cores (-Mask 0x5: two
#                                                 physical cores) + -Load busy
#                                                 threads at normal priority there
#   -Game <dir>  another build's folder   -Set "key = value" (config lines)
#   -UntilEnd (play on until the match-end highlights)   -Env "NAME=value" (environment for the game)   -Fps 30   -Report (results of runs\opt_<Name>.log again)
# Prints a markdown row per phase (also appended to runs\opt_results.md):
# avg fps, p99 (worst 5 s window's), frames over 51 ms, worst frame, from the
# game's 5 s "fps:" / "frame times:" lines; and the PC's CPU load meanwhile.
param([string]$Name = "bench", [ValidateSet("normal", "weak", "phone")][string]$Cond = "normal", [string]$Game = "",
      [int]$Fps = 60, [string[]]$Set = @(), [string[]]$Env = @(), [string]$People = "BATISTA,KANE",
      [int]$Arena = 1, [int]$TitleWait = 100, [int]$MatchSecs = 60, [int]$Mask = 0x5, [int]$Load = 1,
      [switch]$Keep, [switch]$Report, [switch]$UntilEnd)
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("OB" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class OB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}
[void][OB]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")

$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port "runs"
if (-not $Game) { $Game = Join-Path $runs "opt_bench" }
$state = Join-Path $runs "opt_bench.json"
$inputFile = Join-Path $runs "opt_bench_input.txt"
$log = Join-Path $runs "opt_$Name.log"

function Stop-Own {
    if (-not (Test-Path $state)) { return }
    $s = Get-Content $state | ConvertFrom-Json
    foreach ($id in @($s.pid) + @($s.load)) {
        if (-not $id) { continue }
        $p = Get-CimInstance Win32_Process -Filter "ProcessId = $id" -ErrorAction SilentlyContinue
        if ($p -and ([string]$p.CommandLine -match [regex]::Escape("\runs\opt_") -or [string]$p.CommandLine -match "opt_bench_load")) {
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $id -Timeout 15 -ErrorAction SilentlyContinue
        }
    }
    Remove-Item $state -ErrorAction SilentlyContinue
}

function Shot($proc, $stem) {
    try {
        $proc.Refresh(); $h = $proc.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { return }
        $r = New-Object OB+RECT; [void][OB]::GetWindowRect($h, [ref]$r)
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [void][OB]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
        $bmp.Save((Join-Path $runs "opt_${Name}_$stem.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
    } catch {}
}

$cpu = New-Object System.Collections.Generic.List[double]
$script:pinned = $null  # (weak: the game, kept on -Mask - the runtime sets the whole PC's cores as it starts)
function Sample-Cpu {
    $cpu.Add([double](Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime)
    if ($script:pinned) { try { $script:pinned.Refresh(); if ([int64]$script:pinned.ProcessorAffinity -ne $Mask) { $script:pinned.ProcessorAffinity = [IntPtr]$Mask } } catch {} }
}

# The log line number (1-based) of the first line after line $after matching $pattern; 0: not within $seconds.
function Wait-Log($pattern, $seconds, $proc, $after = 0) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds) {
        if ($proc.HasExited) { throw "the game exited (log $log)" }
        if (Test-Path $log) {
            $hit = Select-String -Path $log -Pattern $pattern | Where-Object { $_.LineNumber -gt $after } | Select-Object -First 1
            if ($hit) { return $hit.LineNumber }
        }
        Start-Sleep -Seconds 2; Sample-Cpu
    }
    return 0
}

# -- run ---------------------------------------------------------------------
$others = 0
if (-not $Report) {
Assert-NoPlayerGame
Stop-Own
$cfg = Join-Path $runs "opt_cfg_$Name.toml"
$lines = Get-Content (Join-Path $runs "test_config_opt.toml") | Where-Object { $_ -notmatch '^frame_rate' }
$lines = @($lines | Where-Object { $l = $_; -not ($Set | Where-Object { $l -match ("^" + ($_ -split ' ')[0] + " ") }) })
[IO.File]::WriteAllLines($cfg, $lines + "frame_rate = $Fps" + $Set)
Set-Content $inputFile "" -NoNewline
Remove-Item $log -ErrorAction SilentlyContinue
$framesFile = Join-Path $runs "opt_${Name}_frames.txt"; Remove-Item $framesFile -ErrorAction SilentlyContinue
$env:SVR2011_FRAME_TIMES = $framesFile   # (builds with frame_stats.cpp's test aid: every frame's time)
$env:SVR2011_CONFIG = $cfg
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_opt"
$env:SVR2011_INPUT_FILE = $inputFile
$env:SVR2011_TEST_MATCH = "people=$People" + $(if ($Arena -ge 0) { " arena=$Arena" } else { "" })
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
$env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
Remove-Item Env:SVR2011_NATIVE_SHADERS -ErrorAction SilentlyContinue
foreach ($e in $Env) { $k, $v = $e -split '=', 2; Set-Item "Env:$k" $v }
$a = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2")
Use-SvrTestServer -GameDir $Game -GameArgs $a | Out-Null
$p = Start-Process (Join-Path $Game "svr2011.exe") -WorkingDirectory $Game -ArgumentList $a -PassThru -WindowStyle Minimized
$loadIds = @()
try {
    if ($Cond -eq "weak") {
        $p.ProcessorAffinity = [IntPtr]$Mask
        $script:pinned = $p
        for ($i = 0; $i -lt $Load; $i++) {
            $l = Start-Process powershell -ArgumentList "-NoProfile -Command `"`$host.UI.RawUI.WindowTitle='opt_bench_load'; while (`$true) {}`"" -PassThru -WindowStyle Hidden
            $l.ProcessorAffinity = [IntPtr]$Mask
            $loadIds += $l.Id
        }
    }
    @{ pid = $p.Id; load = $loadIds } | ConvertTo-Json | Set-Content $state
    for ($t = 0; $t -lt 100 -and -not $p.HasExited; $t++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
    if ($p.MainWindowHandle -ne 0) {
        [void][OB]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)  # (screen 2, behind every window)
        [void][OB]::ShowWindow($p.MainWindowHandle, 4)
    }
    Sample-Cpu
    $others = @(Get-SvrGames | Where-Object { $_.ProcessId -ne $p.Id }).Count
    # 1. boot, the title and its demo match
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TitleWait) {
        if ($p.HasExited) { throw "the game exited (log $log)" }
        Start-Sleep -Seconds 5; Sample-Cpu
    }
    Shot $p "title"
    # 2. the menus into the match: SVR2011_TEST_MATCH's people and arena (not
    # "route match": its A presses go on into the loading and skip the entrances)
    Add-Content $inputFile "route normal`npressuntil A test match: people"
    $people = Wait-Log "\] test match: people" 240 $p
    if (-not $people) { Shot $p "stuck_menu"; throw "no match after 240 s" }
    # 3. the load and the entrances
    Start-Sleep -Seconds 30; Shot $p "entrance"
    if (-not (Wait-Log "entrances over|match starts" 300 $p $people)) { Shot $p "stuck_entrance"; throw "no match start after 300 s" }
    Shot $p "entrance_end"
    # 4. the match (-UntilEnd: until the CPU wins and the match-end highlights play)
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $MatchSecs) {
        if ($p.HasExited) { throw "the game exited (log $log)" }
        Start-Sleep -Seconds 5; Sample-Cpu
    }
    Shot $p "match"
    if ($UntilEnd) {
        if (-not (Wait-Log "match-end highlights: \d+ clips" 720 $p)) { Shot $p "stuck_end"; throw "no match-end highlights after 12 min" }
        Start-Sleep -Seconds 20; Shot $p "highlights"
    }
} finally {
    if (-not $Keep) { Stop-Own }
}
}  # (-Report: only the results of an earlier run's log)

# -- results -----------------------------------------------------------------
$ts = { param($line) [datetime]::ParseExact($line.Substring(1, 23), "yyyy-MM-dd HH:mm:ss.fff", $null) }
$all = Get-Content $log
$mark = @{}
$after = 0
foreach ($m in @(@{k = "menus"; p = "script input: route normal"}, @{k = "load"; p = "\] test match: people"},
                 @{k = "match"; p = "entrances over|match starts"})) {
    $hit = $all | Select-String -Pattern $m.p | Where-Object { $_.LineNumber -gt $after } | Select-Object -First 1
    if ($hit) { $mark[$m.k] = & $ts $hit.Line; $after = $hit.LineNumber }
}
$phases = [ordered]@{ "title" = @(); "menus" = @(); "load+entr" = @(); "match" = @() }
$fpsLines = @($all | Select-String -Pattern "\] fps: ")
$ftLines = @($all | Select-String -Pattern "frame times: median")
$perfLines = @($all | Select-String -Pattern "native perf: [0-9.]+ fps, per frame")
for ($i = 0; $i -lt $fpsLines.Count; $i++) {
    $L = $fpsLines[$i].Line; $t = & $ts $L
    if ($L -notmatch "fps: ([0-9.]+) avg, worst frame ([0-9.]+) ms \((\d+) frames in ([0-9.]+) s; .* longer (\d+)\)") { continue }
    $w = [pscustomobject]@{ fps = [double]$Matches[1]; worst = [double]$Matches[2]; frames = [int]$Matches[3]; secs = [double]$Matches[4]; over51 = [int]$Matches[5]; p99 = 0.0; over20 = 0 }
    $ft = $ftLines | Where-Object { [math]::Abs(((& $ts $_.Line) - $t).TotalMilliseconds) -lt 50 } | Select-Object -First 1
    if ($ft -and $ft.Line -match "p99 ([0-9.]+).*over 20 ms (\d+)") { $w.p99 = [double]$Matches[1]; $w.over20 = [int]$Matches[2] }
    $ph = if ($mark.match -and $t -gt $mark.match) { "match" } elseif ($mark.load -and $t -gt $mark.load) { "load+entr" } elseif ($mark.menus -and $t -gt $mark.menus) { "menus" } else { "title" }
    $phases[$ph] += $w
}
# Exact, from every frame's time (SVR2011_FRAME_TIMES), when the build writes them.
$framesFile = Join-Path $runs "opt_${Name}_frames.txt"
$exact = @()
if ((Test-Path $framesFile) -and (Get-Item $framesFile).Length -gt 0) {
    $unix = { param($d) [DateTimeOffset]::new($d).ToUnixTimeMilliseconds() }
    $bounds = @{}; foreach ($k in "menus", "load", "match") { $bounds[$k] = if ($mark[$k]) { & $unix $mark[$k] } else { [int64]::MaxValue } }
    $byPhase = [ordered]@{ "title" = New-Object System.Collections.Generic.List[double]; "menus" = New-Object System.Collections.Generic.List[double]
                           "load+entr" = New-Object System.Collections.Generic.List[double]; "match" = New-Object System.Collections.Generic.List[double] }
    $first = 0
    foreach ($line in [IO.File]::ReadLines($framesFile)) {
        $parts = $line.Split(' '); if ($parts.Count -lt 2) { continue }
        $t = [int64]$parts[0]; $ms = [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture)
        if (-not $first) { $first = $t }
        if ($t - $first -lt 10000) { continue }   # (the first 10 s: start-up)
        $ph = if ($t -gt $bounds.match) { "match" } elseif ($t -gt $bounds.load) { "load+entr" } elseif ($t -gt $bounds.menus) { "menus" } else { "title" }
        $byPhase[$ph].Add($ms)
    }
    $allMs = New-Object System.Collections.Generic.List[double]
    $stat = { param($k, $v)
        if (-not $v.Count) { return }
        $a = $v.ToArray(); [Array]::Sort($a); $sum = 0.0; foreach ($x in $a) { $sum += $x }
        "| $Name | $Cond | $k | {0:N1} | {1:N1} | {2:N1} | {3} | {4} | {5:N1} |" -f ($a.Count * 1000.0 / $sum), $a[[int][math]::Floor(0.5 * ($a.Count - 1))],
            $a[[int][math]::Floor(0.99 * ($a.Count - 1))], @($a | Where-Object { $_ -gt 51 }).Count, @($a | Where-Object { $_ -gt 20 }).Count, $a[-1] }
    foreach ($k in $byPhase.Keys) { $exact += & $stat $k $byPhase[$k]; $allMs.AddRange($byPhase[$k]) }
    $exact += & $stat "**all**" $allMs
}
$load = if ($cpu.Count) { [math]::Round(($cpu | Measure-Object -Average).Average) } else { 0 }
$rows = @()
$total = @()
foreach ($k in $phases.Keys) {
    $ws = $phases[$k]
    if ($k -eq "title") { $ws = @($ws | Select-Object -Skip 2) }   # (the first 10 s: start-up)
    if (-not $ws.Count) { continue }
    $total += $ws
    $frames = ($ws | Measure-Object frames -Sum).Sum; $secs = ($ws | Measure-Object secs -Sum).Sum
    $rows += "| $Name | $Cond | $k | {0:N1} | {1:N1} | {2} | {3} | {4:N1} |" -f ($frames / $secs), ($ws | Measure-Object p99 -Maximum).Maximum,
        ($ws | Measure-Object over51 -Sum).Sum, ($ws | Measure-Object over20 -Sum).Sum, ($ws | Measure-Object worst -Maximum).Maximum
}
$frames = ($total | Measure-Object frames -Sum).Sum; $secs = ($total | Measure-Object secs -Sum).Sum
$rows += "| $Name | $Cond | **all** | {0:N1} | {1:N1} | {2} | {3} | {4:N1} |" -f ($frames / $secs), ($total | Measure-Object p99 -Maximum).Maximum,
    ($total | Measure-Object over51 -Sum).Sum, ($total | Measure-Object over20 -Sum).Sum, ($total | Measure-Object worst -Maximum).Maximum
$draw = @($perfLines | ForEach-Object { if ($_.Line -match "draw ([0-9.]+) ms") { [double]$Matches[1] } })
$note = "PC CPU load avg $load%, other svr2011 games $others, native draw ms/frame avg {0:N2} max {1:N2}" -f `
    (($draw | Measure-Object -Average).Average), (($draw | Measure-Object -Maximum).Maximum)
$out = Join-Path $runs "opt_results.md"
Add-Content $out ("`n### $Name ($Cond, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $Game)`n$note`n`n| run | cond | phase | avg fps | p99 ms (worst window) | frames >51 ms | frames >20 ms | worst ms |`n|---|---|---|---|---|---|---|---|")
Add-Content $out $rows
if ($exact.Count) {
    Add-Content $out ("`nEvery frame:`n`n| run | cond | phase | avg fps | median ms | p99 ms | frames >51 ms | frames >20 ms | worst ms |`n|---|---|---|---|---|---|---|---|---|")
    Add-Content $out $exact
}
$note
$rows
if ($exact.Count) { "every frame:"; $exact }

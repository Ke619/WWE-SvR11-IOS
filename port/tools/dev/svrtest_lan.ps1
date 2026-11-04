# svrtest lan - two hidden dbg games in one LAN Player Match on this PC, then a lockstep check (svrtest.ps1 lan ...;
# docs/DEBUG_SUITE_PLAN.md 5). For us only: tools\dev is never packaged.
#
#   svrtest.ps1 lan --p1 EDGE --p2 KANE [--arena 1] [--rule 3C] [--attire 1,1] [--mods <folder>,...]
#                   [--pad1 <steps file>] [--pad2 <steps file>] [--clock 600,1200,1800] [--play 30s] [--name lan1] [--arg <game option>]
#
# What it does: svrtest's private game folder (svrtest.ps1 prepare, the dbg exe); its own test server (a copy of
# runs\ent_srv's accounts, a free port 8460-8499) and private copies of the two test players' user data and
# configs; the host and the joiner on their own p2p ports (365xx / 366xx, never 36000; the joiner searches the
# host's through p2p_peers; no relay), both started straight in the ONLINE menu (dbg_boot=online), then the online
# menus as np_round.ps1 presses them: the host CREATE SESSION (entrances off), the joiner QUICK MATCH, both pick.
# The match's people / arena / rule are the asked-for ones on both games (SVR2011_TEST_MATCH / SVR2011_TEST_RULE:
# test_match.cpp drops the lobby's person records at the hand-off, match_types.cpp sets the rule online); other
# SVR2011_TEST_* the caller sets (e.g. SVR2011_TEST_BACKSTAGE_ROW) reach both games. Once the match runs: --pad1 / --pad2 (script_input.h steps, one a line) go to
# the host's / joiner's scripted pad (default: both move and strike every 3 s) for --play seconds. Both games dump
# their state at the same match clock values (--clock, 60 a second: dbg_state_clock) and the dumps are compared -
# people, last moves, current motions (character +212) and the body / root positions' exact bits (+304's,
# +288): lockstep games have the very same.
# Writes runs\svrtest\<stamp>_<name>\ (host\ and join\ dumps, host.log, join.log, server.log, result.json); stops
# both games and the server by their PIDs.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
$tools = Split-Path $PSScriptRoot -Parent
$port = Split-Path $tools -Parent
. (Join-Path $tools "test_guard.ps1")

$o = @{ p = @{}; arena = ""; rule = ""; attire = ""; mods = "none"; pad1 = ""; pad2 = ""; clock = "600,1200,1800"; play = "30s"; name = "lan"; args = @(); picks1 = ""; picks2 = ""; joinenv = @() }
for ($i = 0; $i -lt $Rest.Count; $i++) {
    $a = $Rest[$i]; $v = if ($i + 1 -lt $Rest.Count) { $Rest[$i + 1] } else { "" }
    switch -regex ($a) {
        '^--p([1-6])$' { $o.p[[int]$Matches[1]] = $v; $i++; continue }
        '^--join-env$' { $o.joinenv += $v; $i++; continue }  # (NAME=VALUE in the joiner's environment only, e.g. SVR2011_TEST_SLOT_SWAP)
        '^--arg$' { $o.args += $v; $i++; continue }  # (a game option for both games, e.g. --arg --big_roster=true)
        '^--(arena|rule|attire|mods|pad1|pad2|clock|play|name|picks1|picks2)$' { $o[$Matches[1]] = $v; $i++; continue }
        default { throw "svrtest lan: unknown option $a" }
    }
}
function Seconds([string]$t) {
    if ($t -match '^(\d+(\.\d+)?)ms$') { return [double]$Matches[1] / 1000 }
    if ($t -match '^(\d+(\.\d+)?)s?$') { return [double]$Matches[1] }
    throw "svrtest lan: a time is 5s, 1500ms or a number, not $t"
}
$people = @(1..6 | Where-Object { $o.p[$_] } | ForEach-Object { $o.p[$_] })
# (--p1 none: no people set - the lobby's own picks play, e.g. made by --picks1 / --picks2 step files that replace the
# default A presses)
$lobbyPicks = $o.p[1] -eq "none"
if (-not $lobbyPicks -and $people.Count -lt 2) { throw "svrtest lan: --p1 and --p2 at least (or --p1 none)" }

# ---- guards: the player's game, room for two test games (test_guard's limit, all chats)
Assert-NoPlayerGame -Games 2

# ---- the game folder (svrtest's, the dbg exe)
$prep = @(& (Join-Path $PSScriptRoot "svrtest.ps1") prepare --exe dbg --mods $o.mods)
$game = [string]$prep[-1]
$exe = Join-Path $game "svr2011_dbg.exe"
if (-not (Test-Path $exe)) { throw "svrtest lan: no $exe" }

$root = Join-Path $port "runs\svrtest"
$out = Join-Path $root ("{0}_{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $o.name)
New-Item -ItemType Directory -Force $out, (Join-Path $out "host"), (Join-Path $out "join") | Out-Null

# ---- private copies: the server's accounts, the two players' user data, their configs
function Mirror([string]$from, [string]$to) {
    if (Test-Path $to) { return }
    robocopy $from $to /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "svrtest lan: copying $from failed" }
}
# (the sources: this checkout's runs, or the main checkout's - a worktree's runs may not have them)
$src = @((Join-Path $port "runs"), "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\runs") | Where-Object { Test-Path (Join-Path $_ "ent_srv\data") } | Select-Object -Last 1
if (-not $src) { throw "svrtest lan: no runs\ent_srv\data (the test server's accounts) to copy" }
Mirror (Join-Path $src "ent_srv\data") (Join-Path $root "lan_srv\data")
Mirror (Join-Path $src "test_userdata_ent_down") (Join-Path $root "lan_ud_host")
Mirror (Join-Path $src "test_userdata_ent_up") (Join-Path $root "lan_ud_join")
$listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalPort })
$sp = 8460..8499 | Where-Object { $_ -notin $listen } | Select-Object -Last 1
$udp = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | ForEach-Object { $_.LocalPort })
$hp = 36540, 36560, 36580 | Where-Object { $_ -notin $udp } | Select-Object -Last 1
$jp = 36640, 36660, 36680 | Where-Object { $_ -notin $udp } | Select-Object -Last 1
if (-not $sp -or -not $hp -or -not $jp) { throw "svrtest lan: no free server / p2p port" }
foreach ($side in @(@{ n = "host"; from = "test_config_ent_down.toml" }, @{ n = "join"; from = "test_config_ent_up_p2.toml" })) {
    $t = (Get-Content (Join-Path $src $side.from) -Raw) -replace '(?m)^online_server = .*$', "online_server = `"http://127.0.0.1:$sp`"" `
         -replace '(?m)^p2p_(port|address|peers) = .*\r?\n', ''
    [IO.File]::WriteAllText((Join-Path $root "lan_$($side.n).toml"), $t)  # (no BOM)
}

# ---- the test server
$py = "C:\Users\Ranger\AppData\Local\Programs\Python\Python311\python.exe"
$srv = Start-Process $py -ArgumentList "`"$port\server\server.py`" --test-open-accounts --host 127.0.0.1 --port $sp --data `"$(Join-Path $root 'lan_srv\data')`" --log `"$out\server.log`"" -PassThru -WindowStyle Hidden
$procs = @{}
$timings = [ordered]@{}; $ok = $true; $why = ""
$t0 = Get-Date
function Since { [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
function LogHas([string]$side, [string]$pattern) { (Test-Path (Join-Path $out "$side.log")) -and [bool](Select-String -Path (Join-Path $out "$side.log") -Pattern $pattern -ErrorAction SilentlyContinue) }
function Wait-Log([string]$side, [string]$pattern, [double]$secs) {
    $until = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $until -and -not $procs[$side].HasExited) { if (LogHas $side $pattern) { return $true }; Start-Sleep -Milliseconds 500 }
    return (LogHas $side $pattern)
}
function In([string]$side, [string[]]$steps) { Add-Content (Join-Path $out "$side.input.txt") ($steps -join "`n") }
function Start-Side([string]$side, [string[]]$extra) {
    $inputFile = Join-Path $out "$side.input.txt"
    Set-Content $inputFile "" -NoNewline
    $env:SVR2011_CONFIG = Join-Path $root "lan_$side.toml"
    $env:SVR2011_USER_DATA = Join-Path $root "lan_ud_$side"
    $env:SVR2011_INPUT_FILE = $inputFile
    $env:SVR2011_MUSIC = Join-Path $root "music"; $env:SVR2011_MOVIES = Join-Path $root "movies"
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
    $spec = "people=$($people -join ',') arena=$(if ($o.arena) { $o.arena } else { 1 })"
    if ($o.attire) { $spec += " attires=$($o.attire)" }
    $env:SVR2011_TEST_MATCH = if ($lobbyPicks) { $null } else { $spec }
    $env:SVR2011_TEST_RULE = if ($o.rule) { $(if ($o.rule -match '^\d+$' -and $o.rule.Length -lt 3) { "{0:X2}" -f [int]$o.rule } else { $o.rule -replace '^0x', '' }) } else { $null }
    $gameArgs = @("--log_file=`"$(Join-Path $out "$side.log")`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false",
              "--mnk_mode=false", "--p2p_port_mapping=false", "--p2p_relay=false", "--dbg_boot=online",
              "--dbg_state_clock=$($o.clock)", "--dbg_out=`"$(Join-Path $out $side)`"") + $o.args + $extra
    Use-SvrTestServer -GameDir $game -GameArgs $gameArgs | Out-Null
    $sideEnv = @(if ($side -eq "join") { $o.joinenv })
    foreach ($e in $sideEnv) { $k, $val = $e -split "=", 2; [Environment]::SetEnvironmentVariable($k, $val, "Process") }
    $procs[$side] = Start-Process $exe -WorkingDirectory $game -ArgumentList $gameArgs -PassThru -WindowStyle Minimized
    foreach ($e in $sideEnv) { [Environment]::SetEnvironmentVariable(($e -split "=", 2)[0], $null, "Process") }
    Set-SvrTestPriority $procs[$side]
    foreach ($v in "SVR2011_TEST_MATCH", "SVR2011_TEST_RULE", "SVR2011_INPUT_FILE", "SVR2011_CONFIG", "SVR2011_USER_DATA") { [Environment]::SetEnvironmentVariable($v, $null, "Process") }
}
$online = "SVRDBG: boot: page online open"
try {
    for ($k = 0; $k -lt 40 -and -not (Get-NetTCPConnection -LocalPort $sp -State Listen -ErrorAction SilentlyContinue); $k++) { Start-Sleep -Milliseconds 500 }
    # host: ONLINE (dbg_boot) -> MATCH (it loads slowly) -> PLAYER MATCH -> CREATE SESSION, entrances off, CONTINUE
    Start-Side "host" @("--p2p_port=$hp")
    In "host" @("until $online", "wait 15000", "press A 200", "wait 55000", "press DOWN 200", "wait 1500", "press A 200", "wait 12000",
                "press DOWN 200", "wait 1200", "press DOWN 200", "wait 1200", "press A 200", "wait 12000",
                "press DOWN 200", "wait 900", "press DOWN 200", "wait 900", "press DOWN 200", "wait 900", "press RIGHT 200", "wait 1200",
                "press DOWN 200", "wait 900", "press DOWN 200", "wait 900", "press A 200")
    if (-not (Wait-Log "host" "session .* hosted \(flags 42F" 300)) { throw "the host made no session" }
    $timings.hosted_s = Since
    # joiner: ONLINE -> MATCH -> PLAYER MATCH -> QUICK MATCH -> YES (it finds the host through p2p_peers)
    Start-Side "join" @("--p2p_port=$jp", "--p2p_address=127.0.0.2", "--p2p_peers=127.0.0.1:$hp")
    In "join" @("until $online", "wait 15000", "press A 200", "wait 55000", "press DOWN 200", "wait 1500", "press A 200", "wait 12000",
                "press A 200", "wait 15000", "press A 200")
    if (-not (Wait-Log "host" "join remote" 300)) { throw "the joiner didn't join" }
    $timings.joined_s = Since
    Start-Sleep 12
    # picks: the host 1P, the joiner 2P (its cursor one to the right); A on both until the match starts
    if ($o.picks1) { In "host" @(Get-Content $o.picks1) } else { In "host" @("press A 200", "wait 4000", "press A 200", "wait 3000", "press A 200", "wait 3000", "press A 200") }
    if ($o.picks2) { In "join" @(Get-Content $o.picks2) } else { In "join" @("press RIGHT 200", "wait 2000", "press A 200", "wait 4000", "press RIGHT 200", "wait 1500", "press A 200", "wait 3000", "press A 200", "wait 3000", "press A 200") }
    $until = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $until -and -not (LogHas "host" "svr2011\] match starts")) {
        if ($procs.host.HasExited -or $procs.join.HasExited) { throw "a game exited before the match" }
        if (-not $o.picks1) { In "host" @("press A 200") }; if (-not $o.picks2) { In "join" @("press A 200") }; Start-Sleep 4
    }
    if (-not (LogHas "host" "svr2011\] match starts")) { throw "no match started" }
    $timings.match_s = Since
    # the match: each side's pad
    $play = Seconds $o.play
    if ($o.pad1) { In "host" @(Get-Content $o.pad1) }
    if ($o.pad2) { In "join" @(Get-Content $o.pad2) }
    $until = (Get-Date).AddSeconds($play)
    while ((Get-Date) -lt $until) {
        if ($procs.host.HasExited -or $procs.join.HasExited) { $ok = $false; $why = "a game exited in the match"; break }
        if (-not $o.pad1) { In "host" @("press LEFT 500", "press X 150") }
        if (-not $o.pad2) { In "join" @("press RIGHT 500", "press A 150") }
        Start-Sleep 3
    }
} catch {
    $ok = $false; $why = $_.Exception.Message
} finally {
    foreach ($p in $procs.Values) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; [void]$p.WaitForExit(15000) } }
    if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}

# ---- the lockstep check: the same match clock, the same state
$checks = @()
foreach ($n in ($o.clock -split ',' | Where-Object { $_ })) {
    $name = "state_clock_{0:D6}.json" -f [int]$n
    $h = Join-Path $out "host\$name"; $j = Join-Path $out "join\$name"
    if (-not (Test-Path $h) -or -not (Test-Path $j)) { $checks += [ordered]@{ clock = [int]$n; same = $null; note = "a dump is missing (host $(Test-Path $h), joiner $(Test-Path $j))" }; continue }
    $a = Get-Content $h -Raw | ConvertFrom-Json; $b = Get-Content $j -Raw | ConvertFrom-Json
    $diff = @()
    if ($a.match_clock -ne $b.match_clock) { $diff += "match clock $($a.match_clock) / $($b.match_clock)" }
    foreach ($pa in $a.people) {
        $pb = $b.people | Where-Object { $_.index -eq $pa.index }
        if (-not $pb) { $diff += "person $($pa.index) only on the host"; continue }
        # (who by name, its "(id N)" left out: with --join-env SVR2011_TEST_SLOT_SWAP the two games number a mod differently)
        $na = [string]$pa.name -replace ' \(id \d+\)', ''; $nb = [string]$pb.name -replace ' \(id \d+\)', ''
        if ($na -ne $nb) { $diff += "person $($pa.index) name $($pa.name) / $($pb.name)" }
        foreach ($f in "last_move", "position_bits", "motion", "root_bits") { if ([string]$pa.$f -ne [string]$pb.$f) { $diff += "person $($pa.index) $f $($pa.$f) / $($pb.$f)" } }
    }
    $checks += [ordered]@{ clock = [int]$n; same = ($diff.Count -eq 0); differences = $diff }
}
if ($ok -and @($checks | Where-Object { $_.same -eq $false }).Count) { $ok = $false; $why = "the games' states differ (lockstep broken?)" }
if ($ok -and @($checks | Where-Object { $_.same -eq $null }).Count -eq $checks.Count) { $ok = $false; $why = "no state dumps to compare" }
function Lines([string]$side, [string]$pattern) {
    if (-not (Test-Path (Join-Path $out "$side.log"))) { return @() }
    @(Select-String -Path (Join-Path $out "$side.log") -Pattern $pattern -ErrorAction SilentlyContinue | Select-Object -Last 2 | ForEach-Object { $_.Line.Substring([Math]::Min(40, $_.Line.Length)) })
}
$result = [ordered]@{
    ok = $ok; why = $why; command = "lan $($Rest -join ' ')"; ports = [ordered]@{ server = $sp; host = $hp; join = $jp }
    timings = $timings; lockstep = $checks
    match = [ordered]@{ host = (Lines "host" "\] match: rule"); join = (Lines "join" "\] match: rule") }
    errors = [ordered]@{ host = @(if (Test-Path (Join-Path $out "host.log")) { Select-String -Path (Join-Path $out "host.log") -Pattern "\[(error|critical)\]" | Where-Object { $_.Line -notmatch 'BaseHeap|PhysicalHeap|ResolvePath' } }).Count
                         join = @(if (Test-Path (Join-Path $out "join.log")) { Select-String -Path (Join-Path $out "join.log") -Pattern "\[(error|critical)\]" | Where-Object { $_.Line -notmatch 'BaseHeap|PhysicalHeap|ResolvePath' } }).Count }
    folder = $out; games_left = @($procs.Values | Where-Object { $_ -and -not $_.HasExited }).Count; server_left = -not $srv.HasExited
}
$json = $result | ConvertTo-Json -Depth 5
Set-Content (Join-Path $out "result.json") $json -Encoding UTF8
$json

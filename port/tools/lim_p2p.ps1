# PORT RULES two-game test (limits work): this session's own test server
# (127.0.0.1:8411, runs\lim_srv) and two background games from runs\lim_game
# (lim_session.ps1 slots "" and 2): the host picks WEAPONS EVERYWHERE on
# CREATE SESSION and hosts, the joiner searches and joins (the lobby panel),
# then both pick and the match starts (both should play rule 4D).
#   .\lim_p2p.ps1 [-Name lp1] [-RbPresses 1]
# The joiner's account (LimitTest2) is made on the test server the first time:
# runs\test_config_lim_online2.toml. Stops both games and the server at the end.
param([string]$Name = "lp1", [int]$RbPresses = 1, [string]$MatchKeys = "", [string]$RowKey = "RB", [int]$Shots = 3, [string]$HostBase = "", [string]$TargetRule = "", [int]$ShotEvery = 25)
# -TargetRule 0D (00 / 0D / 0E / 08): CREATE SESSION's MATCH TYPE set to that normal match, checked in the
# log ("CREATE SESSION match is rule XX") and tried again (presses get lost under load).
# -HostBase: a saves folder in runs copied fresh for the host each run (runs	est_userdata_lim_run): the
# MATCH TYPE list starts where that save left it (it remembers its last place).
# -MatchKeys: presses on CREATE SESSION before the row (comma-separated, e.g. the MATCH TYPE list:
# "A,DOWN,DOWN,A,A" = TRIPLE THREAT > NORMAL); -RowKey RB / LB, pressed -RbPresses times.
$port = Split-Path $PSScriptRoot -Parent; $runs = Join-Path $port "runs"
$s = Join-Path $PSScriptRoot "lim_session.ps1"
$py = "C:\Users\Ranger\AppData\Local\Programs\Python\Python311\python.exe"
function In([string]$slot, [string[]]$keys) { & $s input -Slot $slot @keys | Out-Null }
function Shot([string]$slot, [string]$stem) { & $s shot -Slot $slot $stem | Out-Null }
function Low([string]$slot) {
    try { (Get-Process -Id ((Get-Content (Join-Path $runs "lim_session$slot.json") | ConvertFrom-Json).pid)).PriorityClass = "BelowNormal" } catch {}
}

$srv = Start-Process $py -ArgumentList "`"$port\server\server.py`" --test-open-accounts --host 127.0.0.1 --port 8411 --data `"$runs\lim_srv\data`" --log `"$runs\lim_srv\server.log`"" -PassThru -WindowStyle Hidden
try { $srv.PriorityClass = "BelowNormal" } catch {}
try {
    Start-Sleep 4
    $cfg2 = Join-Path $runs "test_config_lim_online2.toml"
    if (-not (Test-Path $cfg2)) {
        $pw = -join ((48..57) + (97..122) | Get-Random -Count 20 | ForEach-Object { [char]$_ })
        $r = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:8411/api/register" -ContentType "application/json" -Body (@{ name = "LimitTest2"; password = $pw } | ConvertTo-Json)
        if (-not $r.ok) { throw "register: $($r.error)" }
        $t = (Get-Content (Join-Path $runs "test_config_lim_online.toml") -Raw) -replace 'online_token = ".*"', "online_token = `"$($r.token)`"" `
             -replace 'online_xuid = ".*"', "online_xuid = `"$($r.xuid)`"" -replace 'online_name = ".*"', 'online_name = "LimitTest2"'
        $t = $t.TrimEnd() + "`np2p_port = 36950`np2p_address = `"127.0.0.2`"`n"
        [IO.File]::WriteAllText($cfg2, $t)   # (no BOM)
    }
    $env:SVR2011_WINDOW_BEHIND = "1"
    $hostData = Join-Path $runs "test_userdata_lim"
    if ($HostBase) {
        $hostData = Join-Path $runs "test_userdata_lim_run"
        if (Test-Path $hostData) { [IO.Directory]::Delete($hostData, $true) }
        Copy-Item (Join-Path $runs $HostBase) $hostData -Recurse
    }
    $env:SVR2011_CONFIG = Join-Path $runs "test_config_lim_online.toml"; $env:SVR2011_USER_DATA = $hostData
    & $s start -Name "$Name`_host" | Out-Null
    $env:SVR2011_CONFIG = $cfg2; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim2"
    & $s start -Slot 2 -Name "$Name`_join" | Out-Null
    In "" @("route online"); In "2" @("route online")
    Start-Sleep 70
    # host: MATCH -> PLAYER MATCH -> CREATE SESSION, the PORT RULES row, CONTINUE
    In "" @("press A", "wait 4000", "press DOWN", "wait 1500", "press A", "wait 4000", "press DOWN", "wait 1500", "press DOWN", "wait 1500", "press A", "wait 4000")
    Start-Sleep 16
    if ($TargetRule) {
        $order = @{ "00" = 0; "03" = 1; "0D" = 2; "0E" = 3; "05" = 4; "08" = 5; "0A" = 5 }   # (the MATCH TYPE list's people counts: 1, 2 on 2, 3, 4, 5, 6)
        $hlog = Join-Path $runs "$Name`_host.log"
        foreach ($try in 1..10) {
            Start-Sleep 8   # (the list's result is logged once it has closed: never read a stale one)
            $now = (Select-String -Path $hlog -Pattern "CREATE SESSION match is rule ([0-9A-F]{2})" | Select-Object -Last 1)
            $cur = if ($now) { $now.Matches[0].Groups[1].Value } else { "00" }
            if ($cur -eq $TargetRule) { break }
            $d = (($order[$TargetRule] - $order[$cur]) % 6 + 6) % 6   # (the list wraps: the shorter way round)
            if ($d -gt 3) { $d = $d - 6 }
            $keys = @("press A", "wait 6000")
            $k = if ($d -gt 0) { "DOWN" } else { "UP" }
            for ($i = 0; $i -lt [math]::Abs($d); $i++) { $keys += @("press $k", "wait 3000") }
            $keys += @("press A", "wait 4000", "press A", "wait 4000", "press A", "wait 5000")
            In "" $keys; Start-Sleep ([int](($keys.Count / 2) * 3) + 8)
        }
        Shot "" "$Name`_host_type"
    }
    if ($MatchKeys) { foreach ($k in ($MatchKeys -split ",")) { In "" @("press $k", "wait 4000") }; Start-Sleep ([int](($MatchKeys -split ",").Count * 4.5) + 8); Shot "" "$Name`_host_type" }
    foreach ($i in 1..$RbPresses) { In "" @("press $RowKey", "wait 1200") }
    Start-Sleep 4; Shot "" "$Name`_host_cs"
    In "" @("press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press A", "wait 3000")
    Start-Sleep 4; Shot "" "$Name`_host_cont1"; Start-Sleep 6; Shot "" "$Name`_host_cont2"; Start-Sleep 6; Shot "" "$Name`_host_lobby"
    # joiner: MATCH -> PLAYER MATCH -> CUSTOM MATCH -> SEARCH -> the session -> YES
    In "2" @("press A", "wait 4000", "press DOWN", "wait 1500", "press A", "wait 4000", "press DOWN", "wait 1500", "press A", "wait 4000")
    Start-Sleep 12
    In "2" @("press DOWN", "wait 800", "press DOWN", "wait 800", "press A", "wait 3000"); Start-Sleep 14; Shot "2" "$Name`_join_list"
    In "2" @("press A", "wait 3000"); Start-Sleep 14; Shot "2" "$Name`_join_info"
    In "2" @("press A", "wait 3000"); Start-Sleep 18; Shot "2" "$Name`_join_lobby"; Shot "" "$Name`_host_joined"
    # both pick (the joiner moves to 2P first); the match starts
    In "" @("press A 200", "wait 6000", "press A 200", "wait 3000", "press A 200", "wait 6000")
    In "2" @("press RIGHT 200", "wait 2000", "press A 200", "wait 6000", "press A 200", "wait 3000", "press A 200", "wait 6000")
    # (a lost press leaves a slot unpicked: a second round when no match has started)
    Start-Sleep 30
    $started = if ($TargetRule) { "\] match: rule $TargetRule" } else { "\] match: rule 0[0-9A-F], arena [0-9]+ - .*\(person 1" }
    foreach ($round in 1..3) {
      # (a match line after the joiner joined: the title demo's match lines come before)
      $jl = Join-Path $runs "$Name`_join.log"
      $joinedAt = (Select-String -Path $jl -Pattern "joined \(flags" | Select-Object -Last 1).LineNumber
      if ($joinedAt -and (Select-String -Path $jl -Pattern $started | Where-Object { $_.LineNumber -gt $joinedAt })) { break }
      Shot "" "$Name`_host_round$round"; Shot "2" "$Name`_join_round$round"
      In "" @("press A 200", "wait 3000", "press A 200", "wait 6000")
      In "2" @("press A 200", "wait 6000", "press A 200", "wait 3000", "press A 200", "wait 6000")
      foreach ($k in 1..5) { Start-Sleep 7; Shot "" "$Name`_host_r$round`_$k"; Shot "2" "$Name`_join_r$round`_$k" }  # (the VS / loading / entrances)
    }
    foreach ($i in 1..$Shots) { Start-Sleep $ShotEvery; Shot "" "$Name`_host_match$i"; Shot "2" "$Name`_join_match$i" }
} finally {
    & $s stop | Out-Null; & $s stop -Slot 2 | Out-Null
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    "games: " + (Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match "runs\\lim_" } | Measure-Object).Count + "; 8411: " + [bool](Get-NetTCPConnection -State Listen -LocalPort 8411 -ErrorAction SilentlyContinue)
}
foreach ($log in "$Name`_host", "$Name`_join") {
    "--- $log"
    Select-String -Path (Join-Path $runs "$log.log") -Pattern "online rules|port rules|online cpu|p2p: session|joined|weapons|crash|access violation|match: rule" -ErrorAction SilentlyContinue |
        Select-Object -Last 25 | ForEach-Object { $_.Line.Substring([math]::Min(48, $_.Line.Length)) }
}

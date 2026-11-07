# SvR 2011 Online storage node watchdog (the user's PC behind the VPS; installed as
# <Project Index>\Apps\SvR2011\service\watchdog.ps1, run by the scheduled task "SvR2011 Storage Watchdog" at
# logon + every 2 minutes, hidden via watchdog.vbs).
#
# - Down (nothing on the port): start service.py detached.
# - Up but /__health failing 3 checks in a row: stop whatever holds the port (by PID) and start it again.
# - The dashboard's Stop / Start (server.py admin_storage): with "core_url" in config.json it asks the core
#   which state is wanted (GET <core_url>/api/storage/wanted, the node's key). Stopped: the node has a
#   node.stopped marker in its data folder (it writes it itself on the core's /__stop, and refuses to start
#   while it is there) - nothing is started or restarted then. Running wanted while stopped: the marker goes
#   and the node is started (within 2 minutes of the Start).
# Everything it does goes to <log dir>\svr2011.watchdog.log.
#   -ServiceDir: the service folder (service.py, config.json; default: this script's folder)
#   -Python / -LogDir: default <Project Index>\venv\Scripts\python.exe and <Project Index>\logs
param([string]$ServiceDir = $PSScriptRoot, [string]$Python = "", [string]$LogDir = "")
$ErrorActionPreference = 'Continue'
$svcDir  = $ServiceDir
$root    = (Resolve-Path (Join-Path $svcDir '..\..\..')).Path   # (<Project Index>\Apps\SvR2011\service)
if (-not $Python) { $Python = Join-Path $root 'venv\Scripts\python.exe' }
if (-not $LogDir) { $LogDir = Join-Path $root 'logs' }
$log     = Join-Path $LogDir 'svr2011.watchdog.log'
$state   = Join-Path $svcDir 'watchdog.fails'
$lock    = Join-Path $svcDir 'watchdog.starting'
$quiet   = Join-Path $svcDir 'watchdog.stopped'   # (the "stopped on purpose" line, logged once)
$dataDir = Join-Path $svcDir '..\data'              # (service.py's data folder)
$marker  = Join-Path $dataDir 'node.stopped'

function Log($m) { Add-Content -Path $log -Value ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m) }

$cfg  = Get-Content (Join-Path $svcDir 'config.json') -Raw | ConvertFrom-Json
$hostIp = "$($cfg.bind_host)"
$port = [int]$cfg.bind_port
$key  = ''
if ($cfg.storage_key -and (Test-Path -LiteralPath "$($cfg.storage_key)")) { $key = (Get-Content -LiteralPath "$($cfg.storage_key)" -Raw).Trim() }

# The node binds the WireGuard address: nothing to do until it exists.
if ($hostIp -and $hostIp -ne '127.0.0.1' -and $hostIp -ne '0.0.0.0' -and
    -not (Get-NetIPAddress -IPAddress $hostIp -ErrorAction SilentlyContinue)) {
    Log "waiting: $hostIp (WireGuard) is not up"
    exit 0
}

$listen = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue

# The dashboard's Stop / Start: the state the core wants.
$wanted = ''
if ($cfg.core_url -and $key) {
    try {
        $w = Invoke-RestMethod -Uri ("{0}/api/storage/wanted" -f "$($cfg.core_url)".TrimEnd('/')) -Headers @{ 'X-Storage-Key' = $key } -TimeoutSec 15
        $wanted = "$($w.wanted)"
    } catch { Log "the core didn't say which state it wants ($($_.Exception.Message)) - as before" }
}
if ($wanted -eq 'running' -and (Test-Path $marker)) {
    Log "Start asked from the dashboard: starting the node"
    Remove-Item $marker, $quiet, $lock -ErrorAction SilentlyContinue
}
if ($wanted -eq 'stopped' -and -not (Test-Path $marker)) {
    if ($listen) {  # (the core's own /__stop didn't get there: the node stops itself, writing the marker)
        try {
            Invoke-WebRequest -Uri "http://${hostIp}:$port/__stop" -Method Post -Headers @{ 'X-Storage-Key' = $key } -UseBasicParsing -TimeoutSec 15 | Out-Null
            Log "Stop asked from the dashboard: the node is stopping"
        } catch { Log "Stop asked from the dashboard, but the node didn't take it ($($_.Exception.Message))" }
    } else {
        Set-Content -Path $marker -Value '{"why": "the dashboard''s Stop (written by the watchdog)"}'
        Log "Stop asked from the dashboard: the node is down - it stays so"
    }
    exit 0
}
if (Test-Path $marker) {  # (stopped on purpose: no start, no restarts)
    if (-not (Test-Path $quiet)) { Log "stopped from the dashboard - leaving it until its Start"; Set-Content $quiet (Get-Date) }
    exit 0
}

# A start in the last 3 minutes is still coming up.
if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 3) { exit 0 }

function Start-Node {
    Set-Content -Path $lock -Value (Get-Date)
    $cmd = "cmd.exe /c cd /d `"$svcDir`" && `"$Python`" `"$svcDir\service.py`" 1>>`"$LogDir\svr2011.out.log`" 2>>`"$LogDir\svr2011.err.log`""
    $si  = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
    $r   = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $svcDir; ProcessStartupInformation = $si }
    Log "started the storage node (rc $($r.ReturnValue), pid $($r.ProcessId))"
}

if (-not $listen) {
    Log "down: nothing on ${hostIp}:$port"
    Start-Node
    Remove-Item $state -ErrorAction SilentlyContinue
    exit 0
}

$ok = $false
try {
    $r = Invoke-WebRequest -Uri "http://${hostIp}:$port/__health" -UseBasicParsing -TimeoutSec 15
    $ok = $r.StatusCode -eq 200
} catch { }

if ($ok) {
    if (Test-Path $state) { Log "healthy again"; Remove-Item $state -ErrorAction SilentlyContinue }
    exit 0
}

$fails = 1
if (Test-Path $state) { $fails = [int](Get-Content $state) + 1 }
Set-Content -Path $state -Value $fails
Log "health check failed ($fails of 3)"
if ($fails -lt 3) { exit 0 }

# Hung: stop the process tree that holds the port (by PID, its supervisor too), then start again.
foreach ($c in $listen) {
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$($c.OwningProcess)"
    $top = $p
    while ($top -and $top.ParentProcessId) {
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($top.ParentProcessId)"
        if (-not $parent -or $parent.CommandLine -notmatch 'SvR2011\\service\\service\.py|server\.py.*--role storage|cmd\.exe /c cd /d .*SvR2011\\service') { break }
        $top = $parent
    }
    Log "hung: stopping pid $($top.ProcessId) (holds pid $($c.OwningProcess))"
    taskkill /PID $top.ProcessId /T /F | Out-Null
    taskkill /PID $c.OwningProcess /T /F 2>$null | Out-Null
}
Start-Sleep 2
Start-Node
Remove-Item $state -ErrorAction SilentlyContinue

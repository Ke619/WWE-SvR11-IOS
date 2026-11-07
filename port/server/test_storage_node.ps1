# Storage node panel test (private): core 127.0.0.1:8496 (--role core --local-admin), node 8495 through a copy
# of Project Index's service.py in <Dir>\service, this folder's pc_watchdog.ps1 run by hand (as its 2-minute
# task would). Never the live node: <Dir>\service\config.json binds 127.0.0.1:8495 with its own key.txt and
# "core_url": "http://127.0.0.1:8496"; <Dir>\data, core, logs are the test's.
#   .\test_storage_node.ps1 -Dir <test folder> [-Python <venv python>] [-Keep]   (-Keep: leave both running)
param([Parameter(Mandatory = $true)][string]$Dir, [string]$Python = 'F:\ClaudeProjects\Project Index\venv\Scripts\python.exe',
      [switch]$Keep)
$ErrorActionPreference = 'Stop'
$nt   = $Dir
$svc  = Join-Path $nt 'service'
$py   = $Python
$srv  = $PSScriptRoot
$wd   = Join-Path $srv 'pc_watchdog.ps1'
$core = 'http://127.0.0.1:8496'
$fails = 0
function Check([bool]$c, [string]$what) { if ($c) { "ok    $what" } else { "FAIL  $what"; $script:fails++ } }
function Storage([string]$action) {
    if ($action) { return Invoke-RestMethod -Uri "$core/api/admin/storage" -Method Post -ContentType 'application/json' -Body (@{ action = $action } | ConvertTo-Json) }
    return Invoke-RestMethod -Uri "$core/api/admin/storage"
}
function Watchdog { & powershell -NoProfile -ExecutionPolicy Bypass -File $wd -ServiceDir $svc -Python $py -LogDir (Join-Path $nt 'logs') }
function NodeProcs { @(Get-CimInstance Win32_Process -Filter "Name like 'python%'" | Where-Object { $_.CommandLine -notlike '*--role core*' -and ($_.CommandLine -like "*$svc\service.py*" -or $_.CommandLine -like '*--port 8495*') }) }
function WaitFor([scriptblock]$cond, [int]$sec) { for ($i = 0; $i -lt $sec; $i++) { if (& $cond) { return $true }; Start-Sleep 1 }; return $false }

Remove-Item (Join-Path $nt 'data\node.stopped'), (Join-Path $svc 'watchdog.*'), (Join-Path $nt 'core\storage_node.json') -ErrorAction SilentlyContinue
$coreP = Start-Process $py -ArgumentList "`"$srv\server.py`" --role core --test-open-accounts --host 127.0.0.1 --port 8496 --data `"$nt\core`" --storage-url http://127.0.0.1:8495 --storage-key `"$svc\key.txt`" --local-admin --log `"$nt\core\server.log`"" -WindowStyle Hidden -PassThru
Check (WaitFor { try { (Invoke-WebRequest "$core/__health" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200 } catch { $false } } 20) "the core is up"

$d = Storage
Check ($d.node -and -not $d.up -and $d.wanted -eq 'running') "panel: the node not answering yet, running wanted"
Watchdog
Check (WaitFor { (Storage).up } 40) "the watchdog started the node (it was down)"
$d = Storage
Check ($d.info.blobs -ne $null -and $d.info.free_bytes -gt 0 -and $d.info.supervised) "panel: files $($d.info.blobs), free $([math]::Round($d.info.free_bytes / 1GB)) GB, supervised"
Check ($d.watchdog_polled -gt 0) "the watchdog asked the core for the wanted state"

$d = Storage 'stop'
Check ($d.wanted -eq 'stopped') "Stop: stopped wanted"
Check (WaitFor { (NodeProcs).Count -eq 0 } 20) "Stop: the node and its supervisor exited (no restart)"
Check (Test-Path (Join-Path $nt 'data\node.stopped')) "Stop: the node wrote its marker"
Watchdog
Start-Sleep 3
Check ((NodeProcs).Count -eq 0) "the watchdog leaves a stopped node stopped"
# (a start at the PC's logon, as start-portal.ps1 does: the node refuses while stopped)
$p = Start-Process $py -ArgumentList "`"$svc\service.py`"" -WorkingDirectory $svc -WindowStyle Hidden -PassThru
Check (WaitFor { $p.HasExited } 30) "a start by hand while stopped: the node refuses (exit $($p.ExitCode))"

$d = Storage 'start'
Check ($d.wanted -eq 'running' -and -not $d.up) "Start: running wanted, not up yet (the dashboard says starting)"
Watchdog
Check (WaitFor { (Storage).up } 40) "Start: the watchdog took the marker away and started the node"
Check (-not (Test-Path (Join-Path $nt 'data\node.stopped'))) "Start: the marker is gone"

# Stop while the core can't reach the node: the watchdog does it at its next run
Get-Process -Id $coreP.Id | Stop-Process -Force; Start-Sleep 2
'{"wanted": "stopped", "changed": 0}' | Set-Content (Join-Path $nt 'core\storage_node.json') -Encoding ascii
$coreP = Start-Process $py -ArgumentList "`"$srv\server.py`" --role core --test-open-accounts --host 127.0.0.1 --port 8496 --data `"$nt\core`" --storage-url http://127.0.0.1:8495 --storage-key `"$svc\key.txt`" --local-admin --log `"$nt\core\server.log`"" -WindowStyle Hidden -PassThru
[void](WaitFor { try { (Invoke-WebRequest "$core/__health" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200 } catch { $false } } 20)
Watchdog
Check (WaitFor { (NodeProcs).Count -eq 0 } 20) "a Stop the node missed: the watchdog stops it"
$d = Storage 'start'; Watchdog
Check (WaitFor { (Storage).up } 40) "and Start again brings it back"

"--- watchdog log"; Get-Content (Join-Path $nt 'logs\svr2011.watchdog.log') -Tail 12
"--- node log"; Get-Content (Join-Path $nt 'data\server.log') -Tail 8
if (-not $Keep) {
    Storage 'stop' | Out-Null; [void](WaitFor { (NodeProcs).Count -eq 0 } 20)
    Get-Process -Id $coreP.Id -ErrorAction SilentlyContinue | Stop-Process -Force
    NodeProcs | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
"node test: " + $(if ($fails) { "$fails failed" } else { "all ok" })


# Online tests: (re)starts a Community Creations server (server/server.py) on
# 127.0.0.1:8411 - the launcher's Custom server on this PC - with its data and
# log under runs\online_srv. It needs aiohttp (server/requirements.txt): the
# Project Index venv's Python when it is there, else python on the PATH.
#   .\online_server.ps1 [stop]
param([string]$Action = "start")
$port = Split-Path $PSScriptRoot -Parent
$data = Join-Path $port "runs\online_srv"
New-Item -ItemType Directory -Force $data | Out-Null
$old = Get-NetTCPConnection -LocalPort 8411 -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess
if ($old) { Stop-Process -Id $old -Force; Start-Sleep 1 }
if ($Action -eq "stop") { "stopped"; return }
$python = "F:\ClaudeProjects\Project Index\venv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }
$a = @("`"$(Join-Path $port 'server\server.py')`"", "--host", "127.0.0.1", "--port", "8411", "--test-open-accounts",
       "--data", "`"$(Join-Path $data 'data')`"", "--log", "`"$(Join-Path $data 'server.log')`"")
Start-Process $python -ArgumentList $a -WindowStyle Hidden | Out-Null
for ($t = 0; $t -lt 40 -and -not (Get-NetTCPConnection -LocalPort 8411 -State Listen -ErrorAction SilentlyContinue); $t++) { Start-Sleep -Milliseconds 250 }
if (Get-NetTCPConnection -LocalPort 8411 -State Listen -ErrorAction SilentlyContinue) { "server on 127.0.0.1:8411" } else { "the server didn't start (aiohttp installed? pip install -r server\requirements.txt)" }

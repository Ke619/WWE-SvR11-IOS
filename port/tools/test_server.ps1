# The local online test server (port\server\server.py on 127.0.0.1:8421,
# data runs\ent_srv) only while tests need it - the test configs
# (test_config_*.toml: online_server http://127.0.0.1:8421) use it.
#   test_server.ps1 start   # started if nothing listens on 8421 (no window)
#   test_server.ps1 stop    # stopped once no test game is left running
#   test_server.ps1 status
# start: p2p_test.ps1 / np_test.ps1 and the session scripts' start; stop: the
# session scripts' stop (the last test game gone, the server goes too).
# -Root: another checkout's port folder (its runs and server.py; default: this one's).
param([Parameter(Position = 0)][ValidateSet("start", "stop", "status")][string]$Verb = "status", [string]$Root = "")
$port = if ($Root) { $Root } else { Split-Path $PSScriptRoot -Parent }
$runs = Join-Path $port "runs"
$data = Join-Path $runs "ent_srv"
$server = Join-Path $port "server\server.py"
$listenPort = 8421

function ServerProcs {
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*server.py*" -and $_.CommandLine -like "*--port $listenPort*" }
}
function Listening {
    [bool](Get-NetTCPConnection -LocalPort $listenPort -State Listen -ErrorAction SilentlyContinue)
}
# test games still running (any test folder in runs, any chat's)
function TestGames {
    Get-Process svr2011 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like (Join-Path $runs "*") }
}

switch ($Verb) {
    "start" {
        if (Listening) { "test server: already up on $listenPort"; return }
        New-Item -ItemType Directory -Force (Join-Path $data "data") | Out-Null
        # (a real Python: the py launcher's, or a python not the Store's alias)
        $py = $null
        if (Get-Command py -ErrorAction SilentlyContinue) { $py = (& py -3 -c "import sys; print(sys.executable)" 2>$null) }
        if (-not $py) { $py = (Get-Command python -All -ErrorAction SilentlyContinue | Where-Object { $_.Source -notlike "*WindowsApps*" } | Select-Object -First 1).Source }
        if (-not $py) { "test server: no Python found"; return }
        $sargs = @("`"$server`"", "--host", "127.0.0.1", "--port", "$listenPort", "--test-open-accounts",
                  "--data", "`"$(Join-Path $data 'data')`"", "--log", "`"$(Join-Path $data 'server.log')`"")
        $p = Start-Process -FilePath $py -ArgumentList $sargs -WindowStyle Hidden -PassThru
        for ($i = 0; $i -lt 50 -and -not (Listening); $i++) { Start-Sleep -Milliseconds 200 }
        if (Listening) { "test server: started (pid $($p.Id))" } else { "test server: didn't come up - see $(Join-Path $data 'server.log')" }
    }
    "stop" {
        $games = @(TestGames)
        if ($games.Count) { "test server: $($games.Count) test game(s) still running - left up"; return }
        $procs = @(ServerProcs)
        foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
        if ($procs.Count) { "test server: stopped" } else { "test server: wasn't running" }
    }
    "status" {
        $procs = @(ServerProcs)
        if ($procs.Count) { "test server: up (pid $($procs.ProcessId -join ', '))" } else { "test server: down" }
    }
}

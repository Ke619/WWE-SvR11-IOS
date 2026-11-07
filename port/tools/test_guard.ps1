# Dot-sourced by the test scripts: they must never touch a game the player
# started. A test game is one launched with a log file under port\runs.
#   Stop-TestGames       closes only test games
#   Assert-NoPlayerGame  throws when a game not started by a test is running,
#                        then waits for a test-game slot (Wait-SvrTestSlot)
#   Set-SvrTestPriority  below-normal priority for a new test game while the PC is busy

# Test games open behind every other window (the SDK's SVR2011_WINDOW_BEHIND,
# before the window is ever shown) on the second screen (--monitor=2): the player
# watching something never sees one pop up.
$env:SVR2011_WINDOW_BEHIND = "1"
# One screen (the player's TV): no test window at all - the SDK never shows the
# window (SVR2011_WINDOW_HIDDEN); the game still draws, frame shots
# (SVR2011_TEST_FRAME_SHOT) read its back buffer. Window screenshots (PrintWindow)
# of a hidden window come out black: use frame shots.
$env:SVR2011_WINDOW_HIDDEN = "1"

# Where a test window is parked: the second screen if there is one (x 1920 +),
# else it stays where it opened on the only screen - behind every window
# (HWND_BOTTOM), never activated. Off every screen, the game's own pages
# don't draw. $SvrParkX / $SvrParkFlags: SetWindowPos(h, 1, $SvrParkX + dx, dy, 0, 0, $SvrParkFlags).
Add-Type -AssemblyName System.Windows.Forms
$SvrTwoScreens = @([System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Bounds.X -ge 1920 }).Count -gt 0
$SvrParkX = if ($SvrTwoScreens) { 1920 } else { 0 }
$SvrParkFlags = if ($SvrTwoScreens) { 0x11 } else { 0x13 }   # NOSIZE|NOACTIVATE (+ NOMOVE on one screen)

function Get-SvrGames {
    @(Get-CimInstance Win32_Process -Filter "Name = 'svr2011.exe' OR Name = 'svr2011_trace.exe' OR Name = 'svr2011_dbg.exe'" -ErrorAction SilentlyContinue)
}

# A test folder of any chat: its exe's folder or a parent holds the (empty) marker file .svr2011_test - e.g.
# "D:\Xbox Games Ports\SvR2011 Anthology Test" - so a test run from outside port\runs is never taken for the
# player's game. (Those games still count toward the 4 test games at once; Stop-TestGames leaves them alone.)
function Test-HasTestMarker([string]$exe) {
    if (-not $exe) { return $false }
    $d = Split-Path $exe -Parent
    while ($d) {
        if (Test-Path -LiteralPath (Join-Path $d ".svr2011_test")) { return $true }
        $up = Split-Path $d -Parent
        if (-not $up -or $up -eq $d) { break }
        $d = $up
    }
    return $false
}

function Test-IsTestGame($proc) {
    $cmd = [string]$proc.CommandLine
    return ($cmd -match [regex]::Escape("\port\runs\")) -or (Test-HasTestMarker ([string]$proc.ExecutablePath))
}

function Stop-TestGames {
    foreach ($p in Get-SvrGames) {
        # (never a game run from its own copy under port\runs - onl_game,
        # netperf_game, online_game, lim_game, opt_*...: another session's test,
        # with up to 6 test games from several sessions at once)
        if ([string]$p.ExecutablePath -match [regex]::Escape("\port\runs\")) { continue }
        if (Test-HasTestMarker ([string]$p.ExecutablePath)) { continue }  # (another chat's marked test folder)
        # (not the online tests' games: online_session.ps1 runs them beside others;
        # nor the Limit Breaking and Optimizing sessions' games: lim_game, opt_*)
        if ((Test-IsTestGame $p) -and ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\online_game\")) -and
            ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\lim_game\")) -and
            ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\opt_"))) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $p.ProcessId -Timeout 15 -ErrorAction SilentlyContinue
        }
    }
}

function Assert-NoPlayerGame([int]$Games = 1) {
    Use-SvrTestServer | Out-Null
    # A process that is shutting down reports no command line: look again
    # before taking such a one for the player's.
    for ($try = 0; $try -lt 5; $try++) {
        $player = @(Get-SvrGames | Where-Object { -not (Test-IsTestGame $_) })
        if (-not $player.Count) { Wait-SvrTestSlot $Games; return }
        if (@($player | Where-Object { $_.CommandLine }).Count) { break }
        Start-Sleep -Seconds 2
    }
    throw "The game is running (pid $($player[0].ProcessId), not started by a test) - not starting a test while someone is playing."
}

# No test game ever talks to the live server (sho-ti.me / the VPS): the SDK's
# online_server default is the live one, and a test config without the key (svrtest's
# game folder) or the player's own config would send test reports, uploads and
# stats there. Only a local private server (127.x / localhost) is allowed; anything
# else becomes a dead local address through REX_ONLINE_SERVER (the SDK's environment
# override: above the config file, below --online_server on the command line).
$SvrDeadServer = "http://127.0.0.1:9"

function Test-SvrLocalServer([string]$url) {
    return $url -match '^\s*https?://(127\.\d+\.\d+\.\d+|localhost|\[::1\])(:\d+)?(/|$)'
}

# A file this guard may rewrite: a test's own copy (under port\runs or a marked test folder).
function Test-SvrPrivatePath([string]$path) {
    return ($path -match [regex]::Escape("\port\runs\")) -or (Test-HasTestMarker $path)
}

# Call right before every game start (Assert-NoPlayerGame calls it too, without the
# details). Works out the server the game will use: --online_server in -GameArgs, then
# REX_ONLINE_SERVER, then the config (-Config, else SVR2011_CONFIG, else
# <GameDir>\svr2011.toml), then the SDK default. A non-local one in a test's own
# config is rewritten to the dead address; otherwise REX_ONLINE_SERVER points the
# game there. Throws when the command line or a caller's REX_ONLINE_SERVER names a
# non-local server. Returns the server the game will use.
function Use-SvrTestServer([string]$GameDir = "", $GameArgs = @(), [string]$Config = "") {
    if ($env:REX_ONLINE_SERVER -eq $SvrDeadServer) { Remove-Item Env:REX_ONLINE_SERVER }  # (ours: decide again)
    $cfg = if ($Config) { $Config } elseif ($env:SVR2011_CONFIG) { $env:SVR2011_CONFIG } elseif ($GameDir) { Join-Path $GameDir "svr2011.toml" } else { "" }
    $arg = ""
    foreach ($m in [regex]::Matches(($GameArgs -join " "), '--online_server=("[^"]*"|\S+)')) { $arg = $m.Groups[1].Value.Trim('"') }
    if ($arg) {
        if (-not (Test-SvrLocalServer $arg)) { throw "test_guard: --online_server=$arg is not a local test server - refusing to start a test game on it." }
        return $arg
    }
    if ($env:REX_ONLINE_SERVER) {
        if (-not (Test-SvrLocalServer $env:REX_ONLINE_SERVER)) { throw "test_guard: REX_ONLINE_SERVER=$($env:REX_ONLINE_SERVER) is not a local test server - refusing to start a test game on it." }
        return $env:REX_ONLINE_SERVER
    }
    $toml = ""
    if ($cfg -and (Test-Path -LiteralPath $cfg)) {
        $text = [IO.File]::ReadAllText($cfg)
        foreach ($m in [regex]::Matches($text, '(?m)^\s*online_server\s*=\s*"?([^"\r\n]*)"?')) { $toml = $m.Groups[1].Value.Trim() }
        if ($toml -and -not (Test-SvrLocalServer $toml) -and (Test-SvrPrivatePath $cfg)) {
            [IO.File]::WriteAllText($cfg, ($text -replace '(?m)^(\s*online_server\s*=\s*).*$', "`${1}`"$SvrDeadServer`""))
            Write-Host "test_guard: $cfg pointed at $toml - now $SvrDeadServer"
            $toml = $SvrDeadServer
        }
    }
    if ($toml -and (Test-SvrLocalServer $toml)) { return $toml }
    $env:REX_ONLINE_SERVER = $SvrDeadServer   # (no key, the player's config, or a live server: never the live one)
    return $SvrDeadServer
}

# Call right before a test starts the launcher (or Mod Maker) from <GameDir>. The launcher reads
# <GameDir>\svr2011.toml's top-level keys only - a key under a [section] header is not seen, and with no
# top-level online_server it uses the live server (30 Oct: a scripted sign-up made a live account that way) -
# and it never reads REX_ONLINE_SERVER. So in a test's own folder the top-level online_server is made local:
# a non-local one is rewritten to the dead address, a missing one is added at the top. Throws for a folder
# that isn't a test's own. Returns the server the launcher will use. (The launcher itself also refuses
# account requests off this PC in --capture runs unless SVR2011_ALLOW_LIVE=1.)
function Use-SvrTestLauncher([string]$GameDir) {
    $cfg = Join-Path $GameDir "svr2011.toml"
    if (-not (Test-SvrPrivatePath $cfg)) { throw "test_guard: $GameDir is not a test's own folder - refusing to start a launcher there." }
    $text = if (Test-Path -LiteralPath $cfg) { [IO.File]::ReadAllText($cfg) } else { "" }
    $top = ($text -split '(?m)^\s*\[', 2)[0]   # (the top-level part: before the first [section] header)
    $server = ""
    foreach ($m in [regex]::Matches($top, '(?m)^\s*online_server\s*=\s*"?([^"\r\n]*)"?')) { $server = $m.Groups[1].Value.Trim() }
    if ($server -and (Test-SvrLocalServer $server)) { return $server }
    if ($server) {
        $text = ($top -replace '(?m)^(\s*online_server\s*=\s*).*$', "`${1}`"$SvrDeadServer`"") + $text.Substring($top.Length)
        Write-Host "test_guard: $cfg pointed the launcher at $server - now $SvrDeadServer"
    } else {
        $text = "online_server = `"$SvrDeadServer`"`r`n" + $text
        Write-Host "test_guard: $cfg had no top-level online_server (the launcher's default is the live server) - now $SvrDeadServer"
    }
    [IO.File]::WriteAllText($cfg, $text)
    return $SvrDeadServer
}

# Game load: while the user plays something (or has their own SvR open), at most
# 2 test games run machine-wide (all chats), at below-normal priority; otherwise 4.
# Process names without .exe; add more here, or for one shell in
# SVR2011_BUSY_GAMES="name1,name2".
$SvrBusyGames = @(
    "VALORANT-Win64-Shipping", "VALORANT-Win32-Shipping", "VALORANT", "VRChat", "FortniteClient-Win64-Shipping", "cs2",
    "RocketLeague", "r5apex", "GTA5", "eldenring", "Overwatch", "LeagueClient", "League of Legends",
    "Minecraft.Windows", "Cyberpunk2077", "FC25", "FC26", "WWE2K25", "WWE2K26")
# (27 Oct 2026, the user: up to 6 at once, no lower limit while they game)
$SvrTestLimit = 6
$SvrTestLimitBusy = 6

# Why the PC is busy ("" when it isn't): a known game running, or the user's own
# svr2011.exe (a game not started by a test).
function Get-SvrBusyReason {
    $names = $SvrBusyGames + @(([string]$env:SVR2011_BUSY_GAMES -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $hit = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
    if ($hit.Count) { return "$($hit[0].ProcessName) is running" }
    $player = @(Get-SvrGames | Where-Object { $_.CommandLine -and -not (Test-IsTestGame $_) })
    if ($player.Count) { return "the user's own game is running (pid $($player[0].ProcessId))" }
    return ""
}

# Below-normal priority for a test game while the PC is busy (call right after Start-Process).
function Set-SvrTestPriority($proc) {
    if (-not $proc -or -not (Get-SvrBusyReason)) { return }
    try { (Get-Process -Id $proc.Id -ErrorAction Stop).PriorityClass = "BelowNormal" } catch { }
}

# Before every game start: waits until $Games more test games fit under the limit
# (2 when busy, 4 otherwise); throws after SVR2011_SLOT_WAIT seconds (default 600).
# While busy, test games already running drop to below-normal priority too.
function Wait-SvrTestSlot([int]$Games = 1) {
    $wait = if ($env:SVR2011_SLOT_WAIT) { [int]$env:SVR2011_SLOT_WAIT } else { 600 }
    $until = (Get-Date).AddSeconds($wait)
    $said = ""
    while ($true) {
        $busy = Get-SvrBusyReason
        $limit = if ($busy) { $SvrTestLimitBusy } else { $SvrTestLimit }
        $tests = @(Get-SvrGames | Where-Object { Test-IsTestGame $_ })
        if ($busy) {
            foreach ($t in $tests) {
                try { $g = Get-Process -Id $t.ProcessId -ErrorAction Stop; if ($g.PriorityClass -ne "BelowNormal" -and $g.PriorityClass -ne "Idle") { $g.PriorityClass = "BelowNormal" } } catch { }
            }
        }
        if ($tests.Count + $Games -le $limit) { return }
        $msg = "test_guard: $($tests.Count) test games run, limit $limit" + $(if ($busy) { " ($busy)" } else { "" }) + " - waiting for a slot"
        if ($msg -ne $said) { Write-Host $msg; $said = $msg }
        if ((Get-Date) -gt $until) { throw "$msg - gave up after $wait s." }
        Start-Sleep -Seconds 10
    }
}

# Test games keep their saves in port\runs\test_userdata (SVR2011_USER_DATA),
# never in the player's Game Files\UserData: a test that saves (autosave after a
# match, a created superstar) must not overwrite the player's created content.
# Seeded once from a copy of the player's saves; the installed DLC is linked in
# (the game only reads it). Keeps a value already set (caw_save_test.ps1).
function Use-TestSaves {
    if ($env:SVR2011_USER_DATA) { return }
    $port = Split-Path $PSScriptRoot -Parent
    $player = Join-Path (Split-Path $port -Parent) "Game Files\UserData"
    $ud = Join-Path $port "runs\test_userdata"
    if (-not (Test-Path $ud)) {
        New-Item -ItemType Directory $ud | Out-Null
        Get-ChildItem $player -Directory | Where-Object { $_.Name -match '^[0-9A-F]{16}$' -and $_.Name -ne '0000000000000000' } |
            ForEach-Object { Copy-Item -Recurse $_.FullName $ud }
    }
    # The installed DLC as a private copy, never a link: game code that writes
    # user data would otherwise change the player's own files (an older run
    # left a junction here - replaced).
    $dlc = Join-Path $player "0000000000000000"
    $mine = Join-Path $ud "0000000000000000"
    $item = Get-Item $mine -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType) { cmd /c rmdir "$mine" | Out-Null; $item = $null }
    if (-not $item -and (Test-Path $dlc)) {
        Copy-Item -Recurse $dlc $mine
        Get-ChildItem $mine -Recurse -Filter *.orig | Remove-Item
    }
    $env:SVR2011_USER_DATA = $ud
}

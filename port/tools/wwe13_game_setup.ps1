# Builds the arenas branch's own test game folder: port\runs\opt_wwe13_game
# (the opt_ prefix: the main checkout's test_guard.ps1 Stop-TestGames leaves
# runs\opt_* games running, so other sessions' tests don't close this one).
# Game data is linked read-only from the main install (Game Files), except
# pac\bg, pac\menu and the top-level pac files, which are copies so arena
# tests can replace them. Game Files itself is never written.
#   .\arena_game_setup.ps1 [-Exe <folder with svr2011.exe + dlls>]
param([string]$Exe = "")
$ErrorActionPreference = "Stop"
$port = "D:\Xbox Games Ports\SvR2011 Arenas\port"
$main = "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\Game Files"
$game = Join-Path $port "runs\opt_wwe13_game"
if (-not (Test-Path $main)) { throw "main install not found: $main" }
New-Item -ItemType Directory -Force $game | Out-Null

function Link($name, $target) {
    $p = Join-Path $game $name
    if (-not (Test-Path $p)) { cmd /c mklink /J "$p" "$target" | Out-Null }
}
foreach ($d in '$SystemUpdate', 'DLC', 'movies', 'native_shaders', 'sound') { Link $d (Join-Path $main $d) }
foreach ($d in 'Custom Movies', 'Music', 'UserData', 'Mods') { New-Item -ItemType Directory -Force (Join-Path $game $d) | Out-Null }

$pac = Join-Path $game "pac"
New-Item -ItemType Directory -Force $pac | Out-Null
foreach ($d in Get-ChildItem (Join-Path $main "pac") -Directory) {
    $p = Join-Path $pac $d.Name
    if ($d.Name -in 'bg', 'menu') {
        if (-not (Test-Path $p)) { Copy-Item -Recurse $d.FullName $p }
    } elseif (-not (Test-Path $p)) { cmd /c mklink /J "$p" "$($d.FullName)" | Out-Null }
}
foreach ($f in Get-ChildItem (Join-Path $main "pac") -File) {
    $p = Join-Path $pac $f.Name
    if (-not (Test-Path $p)) { Copy-Item $f.FullName $p }
}
foreach ($f in 'default.xex', 'nxeart', 'plist360.arc', 'plist360_4x3.arc') {
    $p = Join-Path $game $f
    if (-not (Test-Path $p)) { Copy-Item (Join-Path $main $f) $p }
}
# originals kept for restoring after a test
$orig = Join-Path $port "runs\wwe13_orig"
if (-not (Test-Path $orig)) { New-Item -ItemType Directory $orig | Out-Null }

$src = if ($Exe) { $Exe } else { $main }
foreach ($f in @('svr2011.exe') + @(Get-ChildItem $src -Filter 'rex*.dll' | ForEach-Object Name)) { Copy-Item -Force (Join-Path $src $f) (Join-Path $game $f) }
"arena_game ready: $game (exe from $src)"

# Screenshot of the Mod Maker with no window ever shown: --behind keeps the
# window hidden (frames still run), --shot saves the frame from the back buffer
# after -Wait seconds and quits. Nothing appears on any screen or the taskbar.
#   modmaker_shot.ps1 [-Exe <Mod Maker exe>] [-Game <game folder>] [-Out runs\modmaker.png] [-Wait 6]
#                     [-Extra "--editor 4 --editor-view 0"]
param([string]$Exe = "", [string]$Game = "", [string]$Out = "", [int]$Wait = 6, [string]$Extra = "")
$port = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = Join-Path $port "out\build\SourceArenas\SvR2011 Mod Maker.exe" }
if (-not $Game) { $Game = Join-Path $port "runs\opt_arena_game" }
if (-not $Out) { $Out = Join-Path $port "runs\modmaker.png" }
. (Join-Path $PSScriptRoot "test_guard.ps1")
if (Test-SvrPrivatePath (Join-Path $Game "svr2011.toml")) { Use-SvrTestLauncher $Game | Out-Null }  # (a test's own folder: never the live server)
$al = @("--game", "`"$Game`"", "--behind", "--shot", "`"$Out`"", "--shot-after", "$Wait")
if ($Extra) { $al += $Extra }
$p = Start-Process $Exe -ArgumentList $al -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(($Wait + 60) * 1000)) { Stop-Process -Id $p.Id -Force; Write-Warning "Mod Maker did not quit: killed" }
$Out

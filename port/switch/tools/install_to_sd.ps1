# Copies the game program, settings and the files the game needs to the SD card
# (Hekate's USB mode shows it as a drive). Only copies what changed.
#   install_to_sd.ps1 -Drive H [-ProgramOnly]
param([string]$Drive = "H", [switch]$ProgramOnly)
$game = "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\Game Files"
$here = Split-Path -Parent $PSScriptRoot
$sd = "$($Drive):\"
if (-not (Test-Path "$sd\atmosphere")) { throw "$sd is not the Switch SD card" }
$to = "$sd\SvR2011"
New-Item -ItemType Directory -Force $to | Out-Null
Copy-Item "C:\msys64\work\nx\b\svr2011.nro" "$sd\switch\svr_loaded.nro" -Force
Copy-Item "$here\svr2011_switch.toml" "$to\svr2011.toml" -Force
if (-not $ProgramOnly) {
    Copy-Item "$game\default.xex" $to -Force
    foreach ($f in "plist360.arc", "plist360_4x3.arc") { if (Test-Path "$game\$f") { Copy-Item "$game\$f" $to -Force } }
    foreach ($d in "pac", "sound", "movies", "DLC", "pad_icons", "nxeart") {
        robocopy "$game\$d" "$to\$d" /E /FFT /R:1 /W:1 /MT:2 /NFL /NDL /NP /NJH | Select-Object -Last 3
    }
    robocopy "$game\native_shaders" "$to\native_shaders" *.spv *.spvc *.spv.pak *.inputs *.textures pipelines.list /FFT /R:1 /W:1 /NFL /NDL /NP /NJH | Select-Object -Last 3
}
Write-VolumeCache -DriveLetter $Drive
"done"

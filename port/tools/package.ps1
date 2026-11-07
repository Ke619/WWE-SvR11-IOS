# Makes the release files, with no game data: the PC zip (the launcher and the
# game program) and the Android app on its own (released beside it: all an
# Android player needs - it installs the game from their disc image).
#   .\package.ps1 [-Version 0.2.0]    -> port\out\SvR2011-PC-v<version>.zip and
#                                       port\out\SvR2011-Android-v<version>.apk (default: port\VERSION)
# Build first (build.ps1). The player installs the game data from their own
# disc image with the launcher's Install tab, which copies these files beside it.
# -WithModMaker: the Mod Maker too (left out while it isn't ready for players).
param([string]$Version = "", [string]$Build = "", [switch]$WithModMaker)
$ErrorActionPreference = "Stop"
$port  = Split-Path $PSScriptRoot -Parent
if (-not $Version) { $Version = (Get-Content (Join-Path (Split-Path $PSScriptRoot -Parent) "VERSION.txt") -TotalCount 1).Trim() }
$build = if ($Build) { $Build } else { Join-Path $port "out\build\SourceRelease" }
$name  = "SvR2011-PC-v$Version"
$stage = Join-Path $port "out\package\$name"
$zip   = Join-Path $port "out\$name.zip"

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
$files = @("svr2011.exe", "SvR2011 Launcher.exe", "SvR2011 Updater.exe", "rexruntime.dll", "rexgpu-xenos.dll", "rexffmpeg.dll", "rexmspack.dll")
foreach ($f in $files) {
    $src = Join-Path $build $f
    if (-not (Test-Path $src)) { throw "$f is missing from $build - build first" }
    Copy-Item $src $stage
}
# The Mod Maker (the launcher's Mods tab opens it), when built and asked for.
$modmaker = Join-Path $build "SvR2011 Mod Maker.exe"
if ($WithModMaker) {
    if (Test-Path $modmaker) { Copy-Item $modmaker $stage } else { Write-Warning "no SvR2011 Mod Maker.exe in $build" }
    # the converters its Other games page runs (Python 3 + numpy + Pillow on the player's side)
    $mmtools = Join-Path $stage "Mod Maker Tools"
    New-Item -ItemType Directory -Force $mmtools | Out-Null
    foreach ($t in @("svrfmt.py", "jboy.py", "ch_tool.py", "svr10_char.py", "svr10_bruise.py", "svr10_moves.py",
                     "svr10_superstar.py", "movepack.py", "w13_char.py", "svr08_stage.py", "svr08_gimmick.py",
                     "discfs.py", "dlc_src.py", "stfs.py", "arena_port.py", "vs_port.py", "vs_renders.py", "crowd_walkway.py",
                     # the Port page's converters (Mod Maker 3 contract) and what they import / read
                     "superstar_port.py", "abilities_port.py", "ratings_port.py", "ps2_records.py", "ps2_moves.py", "prop_bones.py", "prop_bones_stock.json", "svr08_char.py", "entrance_port.py", "ymks.py", "svr08_entrance.py",
                     "hctp_entrance.py", "hctp_motion.py", "svr10_entrance.py", "wwe13_entrance.py", "w13_retarget.py",
                     "w13_skeletons.py", "yanm_decode.py", "belt_port.py", "weapon_port.py", "reskin_port.py", "title_port.py",
                     "moves_port.py", "dlc_move_ids.json", "ymbs.py", "ymbs_retarget.py", "attire_port.py", "pro_slots.json", "announcer_calls.json", "bink2_convert.py",
                     "wwe13_pack.py", "arena_tool.py", "arena_names.py",  # (imports checked transitively: arena_port needs these; arena_names since the named arena lists)
                     "ps2_entrance.py", "sym_entrance.py", "svr06_entrance.py", "jbi_entrance.py",  # (entrance_port: PS2 discs, SYM, SvR 2006, JBI)
                     "hand_tracks.py", "svr06_hands.py", "finger_retarget.py",  # (entrance_port: finger tracks)
                     "entrance_ring.py",  # (Simple mode: an entrance's tracks for the preview)
                     "model_budget.py", "xact.py", "svr07_char.py", "wwise.py",
                     "svr11_build.py", "svr11_skeleton.json", "rig_maps.json", "rig_maps.py", "svr11_skeleton.py",
                     "moveset_maps.py", "moveset_maps.json", "moveset_port.py", "fix_model.py", "gear_port.py",
                     "ps2_char.py", "ps2_port.py", "ps2_media.py", "lone_pac.py", "threads_parts.py", "entrance_props.py", "hctp_bike.py",  # (FBX import: builder + skeleton / rig maps; ps2_char: PS2 superstar models, imports discfs + the builder; ps2_media: their themes / titantrons)
                     "backstage_port.py")) {  # (backstage_port: HCTP backstage rooms - its scripts in hctp\ below)
        $f = Join-Path $PSScriptRoot $t
        if (Test-Path $f) { Copy-Item $f $mmtools } else { Write-Warning "Mod Maker Tools: no $t yet" }
    }
    # backstage_port.py's room scripts (tools\hctp, the motion converter in hctp\mot): the .py files only
    foreach ($sub in @("hctp", "hctp\mot")) {
        $dst = Join-Path $mmtools $sub
        New-Item -ItemType Directory -Force $dst | Out-Null
        Get-ChildItem (Join-Path $PSScriptRoot $sub) -Filter *.py | Copy-Item -Destination $dst
    }
    $data = Join-Path $PSScriptRoot "data"  # (reskin_port.py's stock texture table)
    if (Test-Path $data) { Copy-Item $data (Join-Path $mmtools "data") -Recurse -Force }
    # Import audit: every staged script's imports (top-level, inside functions, __import__('x')) that are
    # tools\*.py modules must be staged too - beside the script or at the top of Mod Maker Tools (2.1.0 shipped
    # ps2_port.py without ps2_media.py: "ModuleNotFoundError: No module named 'ps2_media'").
    $missing = @()
    foreach ($py in Get-ChildItem $mmtools -Filter *.py -Recurse) {
        $mods = @()
        foreach ($line in Get-Content $py.FullName) {
            if ($line -match '^\s*import\s+(.+)$') {
                $mods += ($Matches[1] -split '#')[0] -split ',' | ForEach-Object { (($_.Trim() -split '\s+')[0] -split '\.')[0] }
            } elseif ($line -match '^\s*from\s+([A-Za-z_][\w.]*)\s+import') {
                $mods += ($Matches[1] -split '\.')[0]
            }
            foreach ($m in [regex]::Matches($line, "__import__\(\s*['""]([A-Za-z_]\w*)['""]")) { $mods += $m.Groups[1].Value }
        }
        foreach ($m in ($mods | Where-Object { $_ } | Sort-Object -Unique)) {
            $rel = $py.DirectoryName.Substring($mmtools.Length).TrimStart('\')   # (the script's subfolder: hctp, hctp\mot)
            if (-not (Test-Path (Join-Path $PSScriptRoot "$m.py")) -and
                -not (Test-Path (Join-Path (Join-Path $PSScriptRoot $rel) "$m.py"))) { continue }   # (not one of ours: the standard library, numpy...)
            if (Test-Path (Join-Path $mmtools "$m.py")) { continue }
            # (beside the script only counts when the script puts its own folder on sys.path: the Tools' embeddable
            # Python doesn't - 2.1.3's hctp_fake56.py: "Missing: hctp2jboy - install now?")
            if ((Test-Path (Join-Path $py.DirectoryName "$m.py")) -and
                (((Get-Content $py.FullName -Raw) -match 'sys\.path\.insert\(0,\s*os\.path\.dirname\(os\.path\.abspath\(__file__\)\)\)') -or
                 (((Get-Content $py.FullName -Raw) -match '(\w+)\s*=\s*os\.path\.dirname\(os\.path\.abspath\(__file__\)\)') -and
                  ((Get-Content $py.FullName -Raw) -match ('sys\.path\.insert\(0,\s*' + $Matches[1] + '\)'))))) { continue }   # (or HERE = ...; sys.path.insert(0, HERE))
            $missing += "$($py.Name) imports $m (tools\$m.py)"
        }
    }
    if ($missing) { throw "Mod Maker Tools is missing modules its scripts import - add them to the list above:`n  " + ($missing -join "`n  ") }
    Write-Host "Mod Maker Tools: imports checked ($(@(Get-ChildItem $mmtools -Filter *.py -Recurse).Count) scripts)"
    # vgmstream (the Wwise Vorbis theme decoder wwise.py runs; port\third_party\vgmstream, not in git:
    # the official release, version and SHA-256 in its VERSION.txt) -> Mod Maker Tools\vgmstream
    $vgm = Join-Path $port "third_party\vgmstream"
    if (Test-Path (Join-Path $vgm "vgmstream-cli.exe")) {
        $vdst = Join-Path $mmtools "vgmstream"
        New-Item -ItemType Directory -Force $vdst | Out-Null
        Get-ChildItem $vgm -File | Copy-Item -Destination $vdst   # (VERSION.txt too: where it came from + its SHA-256)
        # (the exe won't start without a libg719_decode.dll, and no game audio we convert uses G.719: our stand-in,
        # modmaker\g719_stub.c, in place of the release's)
        $g719 = Join-Path $port "out\modmaker\g719\libg719_decode.dll"
        if (-not (Test-Path $g719)) { throw "no $g719 - build it in this checkout (bash port/modmaker/build_cli.sh)" }
        Copy-Item -Force $g719 $vdst
    } else {   # (2.1.0 shipped without it: WWE '12+ themes didn't convert)
        throw "no $vgm\vgmstream-cli.exe - WWE '12+ themes won't convert. Copy the vgmstream release (vgmstream-cli.exe, its DLLs and VERSION.txt) from the main checkout's port\third_party\vgmstream into $vgm, then package again."
    }
    # (and svrmod.exe, which svrfmt.py runs to compress pac entries and the WWE '13+ converters run to unpack)
    $cli = Join-Path $port "out\modmaker\svrmod.exe"
    if (Test-Path $cli) { Copy-Item $cli $mmtools } else {   # (2.1.0 shipped without it: "ToolMissing: svrmod.exe")
        throw "no $cli - the Mod Maker's converters need it (WWE '13+ ports fail with ToolMissing: svrmod.exe). Build it in this checkout (bash port/modmaker/build_cli.sh -> port\out\modmaker\svrmod.exe), then package again."
    }
    # (svrmod.exe doesn't start without its LZX decoder beside it)
    $cliDll = Join-Path $port "out\modmaker\rexmspack.dll"
    if (Test-Path $cliDll) { Copy-Item $cliDll $mmtools } else { throw "no $cliDll - svrmod.exe won't start without it. Build it with bash port/modmaker/build_cli.sh, then package again." }
    # the guide (pictures, diagrams, tutorials): docs/guide -> "Mod Maker Guide"
    $guide = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "docs\guide"
    if (Test-Path $guide) { Copy-Item $guide (Join-Path $stage "Mod Maker Guide") -Recurse }
}
# No Visual C++ runtime DLLs in the release (2.1.1): the programs use Windows' own (the launcher has its
# C runtime built in and points a player without one to Microsoft's vc_redist.x64). 2.1.0 shipped 14.29
# copies next to the programs, and overlays built for the newer runtime (Discord, Steam, RTSS, NVIDIA...)
# failed in our processes there: "unable to start correctly (0xc0000142)".
$vcrt = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_atomic_wait.dll")
Get-ChildItem $build -Filter "*.dll" | Where-Object { $files -notcontains $_.Name -and $vcrt -notcontains $_.Name.ToLower() } |
    Copy-Item -Destination $stage
Copy-Item (Join-Path $port "dist\Read Me.txt") $stage
Copy-Item (Join-Path $port "dist\Third-party notices.txt") $stage   # (credits and licence texts; the APK carries it too: build_apk.py)
# NVIDIA DLSS (experimental): a build with the DLSS SDK (CMake SVR2011_DLSS_SDK) has nvngx_dlss.dll beside the exe -
# copied above with the other DLLs. It ships only in this zip, never in the repository, with NVIDIA's licence
# (the RTX SDKs licence from the same SDK folder) beside it.
$dlssDll = Join-Path $stage "nvngx_dlss.dll"
if (Test-Path $dlssDll) {
    $sig = Get-AuthenticodeSignature $dlssDll
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "O=NVIDIA Corporation") {
        throw "nvngx_dlss.dll in $build isn't signed by NVIDIA ($($sig.Status))"
    }
    $sdk = ""
    $cache = Join-Path $build "CMakeCache.txt"
    if (Test-Path $cache) {
        $line = Select-String -Path $cache -Pattern "^SVR2011_DLSS_SDK:PATH=(.*)$" | Select-Object -First 1
        if ($line) { $sdk = $line.Matches[0].Groups[1].Value }
    }
    if (-not $sdk -and $env:DLSS_SDK) { $sdk = $env:DLSS_SDK }
    $licence = if ($sdk) { Join-Path $sdk "LICENSE.txt" } else { "" }
    if (-not $licence -or -not (Test-Path $licence)) { throw "nvngx_dlss.dll is in the build but NVIDIA's LICENSE.txt isn't (SDK: '$sdk')" }
    Copy-Item $licence (Join-Path $stage "NVIDIA DLSS licence.txt")
    Write-Host "DLSS: nvngx_dlss.dll $((Get-Item $dlssDll).VersionInfo.FileVersion) + NVIDIA DLSS licence.txt"
} else {
    Write-Warning "no nvngx_dlss.dll in $build - the release has no DLSS (built without the DLSS SDK)"
}
# The native renderer's shaders (converted from the game's by
# tools/convert_shaders.py; without them it draws nothing).
$shaders = Join-Path $port "runs\shaders_native\dxil"
if (Test-Path (Join-Path $shaders "present.vs.dxil")) {
    New-Item -ItemType Directory -Force (Join-Path $stage "native_shaders") | Out-Null
    Get-ChildItem $shaders -File | Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
    # (and the Vulkan backend's SPIR-V beside them)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "spirv") -Filter "*.spv" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
    # (and the Direct3D 11 backend's DXBC: older GPUs; .dxbc4 for feature level 10_x)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "dxbc") -Filter "*.dxbc*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
} else {
    Write-Warning "no native shaders in $shaders (tools\convert_shaders.py) - this package uses the emulated renderer"
}
# PlayStation / keyboard button pictures (made by deploy.ps1 into the game
# folder, tools/make_pad_icons.py).
$icons = Join-Path (Split-Path $port -Parent) "Game Files\pad_icons"
if (-not (Test-Path (Join-Path $icons "pad_icons.txt"))) {  # (a release copy in _release\port)
    $icons = Join-Path (Split-Path (Split-Path $port -Parent) -Parent) "Game Files\pad_icons"
}
if (Test-Path (Join-Path $icons "pad_icons.txt")) {
    New-Item -ItemType Directory -Force (Join-Path $stage "pad_icons") | Out-Null
    Get-ChildItem $icons -File | Where-Object { $_.Extension -in ".dds", ".txt", ".bin" } |
        Copy-Item -Destination (Join-Path $stage "pad_icons")
} else {
    Write-Warning "no pad_icons in $icons - the package has Xbox button pictures only"
}

# Mods that come with the port (the launcher installs them into the game):
# the .svrmod files in "Bundled Mods" at the top of the project (outside git;
# a release copy in _release\port looks two levels up).
$bundled = Join-Path (Split-Path $port -Parent) "Bundled Mods"
if (-not (Test-Path $bundled)) { $bundled = Join-Path (Split-Path (Split-Path $port -Parent) -Parent) "Bundled Mods" }
if (Get-ChildItem $bundled -Filter "*.svrmod" -ErrorAction SilentlyContinue) {
    New-Item -ItemType Directory -Force (Join-Path $stage "Bundled Mods") | Out-Null
    Get-ChildItem $bundled -Filter "*.svrmod" | Copy-Item -Destination (Join-Path $stage "Bundled Mods")
} else {
    Write-Warning "no Bundled Mods\*.svrmod - the package comes with no mods"
}

# The known pipelines, built ahead in the menus (tools/merge_pipelines.py).
$plist = Join-Path $port "dist\pipelines.list"
if (Test-Path $plist) {
    New-Item -ItemType Directory -Force (Join-Path $stage "native_shaders") | Out-Null
    Copy-Item $plist (Join-Path $stage "native_shaders") -Force
}
# The shaders in one file per API (one read instead of a thousand: tools/pack_shaders.py).
if (Test-Path (Join-Path $stage "native_shaders")) {
    python (Join-Path $port "tools\pack_shaders.py") (Join-Path $stage "native_shaders") --remove-loose | Out-Host
}
# The Android app, for the launcher's Create APK Package (tools/build_apk.py).
$apk = Join-Path $port "out\android\SvR2011.apk"
if (Test-Path $apk) {
    New-Item -ItemType Directory -Force (Join-Path $stage "Android") | Out-Null
    Copy-Item $apk (Join-Path $stage "Android")
    # (the release's own Android download; the app's updater looks for this name)
    Copy-Item $apk (Join-Path $port "out\SvR2011-Android-v$Version.apk") -Force
} else {
    Write-Warning "no $apk (tools\build_apk.py) - this package has no Android app"
}

# Never the debug / test suite (docs/DEBUG_SUITE_PLAN.md): a debug-suite file or binary fails the package.
. (Join-Path $PSScriptRoot "release_guard.ps1")
Assert-NoDebugSuite $stage @($apk)
"release guard: no debug / test suite in the package"

# The release's file list (release_files.txt: size, SHA-256 and path of every file): the updater checks an
# install against it after copying, and the launcher at every start, so a file an update or an unzip left
# out is found and can be repaired.
$listFile = Join-Path $stage "release_files.txt"
$rootLen = (Resolve-Path $stage).Path.TrimEnd('\').Length + 1
$lines = @("SvR2011 release $Version")
foreach ($f in Get-ChildItem $stage -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($rootLen)
    $lines += "{0}`t{1}`t{2}" -f $f.Length, (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower(), $rel
}
[IO.File]::WriteAllLines($listFile, $lines, (New-Object Text.UTF8Encoding $false))
"release_files.txt: $($lines.Count - 1) files"
Remove-Item $zip -ErrorAction SilentlyContinue
# Entry names with "/": Compress-Archive in Windows PowerShell writes "\",
# which Linux extractors such as the Steam Deck's Ark turn into files named
# "native_shaders\x" instead of folders (no shaders: no native renderer).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# (never the VC++ runtime DLLs, from any source)
foreach ($f in $vcrt) { if (Test-Path (Join-Path $stage $f)) { throw "$f is in the release folder - the release ships no VC++ runtime DLLs" } }
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    $root = (Resolve-Path $stage).Path.TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $entry = $f.FullName.Substring($root.Length).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
"Packaged $zip"
# The bundled mods on their own, for the Android app (it downloads this asset
# and installs them as the PC launcher does - a phone has no PC zip).
$modsZip = Join-Path $port "out\SvR2011-Mods-v$Version.zip"
Remove-Item $modsZip -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $stage "Bundled Mods")) {
    $archive = [IO.Compression.ZipFile]::Open($modsZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem (Join-Path $stage "Bundled Mods") -File) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "Bundled Mods/" + $f.Name,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose() }
    "Packaged $modsZip (upload it to the release too: the Android app's bundled mods)"
}
if (Test-Path $apk) { "Packaged " + (Join-Path $port "out\SvR2011-Android-v$Version.apk") + " (upload it to the release too)" }
Get-ChildItem $stage | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }

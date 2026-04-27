# package-release.ps1 - Build a Windows release zip for Bot2 GitHub Releases
#
# Produces: release\Bot2-<version>-windows.zip
#
# The zip contains a single mod folder layout that drops straight into a
# user's JKA install:
#
#   bot2_jka\
#     jampgamex86.dll      \
#     cgamex86.dll          > built mod DLLs
#     uix86.dll            /
#     maps\
#       <whatever .navmesh files you point at via -NavmeshDir>
#     bot2.cfg              (autoexec config; spawns a bot for demonstration)
#     README-RELEASE.txt    (install + usage guide for non-technical users)
#
# USAGE:
#   .\package-release.ps1 -Version v1.0.0
#   .\package-release.ps1 -Version v1.0.0 -NavmeshDir "C:\some\other\dir"
#
# The -NavmeshDir parameter points to a folder containing .navmesh files
# (and optionally .nav_connections sidecars) to ship. The script copies
# every .navmesh from that folder into the bundled bot2_jka\maps\ dir.
#
# DEFAULT: "navmesh example files\" at the repo root. Drop the navmesh
# files you want shipped into that folder and they'll be tracked in git
# AND included in every release zip - one folder, two distribution
# channels, no separate steps. Override -NavmeshDir if you want to ship
# a different curated set for a specific release.
#
# LEGAL NOTE: a .navmesh is derived data from a .bsp. For maps you didn't
# author, get permission from the map author before shipping the .navmesh.
# Don't ship navmeshes for stock Raven/LucasArts maps.
#
# NEXT STEP after running this:
#   1. Go to https://github.com/RebirthMeow/Bot2/releases/new
#   2. Pick a tag (matching the -Version arg is conventional)
#   3. Drag the produced .zip into the Release page's "Attach binaries" box
#   4. Hit "Publish release"

param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $false)]
    [string]$NavmeshDir,

    [Parameter(Mandatory = $false)]
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# Default -NavmeshDir to the in-repo "navmesh example files" folder.
# Curate that folder once (drop in the .navmesh files you want to ship);
# every subsequent release picks them up automatically.
if (-not $NavmeshDir) {
    $NavmeshDir = Join-Path $ScriptDir "navmesh example files"
    Write-Host "Using default -NavmeshDir: $NavmeshDir" -ForegroundColor DarkGray
}

$BuildDir   = Join-Path $ScriptDir "build\$BuildConfig"
$ReleaseDir = Join-Path $ScriptDir "release"
$StagingDir = Join-Path $ReleaseDir "staging"

# --- Validate prerequisites ---

$requiredDlls = @("jampgamex86.dll", "cgamex86.dll", "uix86.dll")
$missingDlls = @()
foreach ($dll in $requiredDlls) {
    if (-not (Test-Path (Join-Path $BuildDir $dll))) {
        $missingDlls += $dll
    }
}
if ($missingDlls.Count -gt 0) {
    Write-Host "ERROR: missing built DLLs in $BuildDir :" -ForegroundColor Red
    foreach ($m in $missingDlls) { Write-Host "  - $m" -ForegroundColor Red }
    Write-Host "Build the project first (open the solution in Visual Studio and build $BuildConfig)." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $NavmeshDir)) {
    Write-Host "ERROR: -NavmeshDir does not exist: $NavmeshDir" -ForegroundColor Red
    if ($NavmeshDir -like "*navmesh example files*") {
        Write-Host "       The default folder is missing. Create it and drop .navmesh files into it:" -ForegroundColor Red
        Write-Host "         New-Item -ItemType Directory -Path '$NavmeshDir' -Force" -ForegroundColor Red
    }
    exit 1
}

$navmeshFiles = Get-ChildItem -Path $NavmeshDir -Filter "*.navmesh" -File
if ($navmeshFiles.Count -eq 0) {
    Write-Host "ERROR: no .navmesh files found in $NavmeshDir" -ForegroundColor Red
    Write-Host "       Drop at least one .navmesh into that folder before packaging a release." -ForegroundColor Red
    Write-Host "       Build navmeshes with: https://github.com/RebirthMeow/daemonmap-jka" -ForegroundColor Red
    exit 1
}

# --- Clean and recreate staging ---

if (Test-Path $StagingDir) { Remove-Item -Recurse -Force $StagingDir }
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

$ReleaseName = "Bot2-$Version-windows"
$PayloadDir  = Join-Path $StagingDir $ReleaseName
$ModDir      = Join-Path $PayloadDir "bot2_jka"
$ModMapsDir  = Join-Path $ModDir "maps"
New-Item -ItemType Directory -Path $ModMapsDir -Force | Out-Null

Write-Host "Staging release: $ReleaseName" -ForegroundColor Green

# --- Copy mod DLLs ---

foreach ($dll in $requiredDlls) {
    Copy-Item -Path (Join-Path $BuildDir $dll) -Destination $ModDir -Force
    Write-Host "  + bot2_jka\$dll"
}

# --- Copy navmeshes ---

foreach ($nm in $navmeshFiles) {
    Copy-Item -Path $nm.FullName -Destination $ModMapsDir -Force
    Write-Host "  + bot2_jka\maps\$($nm.Name)"

    # Also copy any matching .nav_connections sidecar if it sits next to it
    $sidecar = Join-Path $nm.DirectoryName ($nm.BaseName + ".nav_connections")
    if (Test-Path $sidecar) {
        Copy-Item -Path $sidecar -Destination $ModMapsDir -Force
        Write-Host "  + bot2_jka\maps\$($nm.BaseName).nav_connections"
    }
}

# --- Write a starter bot2.cfg ---
# Autoexecutes when the user runs the mod. Keeps the demo "it works"
# friction-free: pick a map with bots already, no command-line typing.
$bot2Cfg = @"
// bot2.cfg - default config shipped with the Bot2 release.
// Loaded automatically when the bot2_jka mod is selected.
//
// Tweak any of the cvars below or comment them out as you like.
// Console commands (open with the tilde key '~') let you change them at runtime.

// 0/1 - apply Bot2 logic to all bots regardless of name suffix.
//       Default 0 means only bots whose name ends in "_v2" use Bot2 logic.
//       Set to 1 if you want every /addbot to use the new AI.
seta bot_forcebot2 "1"

// Telemetry bitmask - 1 = movement/jump, 2 = combat. OR them together (3 = both).
seta bot_telemetry "0"

// Bots think faster than vanilla. Lower = smarter but more CPU; default JKA is 100.
seta bot_thinktime "50"

echo "[bot2.cfg] loaded - bot_forcebot2 enabled, all /addbot will use Bot2 AI"
"@
$bot2Cfg | Set-Content -LiteralPath (Join-Path $ModDir "bot2.cfg") -Encoding ASCII

# --- Write README-RELEASE.txt aimed at non-technical users ---

$shippedMaps = ($navmeshFiles | ForEach-Object { "  - " + $_.BaseName }) -join "`r`n"

$ReleaseReadme = @"
Bot2 - Windows release $Version
================================

WHAT THIS IS
  Bot2 is an advanced bot AI for "Star Wars: Jedi Knight: Jedi Academy"
  multiplayer. It runs on top of OpenJK (the community engine port). The
  bots use a navmesh for pathfinding, can perform strafe-jumps, wallruns,
  jumppad and elevator routing, and play CTF as offense/chase/base.

PREREQUISITES
  1. A legitimate copy of Jedi Knight: Jedi Academy installed.
  2. OpenJK installed - get the latest release from:
        https://github.com/JACoders/OpenJK/releases
     Install it into your JKA folder so you have OpenJK_MP.exe in
     the same directory as the original jamp.exe (typically
     C:\Program Files (x86)\LucasArts\Star Wars Jedi Knight Jedi
     Academy\GameData\, or wherever you have JKA installed).

INSTALLING THE BOT2 MOD
  Inside this zip is a folder named "bot2_jka". Drop that whole folder
  into your JKA install's "GameData" folder. So you should end up with:
     <JKA install>\GameData\bot2_jka\jampgamex86.dll
     <JKA install>\GameData\bot2_jka\cgamex86.dll
     <JKA install>\GameData\bot2_jka\uix86.dll
     <JKA install>\GameData\bot2_jka\bot2.cfg
     <JKA install>\GameData\bot2_jka\maps\<mapname>.navmesh
     <JKA install>\GameData\bot2_jka\README-RELEASE.txt   (this file)

  Do NOT overwrite anything in <JKA install>\GameData\base\. Bot2 is a
  separate mod folder; your vanilla install stays untouched.

LAUNCHING
  1. Run OpenJK_MP.exe.
  2. From the main menu choose:  Setup -> Mods -> Bot2 (bot2_jka)
     Click "Load Mod". The game restarts under the mod.
     (Or skip the menu and launch with: OpenJK_MP.exe +set fs_game bot2_jka)

  3. Start a multiplayer game on one of the included maps:
$shippedMaps

     Open the in-game console with the tilde key (~). To begin:
        /devmap <mapname>          (loads the map in dev mode, e.g. /devmap mp/ctf1)
        /addbot kyle 5             (adds a Bot2 bot named "kyle" at skill 5)
        /addbot reborn 5 b         (adds another bot to the blue team)
     With "bot_forcebot2 1" set in bot2.cfg, every /addbot uses the Bot2 AI.

USEFUL CONSOLE COMMANDS
  /addbot <name> <skill 1-5> [team r|b]      - spawn a bot
  /bot_forcerole <0|1|2|3>                   - 1=offense, 2=chase, 3=base, 0=auto
  /bot_telemetry <0|1|2|3>                   - 1=movement, 2=combat, 3=both
  /navdraw 800                               - render navmesh polys near you
  /navdrawoffmesh 800                        - render the off-mesh connections
                                               (drops, jumps, wallruns)
  /bot_scan_wallruns                         - headless scanner; writes
                                               <mapname>.nav_connections so
                                               you can rebuild the navmesh
                                               with wallruns. See "BUILDING
                                               YOUR OWN NAVMESHES" below.

BUILDING YOUR OWN NAVMESHES (other maps)
  Bot2 needs a .navmesh file for every map you want bots to play on.
  This release ships navmeshes for the maps listed above. For any other
  map, use the companion tool to build one yourself:

        https://github.com/RebirthMeow/daemonmap-jka

  Quick path: install daemonmap-jka, drag your map's .bsp file onto
  daemonmap-jka.bat. The resulting .navmesh goes into bot2_jka\maps\.

TROUBLESHOOTING
  - "I get 'NavMesh not found' or bots stand still."
    The map you loaded doesn't have a .navmesh in bot2_jka\maps\. Either
    pick one of the included maps, or build a navmesh for that map with
    daemonmap-jka.

  - "/addbot says no bots available."
    Vanilla JKA needs the bot list populated for that game type. Try
    /devmap mp/ctf1 first (CTF maps come with a default bot list); other
    types may need a custom .bot file.

  - "OpenJK won't start / says missing pak files."
    OpenJK requires a working JKA install with the original .pk3 files
    in GameData\base\. Reinstall JKA from your original disc / Steam.

LICENSE
  GPLv2, inherited from OpenJK. See LICENSE.txt in the source repo.

SOURCE
  https://github.com/RebirthMeow/Bot2

  Companion navmesh compiler:
  https://github.com/RebirthMeow/daemonmap-jka
"@

$ReleaseReadme | Set-Content -LiteralPath (Join-Path $ModDir "README-RELEASE.txt") -Encoding UTF8

# --- Zip it up ---

$ZipPath = Join-Path $ReleaseDir "$ReleaseName.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }

Write-Host ""
Write-Host "Compressing to: $ZipPath" -ForegroundColor Green
Compress-Archive -Path "$PayloadDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal

# --- Cleanup staging ---

Remove-Item -Recurse -Force $StagingDir

# --- Report ---

$zipInfo = Get-Item $ZipPath
$sizeMb = [math]::Round($zipInfo.Length / 1MB, 2)

Write-Host ""
Write-Host "=== Release packaged ===" -ForegroundColor Green
Write-Host "  $ZipPath  ($sizeMb MB)"
Write-Host "  Maps included:"
foreach ($nm in $navmeshFiles) { Write-Host "    - $($nm.BaseName)" }
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Open https://github.com/RebirthMeow/Bot2/releases/new"
Write-Host "  2. Tag: $Version"
Write-Host "  3. Drag the zip into the 'Attach binaries' box"
Write-Host "  4. Publish."

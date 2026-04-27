# package-release.ps1 - Build an all-in-one Windows release zip for Bot2 GitHub Releases
#
# Produces: release\Bot2-<version>-windows.zip
#
# The zip is "all-in-one": OpenJK engine binaries + the Bot2 mod folder.
# A user with a working JKA install can drop the contents of this zip
# straight into <JKA install>\GameData\ and play. They do NOT need to
# install OpenJK separately.
#
# Layout inside the zip (everything overlays into <JKA install>\GameData\):
#
#   openjk.x86.exe         <- MP client engine (the user launches this)
#   openjkded.x86.exe      <- dedicated server engine (host bot games)
#   rd-vanilla_x86.dll     <- default renderer
#   rd-rend2_x86.dll       <- alt renderer (modern features)
#   SDL2.dll               <- runtime dependency
#   README-RELEASE.txt     <- install + usage guide for end users
#   bot2_jka\              <- the Bot2 mod folder
#     jampgamex86.dll        - server-side game DLL (Bot2 AI lives here)
#     cgamex86.dll           - client-side game DLL
#     uix86.dll              - client-side UI DLL
#     bot2.cfg               - autoexec config (enables bot_forcebot2 etc.)
#     maps\
#       <whatever .navmesh files the -NavmeshDir contains>
#
# USAGE:
#   .\package-release.ps1 -Version v1.0.0
#   .\package-release.ps1 -Version v1.0.0 -NavmeshDir "C:\some\other\dir"
#
# DEFAULT NavmeshDir: "navmesh example files\" at the repo root. Drop the
# navmesh files you want shipped into that folder; they're tracked in git
# AND included in every release zip from one source.
#
# REQUIRED BUILD ARTIFACTS in build\<BuildConfig>\:
#   - openjk.x86.exe       (build the OpenJKMP target in Visual Studio)
#   - openjkded.x86.exe    (build the OpenJKDed target)
#   - rd-vanilla_x86.dll   (build the rd-vanilla target)
#   - rd-rend2_x86.dll     (build the rd-rend2 target)
#   - jampgamex86.dll      (game DLL — your Bot2 work lives here)
#   - cgamex86.dll, uix86.dll
#
# Build the whole solution at once and you get all of these. If the
# script complains about a missing target, build that target in VS.
#
# LEGAL NOTE: a .navmesh is derived data from a .bsp. For maps you didn't
# author, get permission from the map author before shipping the .navmesh.
# Don't ship navmeshes for stock Raven/LucasArts maps. The OpenJK engine
# binaries shipped here are GPLv2 — fine to redistribute.
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
if (-not $NavmeshDir) {
    $NavmeshDir = Join-Path $ScriptDir "navmesh example files"
    Write-Host "Using default -NavmeshDir: $NavmeshDir" -ForegroundColor DarkGray
}

$BuildDir   = Join-Path $ScriptDir "build\$BuildConfig"
$Sdl2Source = Join-Path $ScriptDir "lib\SDL2\bin\x86\SDL2.dll"
$ReleaseDir = Join-Path $ScriptDir "release"
$StagingDir = Join-Path $ReleaseDir "staging"

# --- Validate build artifacts (engine + game DLLs) ---

# Required = the script fails if these are missing.
# Optional = the script warns and continues without them.
$requiredArtifacts = @(
    @{ Name = "openjk.x86.exe";      Required = $true;  ZipPath = "" },
    @{ Name = "rd-vanilla_x86.dll";  Required = $true;  ZipPath = "" },
    @{ Name = "jampgamex86.dll";     Required = $true;  ZipPath = "bot2_jka" },
    @{ Name = "cgamex86.dll";        Required = $true;  ZipPath = "bot2_jka" },
    @{ Name = "uix86.dll";           Required = $true;  ZipPath = "bot2_jka" },
    @{ Name = "openjkded.x86.exe";   Required = $false; ZipPath = "" },
    @{ Name = "rd-rend2_x86.dll";    Required = $false; ZipPath = "" }
)

$missing = @()
$missingOptional = @()
foreach ($a in $requiredArtifacts) {
    $p = Join-Path $BuildDir $a.Name
    if (-not (Test-Path $p)) {
        if ($a.Required) { $missing += $a.Name } else { $missingOptional += $a.Name }
    }
}

if ($missing.Count -gt 0) {
    Write-Host "ERROR: missing required build artifacts in $BuildDir :" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Red }
    Write-Host "" -ForegroundColor Red
    Write-Host "Build the full solution in Visual Studio (Build -> Build Solution)." -ForegroundColor Red
    Write-Host "If only the game DLLs are present but engine targets are missing, the" -ForegroundColor Red
    Write-Host "OpenJKMP / OpenJKDed / rd-vanilla / rd-rend2 projects in your solution" -ForegroundColor Red
    Write-Host "may be unchecked. Right-click each in Solution Explorer -> Build." -ForegroundColor Red
    exit 1
}

if ($missingOptional.Count -gt 0) {
    Write-Host "WARNING: optional artifacts missing (release will ship without them):" -ForegroundColor Yellow
    foreach ($m in $missingOptional) { Write-Host "  - $m" -ForegroundColor Yellow }
}

if (-not (Test-Path $Sdl2Source)) {
    Write-Host "ERROR: SDL2.dll not found at expected location:" -ForegroundColor Red
    Write-Host "  $Sdl2Source" -ForegroundColor Red
    Write-Host "OpenJK depends on SDL2 at runtime. Check lib\SDL2\bin\x86\." -ForegroundColor Red
    exit 1
}

# --- Validate navmesh source ---

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

# --- Copy engine + mod artifacts to their respective destinations ---

foreach ($a in $requiredArtifacts) {
    $src = Join-Path $BuildDir $a.Name
    if (-not (Test-Path $src)) { continue }    # already-warned optional

    $destDir = if ($a.ZipPath) { Join-Path $PayloadDir $a.ZipPath } else { $PayloadDir }
    Copy-Item -Path $src -Destination $destDir -Force
    $relPath = if ($a.ZipPath) { "$($a.ZipPath)\$($a.Name)" } else { $a.Name }
    Write-Host "  + $relPath"
}

# --- Copy SDL2.dll to zip root ---

Copy-Item -Path $Sdl2Source -Destination $PayloadDir -Force
Write-Host "  + SDL2.dll"

# --- Copy navmeshes (and any matching sidecars) ---

foreach ($nm in $navmeshFiles) {
    Copy-Item -Path $nm.FullName -Destination $ModMapsDir -Force
    Write-Host "  + bot2_jka\maps\$($nm.Name)"

    $sidecar = Join-Path $nm.DirectoryName ($nm.BaseName + ".nav_connections")
    if (Test-Path $sidecar) {
        Copy-Item -Path $sidecar -Destination $ModMapsDir -Force
        Write-Host "  + bot2_jka\maps\$($nm.BaseName).nav_connections"
    }
}

# --- Write a starter bot2.cfg ---

$bot2Cfg = @"
// bot2.cfg - default config shipped with the Bot2 release.
// Loaded automatically when the bot2_jka mod is selected.

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

# --- Write README-RELEASE.txt at the zip root ---

$shippedMaps = ($navmeshFiles | ForEach-Object { "  - " + $_.BaseName }) -join "`r`n"

$ReleaseReadme = @"
Bot2 - Windows release $Version (all-in-one)
=============================================

WHAT THIS IS
  Bot2 is an advanced bot AI for "Star Wars: Jedi Knight: Jedi Academy"
  multiplayer. The bots use a navmesh for pathfinding, can perform
  strafe-jumps, wallruns, jumppad and elevator routing, and play CTF
  with offense / chase / base roles.

  This zip is ALL-IN-ONE: it includes the OpenJK engine (the modern
  community port of the JKA engine) AND the Bot2 mod. You do not need
  to install OpenJK separately.

PREREQUISITE
  A legitimate copy of "Star Wars: Jedi Knight: Jedi Academy" installed
  (Steam, GOG, or original disc). The original .pk3 game-asset files
  must be present in <JKA install>\GameData\base\.

INSTALLATION
  1. Locate your JKA install. The folder structure looks like:
        <JKA install>\GameData\base\assets0.pk3
        <JKA install>\GameData\base\assets1.pk3   (etc.)
        <JKA install>\GameData\jamp.exe           (vanilla launcher)

  2. Open the Bot2 zip you downloaded.

  3. Copy ALL the contents of the zip into <JKA install>\GameData\.
     After copying, your GameData folder should contain:
        <JKA install>\GameData\base\        (already there - untouched)
        <JKA install>\GameData\openjk.x86.exe        (NEW - launches Bot2)
        <JKA install>\GameData\openjkded.x86.exe     (NEW - dedicated server)
        <JKA install>\GameData\rd-vanilla_x86.dll    (NEW)
        <JKA install>\GameData\rd-rend2_x86.dll      (NEW, if shipped)
        <JKA install>\GameData\SDL2.dll              (NEW)
        <JKA install>\GameData\bot2_jka\             (NEW - the mod folder)

     The vanilla "base\" folder is untouched. The OpenJK engine binaries
     sit alongside the original jamp.exe; they don't replace anything.

LAUNCHING
  1. Run openjk.x86.exe (the new file in your GameData folder).

  2. From the main menu choose: Setup -> Mods -> Bot2 (bot2_jka)
     Click "Load Mod". The game restarts under the mod.
     (Or skip the menu and launch with: openjk.x86.exe +set fs_game bot2_jka)

  3. Start a multiplayer game on one of the included maps:
$shippedMaps

     Open the in-game console with the tilde key (~). To begin:
        /devmap <mapname>          (loads the map; e.g. /devmap mp/ctf1)
        /addbot kyle 5             (adds a Bot2 bot named "kyle" at skill 5)
        /addbot reborn 5 b         (adds another bot to the blue team)
     With "bot_forcebot2 1" set in bot2.cfg, every /addbot uses the Bot2 AI.

USEFUL CONSOLE COMMANDS
  /addbot <name> <skill 1-5> [team r|b]      - spawn a bot
  /bot_forcerole <0|1|2|3>                   - 1=offense, 2=chase, 3=base, 0=auto
  /bot_telemetry <0|1|2|3>                   - 1=movement, 2=combat, 3=both
  /navdraw 800                               - render navmesh polys near you
  /navdrawoffmesh 800                        - render off-mesh connections
                                               (drops, jumps, wallruns)
  /bot_scan_wallruns                         - headless scanner; writes
                                               <mapname>.nav_connections
                                               (see "BUILDING NAVMESHES")

BUILDING YOUR OWN NAVMESHES (other maps)
  Bot2 needs a .navmesh file for every map you want bots to play on.
  This release ships navmeshes for the maps listed above. For any other
  map, use the companion tool to build one yourself:

        https://github.com/RebirthMeow/daemonmap-jka

  Quick path: download daemonmap-jka, drag your map's .bsp file onto
  daemonmap-jka.bat. The resulting .navmesh goes into:
        <JKA install>\GameData\bot2_jka\maps\<mapname>.navmesh

TROUBLESHOOTING
  - "I get 'NavMesh not found' or bots stand still."
    The map you loaded doesn't have a .navmesh in bot2_jka\maps\. Pick
    one of the included maps, or build a navmesh with daemonmap-jka.

  - "/addbot says no bots available."
    Vanilla JKA needs the bot list populated for that game type. Try
    /devmap mp/ctf1 first (CTF maps come with a default bot list).

  - "openjk.x86.exe won't start / says missing pak files."
    OpenJK requires a working JKA install with the original .pk3 files
    in GameData\base\. Reinstall JKA from your original disc / Steam.

  - "Antivirus / Windows SmartScreen flags openjk.x86.exe."
    OpenJK is open-source community software and isn't code-signed by a
    commercial CA. SmartScreen flags unsigned executables. If you don't
    trust this build, grab the engine binaries directly from
    https://github.com/JACoders/OpenJK/releases — they're functionally
    equivalent (this fork doesn't modify the engine source).

LICENSE
  GPLv2, inherited from OpenJK. See the source repo for full license text.

SOURCE
  Bot2:           https://github.com/RebirthMeow/Bot2
  Companion tool: https://github.com/RebirthMeow/daemonmap-jka
  Upstream OpenJK: https://github.com/JACoders/OpenJK
"@

$ReleaseReadme | Set-Content -LiteralPath (Join-Path $PayloadDir "README-RELEASE.txt") -Encoding UTF8

# --- Zip it up ---

$ZipPath = Join-Path $ReleaseDir "$ReleaseName.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }

Write-Host ""
Write-Host "Compressing to: $ZipPath" -ForegroundColor Green
Compress-Archive -Path "$PayloadDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal

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

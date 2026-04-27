# Bot2

This is a vibe coded project. Bot2 is a bot that works on any\* map and can kinda strafe jump. Navmesh is generated from BSP (details coming soon), and recast+detour library is added to the game, which is used for realtime pathfinding during gameplay. In addition, the bot2 performs strafe jumping inputs which are validated by pmove predictions. Also has stuff for jumppads, wallruns, elevators, basic CTF class roles logic, some shooting, etc. -Xen

## For non-technical users

Don't want to build from source? Grab the latest pre-built Windows binary from the **[Releases page](https://github.com/RebirthMeow/Bot2/releases)** — download the `Bot2-vX.Y.Z-windows.zip`, unzip it, and drop the included `bot2_jka` folder into your JKA install's `GameData\` folder. The zip ships with a working navmesh for at least one map so you can see the bot in action immediately.

You'll need:

1. A legitimate JKA install.
2. [OpenJK](https://github.com/JACoders/OpenJK/releases) installed alongside it (Bot2 is an OpenJK mod, not a standalone game engine).

Then launch `OpenJK_MP.exe`, go to **Setup → Mods → Bot2 (bot2_jka)**, click Load Mod, start a game, open the console (`~`) and type `/addbot kyle 5`. Full step-by-step is in the `README-RELEASE.txt` inside the zip.

---

*The rest of this is AI summary.*

This repository is a fork of [OpenJK](https://github.com/JACoders/OpenJK) that
adds an advanced multiplayer CTF bot AI ("Bot2") on top of the upstream
codebase.  The bot system uses Recast/Detour navmesh integration, a phantom
pmove simulator for predictive jump-arc evaluation, multi-jump chain scoring,
projectile lead computation with ballistic compensation, force-power
management, and a wallrun executor with map-scanner tooling.  Vanilla
OpenJK bots and the new bot AI coexist on the same server — the new system
only replaces the per-frame think for bots whose name ends in `_v2` (or all
bots when the `bot_forcebot2` cvar is set).

The fork's footprint inside genuinely-stock OpenJK source is intentionally
minimal so this code can be dropped into other OpenJK derivatives without
significant integration work.

**Where to look:**
- [`AGENTS.md`](AGENTS.md) — entry point for AI agents and human
  contributors; states the fork's working principles and points at the bot
  AI subsystem.
- [`codemp/game/ai_bot2_README.md`](codemp/game/ai_bot2_README.md) — full
  architecture documentation for the bot AI: file map, dependency graph,
  shared state convention, public/private API split, hooks into stock
  OpenJK files, console commands and cvars.
- [`tools/bot_test_runner.py`](tools/bot_test_runner.py) — driver script
  that launches `openjkded` across every CTF map on disk and records
  whether the bot scores a flag capture inside a per-map timeout.

**License:** This fork inherits OpenJK's [GPLv2 license](LICENSE.txt).  Any
changes are GPLv2 by virtue of being a derivative work.

## Where the navmeshes come from

Bot2 needs a `.navmesh` file for every map you want bots to play on. The
release zip ships pre-baked navmeshes for at least one demo map so end
users see the bot working out of the box. The shipped navmeshes live in
[`navmesh example files/`](navmesh%20example%20files/) at the repo root —
that folder is the single source of truth for what gets bundled in
releases. See its README for the curation convention.

To build navmeshes for other maps, use the companion tool:

**[daemonmap-jka](https://github.com/RebirthMeow/daemonmap-jka)** — offline
navmesh compiler. Drag a JKA `.bsp` onto its included `daemonmap-jka.bat`
wrapper; out comes a `.navmesh` you drop into `bot2_jka\maps\` (for your
own use) or into this repo's `navmesh example files\` folder (to ship
with the next release).

The two repos are decoupled: this repo is the playable mod, daemonmap-jka
is the asset pipeline. The contract between them is the binary `.navmesh`
format (Recast/Detour) plus an optional `.nav_connections` text sidecar
that Bot2's `/bot_scan_wallruns` command writes for daemonmap-jka to bake
wallrun connections into a second-pass navmesh. See daemonmap-jka's
`docs/nav_connections_format.md` for the full sidecar spec.

## Cutting a release

1. Make sure `build\Release\jampgamex86.dll`, `cgamex86.dll`, and
   `uix86.dll` are up to date (build the solution in Visual Studio).
2. Make sure `navmesh example files\` contains at least one `.navmesh`
   you have rights to ship. Add new ones via the daemonmap-jka workflow.
3. Run:

   ```powershell
   .\package-release.ps1 -Version v1.0.0
   ```

   The script defaults `-NavmeshDir` to `navmesh example files\` and
   produces `release\Bot2-v1.0.0-windows.zip`.
4. Upload the zip to a new release at
   <https://github.com/RebirthMeow/Bot2/releases/new>.

To ship a different curated set for a specific release (e.g. a "wallrun
demo" release with only wallrun-heavy maps), pass `-NavmeshDir` with a
path to a temp folder containing exactly the navmeshes you want.

---

## Upstream acknowledgment

This fork is built on [OpenJK](https://github.com/JACoders/OpenJK) — the
community-maintained engine and game-module reimplementation for *Star
Wars: Jedi Knight: Jedi Academy* and *Jedi Outcast*, distributed under
[GPLv2](LICENSE.txt).  Credit for the engine, the game-DLL framework, and
the platform/CMake plumbing this fork relies on belongs to the OpenJK
[maintainers and contributors](https://github.com/JACoders/OpenJK/graphs/contributors);
the bot AI added in this repository would not exist without their work.
All upstream copyright and license notices are preserved in the source
tree.

For installation guides, the official build instructions, contribution
process, and the OpenJK community's support channels, see the
[upstream README](https://github.com/JACoders/OpenJK/blob/master/README.md).

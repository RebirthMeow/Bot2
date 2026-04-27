# AGENTS.md

This repository is a fork of [OpenJK](https://github.com/JACoders/OpenJK) with
a custom AI bot subsystem (referred to internally as "Bot2") added under
`codemp/game/`. This document gives AI agents and human contributors the
context needed to work in this repo without breaking things.

## Repository layout

The upstream OpenJK source is in its original directories. The fork's
additions are concentrated in:

- `codemp/game/ai_bot2*.{c,h}` — the core Bot2 AI subsystem. Has its own
  README at `codemp/game/ai_bot2_README.md`. **Read that file before
  modifying anything under `ai_bot2_*`.**
- `codemp/game/bot2_commands.{c,h}` — bodies for the navmesh debug client
  console commands.
- `codemp/game/bot2_svcmds.{c,h}` — bodies for the bot2 server console
  commands.
- `codemp/game/g_crash_handler.{c,h}` — optional Win32 minidump handler
  extracted from `g_main.c`. Not bot2-specific.
- `codemp/game/g_navmesh.{cpp,h}` — Recast/Detour navmesh integration. Not
  bot2-specific either; bot2 uses the API but anything else can too.

The bot2 footprint inside genuinely-stock OpenJK files is now minimal:
roughly ten cmd/svcmd table-entry lines plus a handful of lifecycle hooks
in `ai_main.c` (which is itself heavily fork-modified for the navmesh
integration). The README has the full per-file inventory.

## Working principles

**Minimize upstream footprint.** This fork is intended to be drop-in friendly
for other OpenJK derivatives. When extending Bot2, prefer adding new files
under `codemp/game/` (typically `ai_bot2_*.c` or `bot2_*.c`) over modifying
stock OpenJK source. The existing hooks in stock files are deliberately
small; keep them that way. If a feature requires a new hook in a stock file,
make it a single function call to a Bot2-owned file rather than embedding
logic inline.

**Read the subsystem README before editing bot2 code.** The Bot2 subsystem
has internal structure that is not obvious from filenames alone — five source
files, a public header, and a private cross-file header, all sharing one
107-field state struct. Skipping the README will lead to changes that compile
but break invariants in another file. Five minutes of reading saves much
more.

**Trust the build loop.** The codebase uses CMake. Compiler errors will catch
function-signature drift across files. Do not skip a clean build because a
change "looks right." The bot2 subsystem in particular has cross-file calls
that are only safe if signatures match exactly.

**Grep before editing shared state.** Every file under `ai_bot2_*` reads and
writes the shared `bot2_states[]` array. Before changing the meaning or use
of a field on that struct, grep for the field name across all bot2 source
files and read every reference. The compiler will not catch semantic drift
on struct fields.

## Build

Standard OpenJK CMake build. Bot2 source files are listed in
`codemp/game/CMakeLists.txt` (search for `ai_bot2`). When adding a new bot2
source file, add it there.

## What is "Bot2"

A heavily extended CTF bot for Jedi Academy multiplayer. It replaces vanilla
bot logic for any bot whose name has a `_v2` suffix (or all bots when
`bot_forcebot2` is set). Features include Recast/Detour navmesh integration,
predictive pmove simulation for jump landings, multi-jump chain evaluation,
projectile lead computation for combat aiming, force-power management, and a
wallrun off-mesh executor with map-scanner tooling. The vanilla bot AI
(`ai_main.c` and friends) is left untouched, so vanilla bots and Bot2 bots
can coexist on the same server.

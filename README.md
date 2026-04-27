# Bot2

This is a vibe coded project. Bot2 is a strafe jumping bot that works on any\* map. Navmesh is generated from BSP (details coming soon), and recast+detour library is added to the game, which is used for realtime pathfinding during gameplay. In addition, the bot2 performs strafe jumping inputs which are validated by pmove predictions. Also has stuff for jumppads, wallruns, elevators, basic CTF class roles logic, some shooting, etc. -Xen

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

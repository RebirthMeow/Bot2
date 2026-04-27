# Bot2: Advanced CTF Bot AI

Bot2 is a custom AI bot system layered on top of OpenJK's vanilla bot
infrastructure. It replaces the per-frame think for any bot whose name ends
in `_v2` (configurable; see cvars below) and leaves stock bots untouched, so
the two can coexist on the same server.

This README is the entry point for anyone modifying or extending the Bot2
source. Read it before editing under `codemp/game/ai_bot2_*`.

## File map

The subsystem is split across two headers and five implementation files for
the core bot AI, plus three small files for the developer-facing console
commands.  Nothing exceeds ~2000 lines.

| File | Role |
|---|---|
| `ai_bot2.h` | Public API. State struct, enums, externally callable functions. Other parts of OpenJK include this file. |
| `ai_bot2_internal.h` | Private cross-file contract. Helpers shared between bot2 source files but not exposed beyond the subsystem. |
| `ai_bot2.c` | Lifecycle and orchestration: `Bot2_ClearState`, `Bot2_UpdateManagedBots`, the main `Bot2_Think` loop (macro tactics, combat dispatch, force-power management, hit/miss telemetry). |
| `ai_bot2_pmove.c` | Phantom Pmove simulator — runs `bg_pmove` with synthetic input to predict jump landings. Includes the multi-jump chain evaluator and the public `IsSafeToJump` gate. |
| `ai_bot2_combat.c` | Projectile lead computation (`Bot2_GetLeadOrigin`) and clamped angle blending (`Bot2_ApplySmoothing`). |
| `ai_bot2_wallrun.c` | Wallrun simulator (validates whether a wall is runnable), grid scanner (discovers wallruns at map-bake time and writes a navmesh sidecar file), and the developer diagnostic command. |
| `ai_bot2_movement.c` | The 8-state movement driver (`Bot2_ExecuteMovement`), floor-probing geometry helpers, and elevator detection. |
| `bot2_commands.c` / `.h` | Five sv_cheats-gated client console commands for inspecting the navmesh (`navinfo`, `navtest`, `navcheck`, `navdraw`, `navdrawoffmesh`).  Registered via name in the stock `g_cmds.c` command table. |
| `bot2_svcmds.c` / `.h` | Four developer server commands (`bot_test_routing`, `bot_wallrun_check`, `bot_scan_wallruns`, `bot_scan_wallruns_headless`).  Registered via name in the stock `g_svcmds.c` svcmd table. |

## Dependency graph

Function-call dependencies between bot2 source files. Excludes the universal
`bot2_states[]` data dependency (everything reads and writes it) and
`Bot2_PrintTelemetry` (everyone calls it for cvar-gated logging).

```
ai_bot2.c   --> combat.c     (Bot2_GetLeadOrigin)
ai_bot2.c   --> movement.c   (Bot2_ExecuteMovement)

movement.c  --> combat.c     (Bot2_ApplySmoothing — used in nearly every state)
movement.c  --> pmove.c      (SimulatePmoveTrajectory, IsSafeToJump)

pmove.c     --> movement.c   (CheckForTriggerHurt, TraceFloorScore, ...)

wallrun.c   --> pmove.c      (Bot2_PMTrace, Bot2_PMPointContents only)

combat.c    --> (leaf — no internal calls)
```

The `pmove.c <-> movement.c` cycle is real but mediated by
`ai_bot2_internal.h` — both files declare their cross-file helpers in the
internal header, so neither needs to include the other directly. If you ever
want to break the cycle: move the floor-probing helpers from movement.c into
pmove.c. They are pure trace functions and conceptually belong with physics.

## Shared state: `bot2_states[]`

A 107-field per-client struct (`bot2_state_t`) is defined in `ai_bot2.c` and
extern-declared in `ai_bot2.h`. Every bot2 source file reads and writes it
freely. This is by design but creates the tightest implicit coupling in the
subsystem.

**The most common source of bugs is changing how a state field is used in
one file without updating the other consumers.** The compiler will not catch
this. Before changing how any `bot2_states[clientNum].XYZ` field is read or
written, grep across all bot2 source files for `XYZ` and review every
reference. This is non-negotiable for state-field changes.

Logical groupings within the struct (matched by section comments in
`ai_bot2.h`):

- Macro tactics: `role`, `macroState`, `targetEntNum`, `abilityTimer`,
  `chargeTimer`, `macroTargetOrigin`.
- Micro state machine: `state`, `stateTimer`, `strafeDir`, `targetYaw`,
  `ledgeEvading`, `spawnCooldown`.
- Off-mesh traversal: `offmeshType`, `offmeshExitTime`, `tele_predPos`
  (shared landing target).
- Jumppad approach cache: `jumppadCachedEnd`, `jumppadCachedTime`,
  `jumppadEndCached`.
- Elevator state: `elevatorEntNum`, `elevatorPhase`.
- Wallrun state: `wallrunPhase` plus seven related fields including the
  retry counter and cooldown timer.
- Anti-snag engine: `stuck_pos`, `stuck_timer`, `stuck_dist_acc`,
  `unstuck_phase`, `unstuck_phase_timer`.
- Routing test harness: `test_active`, `test_variation`, `test_results[10][5]`,
  etc.
- Mid-air telemetry: ~20 `tele_*` fields capturing jump arcs.
- Aiming and humanization: `lastViewAngles`, `lastAimTime`,
  `combatIntentTime`, `combatAimHoldTime`, `tele_lastAimString`,
  `tele_lastFireTime`, `tele_lastHits`.

## Public vs private API

`ai_bot2.h` declares the surface visible to the rest of OpenJK: state struct,
enums, lifecycle (`Bot2_ClearState`, `Bot2_Think`,
`Bot2_UpdateManagedBots`), telemetry (`Bot2_PrintTelemetry`), physics
(`SimulatePmoveTrajectory`, `IsSafeToJump`), combat (`Bot2_GetLeadOrigin`),
movement (`Bot2_ExecuteMovement`), and wallrun tooling (`Bot2_WallrunCheck`,
`Bot2_ScanWallruns`, `Bot2_ScanWallrunsHeadless`). Anything outside the bot2
subsystem (e.g. `g_svcmds.c` console commands) only calls through this
header.

`ai_bot2_internal.h` declares the cross-file private contract: floor probing
helpers, pmove syscall wrappers, view-angle smoothing, wallrun simulator
entry points. Treat any change to this header as an interface change and
verify all callers compile.

## Working on this code

Quick-reference for common edit targets:

When editing `ai_bot2.c` (Bot2_Think): read `ai_bot2.h` first. Bot2_Think is
the main orchestrator and dispatches to the other files; understand the
public API before changing the call sites. The function is long (~740 lines)
because it covers macro tactics, combat dispatch, and force-power management
in sequence.

When editing `ai_bot2_pmove.c`: read `ai_bot2_internal.h` first. The
simulator is called from movement.c (via `IsSafeToJump`) and from the bot2
escape jump scanner. Signature changes ripple loudly via the compiler;
behavioural changes (what a "successful" landing means) ripple silently and
require manual verification.

When editing `ai_bot2_combat.c`: mostly self-contained — it is the leaf node
in the dependency graph. `Bot2_ApplySmoothing` is hot — it is called from
every movement state. Avoid changing its side effects on
`bot2_states[].lastViewAngles` and `lastAimTime` without updating the
movement state machine in lockstep.

When editing `ai_bot2_wallrun.c`: nearly self-contained — only depends on
the pmove syscall wrappers. The map scanners write
`maps/<mapname>.nav_connections`; coordinate with the navmesh build pipeline
if you change the file format. The wallrun simulator is sensitive to
animation-table state; see the long comment at the top of
`Bot2_SimulateWallrunScenarioEx` before changing input timing.

When editing `ai_bot2_movement.c`: by far the largest file. The 8-state
machine has significant state-field interactions; before editing a state,
identify every `bot2_states[].XYZ` field it reads and writes, and trace
those across the other files. Pay particular attention to `wallrunPhase`,
`offmeshType`, `tele_predPos`, and `state` — these are touched from multiple
states and from external callers.

## Hooks into vanilla OpenJK

Bot2's footprint inside stock OpenJK source is intentionally minimal so
this fork remains easy to drop into other OpenJK derivatives.  Every file
function bodies live in bot2-owned source; the stock files only carry the
table entries needed to register the commands and the lifecycle hooks the
bot system inherently needs.

| Stock file | Bot2 footprint | Purpose |
|---|---|---|
| `g_cmds.c` | 1 `#include`, 5 cmd-table entries | Registers `navinfo`, `navtest`, `navcheck`, `navdraw`, `navdrawoffmesh` against the bodies in `bot2_commands.c`. |
| `g_svcmds.c` | 1 `#include`, 4 svcmd-table entries | Registers `bot_test_routing`, `bot_wallrun_check`, `bot_scan_wallruns`, `bot_scan_wallruns_headless` against the bodies in `bot2_svcmds.c`. |
| `g_main.c` | 1 `#include`, 2 calls | Hooks the optional Win32 minidump crash handler (which lives in `g_crash_handler.c`). Not bot2-specific — leave it in if you want crash dumps for any DLL fault. |
| `ai_main.c` | 1 `#include`, ~6 lines | Lifecycle: defines the `bot_telemetry` cvar, registers it inside `BotAISetup`, calls `Bot2_ClearState` when a bot connects and on map restart, calls `Bot2_UpdateManagedBots` once per frame.  This is the unavoidable "plug bot2 into the engine's bot lifecycle" wiring; `ai_main.c` is itself a heavily fork-modified file (recast/detour integration etc.). |
| `ai_main.h` | 1 line | `extern vmCvar_t bot_telemetry` so the bot2 telemetry path can read it. |

The following stock files are 100% pristine — no bot2 references at all:
`g_active.c`, `g_team.c`, `g_missile.c`, `g_combat.c`, `g_bot.c`,
`g_local.h`.  This was reached by replacing earlier inline instrumentation
with internal mechanisms:

- The old [CAP] capture marker in `g_team.c` was removed; external test
  harnesses now monitor the engine's native `Exit: Capturelimit hit.` log
  line emitted by `LogExit` when `level.teamScores[X] >= capturelimit`.
- The old missile-hit-world telemetry block in `g_missile.c` was replaced
  by timeout-based miss detection inside `Bot2_Think`: when a shot fires,
  `tele_missCheckPending` is armed with `level.time + ToF + 200ms` as the
  deadline; the existing hit-detection block logs HIT on
  `accuracy_hits` increment or MISS on deadline expiry.
- The old `recordinput` console command and its 250-line input recorder
  in `g_cmds.c` were deleted entirely along with the per-tick
  `G_RecordInputFrame` callback in `g_active.c`.

## Build

Bot2 source files are listed in `codemp/game/CMakeLists.txt` (search for
`ai_bot2`). When adding a new bot2 source file, add it there. The headers
(`ai_bot2.h`, `ai_bot2_internal.h`) are also listed for IDE discoverability.

## Console commands and cvars

Bot2 exposes the following:

| Cvar | Effect |
|---|---|
| `bot_telemetry` | Bitmask: 1 = movement/jump telemetry, 2 = combat/aiming telemetry (HIT/MISS lines per shot). Both off by default. |
| `bot_forcebot2` | When nonzero, applies Bot2 logic to all bots regardless of name. Default 0. |
| `bot_forcerole` | 1 = offense, 2 = chase, 3 = base. Overrides automatic role assignment for testing. |
| `bot_forceweapon` | Locks all bots to a specific weapon enum and grants unlimited ammo. For combat testing. |

Client console commands (registered in `g_cmds.c`, bodies in `bot2_commands.c`):

| Command | Effect |
|---|---|
| `navinfo` | Prints world bounds and navmesh debug stats. |
| `navtest` | Traces from the player's view, queries the next nav waypoint, draws it. |
| `navcheck` | CTF-only: queries a full path to the enemy flag stand and draws the waypoint chain. |
| `navdraw [radius]` | Renders navmesh polys within a radius of the player. |
| `navdrawoffmesh [radius] [type ...]` | Renders off-mesh connections (drops, jumps, wallruns, elevators, jumppads). |

| Server command | Effect |
|---|---|
| `bot_test_routing <clientnum>` | Runs the 10-variation routing benchmark against the named bot. |
| `bot_wallrun_check <clientnum>` | Wallrun diagnostic. Bot must be standing facing the wall. |
| `bot_scan_wallruns [gridStep] [radius] [centerClient]` | Full or local wallrun map scan; appends to the navmesh sidecar. |
| `bot_scan_wallruns_headless [gridStep] [radius] [centerClient]` | Optimised variant that pre-filters candidate floors against the navmesh before probing. |

## Conventions for new files

If you add a new bot2 source file, prefer the prefix `ai_bot2_` and place it
in `codemp/game/`. Include `ai_bot2.h` for public types and
`ai_bot2_internal.h` if the file uses any cross-file private helpers. Add
the file to `codemp/game/CMakeLists.txt`.

If a new file would expose a function callable from outside the bot2
subsystem, declare it in `ai_bot2.h`. Otherwise, declare cross-file private
functions in `ai_bot2_internal.h` and keep file-local helpers `static`.

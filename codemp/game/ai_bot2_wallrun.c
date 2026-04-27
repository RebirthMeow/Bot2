// ==============================================================================
// ai_bot2_wallrun.c - Wallrun simulator, map scanner, and diagnostic for the
// Advanced CTF Bot.
//
//   * Bot2_SimulateWallrunScenarioEx / Scenario - the core 6-phase wallrun
//     simulator that drives bg_pmove with synthetic input and watches for the
//     wallrun trigger / animation transitions.
//   * Bot2_SimulateWallrunEx / Bot2_SimulateWallrun - convenience wrappers that
//     run the full scenario sweep against a live client and report whether the
//     bot can wallrun the surface in front of it.  Internal-header exposed so
//     the movement state machine and the WallrunCheck diagnostic both share
//     the same code path.
//   * Bot2_ScanWallruns / Bot2_ScanWallrunsHeadless - grid-based map scanner
//     that probes near-vertical walls and writes validated start->end pairs to
//     maps/<mapname>.nav_connections.
//   * Bot2_WallrunCheck - "/bot_wallrun_check <clientnum>" diagnostic command.
// ==============================================================================

#include "g_local.h"
#include "bg_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "ai_bot2_internal.h"
#include "g_navmesh.h"

#include <stdio.h>

// ==============================================================================
// Wallrun Pmove Validator
// Simulates the full 3-phase wallrun input sequence (jump -> release ->
// second-jump) through the real Pmove engine and checks whether bg_pmove
// actually triggers the wallrun animation (BOTH_FORCEWALLRUNFLIP_START).
//
// Unlike SimulatePmoveTrajectory this function:
//   - Does NOT abort on low horizontal speed (the bot naturally slows when
//     pressing into the wall face — that is expected and correct).
//   - Does NOT fatten the bounding box (inflation against a wall surface
//     causes pmove to eject the simulated bot outward, killing the test).
//   - Forces WP_SABER and FORCE_LEVEL_3, both required by bg_pmove's gate.
//   - Seeds legsAnim to BOTH_JUMP1 so the wallrun check in bg_pmove passes.
//
// Success is detected two ways (either is sufficient):
//   1. legsAnim == BOTH_FORCEWALLRUNFLIP_START after the second-jump tick.
//   2. vz spikes above 400 u/s after the second-jump (the wallrun impulse
//      is significantly larger than a normal jump's ~270 u/s), used as a
//      fallback if dummy_anims prevents PM_SetAnim from updating the index.
// ==============================================================================
// scenario 0 — natural run: hold W throughout; finds mount-ups (ALT) and natural push-off
//              (END) landings, no air-accel input in free-flight.
// scenario 1 — jump-off sweep: hold W until phase5Timer <= jumpOffThreshold, then W+jump to
//              fire PM_CheckJump's manual push-off.  jumpOffThreshold selects the push-off
//              height; the wrapper sweeps multiple values to cover the full allowed window.
//              Free-flight uses forwardmove=-127 (hold-S) to air-accel backward.
// scenario 2 — natural run + hold-S: same climbing as 0, but holds S in free-flight after
//              a natural END push-off.  Catches ledges behind the bot that scenario 0 misses.
//
// out_landPos — if non-NULL, receives the world-space landing position on PASS.
//               Unchanged on FAIL/ABORT.  Used by the map scanner to record end waypoints.
static qboolean Bot2_SimulateWallrunScenarioEx(
	const playerState_t *templatePS, // if non-NULL, memcpy from real PS then override fields
	vec3_t        seedOrigin,       // bot's standing position before the wallrun
	vec3_t        seedVelocity,     // velocity at seed time (typically {0,0,0} for scanner)
	vec3_t        bboxMins,         // player bounding box mins
	vec3_t        bboxMaxs,         // player bounding box maxs
	int           clientNum,        // used as pass-entity in traces (memset path only)
	int           commandTime,      // starting commandTime
	float         faceYaw,
	int           scenario,
	int           jumpOffThreshold,
	vec3_t        out_landPos)      // may be NULL
{
	// ---- PS setup ----
	playerState_t simPS;
	if (templatePS) {
		// Copy a real client's PS so all engine flags (force powers, saber entity refs,
		// ENTITYNUM_NONE sentinels, etc.) are initialised exactly as they are in-game.
		// Then override every field that is specific to this probe position so the
		// simulation is independent of where the template client actually stands.
		memcpy(&simPS, templatePS, sizeof(playerState_t));
		VectorCopy(seedOrigin,   simPS.origin);
		VectorCopy(seedVelocity, simPS.velocity);
		simPS.commandTime     = commandTime;
		simPS.pm_type         = PM_NORMAL;   // guard: template might be PM_DEAD etc.
		simPS.pm_flags        = 0;           // clear PMF_JUMP_HELD, PMF_DUCKED, etc.
		simPS.pm_time         = 0;           // clear movement-timer (crouch-slide, etc.)
		simPS.groundEntityNum = 0;           // touching world
		simPS.delta_angles[0] = 0;
		simPS.delta_angles[1] = 0;
		simPS.delta_angles[2] = 0;
		simPS.legsAnim        = BOTH_JUMP1;
		simPS.torsoAnim       = BOTH_JUMP1;
		simPS.legsTimer       = 0;
		simPS.torsoTimer      = 0;
		// forceJumpZStart reset is done unconditionally below
	} else {
		memset(&simPS, 0, sizeof(playerState_t));
		VectorCopy(seedOrigin,   simPS.origin);
		VectorCopy(seedVelocity, simPS.velocity);
		simPS.clientNum     = clientNum;
		simPS.commandTime   = commandTime;
		simPS.groundEntityNum = 0;         // start on ground
		simPS.pm_type       = PM_NORMAL;

		simPS.saberLockEnemy    = ENTITYNUM_NONE;
		simPS.saberEntityNum    = ENTITYNUM_NONE;
		simPS.rocketLockIndex   = ENTITYNUM_NONE;
		simPS.genericEnemyIndex = ENTITYNUM_NONE;
		simPS.lookTarget        = ENTITYNUM_NONE;

		// Wallrun gate requires saber equipped and force jump 3
		simPS.weapon        = WP_SABER;
		simPS.weaponstate   = WEAPON_READY;
		simPS.fd.forcePowerLevel[FP_LEVITATION] = FORCE_LEVEL_3;
		simPS.fd.forcePowersKnown |= (1 << FP_LEVITATION);

		// Seed the leg animation so the bg_pmove wallrun gate
		// (which checks for BOTH_JUMP1 / BOTH_INAIR1) can pass.
		simPS.legsAnim  = BOTH_JUMP1;
		simPS.torsoAnim = BOTH_JUMP1;
	}

	// ---- pmove_t setup ----
	pmove_t pm;
	memset(&pm, 0, sizeof(pmove_t));
	pm.ps            = &simPS;
	pm.trace         = Bot2_PMTrace;
	pm.pointcontents = Bot2_PMPointContents;
	pm.tracemask     = MASK_PLAYERSOLID;
	pm.watertype     = 0;
	pm.waterlevel    = 0;

	// Real bounding box — no inflation.  Fattening the box here would push
	// the simulated bot away from the wall on the very first tick.
	VectorCopy(bboxMins, pm.mins);
	VectorCopy(bboxMaxs, pm.maxs);
	pm.modelScale[0] = pm.modelScale[1] = pm.modelScale[2] = 1.0f;

	pm.baseEnt = (bgEntity_t*)g_entities;
	pm.entSize = sizeof(gentity_t);

	// Use the real humanoid animation table (bgAllAnims[0].anims) instead of a
	// zeroed dummy.  PM_SetAnim checks numFrames > 0 before accepting a transition;
	// a zeroed table makes EVERY PM_SetAnim call silently no-op, which breaks:
	//   - mount-up detection  (legsAnim never becomes BOTH_FORCEWALLRUNFLIP_ALT=874)
	//   - push-off detection  (legsAnim never becomes BOTH_FORCEWALLRUNFLIP_END=873)
	//   - sub-phase B physics (PM_AdjustAngleForWallRunUp re-fires "failed!" every tick,
	//     adding vel[2]+=200 each tick and permanently cancelling gravity)
	// bgAllAnims is extern-declared in bg_public.h and populated during level load.
	pm.animations = bgAllAnims[0].anims;
	pm.ghoul2     = NULL;

	int pmove_fixed = trap->Cvar_VariableIntegerValue("pmove_fixed");
	int sv_fps      = trap->Cvar_VariableIntegerValue("sv_fps");
	if (sv_fps <= 0) sv_fps = 20;
	pm.pmove_msec  = pmove_fixed
	    ? max(1, trap->Cvar_VariableIntegerValue("pmove_msec"))
	    : (1000 / sv_fps);
	if (pm.pmove_msec <= 0) pm.pmove_msec = 25;
	pm.pmove_fixed = pmove_fixed;

	// ---- cmd: always face the wall, forwardmove=127 ----
	usercmd_t cmd;
	memset(&cmd, 0, sizeof(usercmd_t));
	cmd.weapon      = WP_SABER;
	cmd.forwardmove = 127;
	cmd.angles[YAW] = ANGLE2SHORT(faceYaw);
	cmd.angles[PITCH] = 0;
	cmd.serverTime  = simPS.commandTime;

	// ---- Reset forceJumpZStart to the simulation origin ----
	// The real playerState may carry a stale forceJumpZStart from a previous
	// jump the player did before the check command ran.  bg_pmove's height-limit
	// gate for wall grabs uses (origin.z - forceJumpZStart); a stale high value
	// can exhaust the budget before the trigger tick fires.
	// The wallrun trigger itself does NOT check this limit, but resetting it is
	// cheap insurance and keeps the physics consistent.
	simPS.fd.forceJumpZStart = simPS.origin[2];

	// Record starting Z so the landing check can compare against it.
	float startZ = simPS.origin[2];

	// ---- scenario flag ----
	// scenario 0: hold W throughout — detects mount-ups (ALT) and natural push-offs (END)
	//             with no air-accel input in free-flight.
	// scenario 1: hold W until last climbing tick, then W+jump — fires PM_CheckJump's manual
	//             push-off (vel[XY]*=0.5, VectorMA(-300*fwd), vel[Z]+=200).  Free-flight uses
	//             forwardmove=-127 (hold-S) to air-accel backward toward ledges behind the bot.
	// scenario 2: hold W throughout (same climbing as 0) — natural push-off (END) then
	//             forwardmove=-127 in free-flight.  Catches ledges reachable only by natural
	//             timer-expire push-off + holding S that scenario 0 (no input) misses.
	//             No effect on ALT mount-ups (forwardmove stays 0 after vault).
	const qboolean doJumpOff     = (scenario == 1);
	const qboolean doNaturalHoldS = (scenario == 2);

	// ---- phase state ----
	// 1 = hold jump until airborne
	// 2 = release jump until PMF_JUMP_HELD clears
	// 3 = press jump one tick (second jump — wallrun trigger)
	// 4 = observe result for up to 4 ticks
	// 5a = climbing  (BOTH_FORCEWALLRUNFLIP_START, timer counts down)
	// 5b = free flight (after mount-up or push-off; wait for landing)
	int phase             = 1;
	int phase4Start       = -1;
	int phase5Start       = -1;
	int phase5Timer       = -1;  // ms remaining in wallrun animation; counts down each tick
	qboolean phase5Airborne  = qfalse;
	qboolean phase5Climbing  = qtrue;
	qboolean phase5WasPushOff = qfalse; // true if free-flight was entered via END push-off (not ALT mount-up)
	int sim_time          = simPS.commandTime;
	const int MAX_TICKS   = 200;

	for (int i = 0; i < MAX_TICKS; i++) {
		sim_time += pm.pmove_msec;
		cmd.serverTime = sim_time;

		qboolean grounded = (simPS.groundEntityNum != ENTITYNUM_NONE);
		qboolean jumpHeld = (simPS.pm_flags & PMF_JUMP_HELD) != 0;

		// ---- Phase 5: climb then land ----
		//
		// Sub-phase A (phase5Climbing=true):
		//   Re-seed legsAnim=BOTH_FORCEWALLRUNFLIP_START so PM_AdjustAngleForWallRunUp
		//   runs each tick and applies velocity[2]=300.  We drive our own phase5Timer
		//   (initialised from BG_AnimLength at phase 5 entry) and set simPS.legsTimer
		//   from it so the animation expires at the correct real-world time.
		//   - BOTH_FORCEWALLRUNFLIP_ALT → wall top reached; enter free-flight (sub-phase B)
		//   - BOTH_FORCEWALLRUNFLIP_END OR phase5Timer==0 → push-off/ceiling; check Z now
		//
		// Sub-phase B (phase5Climbing=false):
		//   After ALT mount-up: bot vaulted over wall top, flying forward and up.
		//   After END push-off: bot launched away from wall.
		//   In both cases: wait for grounded, then check landZ > startZ + 32.
		if (phase == 5) {
			Bot2_PrintTelemetry(1, "[WR-SIM] p5 tick=%-3d z=%6.0f vz=%5.0f  climb=%d  grnd=%d  anim=%d  timer=%d\n",
				i - phase5Start,
				simPS.origin[2], simPS.velocity[2],
				phase5Climbing ? 1 : 0,
				grounded ? 1 : 0,
				simPS.legsAnim,
				simPS.legsTimer);

			if (phase5Climbing) {
				// Drive our own phase5Timer so push-off timing is independent of
				// whatever legsTimer the engine leaves behind after each Pmove tick.
				phase5Timer -= pm.pmove_msec;
				if (phase5Timer < 0) phase5Timer = 0;

				// ORDERING INVARIANT: re-seed legsAnim=START *before* Pmove but *after*
				// the post-Pmove END/ALT check below.  Because real anim data makes
				// PM_SetAnim functional, Pmove will happily transition legsAnim to other
				// animations (landing, idle, etc.) during the climb.  Re-seeding forces
				// PM_AdjustAngleForWallRunUp to run every tick.  Moving this re-seed
				// above the END/ALT check would clobber a real transition before we
				// detect it — breaking both push-off and mount-up tracking.
				simPS.legsAnim  = BOTH_FORCEWALLRUNFLIP_START;
				simPS.torsoAnim = BOTH_FORCEWALLRUNFLIP_START;
				simPS.legsTimer  = phase5Timer;
				simPS.torsoTimer = phase5Timer;

				// Scenario 1: press jump when phase5Timer drops to <= jumpOffThreshold.
				// This fires PM_CheckJump's manual push-off:
				//   vel[XY]*=0.5; VectorMA(vel,-300,fwd,vel); vel[2]+=200.
				// jumpOffThreshold is chosen well above 0 so simPS.legsTimer is non-zero
				// when the tick fires (legsTimer=0 would gate off PM_AdjustAngleForWallRunUp's
				// climbing branch before PM_CheckJump runs, silently killing the push-off).
				// PM_CheckJump's guard is legsTimer < animLen-400 (~950ms); all thresholds
				// the wrapper uses (≤900ms) satisfy this.
				//
				// Key: on a wall shorter than the full anim (wall ends before timer expires),
				// the engine fires natural END when the 128u forward trace clears the wall top.
				// If that happens before phase5Timer reaches jumpOffThreshold, push-off fires
				// via the END/timer==0 branch below instead — and this branch never runs.
				// The wrapper sweeps multiple thresholds to catch the push-off at whichever
				// height the wall actually terminates.
				if (doJumpOff && phase5Timer <= jumpOffThreshold && phase5Timer > 0) {
					cmd.upmove = 127; // fire manual push-off via PM_CheckJump
					Bot2_PrintTelemetry(1, "[WR-SIM] p5 s1: W+jump input z=%.0f timer=%d\n",
						simPS.origin[2], phase5Timer);
				} else {
					cmd.upmove = 0;
				}
				cmd.forwardmove = 127; // required by PM_AdjustAngleForWallRunUp
				pm.cmd = cmd;
				Pmove(&pm);

				if (simPS.legsAnim == BOTH_FORCEWALLRUNFLIP_ALT) {
					// Wall top reached → enter free-flight to landing
					Bot2_PrintTelemetry(1, "[WR-SIM] p5 mount-up tick=%d z=%.0f\n",
						i - phase5Start, simPS.origin[2]);
					phase5Climbing = qfalse;

				} else if (simPS.legsAnim == BOTH_FORCEWALLRUNFLIP_END || phase5Timer == 0) {
					// Push-off fired (timer expired or ceiling blocked).
					float pushZ = simPS.origin[2];
					Bot2_PrintTelemetry(1, "[WR-SIM] p5 pushoff tick=%d z=%.0f startZ=%.0f delta=%.0f\n",
						i - phase5Start, pushZ, startZ, pushZ - startZ);

					// Mount-up geometry check — run the same two-trace logic that
					// PM_AdjustAngleForWallRunUp uses for ALT, but from the push-off
					// position.  ALT can miss by a few ticks when legsTimer hits the
					// <=200 gate just before the bot physically clears the wall top:
					// vz drops to -20 (gravity only), the bot hovers a few units below
					// the top, trace.fraction never exceeds 0.5, and "failed!" fires
					// instead.  At push-off the bot is at its highest climbing point;
					// if a mountable surface exists here the wall is a valid mount-up.
					//
					// Scenario 1 tests a *backward* jump-off trajectory; a forward-mount
					// surface at push-off height is irrelevant there and must not trigger
					// a false PASS.
					if (!doJumpOff)
					{
						vec3_t fwdAngles = {0, simPS.viewangles[YAW], 0};
						vec3_t fwd;
						AngleVectors(fwdAngles, fwd, NULL, NULL);

						// Trace 1: forward 128u — is there open space in front?
						vec3_t traceTo;
						VectorMA(simPS.origin, 128.0f, fwd, traceTo);
						trace_t wallTr;
						// Use the same slim box PM_AdjustAngleForWallRunUp uses
						vec3_t trMins = {-15,-15,0}, trMaxs = {15,15,24};
						trap->Trace(&wallTr, simPS.origin, trMins, trMaxs, traceTo,
							simPS.clientNum, MASK_PLAYERSOLID, qfalse, 0, 0);

						if (wallTr.fraction > 0.5f) {
							// Trace 2: down 64u from above the forward endpoint
							//   top   = endpos + (|mins[2]| + 4) = endpos + 28
							//   bottom= top - 64
							vec3_t top, bottom;
							VectorCopy(wallTr.endpos, top);
							top[2] += (-pm.mins[2]) + 4.0f; // 24 + 4 = 28
							VectorCopy(top, bottom);
							bottom[2] -= 64.0f;
							trace_t floorTr;
							trap->Trace(&floorTr, top, pm.mins, pm.maxs, bottom,
								simPS.clientNum, MASK_PLAYERSOLID, qfalse, 0, 0);
							if (!floorTr.allsolid && !floorTr.startsolid
								&& floorTr.fraction < 1.0f
								&& floorTr.plane.normal[2] > 0.7f)
							{
								float mountZ = floorTr.endpos[2];
								Bot2_PrintTelemetry(1,
									"[WR-SIM] p5 mount-up (geo) z=%.0f mountZ=%.0f startZ=%.0f\n",
									pushZ, mountZ, startZ);
								if (mountZ > startZ + 32.0f) {
									Bot2_PrintTelemetry(1, "[WR-SIM] PASS (mount-up geo)\n");
									if (out_landPos) VectorCopy(floorTr.endpos, out_landPos);
									return qtrue;
								}
							}
						}
					}

					// No mountable surface at push-off height — enter free-flight and
					// wait for the bot to actually land somewhere.
					phase5Climbing  = qfalse;
					phase5Airborne  = qtrue;
					phase5WasPushOff = qtrue;

					// With real anim data, PM_SetAnim(BOTH_FORCEWALLRUNFLIP_END) already
					// succeeded inside Pmove and legsAnim is now 873.  PM_AdjustAngleForWallRunUp
					// checks for legsAnim==872, so it will NOT re-trigger in sub-phase B.
					// This explicit override is a belt-and-suspenders safety net.
					simPS.legsAnim  = BOTH_FORCEWALLRUNFLIP_END;
					simPS.torsoAnim = BOTH_FORCEWALLRUNFLIP_END;
					simPS.legsTimer  = 0;
					simPS.torsoTimer = 0;
				}
				// Still climbing — continue to next tick

			} else {
				// Sub-phase B: free flight after mount-up OR push-off.
				// phase5Airborne is pre-set to qtrue on push-off entry so the
				// landing check fires on the very first grounded tick.
				if (!grounded) phase5Airborne = qtrue;
				if (grounded && phase5Airborne) {
					float landZ = simPS.origin[2];
					Bot2_PrintTelemetry(1, "[WR-SIM] p5 landed tick=%d landZ=%.0f startZ=%.0f delta=%.0f\n",
						i - phase5Start, landZ, startZ, landZ - startZ);
					if (landZ > startZ + 32.0f) {
						Bot2_PrintTelemetry(1, "[WR-SIM] PASS (landing) landZ=%.0f > startZ+32=%.0f\n",
							landZ, startZ + 32.0f);
						if (out_landPos) VectorCopy(simPS.origin, out_landPos);
						return qtrue;
					} else {
						Bot2_PrintTelemetry(1, "[WR-SIM] FAIL (landing) landZ=%.0f not above startZ+32=%.0f\n",
							landZ, startZ + 32.0f);
						return qfalse;
					}
				}
				if (i >= phase5Start + 160) {
					Bot2_PrintTelemetry(1, "[WR-SIM] ABORT p5: timeout in free flight z=%.0f\n",
						simPS.origin[2]);
					return qfalse;
				}
				// Free-flight input:
				//   scenario 0 — no input; landing zone driven purely by push-off velocity.
				//   scenario 1 — hold S (-127) throughout; manual push-off is backward so
				//                S air-accels in the same direction to extend reach.
				//   scenario 2 — hold S only after a natural END push-off (phase5WasPushOff);
				//                after an ALT mount-up use no input (bot is vaulting forward).
				cmd.upmove = 0;
				cmd.forwardmove = (doJumpOff || (doNaturalHoldS && phase5WasPushOff)) ? -127 : 0;
				pm.cmd = cmd;
				Pmove(&pm);
			}

			continue;
		}

		switch (phase) {
		case 1: // Hold jump until we leave the ground
			cmd.upmove = 127;
			if (!grounded) phase = 2;
			// Safety: if we haven't left the ground in 10 ticks, abort
			if (i >= 10) {
				Bot2_PrintTelemetry(1, "[WR-SIM] ABORT p1: still grounded after %d ticks\n", i);
				return qfalse;
			}
			break;

		case 2: // Release jump, wait for PMF_JUMP_HELD to clear
			cmd.upmove = 0;
			if (!jumpHeld) phase = 3;
			// Safety: if flag won't clear in 25 ticks, abort
			if (i >= 25) {
				Bot2_PrintTelemetry(1, "[WR-SIM] ABORT p2: PMF_JUMP_HELD stuck after %d ticks\n", i);
				return qfalse;
			}
			break;

		case 3: // Second jump — one tick, this is the wallrun trigger
			cmd.upmove  = 127;
			phase       = 4;
			phase4Start = i + 1;
			Bot2_PrintTelemetry(1, "[WR-SIM] p3 trigger tick=%d vz=%.0f groundDist=? legsAnim=%d\n",
				i, simPS.velocity[2], simPS.legsAnim);
			break;

		case 4: // Observe
			cmd.upmove = 0;
			// Abort after 4 observation ticks — if it hasn't triggered by now it won't
			if (i >= phase4Start + 4) {
				Bot2_PrintTelemetry(1, "[WR-SIM] ABORT p4: no trigger in 4 obs ticks vz=%.0f legsAnim=%d\n",
					simPS.velocity[2], simPS.legsAnim);
				return qfalse;
			}
			break;
		}

		// Re-seed the jump animation before every Pmove tick through the trigger phase.
		// bg_pmove's wallrun gate requires legsAnim == BOTH_JUMP1 or BOTH_INAIR1; without
		// this seed the previous Pmove's PM_SetAnim transitions (now live with real anim
		// data) can put the bot into other animations that fail the wallrun check.
		if (phase <= 4) {
			simPS.legsAnim  = BOTH_JUMP1;
			simPS.torsoAnim = BOTH_JUMP1;
		}

		pm.cmd = cmd;
		Pmove(&pm);

		if (phase < 4) continue; // Only check for success during observation

		// Diagnostic: log every observation tick so failures are easy to diagnose.
		Bot2_PrintTelemetry(1, "[WR-SIM] obs tick=%d vz=%.0f legsAnim=%d\n",
			i, simPS.velocity[2], simPS.legsAnim);

		// Success signal 1: animation was set to the wallrun start anim.
		// OpenJK stores legsAnim as a plain index (no ANIM_TOGGLEBIT packing —
		// flip is tracked separately via legsFlip), so compare directly.
		if (simPS.legsAnim == BOTH_FORCEWALLRUNFLIP_START) {
			Bot2_PrintTelemetry(1, "[WR-SIM] wallrun fired (anim) tick=%d vz=%.0f — climbing...\n",
				i, simPS.velocity[2]);
			phase = 5;
			phase5Start = i;
			phase5Timer = BG_AnimLength(0, (animNumber_t)BOTH_FORCEWALLRUNFLIP_START);
			if (phase5Timer <= 0) phase5Timer = 1400; // fallback: measured real wallrun = ~1350 ms
			continue;
		}

		// Success signal 2: vz spike from the wallrun impulse.
		// forceJumpStrength[FORCE_LEVEL_3] / 2.0f = 420 u/s.
		// Within the same Pmove tick, gravity reduces this by at most
		// gravity × pmove_msec (worst case: 800 × 0.05 = 40 at sv_fps 20).
		// Post-gravity minimum = 420 - 40 = 380.  We use 350 as the threshold
		// to give a comfortable margin above both the normal-jump apex (~270)
		// and any force-jump sustain boost (~350), while still staying well
		// below the minimum wallrun vz.
		if (simPS.velocity[2] > 350.0f) {
			Bot2_PrintTelemetry(1, "[WR-SIM] wallrun fired (vz-spike) tick=%d vz=%.0f — climbing...\n",
				i, simPS.velocity[2]);
			phase = 5;
			phase5Start = i;
			phase5Timer = BG_AnimLength(0, (animNumber_t)BOTH_FORCEWALLRUNFLIP_START);
			if (phase5Timer <= 0) phase5Timer = 1400; // fallback: measured real wallrun = ~1350 ms
			continue;
		}
	}

	Bot2_PrintTelemetry(1, "[WR-SIM] FAIL: no wallrun trigger detected\n");
	return qfalse;
}

// Entity-based thin wrapper — extracts seed data from a live entity and forwards to Ex.
// Used by the live wallrun pre-check path.
static qboolean Bot2_SimulateWallrunScenario(gentity_t *ent, float faceYaw, int scenario, int jumpOffThreshold) {
	if (!ent || !ent->inuse || !ent->client) return qfalse;
	return Bot2_SimulateWallrunScenarioEx(
		&ent->client->ps,
		ent->client->ps.origin,
		ent->client->ps.velocity,
		ent->r.mins,
		ent->r.maxs,
		ent->client->ps.clientNum,
		ent->client->ps.commandTime,
		faceYaw, scenario, jumpOffThreshold, NULL);
}

// Runs all wallrun scenarios; returns qtrue if any produces a valid landing.
// out_landPos (may be NULL): receives the world-space landing position on PASS.
//
//   Scenario 0 — hold W, no air input: mount-up (ALT) + natural push-off (END, no S).
//
//   Scenario 1 sweep — W then W+jump: fires manual push-off (PM_CheckJump) at 5 different
//     heights by varying when the jump input is injected (jumpOffThreshold ms remaining).
//     Thresholds span the allowed push-off window (legsTimer < animLen-400 ≈ 950ms) at
//     200ms intervals.  A real player picks their moment based on target height; the sweep
//     approximates that without knowing the target in advance.  Each run is a full fresh
//     Pmove simulation; pass on the first threshold that produces a landing above startZ+32.
//
//   Scenario 2 — hold W, hold-S in free-flight after natural END: catches ledges reachable
//     by natural timer-expire push-off + backing up that scenario 0 (no input) misses.
qboolean Bot2_SimulateWallrunEx(gentity_t *ent, float faceYaw, vec3_t out_landPos) {
	if (!ent || !ent->inuse || !ent->client) return qfalse;
	vec3_t *lp = out_landPos;

	const playerState_t *tps = &ent->client->ps;

	Bot2_PrintTelemetry(1, "[WR-SIM] --- scenario 0: W-hold (mount-up / natural push-off, no air input) ---\n");
	if (Bot2_SimulateWallrunScenarioEx(tps,
		ent->client->ps.origin, ent->client->ps.velocity,
		ent->r.mins, ent->r.maxs, ent->client->ps.clientNum, ent->client->ps.commandTime,
		faceYaw, 0, 0, lp)) return qtrue;

	// Scenario 1 sweep: try manual push-off at 5 heights across the valid window.
	// animLen ≈ 1350ms, window opens at animLen-400 ≈ 950ms.
	// Thresholds (ms remaining when jump fires): 900, 700, 500, 300, 100.
	static const int jumpThresholds[] = { 900, 700, 500, 300, 100 };
	for (int t = 0; t < (int)(sizeof(jumpThresholds) / sizeof(jumpThresholds[0])); t++) {
		Bot2_PrintTelemetry(1, "[WR-SIM] --- scenario 1 (threshold=%dms): W+jump at timer<=%d ---\n",
			jumpThresholds[t], jumpThresholds[t]);
		if (Bot2_SimulateWallrunScenarioEx(tps,
			ent->client->ps.origin, ent->client->ps.velocity,
			ent->r.mins, ent->r.maxs, ent->client->ps.clientNum, ent->client->ps.commandTime,
			faceYaw, 1, jumpThresholds[t], lp)) return qtrue;
	}

	Bot2_PrintTelemetry(1, "[WR-SIM] --- scenario 2: W-hold (natural push-off + hold-S) ---\n");
	return Bot2_SimulateWallrunScenarioEx(tps,
		ent->client->ps.origin, ent->client->ps.velocity,
		ent->r.mins, ent->r.maxs, ent->client->ps.clientNum, ent->client->ps.commandTime,
		faceYaw, 2, 0, lp);
}

// Convenience wrapper for the live pre-check path (doesn't need the landing position).
qboolean Bot2_SimulateWallrun(gentity_t *ent, float faceYaw) {
	return Bot2_SimulateWallrunEx(ent, faceYaw, NULL);
}

// ==============================================================================
// Wallrun Map Scanner
//
// Sweeps a grid at a configurable step, probing for near-vertical walls at each
// ground-level sample point, and runs the full wallrun simulation against every
// candidate.  Valid connections are appended to:
//   maps/<mapname>.nav_connections
// in a whitespace-delimited text format readable by the navmesh builder:
//   WALLRUN  sx sy sz  ex ey ez  bidir  radius  area
//
// Usage:
//   bot_scan_wallruns                    — full map, 96u grid
//   bot_scan_wallruns 64                 — finer grid, full map
//   bot_scan_wallruns 96 1024 0          — 1024u radius around client 0
//   bot_scan_wallruns 64 512  1          — 512u radius around client 1
//
// radius=0 scans the full map.  radius>0 restricts the grid to a square AABB
// centred on that client's current position — useful for iteratively scanning
// specific walls without waiting for the whole map.
//
// Results are APPENDED so you can run multiple local scans and accumulate them.
// One-time bake operation — rebuild the navmesh after scanning.
// ==============================================================================
#define WALLRUN_SCAN_PROBE_DIRS   16     // horizontal probe directions per ground point
#define WALLRUN_SCAN_WALL_MAXDIST 256.0f // furthest a wall we bother probing for
#define WALLRUN_SCAN_NORMAL_Z_MAX 0.3f   // |normal.z| threshold for "near-vertical"
#define WALLRUN_OFFMESH_RADIUS    48.0f  // activation radius written to sidecar
#define WALLRUN_OFFMESH_AREA      4      // OFFMESH_AREA_WALLRUN
#define WALLRUN_SCAN_MAX_FLOORS   8

// Standard humanoid player bbox — used when synthesising start positions for the scanner.
static const vec3_t s_playerMins = { -15.0f, -15.0f, -24.0f };
static const vec3_t s_playerMaxs = {  15.0f,  15.0f,  32.0f };

// WallrunConnection — one validated start→end pair collected by the scanner.
typedef struct {
	vec3_t start;   // bot's ground position before the wallrun
	vec3_t end;     // landing position after the wallrun
} WallrunConnection;

void Bot2_ScanWallruns(int gridStep, float radius, int centerClient) {
	if (gridStep <= 0) gridStep = 96;

	// ---- scan bounds from navmesh ----
	vec3_t wmin, wmax;
	if (!NavMesh_GetBounds(wmin, wmax)) {
		trap->Print("[WR-SCAN] ERROR: navmesh not loaded — cannot determine map bounds.\n");
		return;
	}

	if (radius > 0.0f
		&& centerClient >= 0 && centerClient < MAX_CLIENTS
		&& g_entities[centerClient].inuse
		&& g_entities[centerClient].client)
	{
		float *org = g_entities[centerClient].client->ps.origin;
		wmin[0] = org[0] - radius;  wmax[0] = org[0] + radius;
		wmin[1] = org[1] - radius;  wmax[1] = org[1] + radius;
		// Z: keep full navmesh Z extent so downward traces find all floors in the column
		trap->Print("[WR-SCAN] Local scan: center=(%.0f %.0f %.0f) radius=%.0f gridStep=%d\n",
			org[0], org[1], org[2], radius, gridStep);
	} else {
		trap->Print("[WR-SCAN] Full-map scan: bounds (%.0f %.0f %.0f)–(%.0f %.0f %.0f)  gridStep=%d\n",
			wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2], gridStep);
	}

	// ---- allocate result buffer ----
	// Upper bound: every grid cell has up to WALLRUN_SCAN_MAX_FLOORS floor levels,
	// each producing at most PROBE_DIRS connections.
	int gridW = (int)((wmax[0] - wmin[0]) / gridStep) + 1;
	int gridD = (int)((wmax[1] - wmin[1]) / gridStep) + 1;
	int maxCons = gridW * gridD * WALLRUN_SCAN_MAX_FLOORS * WALLRUN_SCAN_PROBE_DIRS;
	WallrunConnection *cons = (WallrunConnection *)malloc(maxCons * sizeof(WallrunConnection));
	if (!cons) {
		trap->Print("[WR-SCAN] ERROR: out of memory allocating result buffer.\n");
		return;
	}
	int conCount   = 0;
	int simCount   = 0;
	int passCount  = 0;

	// Find a connected client to borrow a real playerState_t from.
	// The wallrun trigger in bg_pmove checks numerous flags that are initialised
	// correctly only in a live client's PS (force-power flags, saber entity refs,
	// ENTITYNUM_NONE sentinels, etc.).  A memset-zeroed PS can miss the trigger
	// even when the wall geometry is valid.  We copy the template and override
	// every position-specific field for each probe point.
	const playerState_t *templatePS = NULL;
	int tracePass = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client) {
			templatePS = &g_entities[i].client->ps;
			tracePass  = i;
			break;
		}
	}
	if (!templatePS) {
		trap->Print("[WR-SCAN] ERROR: no connected client — need at least one player in-game to borrow a PS template.\n");
		free(cons);
		return;
	}
	trap->Print("[WR-SCAN] Using client %d as PS template.\n", tracePass);

	// ---- grid sweep ----
	int colsDone    = 0;
	int colsTotal   = gridW * gridD;
	int lastPct     = -1;
	int floorSamples   = 0;  // total walkable floors found across all columns
	int floorFiltered  = 0;  // floors rejected by NavMesh_IsPointOnMesh (pre-sim filter)
	int floorLevel[WALLRUN_SCAN_MAX_FLOORS]; // how many walkable floors found at each depth
	memset(floorLevel, 0, sizeof(floorLevel));

	for (float gx = wmin[0]; gx <= wmax[0]; gx += gridStep) {
		for (float gy = wmin[1]; gy <= wmax[1]; gy += gridStep) {
			colsDone++;
			int pct = (colsDone * 100) / (colsTotal > 0 ? colsTotal : 1);
			if (pct / 10 != lastPct / 10) { // print every 10%
				lastPct = pct;
				trap->Print("[WR-SCAN] %3d%%  sims=%-5d  pass=%-4d  unique=%d\n",
					pct, simCount, passCount, conCount);
			}
			// Multi-level floor scan: trace downward from the top, step below each
			// floor found, and repeat until we hit the bottom or the floor limit.
			float scanTop = wmax[2] + 32.0f;
			const float scanBot = wmin[2] - 32.0f;

			for (int flv = 0; flv < WALLRUN_SCAN_MAX_FLOORS && scanTop > scanBot; flv++) {
				vec3_t traceFrom = { gx, gy, scanTop };
				vec3_t traceTo   = { gx, gy, scanBot };
				trace_t groundTr;
				trap->Trace(&groundTr, traceFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					traceTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (groundTr.fraction >= 1.0f)
					break; // no ground below scanTop in this column

				if (groundTr.startsolid) {
					// scanTop landed inside a floor brush — walk down with PointContents
					// until we exit, then retry the bbox trace at that depth.
					const float SOLID_STEP  = 32.0f;
					const int   MAX_SOLID_STEPS = 512;
					float probeZ    = scanTop;
					int   solidSteps = 0;
					while (probeZ > scanBot && solidSteps < MAX_SOLID_STEPS) {
						probeZ -= SOLID_STEP;
						solidSteps++;
						vec3_t pt = { gx, gy, probeZ };
						if (!(trap->PointContents(pt, tracePass) & CONTENTS_SOLID))
							break;
					}
					if (probeZ <= scanBot || solidSteps >= MAX_SOLID_STEPS)
						break;
					scanTop = probeZ - 32.0f;
					flv--;
					continue;
				}

				if (groundTr.allsolid)
					break;

				scanTop = groundTr.endpos[2] - 64.0f;

				if (groundTr.plane.normal[2] < 0.7f)
					continue; // steep slope — not walkable, keep looking below

			floorSamples++;
			floorLevel[flv < WALLRUN_SCAN_MAX_FLOORS ? flv : WALLRUN_SCAN_MAX_FLOORS-1]++;

			// Bot stands with feet at groundTr.endpos.
			vec3_t groundPos;
			VectorCopy(groundTr.endpos, groundPos);

			// Probe horizontally in WALLRUN_SCAN_PROBE_DIRS directions.
			for (int d = 0; d < WALLRUN_SCAN_PROBE_DIRS; d++) {
				float probeYaw = d * (360.0f / WALLRUN_SCAN_PROBE_DIRS);
				vec3_t probeAngles = { 0, probeYaw, 0 };
				vec3_t probeFwd;
				AngleVectors(probeAngles, probeFwd, NULL, NULL);

				vec3_t eyePos;
				VectorCopy(groundPos, eyePos);
				eyePos[2] += 28.0f;

				vec3_t wallProbeEnd;
				VectorMA(eyePos, WALLRUN_SCAN_WALL_MAXDIST, probeFwd, wallProbeEnd);

				trace_t wallTr;
				trap->Trace(&wallTr, eyePos, NULL, NULL, wallProbeEnd,
					tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (wallTr.fraction >= 1.0f) continue;

				float normalZ = fabsf(wallTr.plane.normal[2]);
				if (normalZ > WALLRUN_SCAN_NORMAL_Z_MAX) continue; // not vertical enough

				// ---- Compute flush simulation start position ----
				vec3_t flushXY;
				VectorMA(wallTr.endpos, 16.0f, wallTr.plane.normal, flushXY);

				vec3_t flushGroundFrom = { flushXY[0], flushXY[1], flushXY[2] + 64.0f };
				vec3_t flushGroundTo   = { flushXY[0], flushXY[1], flushXY[2] - 128.0f };
				trace_t flushGnd;
				trap->Trace(&flushGnd, flushGroundFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					flushGroundTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (flushGnd.allsolid || flushGnd.startsolid || flushGnd.fraction >= 1.0f)
					continue;
				if (flushGnd.plane.normal[2] < 0.7f)
					continue;

				vec3_t simStart;
				VectorCopy(flushGnd.endpos, simStart);

				if (!NavMesh_IsPointOnMesh(simStart)) { floorFiltered++; continue; }

				float faceYaw = probeYaw;

				simCount++;

				// ---- Run full scenario sweep from flush position ----
				vec3_t landPos = { 0, 0, 0 };
				vec3_t zeroVel = { 0, 0, 0 };

				qboolean passed = Bot2_SimulateWallrunScenarioEx(
					templatePS,
					simStart, zeroVel,
					(float*)s_playerMins, (float*)s_playerMaxs,
					tracePass, level.time,
					faceYaw, 0, 0, landPos);

				if (!passed) {
					static const int thresholds[] = { 900, 700, 500, 300, 100 };
					for (int t = 0; t < 5 && !passed; t++) {
						passed = Bot2_SimulateWallrunScenarioEx(
							templatePS,
							simStart, zeroVel,
							(float*)s_playerMins, (float*)s_playerMaxs,
							tracePass, level.time,
							faceYaw, 1, thresholds[t], landPos);
					}
				}
				if (!passed) {
					passed = Bot2_SimulateWallrunScenarioEx(
						templatePS,
						simStart, zeroVel,
						(float*)s_playerMins, (float*)s_playerMaxs,
						tracePass, level.time,
						faceYaw, 2, 0, landPos);
				}

				if (!passed) continue;

				if (!NavMesh_IsPointOnMesh(landPos)) continue;

				passCount++;

				qboolean dup = qfalse;
				for (int c = 0; c < conCount; c++) {
					vec3_t ds, de;
					VectorSubtract(cons[c].start, simStart, ds);
					VectorSubtract(cons[c].end,   landPos,  de);
					if (DotProduct(ds, ds) < 32.0f*32.0f &&
						DotProduct(de, de) < 32.0f*32.0f) {
						dup = qtrue;
						break;
					}
				}
				if (dup) continue;

				if (conCount < maxCons) {
					VectorCopy(simStart, cons[conCount].start);
					VectorCopy(landPos,  cons[conCount].end);
					conCount++;
				}
			} // end probe directions loop (d)
			} // end floor levels loop (flv)
		}
	}

	trap->Print("[WR-SCAN] Grid sweep done. simCount=%d  passCount=%d  unique=%d\n",
		simCount, passCount, conCount);
	trap->Print("[WR-SCAN] Floor levels sampled: total=%d  navmesh-filtered=%d\n",
		floorSamples, floorFiltered);
	trap->Print("[WR-SCAN] Floors by depth: lvl0=%d lvl1=%d lvl2=%d lvl3=%d lvl4=%d+\n",
		floorLevel[0], floorLevel[1], floorLevel[2], floorLevel[3],
		floorLevel[4]+floorLevel[5]+floorLevel[6]+floorLevel[7]);

	if (conCount == 0) {
		trap->Print("[WR-SCAN] No valid wallruns found — sidecar file not written.\n");
		free(cons);
		return;
	}

	// ---- write sidecar file ----
	char mapname[MAX_QPATH];
	trap->Cvar_VariableStringBuffer("mapname", mapname, sizeof(mapname));
	char path[MAX_QPATH];
	Com_sprintf(path, sizeof(path), "maps/%s.nav_connections", mapname);

	fileHandle_t f;
	fileHandle_t fCheck;
	int existing = trap->FS_Open(path, &fCheck, FS_READ);
	qboolean needHeader = (existing <= 0);
	if (fCheck) trap->FS_Close(fCheck);

	trap->FS_Open(path, &f, FS_APPEND);
	if (!f) {
		trap->Print("[WR-SCAN] ERROR: could not open '%s' for writing.\n", path);
		free(cons);
		return;
	}

	char line[256];
	if (needHeader) {
		Com_sprintf(line, sizeof(line),
			"# Auto-generated by Bot2_ScanWallruns — map: %s\n"
			"# type      sx       sy       sz       ex       ey       ez      bidir  radius  area\n",
			mapname);
		trap->FS_Write(line, strlen(line), f);
	}

	for (int c = 0; c < conCount; c++) {
		Com_sprintf(line, sizeof(line),
			"WALLRUN  %8.1f %8.1f %8.1f  %8.1f %8.1f %8.1f  0  %.1f  %d\n",
			cons[c].start[0], cons[c].start[1], cons[c].start[2],
			cons[c].end[0],   cons[c].end[1],   cons[c].end[2],
			WALLRUN_OFFMESH_RADIUS, WALLRUN_OFFMESH_AREA);
		trap->FS_Write(line, strlen(line), f);
	}

	trap->FS_Close(f);
	trap->Print("[WR-SCAN] Wrote %d connections to '%s'.\n", conCount, path);

	free(cons);
}

// Optimized variant: same scan as Bot2_ScanWallruns but pre-filters candidate
// floors against NavMesh_IsPointOnMesh BEFORE running the 16-direction probe,
// and dedupes start positions before simulation.  Use this for headless runs
// where the savings on wasted Pmove sweeps add up.
void Bot2_ScanWallrunsHeadless(int gridStep, float radius, int centerClient) {
	if (gridStep <= 0) gridStep = 96;

	// ---- scan bounds from navmesh ----
	vec3_t wmin, wmax;
	if (!NavMesh_GetBounds(wmin, wmax)) {
		trap->Print("[WR-SCAN] ERROR: navmesh not loaded — cannot determine map bounds.\n");
		return;
	}

	if (radius > 0.0f
		&& centerClient >= 0 && centerClient < MAX_CLIENTS
		&& g_entities[centerClient].inuse
		&& g_entities[centerClient].client)
	{
		float *org = g_entities[centerClient].client->ps.origin;
		wmin[0] = org[0] - radius;  wmax[0] = org[0] + radius;
		wmin[1] = org[1] - radius;  wmax[1] = org[1] + radius;
		trap->Print("[WR-SCAN] Local scan: center=(%.0f %.0f %.0f) radius=%.0f gridStep=%d\n",
			org[0], org[1], org[2], radius, gridStep);
	} else {
		trap->Print("[WR-SCAN] Full-map scan: bounds (%.0f %.0f %.0f)–(%.0f %.0f %.0f)  gridStep=%d\n",
			wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2], gridStep);
	}

	int gridW = (int)((wmax[0] - wmin[0]) / gridStep) + 1;
	int gridD = (int)((wmax[1] - wmin[1]) / gridStep) + 1;
	int maxCons = gridW * gridD * WALLRUN_SCAN_MAX_FLOORS * WALLRUN_SCAN_PROBE_DIRS;
	WallrunConnection *cons = (WallrunConnection *)malloc(maxCons * sizeof(WallrunConnection));
	if (!cons) {
		trap->Print("[WR-SCAN] ERROR: out of memory allocating result buffer.\n");
		return;
	}
	int conCount   = 0;
	int simCount   = 0;
	int passCount  = 0;

	const playerState_t *templatePS = NULL;
	int tracePass = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client) {
			templatePS = &g_entities[i].client->ps;
			tracePass  = i;
			break;
		}
	}
	if (!templatePS) {
		trap->Print("[WR-SCAN] ERROR: no connected client — need at least one player in-game to borrow a PS template.\n");
		free(cons);
		return;
	}
	trap->Print("[WR-SCAN] Using client %d as PS template.\n", tracePass);

	int colsDone    = 0;
	int colsTotal   = gridW * gridD;
	int lastPct     = -1;
	int floorSamples   = 0;
	int floorFiltered  = 0;
	int floorLevel[WALLRUN_SCAN_MAX_FLOORS];
	memset(floorLevel, 0, sizeof(floorLevel));

	for (float gx = wmin[0]; gx <= wmax[0]; gx += gridStep) {
		for (float gy = wmin[1]; gy <= wmax[1]; gy += gridStep) {
			colsDone++;
			int pct = (colsDone * 100) / (colsTotal > 0 ? colsTotal : 1);
			if (pct / 10 != lastPct / 10) {
				lastPct = pct;
				trap->Print("[WR-SCAN] %3d%%  sims=%-5d  pass=%-4d  unique=%d\n",
					pct, simCount, passCount, conCount);
			}
			float scanTop = wmax[2] + 32.0f;
			const float scanBot = wmin[2] - 32.0f;

			for (int flv = 0; flv < WALLRUN_SCAN_MAX_FLOORS && scanTop > scanBot; flv++) {
				vec3_t traceFrom = { gx, gy, scanTop };
				vec3_t traceTo   = { gx, gy, scanBot };
				trace_t groundTr;
				trap->Trace(&groundTr, traceFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					traceTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (groundTr.fraction >= 1.0f)
					break;

				if (groundTr.startsolid) {
					const float SOLID_STEP  = 32.0f;
					const int   MAX_SOLID_STEPS = 512;
					float probeZ    = scanTop;
					int   solidSteps = 0;
					while (probeZ > scanBot && solidSteps < MAX_SOLID_STEPS) {
						probeZ -= SOLID_STEP;
						solidSteps++;
						vec3_t pt = { gx, gy, probeZ };
						if (!(trap->PointContents(pt, tracePass) & CONTENTS_SOLID))
							break;
					}
					if (probeZ <= scanBot || solidSteps >= MAX_SOLID_STEPS)
						break;
					scanTop = probeZ - 32.0f;
					flv--;
					continue;
				}

				if (groundTr.allsolid)
					break;

				scanTop = groundTr.endpos[2] - 64.0f;

				if (groundTr.plane.normal[2] < 0.7f)
					continue;

			floorSamples++;
			floorLevel[flv < WALLRUN_SCAN_MAX_FLOORS ? flv : WALLRUN_SCAN_MAX_FLOORS-1]++;

			vec3_t groundPos;
			VectorCopy(groundTr.endpos, groundPos);

			// NavMesh Pre-Filtering: if the floor itself isn't on the navmesh, skip 16 probes!
			vec3_t testGround;
			VectorCopy(groundPos, testGround);
			testGround[2] += 2.0f;
			if (!NavMesh_IsPointOnMesh(testGround)) { floorFiltered++; continue; }

			for (int d = 0; d < WALLRUN_SCAN_PROBE_DIRS; d++) {
				float probeYaw = d * (360.0f / WALLRUN_SCAN_PROBE_DIRS);
				vec3_t probeAngles = { 0, probeYaw, 0 };
				vec3_t probeFwd;
				AngleVectors(probeAngles, probeFwd, NULL, NULL);

				vec3_t eyePos;
				VectorCopy(groundPos, eyePos);
				eyePos[2] += 28.0f;

				vec3_t wallProbeEnd;
				VectorMA(eyePos, WALLRUN_SCAN_WALL_MAXDIST, probeFwd, wallProbeEnd);

				trace_t wallTr;
				trap->Trace(&wallTr, eyePos, NULL, NULL, wallProbeEnd,
					tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (wallTr.fraction >= 1.0f) continue;

				float normalZ = fabsf(wallTr.plane.normal[2]);
				if (normalZ > WALLRUN_SCAN_NORMAL_Z_MAX) continue;

				vec3_t flushXY;
				VectorMA(wallTr.endpos, 16.0f, wallTr.plane.normal, flushXY);

				vec3_t flushGroundFrom = { flushXY[0], flushXY[1], flushXY[2] + 64.0f };
				vec3_t flushGroundTo   = { flushXY[0], flushXY[1], flushXY[2] - 128.0f };
				trace_t flushGnd;
				trap->Trace(&flushGnd, flushGroundFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					flushGroundTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (flushGnd.allsolid || flushGnd.startsolid || flushGnd.fraction >= 1.0f)
					continue;
				if (flushGnd.plane.normal[2] < 0.7f)
					continue;

				vec3_t simStart;
				VectorCopy(flushGnd.endpos, simStart);

				if (!NavMesh_IsPointOnMesh(simStart)) { floorFiltered++; continue; }

				float faceYaw = probeYaw;

				simCount++;

				// ---- Pre-Simulation Deduplication ----
				qboolean dupStart = qfalse;
				for (int c = 0; c < conCount; c++) {
					vec3_t ds;
					VectorSubtract(cons[c].start, simStart, ds);
					if (DotProduct(ds, ds) < 16.0f*16.0f) {
						dupStart = qtrue;
						break;
					}
				}
				if (dupStart) continue;

				vec3_t landPos = { 0, 0, 0 };
				vec3_t zeroVel = { 0, 0, 0 };

				qboolean passed = Bot2_SimulateWallrunScenarioEx(
					templatePS,
					simStart, zeroVel,
					(float*)s_playerMins, (float*)s_playerMaxs,
					tracePass, level.time,
					faceYaw, 0, 0, landPos);

				if (!passed) {
					static const int thresholds[] = { 900, 700, 500, 300, 100 };
					for (int t = 0; t < 5 && !passed; t++) {
						passed = Bot2_SimulateWallrunScenarioEx(
							templatePS,
							simStart, zeroVel,
							(float*)s_playerMins, (float*)s_playerMaxs,
							tracePass, level.time,
							faceYaw, 1, thresholds[t], landPos);
					}
				}
				if (!passed) {
					passed = Bot2_SimulateWallrunScenarioEx(
						templatePS,
						simStart, zeroVel,
						(float*)s_playerMins, (float*)s_playerMaxs,
						tracePass, level.time,
						faceYaw, 2, 0, landPos);
				}

				if (!passed) continue;

				if (!NavMesh_IsPointOnMesh(landPos)) continue;

				passCount++;

				qboolean dup = qfalse;
				for (int c = 0; c < conCount; c++) {
					vec3_t ds, de;
					VectorSubtract(cons[c].start, simStart, ds);
					VectorSubtract(cons[c].end,   landPos,  de);
					if (DotProduct(ds, ds) < 32.0f*32.0f &&
						DotProduct(de, de) < 32.0f*32.0f) {
						dup = qtrue;
						break;
					}
				}
				if (dup) continue;

				if (conCount < maxCons) {
					VectorCopy(simStart, cons[conCount].start);
					VectorCopy(landPos,  cons[conCount].end);
					conCount++;
				}
			} // end probe directions loop (d)
			} // end floor levels loop (flv)
		}
	}

	trap->Print("[WR-SCAN] Grid sweep done. simCount=%d  passCount=%d  unique=%d\n",
		simCount, passCount, conCount);
	trap->Print("[WR-SCAN] Floor levels sampled: total=%d  navmesh-filtered=%d\n",
		floorSamples, floorFiltered);
	trap->Print("[WR-SCAN] Floors by depth: lvl0=%d lvl1=%d lvl2=%d lvl3=%d lvl4=%d+\n",
		floorLevel[0], floorLevel[1], floorLevel[2], floorLevel[3],
		floorLevel[4]+floorLevel[5]+floorLevel[6]+floorLevel[7]);

	if (conCount == 0) {
		trap->Print("[WR-SCAN] No valid wallruns found — sidecar file not written.\n");
		free(cons);
		return;
	}

	// ---- write sidecar file ----
	char mapname[MAX_QPATH];
	trap->Cvar_VariableStringBuffer("mapname", mapname, sizeof(mapname));
	char path[MAX_QPATH];
	Com_sprintf(path, sizeof(path), "maps/%s.nav_connections", mapname);

	fileHandle_t f;
	fileHandle_t fCheck;
	int existing = trap->FS_Open(path, &fCheck, FS_READ);
	qboolean needHeader = (existing <= 0);
	if (fCheck) trap->FS_Close(fCheck);

	trap->FS_Open(path, &f, FS_APPEND);
	if (!f) {
		trap->Print("[WR-SCAN] ERROR: could not open '%s' for writing.\n", path);
		free(cons);
		return;
	}

	char line[256];
	if (needHeader) {
		Com_sprintf(line, sizeof(line),
			"# Auto-generated by Bot2_ScanWallruns — map: %s\n"
			"# type      sx       sy       sz       ex       ey       ez      bidir  radius  area\n",
			mapname);
		trap->FS_Write(line, strlen(line), f);
	}

	for (int c = 0; c < conCount; c++) {
		Com_sprintf(line, sizeof(line),
			"WALLRUN  %8.1f %8.1f %8.1f  %8.1f %8.1f %8.1f  0  %.1f  %d\n",
			cons[c].start[0], cons[c].start[1], cons[c].start[2],
			cons[c].end[0],   cons[c].end[1],   cons[c].end[2],
			WALLRUN_OFFMESH_RADIUS, WALLRUN_OFFMESH_AREA);
		trap->FS_Write(line, strlen(line), f);
	}

	trap->FS_Close(f);
	trap->Print("[WR-SCAN] Wrote %d connections to '%s'.\n", conCount, path);

	free(cons);
}

// ==============================================================================
// Wallrun Surface Checker  (diagnostic / developer tool)
//
// Called by the "bot_wallrun_check <clientnum>" server command.
// Traces forward from the client's eye position along their view direction to
// find the surface they're looking at, then runs Bot2_SimulateWallrun against
// it and prints a full report to the server console.
//
// Usage from the game console:
//   bot_wallrun_check 0        // check client 0
//   bot_wallrun_check 1        // check client 1, etc.
//
// The client should be standing at the base of the wall they want to test,
// facing directly into it.
// ==============================================================================
void Bot2_WallrunCheck(gentity_t *ent) {
	if (!ent || !ent->inuse || !ent->client) {
		trap->Print("[WR-CHECK] Invalid entity.\n");
		return;
	}

	// Build the trace start from the eye (not feet) so the ray hits the
	// wall face rather than the floor in front of it.
	vec3_t eyePos;
	VectorCopy(ent->client->ps.origin, eyePos);
	eyePos[2] += ent->client->ps.viewheight;

	// Trace 200 u forward along the client's horizontal aim direction.
	vec3_t flatAngles;
	VectorCopy(ent->client->ps.viewangles, flatAngles);
	flatAngles[PITCH] = 0.0f;
	vec3_t forward;
	AngleVectors(flatAngles, forward, NULL, NULL);

	vec3_t traceEnd;
	VectorMA(eyePos, 200.0f, forward, traceEnd);

	trace_t tr;
	trap->Trace(&tr, eyePos, NULL, NULL, traceEnd,
	            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float hitDist = tr.fraction * 200.0f;

	if (tr.fraction >= 1.0f) {
		trap->Print("[WR-CHECK] No surface found within 200 u in facing direction.\n");
		return;
	}

	trap->Print("[WR-CHECK] Surface hit at %.1f u | normal (%.2f %.2f %.2f) | contents 0x%X\n",
		hitDist,
		tr.plane.normal[0], tr.plane.normal[1], tr.plane.normal[2],
		tr.surfaceFlags);

	// ---- Verticality check (mirrors bg_pmove's own gate) ----
	float normalZ = fabsf(tr.plane.normal[2]);
	if (normalZ > 0.3f) {
		trap->Print("[WR-CHECK] FAIL: surface too sloped for wallrun (|normal.z|=%.2f, threshold 0.3).\n",
			normalZ);
		return;
	}
	trap->Print("[WR-CHECK] Verticality OK (|normal.z|=%.2f).\n", normalZ);

	// ---- Wall height trace ----
	vec3_t heightOrg;
	VectorCopy(ent->client->ps.origin, heightOrg);
	heightOrg[2] += 200.0f;
	vec3_t heightEnd;
	VectorMA(heightOrg, 30.0f, forward, heightEnd);
	trace_t heightTr;
	trap->Trace(&heightTr, heightOrg, NULL, NULL, heightEnd,
	            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (heightTr.fraction >= 0.9f) {
		trap->Print("[WR-CHECK] FAIL: wall too short — must extend >= 200 u above bot feet "
		            "(height trace fraction %.2f at +200 u, need < 0.9).\n", heightTr.fraction);
		return;
	}
	trap->Print("[WR-CHECK] Wall height OK (height trace fraction %.2f).\n", heightTr.fraction);

	// ---- Ceiling clearance trace ----
	vec3_t ceilEnd;
	VectorCopy(ent->client->ps.origin, ceilEnd);
	ceilEnd[2] += 72.0f;
	trace_t ceilTr;
	trap->Trace(&ceilTr, ent->client->ps.origin, NULL, NULL, ceilEnd,
	            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (ceilTr.fraction < 0.9f) {
		trap->Print("[WR-CHECK] FAIL: ceiling too low — need >= 72 u headroom "
		            "(ceil trace fraction %.2f, need >= 0.9).\n", ceilTr.fraction);
		return;
	}
	trap->Print("[WR-CHECK] Ceiling clearance OK (ceil trace fraction %.2f).\n", ceilTr.fraction);

	// ---- Pmove simulation ----
	// The face yaw is the direction directly into the wall
	// (opposite the surface normal).
	vec3_t faceDir;
	VectorNegate(tr.plane.normal, faceDir);
	vec3_t faceAngles;
	vectoangles(faceDir, faceAngles);
	float faceYaw = faceAngles[YAW];

	trap->Print("[WR-CHECK] Running pmove simulation (faceYaw=%.1f)...\n", faceYaw);
	vec3_t landPos = { 0, 0, 0 };
	qboolean simResult = Bot2_SimulateWallrunEx(ent, faceYaw, landPos);

	// ---- Final verdict ----
	if (simResult) {
		vec3_t *startPos = &ent->client->ps.origin;
		trap->Print("[WR-CHECK] *** PASS ***\n");
		trap->Print("[WR-CHECK]   start: (%.1f  %.1f  %.1f)\n",
			(*startPos)[0], (*startPos)[1], (*startPos)[2]);
		trap->Print("[WR-CHECK]   end:   (%.1f  %.1f  %.1f)\n",
			landPos[0], landPos[1], landPos[2]);
		trap->Print("[WR-CHECK]   sidecar line:\n");
		trap->Print("[WR-CHECK]   WALLRUN  %8.1f %8.1f %8.1f  %8.1f %8.1f %8.1f  0  48.0  4\n",
			(*startPos)[0], (*startPos)[1], (*startPos)[2],
			landPos[0], landPos[1], landPos[2]);
	} else {
		trap->Print("[WR-CHECK] *** FAIL — see [WR-SIM] lines above for details. ***\n");
		trap->Print("[WR-CHECK]   Possible causes: surface normal rejected by bg_pmove, "
		            "wall too short, ceiling cutting the climb short (bot pushed off before reaching top), "
		            "or landing Z not meaningfully above start Z.\n");
	}
}

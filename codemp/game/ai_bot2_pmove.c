// ==============================================================================
// ai_bot2_pmove.c - Phantom Pmove Simulation Engine for Advanced CTF Bot
//
// Hosts the bot's predictive physics:
//   * Bot2_PMTrace / Bot2_PMPointContents - safely-wrapped engine syscalls used
//     by every phantom pmove run (here and in the wallrun simulator).
//   * SimulatePmoveTrajectory          - core jump-arc predictor; runs bg_pmove
//                                        with synthetic input and reports
//                                        landing pos + post-sim playerState.
//   * ScoreJumpChain / EvaluateJumpChain - up to 4-jump recursive chain scorer.
//   * IsSafeToJump                     - public gate that fronts the chain
//                                        evaluator before the movement state
//                                        machine commits to a jump.
// ==============================================================================

#include "g_local.h"
#include "bg_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "ai_bot2_internal.h"
#include "g_navmesh.h"

#include <stdio.h>
#include <stdarg.h>

// ==============================================================================
// Safely Wrapped Syscalls for Pmove Simulation
// ==============================================================================
void Bot2_PMTrace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentMask) {
	// passEntityNum comes directly from Pmove's internal calls (e.g. PM_TraceAll passes
	// pm->ps->clientNum).  For players, clientNum == s.number, so the simulating bot's
	// own entity is always excluded — no self-blocking of the forward 128u trace that
	// PM_AdjustAngleForWallRunUp uses for ALT detection.
	trap->Trace(results, start, mins, maxs, end, passEntityNum, contentMask, qfalse, 0, 0);

	// --- PHANTOM PMOVE VISUAL SUPPRESSION ---
	// 1. Prevent real-world water splashes: PM_WaterEvents runs a standalone MASK_WATER
	//    trace before spawning a splash particle. Forcing it to fail aborts the particle.
	if (contentMask == MASK_WATER) {
		results->fraction = 1.0f;
	}

	// 2. Prevent real-world dust/dirt landing particles: PM_CrashLandEffect checks the
	//    ground material before spawning dust. Stripping the material aborts the particle
	//    while preserving critical physics flags like SURF_SLICK (ice) and SURF_NODAMAGE (bouncepads).
	results->surfaceFlags &= ~MATERIAL_MASK;
}

int Bot2_PMPointContents(const vec3_t point, int passEntityNum) {
	return trap->PointContents(point, passEntityNum);
}

void BotBreadcrumb(const char* format, ...) {
	return; // Disabled for performance
}

// ==============================================================================
// Phantom Pmove Simulation Engine
//
// Runs the engine's real bg_pmove loop with synthetic input to predict where
// a jump (or other movement) would actually land if the bot committed to it
// right now.  The "phantom" name comes from the fact that the bot itself
// never moves during the simulation — we copy its playerState into a local
// dummy_ps, drive that copy through Pmove() ticks with hand-built usercmds,
// and read the landing position out of the dummy without touching the real
// world or the real bot.
//
// Inputs:
//   in_ps          — starting playerState (NULL means "use the bot's current
//                    real PS"); the simulator copies it and never mutates it.
//   start_yaw      — bot's velocity yaw at takeoff (used as the reference for
//                    the strafe-jump magic-angle math, not the view angle).
//   strafeDir      — sign indicates strafe direction (+1 right, -1 left);
//                    abs == 2 means "hard strafe" (no forward, just A or D).
//   angle_fraction — 0.0 holds the lower-boundary key angle, 1.0 holds the
//                    speed-optimal angle; intermediates blend the two.
//   max_run_speed  — bot's effective ps->speed; gates the magic-angle
//                    computation.
//   lockAim/aimDir — when true, view yaw is held facing aimDir (combat lock)
//                    instead of pointing along velocity.
//
// Outputs:
//   out_pmove_land_pos — landing position on success.  Unchanged on fail.
//   out_ps             — full post-landing playerState on success.  Used by
//                        the chain evaluator to recurse into a follow-up jump.
//   Returns qtrue on a clean landing, qfalse if the trajectory bailed out
//   (hit a wall, slid off, dropped into water/lava/trigger_hurt, lost speed).
//
// The simulator inflates the bot's bbox by 12u in XY (except wallruns) so
// jumps only succeed if the landing has a generous safety margin — prevents
// "needle threading" landings that work in sim but fail in practice.
// ==============================================================================
qboolean SimulatePmoveTrajectory(gentity_t* ent, playerState_t* in_ps, float start_yaw, int strafeDir, float angle_fraction, float max_run_speed, vec3_t out_pmove_land_pos, playerState_t* out_ps, qboolean lockAim, vec3_t aimDir) {
	if (!ent || !ent->inuse || !ent->client || level.intermissiontime || level.time == 0) return qfalse;
	if (!g_entities) return qfalse;

	BotBreadcrumb("SimulatePmoveTrajectory START - ClientNum: %d", ent->s.number);

	pmove_t sim_pm;
	playerState_t dummy_ps;
	usercmd_t dummy_cmd;

	BotBreadcrumb("Copying dummy_ps...");
	if (in_ps) {
		memcpy(&dummy_ps, in_ps, sizeof(playerState_t));
	} else {
		memcpy(&dummy_ps, &ent->client->ps, sizeof(playerState_t));
	}
	memset(&dummy_cmd, 0, sizeof(usercmd_t));

	dummy_ps.delta_angles[0] = 0;
	dummy_ps.delta_angles[1] = 0;
	dummy_ps.delta_angles[2] = 0;

	dummy_ps.m_iVehicleNum = 0;

	dummy_ps.weapon = WP_MELEE;
	dummy_ps.weaponstate = WEAPON_READY;
	dummy_cmd.weapon = WP_MELEE;
	dummy_cmd.serverTime = dummy_ps.commandTime;

	// Prevent phantom Pmove from locking onto live players
	dummy_ps.saberLockEnemy = ENTITYNUM_NONE;
	dummy_ps.saberLockTime = 0;
	dummy_ps.saberInFlight = qfalse;
	dummy_ps.saberEntityNum = ENTITYNUM_NONE;
	dummy_ps.rocketLockIndex = ENTITYNUM_NONE;
	dummy_ps.genericEnemyIndex = ENTITYNUM_NONE;
	dummy_ps.lookTarget = ENTITYNUM_NONE;
	dummy_ps.hasLookTarget = qfalse;

	BotBreadcrumb("Initializing sim_pm...");
	memset(&sim_pm, 0, sizeof(pmove_t));
	sim_pm.ps = &dummy_ps;
	sim_pm.cmd = dummy_cmd;

	sim_pm.trace = Bot2_PMTrace;
	sim_pm.pointcontents = Bot2_PMPointContents;
	sim_pm.tracemask = MASK_PLAYERSOLID;

	sim_pm.watertype = 0;
	sim_pm.waterlevel = 0;

	// Copy the real bounding box
	VectorCopy(ent->r.mins, sim_pm.mins);
	VectorCopy(ent->r.maxs, sim_pm.maxs);

	// --- NEW: FATTEN THE PHANTOM BOUNDING BOX ---
	// Add a 12-unit margin of error to the X and Y axes.
	// This forces the bot to only accept paths with wide, safe landing zones,
	// preventing "needle threading" on narrow ramps at high speeds.
	// EXCEPT during wallruns! Fattening the box while flush against a wall
	// causes pmove to instantly eject the bot 12+ units outward, ruining the sim.
	if (bot2_states[ent->s.number].offmeshType != OFFMESH_AREA_WALLRUN) {
		sim_pm.mins[0] -= 12.0f;
		sim_pm.mins[1] -= 12.0f;
		sim_pm.maxs[0] += 12.0f;
		sim_pm.maxs[1] += 12.0f;
	}

	VectorCopy(ent->modelScale, sim_pm.modelScale);

	sim_pm.baseEnt = (bgEntity_t*)g_entities;
	sim_pm.entSize = sizeof(gentity_t);

	// The Animation Blindfold Fix
	static animation_t dummy_anims[MAX_TOTALANIMATIONS];
	memset(dummy_anims, 0, sizeof(dummy_anims));
	sim_pm.animations = dummy_anims;
	sim_pm.ghoul2 = NULL;

	int pmove_fixed = trap->Cvar_VariableIntegerValue("pmove_fixed");
	if (pmove_fixed) {
		int pmove_msec = trap->Cvar_VariableIntegerValue("pmove_msec");
		sim_pm.pmove_msec = (pmove_msec > 0) ? pmove_msec : 8;
	}
	else {
		int sv_fps = trap->Cvar_VariableIntegerValue("sv_fps");
		if (sv_fps <= 0) sv_fps = 20;
		sim_pm.pmove_msec = 1000 / sv_fps;
	}

	if (sim_pm.pmove_msec <= 0) sim_pm.pmove_msec = 25;
	sim_pm.pmove_fixed = pmove_fixed;
	// sim_pm.noAnimate = qtrue; // Bypassed as not in struct

	int max_sim_ticks = 2500 / sim_pm.pmove_msec;
	qboolean landed = qfalse;
	int sim_time = dummy_ps.commandTime;
	float start_z = dummy_ps.origin[2];

	vec3_t sim_viewangles;
	VectorCopy(dummy_ps.viewangles, sim_viewangles);

	BotBreadcrumb("Entering tick loop. max_sim_ticks = %d", max_sim_ticks);

	for (int i = 0; i < max_sim_ticks; i++) {
		sim_time += sim_pm.pmove_msec;
		dummy_cmd.serverTime = sim_time;

		int actualDir = (strafeDir > 0) ? 1 : -1;
		qboolean isHardStrafe = (abs(strafeDir) == 2);

		// THE FIX: Hold jump for exactly 100ms, mirroring State 2 execution!
		dummy_cmd.upmove = ((i * sim_pm.pmove_msec) < 100) ? 127 : 0;

		float sim_vel_x = dummy_ps.velocity[0];
		float sim_vel_y = dummy_ps.velocity[1];
		float sim_current_speed = sqrt((sim_vel_x * sim_vel_x) + (sim_vel_y * sim_vel_y));
		float sim_vel_yaw = atan2(sim_vel_y, sim_vel_x) * (180.0f / M_PI);

		float magic_angle = 0.0f;
		if (sim_current_speed > max_run_speed - 15.0f) {
			float wishspeed = max_run_speed - 15.0f;
			float low_acos = wishspeed / sim_current_speed;
			low_acos = (low_acos < -1.0f) ? -1.0f : ((low_acos > 1.0f) ? 1.0f : low_acos);
			float low_angle = acos(low_acos) * (180.0f / M_PI);

			if (!isHardStrafe && angle_fraction > 0.0f) {
				// Optimal angle: arccos(wishspeed / (2*speed)) — maximises per-frame speed gain
				float opt_acos = wishspeed / (2.0f * sim_current_speed);
				opt_acos = (opt_acos < -1.0f) ? -1.0f : ((opt_acos > 1.0f) ? 1.0f : opt_acos);
				float opt_angle = acos(opt_acos) * (180.0f / M_PI);
				magic_angle = low_angle + angle_fraction * (opt_angle - low_angle);
			} else {
				magic_angle = low_angle;
			}
		}

		if (isHardStrafe) {
			magic_angle = min(80.0f, magic_angle * 1.5f);
		}
		float keyOffset = isHardStrafe ? 90.0f : 45.0f;
		float tYaw = AngleMod(sim_vel_yaw - ((magic_angle - keyOffset) * actualDir));

		// Calculate desired_angles
		vec3_t desired_angles;
		if (lockAim) {
			vectoangles(aimDir, desired_angles);
		} else {
			desired_angles[PITCH] = 0;
			desired_angles[YAW] = tYaw;
		}

		// --- MIRRORED SMOOTHING LOGIC ---
		float turnSpeed = 1200.0f; // Match Bot2_ApplySmoothing
		float frameTime = sim_pm.pmove_msec / 1000.0f;
		float maxDelta = turnSpeed * frameTime;

		// Smooth Yaw
		float yDelta = AngleDelta(desired_angles[YAW], sim_viewangles[YAW]);
		if (yDelta > maxDelta) yDelta = maxDelta;
		else if (yDelta < -maxDelta) yDelta = -maxDelta;
		sim_viewangles[YAW] = AngleNormalize360(sim_viewangles[YAW] + yDelta);

		// Smooth Pitch (Simulate looking at horizon for movement)
		float pDelta = AngleDelta(0, sim_viewangles[PITCH]);
		if (pDelta > maxDelta) pDelta = maxDelta;
		else if (pDelta < -maxDelta) pDelta = -maxDelta;
		sim_viewangles[PITCH] = AngleNormalize360(sim_viewangles[PITCH] + pDelta);

		dummy_cmd.angles[YAW] = ANGLE2SHORT(sim_viewangles[YAW]);
		dummy_cmd.angles[PITCH] = ANGLE2SHORT(sim_viewangles[PITCH]);

		// Calculate moves relative to current view angles
		if (lockAim) {
			float tDiff = (tYaw - sim_viewangles[YAW]) * (M_PI / 180.0f);
			dummy_cmd.forwardmove = (char)((isHardStrafe ? 0.0f : 127.0f) * cos(tDiff));
			dummy_cmd.rightmove = (char)(127.0f * -sin(tDiff));
		} else {
			dummy_cmd.forwardmove = isHardStrafe ? 0 : 127;
			dummy_cmd.rightmove = 127 * actualDir;
		}
		// --------------------------------

		sim_pm.cmd = dummy_cmd;

		// Track position before engine math for accurate swept-sphere hazard tracing
		vec3_t prev_origin;
		VectorCopy(dummy_ps.origin, prev_origin);

		BotBreadcrumb("Calling Pmove for tick %d...", i);
		Pmove(&sim_pm);
		BotBreadcrumb("Pmove tick %d completed.", i);

		// ==============================================================================
		// HAZARD GATES (Evaluated before checking for a successful landing)
		// ==============================================================================

		// 1. Did we fall into the abyss?
		if (start_z - dummy_ps.origin[2] > 800.0f) {
			BotBreadcrumb("Fell too far on tick %d.", i);
			break;
		}

		// 2. Are we swimming in a death pit? (Instant bitmask check for liquid brushes)
		int currentContents = trap->PointContents(dummy_ps.origin, ent->s.number);
		if (currentContents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER)) {
			BotBreadcrumb("Simulated trajectory hit Water/Lava/Slime on tick %d.", i);
			break;
		}

		// 3. Did our arc intersect a trigger_hurt volume?
		vec3_t p_mins = { -15, -15, -24 }, p_maxs = { 15, 15, 32 };
		if (CheckForTriggerHurt(ent, prev_origin, dummy_ps.origin, p_mins, p_maxs)) {
			BotBreadcrumb("Hit trigger_hurt on tick %d.", i);
			break;
		}

		// LANDING VALIDATION
		// ==============================================================================

		if (i > 2 && dummy_ps.groundEntityNum != ENTITYNUM_NONE) {

			// Capture the exact coordinate of first contact
			vec3_t initial_landing_pos;
			VectorCopy(dummy_ps.origin, initial_landing_pos);

			// Simulate the clumsy landing for 2 ticks
			// dummy_cmd.forwardmove = 0; // Commented out to simulate slow reaction
			// dummy_cmd.rightmove = 0;   // Commented out to simulate slow reaction
			dummy_cmd.upmove = 0;         // CRITICAL: Prevent accidental bunnyhopping

			for (int slideTick = 0; slideTick < 2; slideTick++) {
				sim_time += sim_pm.pmove_msec;
				dummy_cmd.serverTime = sim_time;
				sim_pm.cmd = dummy_cmd;

				Pmove(&sim_pm);
			}

			// --- POST-SLIDE HAZARD GAUNTLET ---
			qboolean slide_survived = qtrue;

			// 1. Did the slide push us off the physical ledge?
			if (dummy_ps.groundEntityNum == ENTITYNUM_NONE) {
				BotBreadcrumb("Landed, but momentum slid us off the edge on tick %d.", i);
				slide_survived = qfalse;
			}

			// 2. Did we slide into a shallow pool of Lava/Slime?
			if (slide_survived) {
				int postSlideContents = trap->PointContents(dummy_ps.origin, ent->s.number);
				if (postSlideContents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER)) {
					BotBreadcrumb("Landed safely, but slid into Water/Lava/Slime on tick %d.", i);
					slide_survived = qfalse;
				}
			}

			// 3. Did the slide path drag us through a trigger_hurt?
			if (slide_survived) {
				vec3_t p_mins = { -15, -15, -24 }, p_maxs = { 15, 15, 32 };
				if (CheckForTriggerHurt(ent, initial_landing_pos, dummy_ps.origin, p_mins, p_maxs)) {
					BotBreadcrumb("Landed safely, but slid into a trigger_hurt on tick %d.", i);
					slide_survived = qfalse;
				}
			}

			// --- THE FINAL VERDICT ---
			if (slide_survived) {
				landed = qtrue;

				// Revert the position back to pure arc contact for pristine NavMesh & Telemetry data
				VectorCopy(initial_landing_pos, dummy_ps.origin);

				BotBreadcrumb("Landed and stabilized safely on tick %d.", i);
				break;
			}
			else {
				// landed remains qfalse, jump is denied.
				break;
			}
		}

		// Speed bleed-out check
		float horiz_speed = sqrt(dummy_ps.velocity[0] * dummy_ps.velocity[0] + dummy_ps.velocity[1] * dummy_ps.velocity[1]);
		if (i > 5 && horiz_speed < 50.0f) {
			BotBreadcrumb("Lost speed on tick %d.", i);
			break;
		}
	}

	if (landed && out_pmove_land_pos) {
		VectorCopy(dummy_ps.origin, out_pmove_land_pos);
	}
	if (landed && out_ps) {
		memcpy(out_ps, &dummy_ps, sizeof(playerState_t));
	}

	BotBreadcrumb("SimulatePmoveTrajectory DONE");
	return landed;
}

// ==============================================================================
// Kinematic Math & Jump Arc Prediction
// ==============================================================================

// Computes a heuristic cost for a candidate jump or jump-chain landing.
// Lower is better.  IsSafeToJump picks the strafe direction whose chain has
// the lowest score, so the formula is what determines the bot's pathing
// personality (cautious vs. aggressive, line-cutting vs. waypoint-following).
//
// Inputs:
//   nav_dist  — Detour topological path distance from landing to goal.
//   line_dist — straight-line Euclidean distance from landing to goal.
//   avg_spd2d — average horizontal speed across all jumps in the chain so
//               far (longer chains with higher speed get a discount).
//   z_loss    — drop in altitude from chain start to this landing (penalty
//               for dropping into pits we then have to climb back out of).
//
// The function dispatches on `bot2_states[].test_variation` when the bot is
// running the routing-test harness, exposing 10 different weight blends so
// the harness can A/B test which scoring style wins.  Variation 2 (Corner
// Cutter, 50/50 nav/line) is the proven default for live play.
static float ScoreJumpChain(int clientNum, float nav_dist, float line_dist, float avg_spd2d, float z_loss) {
	float speed_weight;
	float two_jump_dist = avg_spd2d * 2.0f;
	if (two_jump_dist < 200.0f) two_jump_dist = 200.0f;

	int variation = bot2_states[clientNum].test_active ? bot2_states[clientNum].test_variation : 2; // Var 2 (Corner Cutter) is the proven default

	switch (variation) {
	case 0: // Baseline
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 2.0f : 2.0f;
		return (nav_dist * 0.70f) + (line_dist * 0.30f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 1: // Pure Speed
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 4.0f : 4.0f;
		return (nav_dist * 1.00f) + (line_dist * 0.00f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 2: // Corner Cutter
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 1.0f : 1.0f;
		return (nav_dist * 0.50f) + (line_dist * 0.50f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 3: // Fearless
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 2.0f : 2.0f;
		return (nav_dist * 0.70f) + (line_dist * 0.30f) - (avg_spd2d * speed_weight) + (z_loss * 0.0f);
	case 4: // Anti-Speed (For science)
		return (nav_dist * 0.70f) + (line_dist * 0.30f) + (z_loss * 2.0f);
	case 5: // Heavy Z Penalty
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 2.0f : 2.0f;
		return (nav_dist * 0.70f) + (line_dist * 0.30f) - (avg_spd2d * speed_weight) + (z_loss * 5.0f);
	case 6: // Pure Line
		speed_weight = (line_dist < two_jump_dist) ? (line_dist / two_jump_dist) * 2.0f : 2.0f;
		return (nav_dist * 0.00f) + (line_dist * 1.00f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 7: // Speed > Distance
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 10.0f : 10.0f;
		return (nav_dist * 0.50f) + (line_dist * 0.50f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 8: // Hyper-Corner Cutter
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 0.5f : 0.5f;
		return (nav_dist * 0.20f) + (line_dist * 0.80f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	case 9: // The Turtle
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 0.1f : 0.1f;
		return (nav_dist * 0.90f) + (line_dist * 0.10f) - (avg_spd2d * speed_weight) + (z_loss * 10.0f);
	default:
		speed_weight = (nav_dist < two_jump_dist) ? (nav_dist / two_jump_dist) * 2.0f : 2.0f;
		return (nav_dist * 0.70f) + (line_dist * 0.30f) - (avg_spd2d * speed_weight) + (z_loss * 2.0f);
	}
}

// Written at depth==1 whenever a new best score is found.
// Single-threaded game loop means this is safe without a lock.
static int s_lastFracWinner = 0;

// Recursively explores 4-jump-deep continuations from a candidate first-jump
// landing.  At each depth, tries 4 input combos (2 strafe directions × 2
// angle fractions: 0.0 boundary, 1.0 optimal), simulates each through
// SimulatePmoveTrajectory, and recurses if the landing is on the navmesh
// and not in ledge danger.  Returns the LOWEST score reachable from this
// chain, where "score" is whatever ScoreJumpChain returns (lower = better).
//
// The terminal-depth gate: only the 4th-jump landing is unconditionally
// scored.  Intermediate landings only get to win if they're already within
// 500u of the goal.  This stops a shallow chain with a slightly-better
// score from beating a deeper chain that actually makes more progress.
//
// As a side effect, depth==1 frames write s_lastFracWinner (which fraction
// won at the *first* recursive level) so the caller can record histograms
// of which angle strategy is paying off in live play.
//
// Returns 9999999.0f if no valid chain was found from this position.
static float EvaluateJumpChain(gentity_t* ent, vec3_t originalStart, playerState_t* ps, float total_time, float total_dist2d, int depth, int max_depth, vec3_t targetOrigin, float max_run_speed, qboolean lockAim, vec3_t aimDir) {
	if (depth >= max_depth) return 9999999.0f;

	// Terminal depth: this is the 4th jump — always score it regardless of distance.
	// Non-terminal: only allow a landing to win if it's within 500u of the goal.
	// This prevents shallow chains from "kicking the can" with a marginally better
	// score while making less real progress toward the target.
	qboolean is_terminal = (depth == max_depth - 1);

	float land_vel_yaw = atan2(ps->velocity[1], ps->velocity[0]) * (180.0f / M_PI);
	static const int chainDirs[2] = { 1, -1 };
	// 2 angle variants: lower boundary and optimal.
	// Intermediates removed after telemetry showed the extremes win ~62% combined.
	static const float chainFracs[2] = { 0.0f, 1.0f };

	float best_score = 9999999.0f;
	for (int d = 0; d < 2; d++) {
		for (int a = 0; a < 2; a++) {
			vec3_t chain_land;
			playerState_t chain_ps;
			if (SimulatePmoveTrajectory(ent, ps, land_vel_yaw, chainDirs[d], chainFracs[a], max_run_speed, chain_land, &chain_ps, lockAim, aimDir)) {
				float chain_land_yaw = atan2(chain_ps.velocity[1], chain_ps.velocity[0]) * (180.0f / M_PI);
				if (NavMesh_IsPointOnMesh(chain_land) && TraceFloorScore(ent, chain_land, chain_land_yaw, 32.0f, NULL) >= 0.0f) {
					float jump_time = (chain_ps.commandTime - ps->commandTime) / 1000.0f;
					if (jump_time <= 0.001f) jump_time = 0.1f;

					float j_dx = chain_land[0] - ps->origin[0];
					float j_dy = chain_land[1] - ps->origin[1];
					float jump_dist2d = sqrt((j_dx*j_dx) + (j_dy*j_dy));

					float new_total_time = total_time + jump_time;
					float new_total_dist2d = total_dist2d + jump_dist2d;
					float avg_spd2d = new_total_dist2d / new_total_time;

					float c_nav = NavMesh_GetPathDistance(ent->s.number, chain_land, targetOrigin);
					float c_line = Distance(chain_land, targetOrigin);
					float new_z_loss = max(0.0f, originalStart[2] - chain_land[2]);

					float current_score = ScoreJumpChain(ent->s.number, c_nav, c_line, avg_spd2d, new_z_loss);

					// Allow this landing's score only if terminal or close enough to the goal
					if (is_terminal || c_line < 500.0f) {
						if (current_score < best_score) {
							best_score = current_score;
							if (depth == 1) s_lastFracWinner = a;
						}
					}

					// Always recurse deeper (only at non-terminal depths)
					if (!is_terminal) {
						float child_score = EvaluateJumpChain(ent, originalStart, &chain_ps, new_total_time, new_total_dist2d, depth + 1, max_depth, targetOrigin, max_run_speed, qfalse, NULL);
						if (child_score < best_score) {
							best_score = child_score;
							if (depth == 1) s_lastFracWinner = a;
						}
					}
				}
			}
		}
	}
	return best_score;
}

// Public gate the WALK-state running-jump logic calls before committing to
// a jump.  Asks: "if I jump right now in direction testDir, will I land on
// the navmesh in a safe spot, AND is the best 4-jump chain that follows
// this one good enough to bother?"
//
// On success (qtrue) returns:
//   out_land_pos    — predicted first-jump landing.
//   out_land_speed  — bot's effective max speed at landing (currently just
//                     echoes max_run_speed; reserved for future tuning).
//   out_chain_dist  — winning chain score (lower is better; the WALK code
//                     compares the two strafe-direction scores and picks the
//                     better one).
//   out_chain_frac  — which angle fraction (0=boundary, 1=optimal) won at
//                     depth-1 of the chain that produced out_chain_dist.
//
// On failure (qfalse): failReason is filled with a short diagnostic string
// describing why the jump was rejected (off-navmesh, in trigger_hurt, ledge
// danger, water/lava intersection, no valid chain, etc.).  Other out
// parameters are unchanged.
//
// Internally: runs one SimulatePmoveTrajectory pass for the candidate
// landing, validates it via NavMesh_IsPointOnMesh + TraceFloorScore, then
// hands off to EvaluateJumpChain to score 4 jumps deep and pick the lowest.
// If jump1 alone lands within 500u of the goal it competes with the chain
// score; otherwise the chain evaluator owns the decision.
qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed, vec3_t targetOrigin, float* out_chain_dist, int* out_chain_frac, qboolean lockAim, vec3_t aimDir) {
	vec3_t pmove_land;
	playerState_t pmove_ps;

	if (!SimulatePmoveTrajectory(ent, NULL, vel_yaw, testDir, 0.0f, max_run_speed, pmove_land, &pmove_ps, lockAim, aimDir)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Trajectory aborted (hit wall, hurt, or pit)", 128);
		return qfalse;
	}

	// Validate landing spot
	if (!NavMesh_IsPointOnMesh(pmove_land)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Landing spot is NOT on the NavMesh", 128);
		return qfalse;
	}

	// Reject landing spots that would immediately trigger ledge detection —
	// landing there forces the bot into walk mode and kills the strafe-jump chain.
	{
		float land_vel_yaw = atan2(pmove_ps.velocity[1], pmove_ps.velocity[0]) * (180.0f / M_PI);
		if (TraceFloorScore(ent, pmove_land, land_vel_yaw, 32.0f, NULL) < 0.0f) {
			if (failReason) Q_strncpyz(failReason, "Jump lands in ledge-danger zone (chain killer)", 128);
			return qfalse;
		}
	}

	// --- 4-JUMP PREDICTION CHAIN ---
	float jump1_time = (pmove_ps.commandTime - ent->client->ps.commandTime) / 1000.0f;
	if (jump1_time <= 0.001f) jump1_time = 0.1f;
	float j1_dx = pmove_land[0] - start[0];
	float j1_dy = pmove_land[1] - start[1];
	float jump1_dist2d = sqrt((j1_dx*j1_dx) + (j1_dy*j1_dy));
	float base_avg_spd2d = jump1_dist2d / jump1_time;

	float base_nav = NavMesh_GetPathDistance(ent->s.number, pmove_land, targetOrigin);
	float base_line = Distance(pmove_land, targetOrigin);
	float base_z_loss = max(0.0f, start[2] - pmove_land[2]);

	float jump1_score = ScoreJumpChain(ent->s.number, base_nav, base_line, base_avg_spd2d, base_z_loss);

	// Jump1 only competes if it already lands within 500u of the goal.
	// If it doesn't, force the chain evaluator to own the decision — this prevents
	// a marginally cheaper single jump from winning when we're still far away.
	float best_chain_dist = (base_line < 500.0f) ? jump1_score : 9999999.0f;

	float future_chain_score = EvaluateJumpChain(ent, start, &pmove_ps, jump1_time, jump1_dist2d, 1, 4, targetOrigin, max_run_speed, lockAim, aimDir);
	if (future_chain_score < best_chain_dist) {
		best_chain_dist = future_chain_score;
	}

	// Fallback: if no chain produced a valid score (all 4th-jump landings failed
	// navmesh/ledge checks), accept jump1 unconditionally so we don't stall.
	if (best_chain_dist >= 9999999.0f) {
		best_chain_dist = jump1_score;
	}

	if (out_chain_dist) *out_chain_dist = best_chain_dist;
	if (out_chain_frac) *out_chain_frac = s_lastFracWinner;
	// -----------------------------------

	if (out_land_pos) VectorCopy(pmove_land, out_land_pos);
	if (out_land_speed) *out_land_speed = max_run_speed;

	return qtrue;
}

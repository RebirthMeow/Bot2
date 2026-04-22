// ==============================================================================
// ai_bot2_nav.c - Movement and Physics Engine for Advanced CTF Bot
// ==============================================================================

#include "g_local.h"
#include "bg_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"
#include <stdio.h> // Required for raw disk I/O logging

// Prevents bots from jumping into death pits
qboolean CheckForTriggerHurt(gentity_t* ent, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs) {
	vec3_t cs; VectorCopy(start, cs);
	int safety = 0;
	while (cs[2] > end[2] && safety < 32) {
		trace_t tr;
		vec3_t stepEnd; VectorCopy(cs, stepEnd);
		stepEnd[2] -= 128.0f; // Step down in 128 unit chunks
		if (stepEnd[2] < end[2]) VectorCopy(end, stepEnd);

		trap->Trace(&tr, cs, mins, maxs, stepEnd, ent->s.number, CONTENTS_TRIGGER, qfalse, 0, 0);

		if (tr.fraction < 1.0f && tr.entityNum < ENTITYNUM_WORLD && tr.entityNum >= 0) {
			gentity_t* hit = &g_entities[tr.entityNum];
			if (hit->inuse && hit->classname && Q_stricmp(hit->classname, "trigger_hurt") == 0) {
				return qtrue;
			}
		}

		if (tr.fraction == 1.0f) VectorCopy(stepEnd, cs);
		else VectorCopy(tr.endpos, cs);

		cs[2] -= 1.0f; // Ensure progress
		safety++;
	}
	return qfalse;
}

// Returns a "score" for floor safety based on tracing ahead and down
float TraceFloorScore(gentity_t* ent, vec3_t start, float yaw, float dist, qboolean* hitWall) {
	vec3_t forward, end, downEnd, tStart; trace_t tr, trDrop;
	vec3_t mins = { -15, -15, -24 }, maxs = { 15, 15, 32 };
	vec3_t yaw_angles = { 0, yaw, 0 };
	AngleVectors(yaw_angles, forward, NULL, NULL);

	VectorCopy(start, tStart); tStart[2] += 24.0f;
	VectorMA(tStart, dist, forward, end);
	trap->Trace(&tr, tStart, mins, maxs, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (hitWall) *hitWall = (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f);

	float actual_dist = dist * tr.fraction;
	for (int i = 1; i <= 3; i++) {
		vec3_t probe; VectorMA(tStart, actual_dist * (i / 3.0f), forward, probe);
		VectorCopy(probe, downEnd); downEnd[2] -= 512.0f; // Use a more reasonable 512 unit pit check
		trap->Trace(&trDrop, probe, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		if (trDrop.fraction == 1.0f || (probe[2] - trDrop.endpos[2]) > 400.0f || CheckForTriggerHurt(ent, probe, trDrop.endpos, mins, maxs)) return -1.0f;
	}
	return tr.fraction;
}

// Finds an alternate path if the direct path leads off a cliff
qboolean GetSafeEscapeYaw(gentity_t* ent, float danger_yaw, float* safe_yaw) {
	float escape_yaw = AngleMod(danger_yaw + 180.0f); float best_score = -1.0f, best_yaw = escape_yaw;
	for (int i = 0; i < 8; i++) {
		float test_yaw = AngleMod(escape_yaw + ((i % 2 == 0 ? 1.0f : -1.0f) * ((i + 1) / 2) * 45.0f));
		float score = TraceFloorScore(ent, ent->client->ps.origin, test_yaw, 128.0f, NULL);
		if (score > best_score) { best_score = score; best_yaw = test_yaw; if (best_score == 1.0f) break; }
	}
	if (best_score >= 0.0f) { if (safe_yaw) *safe_yaw = best_yaw; return qtrue; }
	return qfalse;
}

// Decide whether to strafe left (-1) or right (+1) based on floor geometry
int EvaluateStrafeDir(gentity_t* ent, float base_target_yaw) {
	float scoreR = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw - 30.0f), 256.0f, NULL);
	float scoreL = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw + 30.0f), 256.0f, NULL);
	return (scoreL > scoreR) ? -1 : ((scoreR > scoreL) ? 1 : ((Q_irand(0, 1) == 0) ? 1 : -1));
}

// Bounding box check for jump pads
qboolean IsInTriggerPush(gentity_t* ent) {
	gentity_t* push = NULL;
	while ((push = G_Find(push, FOFS(classname), "trigger_push")) != NULL) {
		if (push->r.linked) {
			vec3_t mins, maxs; VectorAdd(ent->client->ps.origin, ent->r.mins, mins); VectorAdd(ent->client->ps.origin, ent->r.maxs, maxs);
			if (maxs[0] >= push->r.absmin[0] && mins[0] <= push->r.absmax[0] && maxs[1] >= push->r.absmin[1] && mins[1] <= push->r.absmax[1] && maxs[2] >= push->r.absmin[2] && mins[2] <= push->r.absmax[2])
				return qtrue;
		}
	}
	return qfalse;
}

// ==============================================================================
// Safely Wrapped Syscalls for Pmove Simulation
// ==============================================================================
static void Bot2_PMTrace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentMask) {
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

static int Bot2_PMPointContents(const vec3_t point, int passEntityNum) {
	return trap->PointContents(point, passEntityNum);
}

#include <stdio.h>
#include <stdarg.h>

void BotBreadcrumb(const char* format, ...) {
	return; // Disabled for performance
}

// ==============================================================================
// Phantom Pmove Simulation Engine
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

	// Strip the weapon to bypass PM_Weapon() saber logic, but use WP_MELEE so Force Jump is still allowed by the engine!
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
	sim_pm.mins[0] -= 12.0f;
	sim_pm.mins[1] -= 12.0f;
	sim_pm.maxs[0] += 12.0f;
	sim_pm.maxs[1] += 12.0f;

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

		// --- MIRRORED SMOOTHING LOGIC (DISABLED FOR BASE CASE) ---
		sim_viewangles[YAW] = AngleNormalize360(desired_angles[YAW]);
		sim_viewangles[PITCH] = AngleNormalize360(desired_angles[PITCH]);

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

qboolean Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos, qboolean doTelemetry) {
	if (!target || !target->client) { VectorCopy(target->s.origin, out_leadPos); return qtrue; }
	vec3_t myEye, tOrigin; 
	VectorCopy(ent->client->ps.origin, myEye); myEye[2] += ent->client->ps.viewheight;
	VectorCopy(target->client->ps.origin, tOrigin);

	float projVel = 0;
	qboolean isSplash = qfalse, needsSync = qfalse, isBallistic = qfalse;
	float z_boost = 0.0f;
	int weapon = ent->client->ps.weapon;
	qboolean altFire = (ent->client->pers.cmd.buttons & BUTTON_ALT_ATTACK);
	qboolean synced = qtrue;

	switch (weapon) {
	case WP_BRYAR_PISTOL: 
		projVel = 1600.0f; // Engine defines BRYAR_PISTOL_VEL as 1600 for both primary and alt-fire
		break;
	case WP_BLASTER: 
		projVel = 2300.0f; 
		break; 
	case WP_BOWCASTER: 
		projVel = 1300.0f; 
		break;
	case WP_REPEATER: 
		if (altFire) { projVel = 1100.0f; isSplash = qtrue; needsSync = qtrue; isBallistic = qtrue; z_boost = 40.0f; }
		else projVel = 1600.0f; 
		break; 
	case WP_DEMP2: 
		projVel = 1800.0f; 
		break;
	case WP_ROCKET_LAUNCHER: 
		projVel = 900.0f; isSplash = qtrue; needsSync = qtrue;
		break; 
	case WP_FLECHETTE: 
		if (altFire) { projVel = 1050.0f; isSplash = qtrue; needsSync = qtrue; isBallistic = qtrue; z_boost = 185.0f; /* ~10 deg pitch up */ }
		else projVel = 3500.0f; 
		break;
	case WP_CONCUSSION: 
		projVel = 3000.0f; isSplash = qtrue; needsSync = qtrue;
		break;
	case WP_THERMAL:
		projVel = altFire ? 600.0f : 135.0f; isSplash = qtrue; needsSync = qtrue; isBallistic = qtrue; z_boost = 120.0f;
		break;
	}

	if (projVel <= 0) {
		VectorCopy(tOrigin, out_leadPos);
		out_leadPos[2] += target->client->ps.viewheight; // Aim at head for hitscan
		if (doTelemetry) {
			Com_sprintf(bot2_states[ent->s.number].tele_lastAimString, sizeof(bot2_states[ent->s.number].tele_lastAimString),
				"[%s | FIRED %d] Tgt: %s | Hitscan", ent->client->pers.netname, weapon, target->client->pers.netname);
		}
		return qtrue;
	}

	// Calculate torso offset: The geometric center of the bounding box.
	float torsoOffset = (target->r.maxs[2] + target->r.mins[2]) * 0.5f;

	// Iterative Prediction (2-pass)
	vec3_t predictedPos;
	VectorCopy(tOrigin, predictedPos);
	predictedPos[2] += torsoOffset;

	float dist = Distance(myEye, predictedPos);
	float timeOfFlight = (dist / projVel);
	if (timeOfFlight > 1.5f) timeOfFlight = 1.5f; // Safety Cap

	// Pass 1
	VectorMA(predictedPos, timeOfFlight, target->client->ps.velocity, predictedPos);

	// Trace to find floor for clamping gravity drop (Future Floor Trace)
	float maxDrop = 99999.0f;
	if (target->client->ps.groundEntityNum == ENTITYNUM_NONE) {
		vec3_t traceOrigin; VectorCopy(predictedPos, traceOrigin);
		traceOrigin[2] -= torsoOffset; // Revert to entity origin height for the trace
		
		vec3_t traceEnd; VectorCopy(traceOrigin, traceEnd); traceEnd[2] -= 8192.0f;
		trace_t tr; trap->Trace(&tr, traceOrigin, target->r.mins, target->r.maxs, traceEnd, target->s.number, MASK_SOLID, qfalse, 0, 0);
		maxDrop = traceOrigin[2] - tr.endpos[2];
		if (maxDrop < 0.0f) maxDrop = 0.0f;
	}

	if (target->client->ps.groundEntityNum == ENTITYNUM_NONE) {
		float drop = 0.5f * 800.0f * timeOfFlight * timeOfFlight;
		if (drop > maxDrop) drop = maxDrop;
		predictedPos[2] -= drop;
	}

	// Pass 2
	dist = Distance(myEye, predictedPos);
	timeOfFlight = (dist / projVel);
	if (timeOfFlight > 1.5f) timeOfFlight = 1.5f;
	
	VectorCopy(tOrigin, predictedPos);
	predictedPos[2] += torsoOffset;
	
	VectorMA(predictedPos, timeOfFlight, target->client->ps.velocity, predictedPos);
	if (target->client->ps.groundEntityNum == ENTITYNUM_NONE) {
		float drop = 0.5f * 800.0f * timeOfFlight * timeOfFlight;
		if (drop > maxDrop) drop = maxDrop;
		predictedPos[2] -= drop;
	}

	// Synchronization & Splash Logic
	float fallTime = 0.0f;
	if (needsSync) {
		if (target->client->ps.groundEntityNum != ENTITYNUM_NONE) {
			// Grounded Target: Aim at floor only for splash weapons
			if (isSplash) predictedPos[2] = tOrigin[2] + target->r.mins[2] - 4.0f;
		} else {
			// Airborne Target
			float gravity = 800.0f;
			if (target->client->ps.velocity[2] < 0) {
				vec3_t traceEnd; VectorCopy(predictedPos, traceEnd); traceEnd[2] -= 1024.0f;
				trace_t tr; trap->Trace(&tr, predictedPos, NULL, NULL, traceEnd, target->s.number, MASK_SOLID, qfalse, 0, 0);
				if (tr.fraction < 1.0f) {
					float dropDist = predictedPos[2] - tr.endpos[2];
					float vz = -target->client->ps.velocity[2];
					fallTime = (vz + sqrt(vz * vz + 2.0f * gravity * dropDist)) / gravity;
					
					if (timeOfFlight < fallTime) synced = qfalse;

					if (dropDist > 128.0f) { // High Airborne
						if (timeOfFlight < fallTime) {
							// If it's a splash weapon, we still aim at feet to be safe
							if (isSplash) predictedPos[2] -= 24.0f; 
						} else {
							// Aim at landing spot
							if (isSplash) VectorCopy(tr.endpos, predictedPos);
							else predictedPos[2] = tr.endpos[2] - target->r.mins[2] + torsoOffset;
						}
					} else {
						// Predictable landing: Aim at floor if splash, else stay on chest unless they land first
						if (isSplash) {
							VectorCopy(tr.endpos, predictedPos);
						} else if (timeOfFlight >= fallTime) {
							predictedPos[2] = tr.endpos[2] - target->r.mins[2] + torsoOffset;
						}
					}
				}
			} else {
				// Rising: aim at feet only if splash
				if (isSplash) predictedPos[2] -= 24.0f;
			}
		}
	}

	// Ballistic Arc Compensation: Aim higher to account for gravity drop over time
	float gDrop = 0;
	if (isBallistic) {
		gDrop = 0.5f * 800.0f * timeOfFlight * timeOfFlight;
		float zComp = z_boost * timeOfFlight;
		predictedPos[2] += (gDrop - zComp);
	}

	if (doTelemetry) {
		Com_sprintf(bot2_states[ent->s.number].tele_lastAimString, sizeof(bot2_states[ent->s.number].tele_lastAimString),
			"[%s | FIRED %d] Tgt: %s | Dist: %.1f | ToF: %.2fs | Fall: %.2fs | Z-Adj: %+.1f | %s",
			ent->client->pers.netname, weapon, target->client->pers.netname, dist, timeOfFlight, fallTime, 
			(isBallistic ? gDrop : 0.0f), (synced ? "SYNCED" : "UNSYNCED"));
	}

	VectorCopy(predictedPos, out_leadPos);
	return synced;
}

static void Bot2_ApplySmoothing(int clientNum, vec3_t desiredAngles, usercmd_t* ucmd) {
	gentity_t* ent = &g_entities[clientNum];
	bot2_state_t* bs = &bot2_states[clientNum];

	if (bs->lastAimTime == 0) {
		VectorCopy(ent->client->ps.viewangles, bs->lastViewAngles);
	}

	vec3_t finalAngles;
	
	// BASE CASE: Instant Snap
	finalAngles[PITCH] = AngleNormalize360(desiredAngles[PITCH]);
	finalAngles[YAW] = AngleNormalize360(desiredAngles[YAW]);
	finalAngles[ROLL] = 0;

	ucmd->angles[PITCH] = ANGLE2SHORT(finalAngles[PITCH]) - ent->client->ps.delta_angles[PITCH];
	ucmd->angles[YAW] = ANGLE2SHORT(finalAngles[YAW]) - ent->client->ps.delta_angles[YAW];

	VectorCopy(finalAngles, bs->lastViewAngles);
	bs->lastAimTime = level.time;
}

// ==============================================================================
// Driver: Executes Macro Intent via Physical Engine
// ==============================================================================
void Bot2_ExecuteMovement(int clientNum, usercmd_t* ucmd, vec3_t targetOrigin, vec3_t aimDir, qboolean lockAim, qboolean wantsSpeed, qboolean hasFallback, vec3_t fallbackOrigin) {
	gentity_t* ent = &g_entities[clientNum];
	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
	float vel_x = ent->client->ps.velocity[0], vel_y = ent->client->ps.velocity[1], vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));

	vec3_t dir, angles, nextWp = { 0,0,0 }; qboolean validPath = qfalse; float base_target_yaw;
	float distToTarget = VectorLength(targetOrigin) > 1.0f ? Distance(targetOrigin, ent->client->ps.origin) : 999999.0f;

	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0; 
		vectoangles(aimDir, angles);
		Bot2_ApplySmoothing(clientNum, angles, ucmd);
		return;
	}

	if (VectorLength(targetOrigin) > 1.0f) {
		if (NavMesh_GetNextWaypoint(ent->s.number, ent->client->ps.origin, targetOrigin, nextWp) && (VectorSubtract(nextWp, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) validPath = qtrue;
		else if (hasFallback) {
			VectorCopy(fallbackOrigin, targetOrigin);
			if (NavMesh_GetNextWaypoint(ent->s.number, ent->client->ps.origin, targetOrigin, nextWp) && (VectorSubtract(nextWp, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) validPath = qtrue;
		}
	}

	base_target_yaw = validPath ? (vectoangles(dir, angles), angles[YAW]) : bot2_states[clientNum].targetYaw;
	float distToWp = validPath ? Distance(ent->client->ps.origin, nextWp) : distToTarget;

	if (IsInTriggerPush(ent) && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 3;
		bot2_states[clientNum].stateTimer = level.time;

		if (validPath) {
			vec3_t waypointAfter;
			if (NavMesh_GetNextWaypoint(ent->s.number, nextWp, targetOrigin, waypointAfter)) {
				VectorCopy(waypointAfter, bot2_states[clientNum].tele_predPos);
			}
			else {
				VectorCopy(nextWp, bot2_states[clientNum].tele_predPos);
			}
		}
		else {
			VectorCopy(targetOrigin, bot2_states[clientNum].tele_predPos);
		}

		Bot2_PrintTelemetry(1,"[%s] STATE Transition: Entered Jump Pad (State 3) - Locked Target {%.0f, %.0f, %.0f}\n",
			ent->client->pers.netname, bot2_states[clientNum].tele_predPos[0], bot2_states[clientNum].tele_predPos[1], bot2_states[clientNum].tele_predPos[2]);
	}
	if (ent->client->ps.groundEntityNum == ENTITYNUM_NONE && bot2_states[clientNum].state != 2 && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 2; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw; bot2_states[clientNum].strafeDir = EvaluateStrafeDir(ent, base_target_yaw);
		Bot2_PrintTelemetry(1,"[%s] STATE Transition: Walking -> Falling/Airborne (State 2)\n", ent->client->pers.netname);
	}

	// --- STATE 3: JUMP PAD / PUSHED ---
	if (bot2_states[clientNum].state == 3) {

		vec3_t padDir, padAngles;
		VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, padDir);
		vectoangles(padDir, padAngles);
		
		vec3_t desiredAngles;
		if (lockAim) {
			vectoangles(aimDir, desiredAngles);
		}
		else {
			VectorCopy(padAngles, desiredAngles);
		}

		Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd);

		ucmd->forwardmove = 127;
		ucmd->rightmove = ucmd->upmove = 0;

		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 500)) {
			bot2_states[clientNum].state = 0;
			Bot2_PrintTelemetry(1,"[%s] STATE Transition: Landed from Jump Pad -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname);
		}
	}
	// --- STATE 0: WALK/IDLE & RUNNING JUMPS ---
	else if (bot2_states[clientNum].state == 0) {
		bot2_states[clientNum].tele_jumpSeq = 0; float bDist = 32.0f, dYaw = base_target_yaw; qboolean lDang = qfalse;

		if (current_speed > 100.0f && TraceFloorScore(ent, ent->client->ps.origin, vel_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = vel_yaw; }
		else if (TraceFloorScore(ent, ent->client->ps.origin, base_target_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = base_target_yaw; }

		if (lDang && !bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 1; Bot2_PrintTelemetry(1,"[%s] STATE Event: Ledge Danger Detected! Evading...\n", ent->client->pers.netname); }
		else if (!lDang && bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 0; Bot2_PrintTelemetry(1,"[%s] STATE Event: Ledge Clear. Resuming normal pathing.\n", ent->client->pers.netname); }

		qboolean jExec = qfalse;

		// --- DIAGNOSTIC GATEKEEPER (STATE 0) ---
		if (wantsSpeed) {
			if (current_speed < max_run_speed - 35.0f) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Speed too low for running jump", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else if (distToTarget <= 400.0f) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Target too close (< 300 units)", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else if (lDang) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Ledge danger detected ahead", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else {
				int pDir = EvaluateStrafeDir(ent, base_target_yaw);
				int tD[] = { pDir, pDir * -1 }; //, pDir * 2, pDir * -2 };

				if (level.time - bot2_states[clientNum].jumpRetryTimer > 250) {
					int best_d = -1;
					float best_score = 9999999.0f;
					vec3_t best_predLandPos;
					int best_frac = 0;

					for (int d = 0; d < 2; d++) {
						vec3_t predLandPos;
						float chain_dist = 9999999.0f;
						int chain_frac = 0;

						if (IsSafeToJump(ent, clientNum, ent->client->ps.origin, current_speed, vel_yaw, tD[d], max_run_speed, bot2_states[clientNum].lastFailReason, NULL, predLandPos, NULL, targetOrigin, &chain_dist, &chain_frac, lockAim, aimDir)) {
							qboolean goodJump = qtrue;
							float jumpDist = Distance(ent->client->ps.origin, predLandPos);

							if (jumpDist > distToWp) {
								if (validPath && Distance(nextWp, targetOrigin) < 32.0f) {
									goodJump = qfalse;
									Q_strncpyz(bot2_states[clientNum].lastFailReason, "Pathing: Overshooting final flagstand", sizeof(bot2_states[clientNum].lastFailReason));
								}
								else if (validPath) {
									vec3_t vecOvershoot, vecIdeal;
									VectorSubtract(predLandPos, nextWp, vecOvershoot);
									VectorNormalize(vecOvershoot);
									VectorSubtract(targetOrigin, nextWp, vecIdeal);
									VectorNormalize(vecIdeal);

									if (DotProduct(vecOvershoot, vecIdeal) < 0.0f) {
										goodJump = qfalse;
										Q_strncpyz(bot2_states[clientNum].lastFailReason, "Pathing: Overshoot turn too sharp (Dot < 0.0)", sizeof(bot2_states[clientNum].lastFailReason));
									}
								}
							}

							if (goodJump && chain_dist < best_score) {
								best_score = chain_dist;
								best_d = d;
								best_frac = chain_frac;
								VectorCopy(predLandPos, best_predLandPos);
							}
						}
					}
					
					if (best_d != -1) {
						bot2_states[clientNum].state = 2; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw;
						bot2_states[clientNum].strafeDir = tD[best_d];
						ucmd->upmove = 127; jExec = qtrue;
						bot2_states[clientNum].lastFailReason[0] = '\0';
						
						bot2_states[clientNum].tele_jumpSeq++;
						bot2_states[clientNum].tele_jumpStartTime = level.time;
						bot2_states[clientNum].tele_crossedZ = qfalse;
						bot2_states[clientNum].tele_predDir = tD[best_d];
						bot2_states[clientNum].tele_takeoffYaw = vel_yaw;
						VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
						VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);
						bot2_states[clientNum].tele_takeoffSpd = current_speed;
						bot2_states[clientNum].tele_inAir = 1;
						VectorCopy(best_predLandPos, bot2_states[clientNum].tele_predPos);
						VectorCopy(best_predLandPos, bot2_states[clientNum].tele_pmovePredPos);
						float dx = best_predLandPos[0] - ent->client->ps.origin[0], dy = best_predLandPos[1] - ent->client->ps.origin[1];
						bot2_states[clientNum].tele_predDist = sqrt((dx * dx) + (dy * dy));

						// Record winning angle fraction and update cumulative histogram
						bot2_states[clientNum].tele_lastChainFrac = best_frac;
						bot2_states[clientNum].tele_chainFracCounts[best_frac]++;
						{
							int* fc = bot2_states[clientNum].tele_chainFracCounts;
							int total = fc[0]+fc[1];
							Bot2_PrintTelemetry(1,"[%s] STATE Transition: Walking -> Executing Running Jump (State 2) | Chain Score: %.1f | AngleFrac: %s\n",
								ent->client->pers.netname, best_score, best_frac == 0 ? "boundary" : "optimal");
							Bot2_PrintTelemetry(1,"[%s] CHAIN AngleFrac Hist | boundary=%.0f%% optimal=%.0f%% | counts: %d %d\n",
								ent->client->pers.netname,
								total > 0 ? 100.0f*fc[0]/total : 0.0f,
								total > 0 ? 100.0f*fc[1]/total : 0.0f,
								fc[0], fc[1]);
						}
					} else {
						bot2_states[clientNum].jumpRetryTimer = level.time;
					}
				}
			}

			if (!jExec && level.time - bot2_states[clientNum].diagTimer > 1000) {
				Bot2_PrintTelemetry(1,"[%s] Jump Refused: %s\n", ent->client->pers.netname, bot2_states[clientNum].lastFailReason);
				bot2_states[clientNum].diagTimer = level.time;
			}
		}

		if (!jExec) {
			float safeYaw; qboolean hasEsc = lDang ? GetSafeEscapeYaw(ent, dYaw, &safeYaw) : qfalse;
			vec3_t desiredAngles = { 0, 0, 0 };

			if (lockAim) {
				float mYaw = lDang ? (hasEsc ? safeYaw : dYaw) : base_target_yaw; 
				vectoangles(aimDir, desiredAngles);
				
				if (lDang && !hasEsc) {
					ucmd->forwardmove = ucmd->rightmove = 0;
				} else if (current_speed <= max_run_speed + 10.0f) { 
					float df = (mYaw - desiredAngles[YAW]) * (M_PI / 180.0f); 
					ucmd->forwardmove = (char)max(-127, min(127, 127.0f * cos(df))); 
					ucmd->rightmove = (char)max(-127, min(127, 127.0f * -sin(df))); 
				} else {
					ucmd->forwardmove = ucmd->rightmove = 0;
				}
			}
			else {
				desiredAngles[PITCH] = 0;
				if (lDang) { 
					desiredAngles[YAW] = hasEsc ? safeYaw : dYaw; 
					ucmd->forwardmove = hasEsc ? 127 : 0; ucmd->rightmove = 0; 
				}
				else { 
					desiredAngles[YAW] = base_target_yaw; 
					ucmd->forwardmove = (current_speed > max_run_speed + 10.0f) ? 0 : 127; ucmd->rightmove = 0; 
				}
			}
			Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd);
			ucmd->upmove = 0;
		}
	}
	// --- STATE 2: AIRBORNE ---
	else if (bot2_states[clientNum].state == 2) {
		int rawDir = bot2_states[clientNum].strafeDir;
		int sDir = (rawDir > 0) ? 1 : -1;
		qboolean isHardStrafe = (abs(rawDir) == 2);

		const char* keyStr = "W";
		if (!isHardStrafe) {
			keyStr = (sDir == 1) ? "W+D" : "W+A";
		}
		else {
			keyStr = (sDir == 1) ? "Just D" : "Just A";
		}

		float mA = (current_speed > max_run_speed - 15.0f) ? acos(max(-1.0f, min(1.0f, (max_run_speed - 15.0f) / current_speed))) * (180.0f / M_PI) : 0.0f;

		// THE FIX: If the prediction asked for a speed-bleeding hook, FORCE the execution to hook harder!
		if (isHardStrafe) {
			mA = min(80.0f, mA * 1.5f);
		}

		float keyOffset = isHardStrafe ? 90.0f : 45.0f;
		float tYaw = AngleMod(vel_yaw - ((mA - keyOffset) * sDir));

		vec3_t desiredAngles;
		if (lockAim) {
			vectoangles(aimDir, desiredAngles);
			float tDiff = (tYaw - desiredAngles[YAW]) * (M_PI / 180.0f);
			ucmd->forwardmove = (char)((isHardStrafe ? 0.0f : 127.0f) * cos(tDiff));
			ucmd->rightmove = (char)(127.0f * -sin(tDiff));
		}
		else {
			desiredAngles[PITCH] = 0;
			desiredAngles[YAW] = tYaw;
			ucmd->forwardmove = isHardStrafe ? 0 : 127;
			ucmd->rightmove = 127 * sDir;
		}
		Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd);

		ucmd->upmove = (level.time - bot2_states[clientNum].stateTimer < 100) ? 127 : 0;

		// --- MID-AIR TELEMETRY: Z-CROSSING SNAPSHOT ---
		if (bot2_states[clientNum].tele_inAir && !bot2_states[clientNum].tele_crossedZ &&
			(level.time - bot2_states[clientNum].tele_jumpStartTime > 150) &&
			ent->client->ps.velocity[2] <= 0.0f) {
			if (ent->client->ps.origin[2] <= bot2_states[clientNum].tele_predPos[2]) {
				bot2_states[clientNum].tele_crossedZ = qtrue;

				vec3_t cPos; VectorCopy(ent->client->ps.origin, cPos);
				float dx = cPos[0] - bot2_states[clientNum].tele_startPos[0], dy = cPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy));

				// Calculate PMove Predicted Distance
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0], pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));
				float pm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2], aZDrop = bot2_states[clientNum].tele_startPos[2] - cPos[2];

				VectorCopy(cPos, bot2_states[clientNum].tele_crossPos);

				Bot2_PrintTelemetry(1,"[%s] Z-CROSS %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime, bot2_states[clientNum].tele_takeoffSpd,
					aDist, pMoveDist, aZDrop, pm_ZDrop, cPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], cPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr);
			}
		}

		// --- LANDING DETECTION (STATE 2 -> STATE 0) ---
		// Guard with 150ms minimum: prevents false triggers on the same tick jump is initiated
		// (groundEntityNum is still set for 1-2 ticks before physics lifts the bot off)
		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 150)) {
			if (bot2_states[clientNum].tele_inAir) {
				vec3_t lPos; VectorCopy(ent->client->ps.origin, lPos);
				float ldx = lPos[0] - bot2_states[clientNum].tele_startPos[0], ldy = lPos[1] - bot2_states[clientNum].tele_startPos[1];
				float laDist = sqrt((ldx * ldx) + (ldy * ldy));
				float lpm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0], lpm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float lpMoveDist = sqrt((lpm_dx * lpm_dx) + (lpm_dy * lpm_dy));
				float laZDrop = bot2_states[clientNum].tele_startPos[2] - lPos[2];
				float lpm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2];

				Bot2_PrintTelemetry(1,"[%s] LAND %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime, bot2_states[clientNum].tele_takeoffSpd,
					laDist, lpMoveDist, laZDrop, lpm_ZDrop, lPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], lPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr);

				bot2_states[clientNum].tele_inAir = 0;
			}
			bot2_states[clientNum].state = 0;
			bot2_states[clientNum].stateTimer = level.time;
		}
	}
}
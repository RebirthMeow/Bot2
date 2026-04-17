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
qboolean SimulatePmoveTrajectory(gentity_t* ent, float start_yaw, int strafeDir, float max_run_speed, vec3_t out_pmove_land_pos) {
	if (!ent || !ent->inuse || !ent->client || level.intermissiontime || level.time == 0) return qfalse;
	if (!g_entities) return qfalse;

	BotBreadcrumb("SimulatePmoveTrajectory START - ClientNum: %d", ent->s.number);

	pmove_t sim_pm;
	playerState_t dummy_ps;
	usercmd_t dummy_cmd;

	BotBreadcrumb("Copying dummy_ps...");
	memcpy(&dummy_ps, &ent->client->ps, sizeof(playerState_t));
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

	int max_sim_ticks = 2500 / sim_pm.pmove_msec;
	qboolean landed = qfalse;
	int sim_time = dummy_ps.commandTime;
	float start_z = dummy_ps.origin[2];

	BotBreadcrumb("Entering tick loop. max_sim_ticks = %d", max_sim_ticks);

	for (int i = 0; i < max_sim_ticks; i++) {
		sim_time += sim_pm.pmove_msec;
		dummy_cmd.serverTime = sim_time;

		int actualDir = (strafeDir > 0) ? 1 : -1;
		qboolean isHardStrafe = (abs(strafeDir) == 2);

		// THE FIX: Hold jump for exactly 100ms, mirroring State 2 execution!
		dummy_cmd.upmove = ((i * sim_pm.pmove_msec) < 100) ? 127 : 0;
		dummy_cmd.forwardmove = isHardStrafe ? 0 : 127;
		dummy_cmd.rightmove = 127 * actualDir;

		float sim_vel_x = dummy_ps.velocity[0];
		float sim_vel_y = dummy_ps.velocity[1];
		float sim_current_speed = sqrt((sim_vel_x * sim_vel_x) + (sim_vel_y * sim_vel_y));
		float sim_vel_yaw = atan2(sim_vel_y, sim_vel_x) * (180.0f / M_PI);

		float magic_angle = 0.0f;
		if (sim_current_speed > max_run_speed - 15.0f) {
			float acos_val = (max_run_speed - 15.0f) / sim_current_speed;
			magic_angle = acos((acos_val < -1.0f) ? -1.0f : ((acos_val > 1.0f) ? 1.0f : acos_val)) * (180.0f / M_PI);
		}

		if (isHardStrafe) {
			magic_angle = min(80.0f, magic_angle * 1.5f);
		}
		float keyOffset = isHardStrafe ? 90.0f : 45.0f;
		float desired_yaw = AngleMod(sim_vel_yaw - ((magic_angle - keyOffset) * actualDir));

		dummy_cmd.angles[YAW] = ANGLE2SHORT(desired_yaw);
		dummy_cmd.angles[PITCH] = 0;
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

	BotBreadcrumb("SimulatePmoveTrajectory DONE");
	return landed;
}

// ==============================================================================
// Kinematic Math & Jump Arc Prediction
// ==============================================================================
qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed) {
	vec3_t pmove_land;

	if (!SimulatePmoveTrajectory(ent, vel_yaw, testDir, max_run_speed, pmove_land)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Trajectory aborted (hit wall, hurt, or pit)", 128);
		return qfalse;
	}

	// Validate landing spot
	if (!NavMesh_IsPointOnMesh(pmove_land)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Landing spot is NOT on the NavMesh", 128);
		return qfalse;
	}

	if (out_land_pos) VectorCopy(pmove_land, out_land_pos);
	if (out_land_speed) *out_land_speed = max_run_speed;

	bot2_states[clientNum].tele_jumpSeq++;
	bot2_states[clientNum].tele_jumpStartTime = level.time;
	bot2_states[clientNum].tele_crossedZ = qfalse;
	bot2_states[clientNum].tele_predDir = testDir;
	bot2_states[clientNum].tele_takeoffYaw = vel_yaw;

	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);

	bot2_states[clientNum].tele_takeoffSpd = current_speed;
	bot2_states[clientNum].tele_inAir = 1;

	// Populate telemetry with PMove's ground truth so the logs keep working
	VectorCopy(pmove_land, bot2_states[clientNum].tele_predPos);
	VectorCopy(pmove_land, bot2_states[clientNum].tele_pmovePredPos);

	float dx = pmove_land[0] - start[0], dy = pmove_land[1] - start[1];
	bot2_states[clientNum].tele_predDist = sqrt((dx * dx) + (dy * dy));

	return qtrue;
}

void Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos) {
	if (!target || !target->client) { VectorCopy(target->s.origin, out_leadPos); return; }
	vec3_t myEye, tHead; VectorCopy(ent->client->ps.origin, myEye); myEye[2] += ent->client->ps.viewheight;
	VectorCopy(target->client->ps.origin, tHead); tHead[2] += target->client->ps.viewheight;

	float projVel = 0;
	switch (ent->client->ps.weapon) {
	case WP_BLASTER: projVel = 2300.0f; break; case WP_BOWCASTER: projVel = 1300.0f; break;
	case WP_REPEATER: projVel = 1600.0f; break; case WP_DEMP2: projVel = 1800.0f; break;
	case WP_ROCKET_LAUNCHER: projVel = 900.0f; break; case WP_FLECHETTE: projVel = 3500.0f; break;
	case WP_CONCUSSION: projVel = 3000.0f; break;
	}

	if (projVel <= 0) VectorCopy(tHead, out_leadPos); // Hitscan/Melee
	else VectorMA(tHead, Distance(myEye, tHead) / projVel, target->client->ps.velocity, out_leadPos);
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
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0; vectoangles(aimDir, angles);
		ucmd->angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW]; ucmd->angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
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

		trap->Print("[%s] STATE Transition: Entered Jump Pad (State 3) - Locked Target {%.0f, %.0f, %.0f}\n",
			ent->client->pers.netname, bot2_states[clientNum].tele_predPos[0], bot2_states[clientNum].tele_predPos[1], bot2_states[clientNum].tele_predPos[2]);
	}
	if (ent->client->ps.groundEntityNum == ENTITYNUM_NONE && bot2_states[clientNum].state != 2 && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 2; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw; bot2_states[clientNum].strafeDir = EvaluateStrafeDir(ent, base_target_yaw);
		trap->Print("[%s] STATE Transition: Walking -> Falling/Airborne (State 2)\n", ent->client->pers.netname);
	}

	// --- STATE 3: JUMP PAD / PUSHED ---
	if (bot2_states[clientNum].state == 3) {

		vec3_t padDir, padAngles;
		VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, padDir);
		vectoangles(padDir, padAngles);
		float lookYaw = padAngles[YAW];

		if (lockAim) {
			vectoangles(aimDir, angles);
			lookYaw = angles[YAW];
			ucmd->angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		}
		else {
			ucmd->angles[PITCH] = ANGLE2SHORT(padAngles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		}

		ucmd->angles[YAW] = ANGLE2SHORT(lookYaw) - ent->client->ps.delta_angles[YAW];

		ucmd->forwardmove = 127;
		ucmd->rightmove = ucmd->upmove = 0;

		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 500)) {
			bot2_states[clientNum].state = 0;
			trap->Print("[%s] STATE Transition: Landed from Jump Pad -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname);
		}
	}
	// --- STATE 0: WALK/IDLE & RUNNING JUMPS ---
	else if (bot2_states[clientNum].state == 0) {
		bot2_states[clientNum].tele_jumpSeq = 0; float bDist = 64.0f + (current_speed * 0.0f), dYaw = base_target_yaw; qboolean lDang = qfalse;

		if (current_speed > 100.0f && TraceFloorScore(ent, ent->client->ps.origin, vel_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = vel_yaw; }
		else if (TraceFloorScore(ent, ent->client->ps.origin, base_target_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = base_target_yaw; }

		if (lDang && !bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 1; trap->Print("[%s] STATE Event: Ledge Danger Detected! Evading...\n", ent->client->pers.netname); }
		else if (!lDang && bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 0; trap->Print("[%s] STATE Event: Ledge Clear. Resuming normal pathing.\n", ent->client->pers.netname); }

		float aD = vel_yaw - base_target_yaw; while (aD > 180.0f) aD -= 360.0f; while (aD < -180.0f) aD += 360.0f; aD = fabs(aD);
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
			else if (aD >= 45.0f) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Not facing target (aD >= 45.0)", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else {
				int pDir = EvaluateStrafeDir(ent, base_target_yaw);
				int tD[] = { pDir, pDir * 2, pDir * -1, pDir * -2 };

				if (level.time - bot2_states[clientNum].jumpRetryTimer > 250) {
					for (int d = 0; d < 4; d++) {
						vec3_t predLandPos;

						if (IsSafeToJump(ent, clientNum, ent->client->ps.origin, current_speed, vel_yaw, tD[d], max_run_speed, bot2_states[clientNum].lastFailReason, NULL, predLandPos, NULL)) {
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

							if (goodJump) {
								bot2_states[clientNum].state = 2; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw;
								bot2_states[clientNum].strafeDir = tD[d];
								ucmd->upmove = 127; jExec = qtrue;
								bot2_states[clientNum].lastFailReason[0] = '\0';
								trap->Print("[%s] STATE Transition: Walking -> Executing Running Jump (State 2)\n", ent->client->pers.netname);
								break;
							}
						}
					}
					if (!jExec) bot2_states[clientNum].jumpRetryTimer = level.time;
				}
			}

			if (!jExec && level.time - bot2_states[clientNum].diagTimer > 1000) {
				trap->Print("[%s] Jump Refused: %s\n", ent->client->pers.netname, bot2_states[clientNum].lastFailReason);
				bot2_states[clientNum].diagTimer = level.time;
			}
		}

		if (!jExec) {
			float safeYaw; qboolean hasEsc = lDang ? GetSafeEscapeYaw(ent, dYaw, &safeYaw) : qfalse;
			if (lockAim) {
				float mYaw = lDang ? (hasEsc ? safeYaw : dYaw) : base_target_yaw; vectoangles(aimDir, angles);
				ucmd->angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW]; ucmd->angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
				if (lDang && !hasEsc) ucmd->forwardmove = ucmd->rightmove = 0; else if (current_speed <= max_run_speed + 10.0f) { float df = (mYaw - angles[YAW]) * (M_PI / 180.0f); ucmd->forwardmove = (char)max(-127, min(127, 127.0f * cos(df))); ucmd->rightmove = (char)max(-127, min(127, 127.0f * -sin(df))); }
				else ucmd->forwardmove = ucmd->rightmove = 0;
			}
			else {
				ucmd->angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
				if (lDang) { ucmd->angles[YAW] = ANGLE2SHORT(hasEsc ? safeYaw : dYaw) - ent->client->ps.delta_angles[YAW]; ucmd->forwardmove = hasEsc ? 127 : 0; ucmd->rightmove = 0; }
				else { ucmd->angles[YAW] = ANGLE2SHORT(base_target_yaw) - ent->client->ps.delta_angles[YAW]; ucmd->forwardmove = (current_speed > max_run_speed + 10.0f) ? 0 : 127; ucmd->rightmove = 0; }
			}
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

		if (lockAim) {
			vectoangles(aimDir, angles);
			ucmd->angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
			ucmd->angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW];

			float tDiff = (tYaw - angles[YAW]) * (M_PI / 180.0f);
			ucmd->forwardmove = (char)((isHardStrafe ? 0.0f : 127.0f) * cos(tDiff));
			ucmd->rightmove = (char)(127.0f * -sin(tDiff));
		}
		else {
			ucmd->angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
			ucmd->angles[YAW] = ANGLE2SHORT(tYaw) - ent->client->ps.delta_angles[YAW];

			ucmd->forwardmove = isHardStrafe ? 0 : 127;
			ucmd->rightmove = 127 * sDir;
		}

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
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0];
				float pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));

				float yawD = vel_yaw - bot2_states[clientNum].tele_takeoffYaw;
				while (yawD > 180.0f) yawD -= 360.0f; while (yawD < -180.0f) yawD += 360.0f;

				trap->Print("[%s] Z-CROSS %d | Act T: %dms | Act D: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq,
					level.time - bot2_states[clientNum].tele_jumpStartTime,
					aDist, pMoveDist,
					cPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], cPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr
				);
			}
		}

		// Landing Logic
		qboolean minAirTimeMet = (level.time - bot2_states[clientNum].stateTimer > 250);
		qboolean wasJustADrop = (bot2_states[clientNum].tele_jumpSeq == 0);

		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (minAirTimeMet || wasJustADrop)) {
			trace_t lTr; vec3_t lE, zV = { 0,0,0 }; VectorCopy(ent->client->ps.origin, lE); lE[2] -= 64.0f; trap->Trace(&lTr, ent->client->ps.origin, zV, zV, lE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			float lSlope = (lTr.fraction < 1.0f) ? acos(lTr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

			if (bot2_states[clientNum].tele_inAir && ent->client) {
				bot2_states[clientNum].tele_inAir = 0;
				vec3_t aLPos; VectorCopy(ent->client->ps.origin, aLPos);
				float dx = aLPos[0] - bot2_states[clientNum].tele_startPos[0], dy = aLPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy));

				// Calculate PMove Predicted Distance & Z-Drop
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0];
				float pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));
				float pm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2];

				float aZDrop = bot2_states[clientNum].tele_startPos[2] - aLPos[2];

				trap->Print("[%s] JUMP %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq,
					level.time - bot2_states[clientNum].tele_jumpStartTime,
					bot2_states[clientNum].tele_takeoffSpd,
					aDist, pMoveDist,
					aZDrop, pm_ZDrop,
					aLPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], aLPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr
				);
			}

			int nDir = (lSlope > 5.0f) ? EvaluateStrafeDir(ent, base_target_yaw) : sDir * -1; qboolean cJmp = qfalse;
			float aD = vel_yaw - base_target_yaw; while (aD > 180.0f) aD -= 360.0f; while (aD < -180.0f) aD += 360.0f; aD = fabs(aD);

			if (wantsSpeed) {
				if (current_speed < (max_run_speed - 30.0f)) {
					// FIX: Do not blindly force cJmp = qtrue here! 
					// If we lost speed (e.g., hit a wall), drop back to State 0 to safely walk and rebuild momentum.
					Q_strncpyz(bot2_states[clientNum].lastFailReason, "Lost momentum; dropping to walk to rebuild speed", sizeof(bot2_states[clientNum].lastFailReason));
					// cJmp remains false, naturally dropping the bot to State 0.
				}
				else if (distToTarget <= 500.0f) {
					Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Target too close (< 500 units)", sizeof(bot2_states[clientNum].lastFailReason));
				}
				else if (aD > 135.0f) {
					Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Spun around too far (aD > 135)", sizeof(bot2_states[clientNum].lastFailReason));
				}
				else {
					int tD[] = { nDir, nDir * 2, nDir * -1, nDir * -2 };

					if (level.time - bot2_states[clientNum].jumpRetryTimer > 250) {
						for (int d = 0; d < 4; d++) {
							vec3_t predLandPos;
							if (IsSafeToJump(ent, clientNum, ent->client->ps.origin, current_speed, vel_yaw, tD[d], max_run_speed, bot2_states[clientNum].lastFailReason, NULL, predLandPos, NULL)) {

								qboolean goodJump = qtrue;
								float jumpDist = Distance(ent->client->ps.origin, predLandPos);

								if (jumpDist > distToWp) {
									if (validPath && Distance(nextWp, targetOrigin) < 32.0f) {
										goodJump = qfalse;
										Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Pathing - Overshooting flagstand", sizeof(bot2_states[clientNum].lastFailReason));
									}
									else if (validPath) {
										vec3_t vecOvershoot, vecIdeal;
										VectorSubtract(predLandPos, nextWp, vecOvershoot);
										VectorNormalize(vecOvershoot);
										VectorSubtract(targetOrigin, nextWp, vecIdeal);
										VectorNormalize(vecIdeal);

										if (DotProduct(vecOvershoot, vecIdeal) < 0.0f) {
											goodJump = qfalse;
											Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Pathing - Overshoot turn too sharp", sizeof(bot2_states[clientNum].lastFailReason));
										}
									}
								}

								if (goodJump) {
									bot2_states[clientNum].lastFailReason[0] = '\0';
									cJmp = qtrue; nDir = tD[d]; break;
								}
							}
						}
						if (!cJmp) bot2_states[clientNum].jumpRetryTimer = level.time;
					}
				}

				if (!cJmp && level.time - bot2_states[clientNum].diagTimer > 1000) {
					trap->Print("[%s] Chain Jump Refused: %s\n", ent->client->pers.netname, bot2_states[clientNum].lastFailReason);
					bot2_states[clientNum].diagTimer = level.time;
				}
			}

			if (cJmp) { bot2_states[clientNum].strafeDir = nDir; ucmd->upmove = 127; trap->Print("[%s] STATE Event: Executing Chain Jump (State 2)\n", ent->client->pers.netname); }
			else { bot2_states[clientNum].state = 0; trap->Print("[%s] STATE Transition: Landed -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname); }
			bot2_states[clientNum].stateTimer = level.time;
		}
	}

	// --- ANTI-SNAG ENGINE ---
	float dx = ent->client->ps.origin[0] - bot2_states[clientNum].stuck_pos[0], dy = ent->client->ps.origin[1] - bot2_states[clientNum].stuck_pos[1], dz = ent->client->ps.origin[2] - bot2_states[clientNum].stuck_pos[2];

	if ((dx * dx) + (dy * dy) + (dz * dz) > 625.0f) { VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos); bot2_states[clientNum].stuck_timer = level.time; }
	else if (distToTarget > 150.0f) {
		if (level.time - bot2_states[clientNum].stuck_timer > 5000) {
			if (bot2_states[clientNum].tele_inAir) {
				vec3_t cPos; VectorCopy(ent->client->ps.origin, cPos);
				float dx = cPos[0] - bot2_states[clientNum].tele_startPos[0], dy = cPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy));
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0], pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));
				float pm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2], aZDrop = bot2_states[clientNum].tele_startPos[2] - cPos[2];

				int rawDir = bot2_states[clientNum].strafeDir;
				int sDir = (rawDir > 0) ? 1 : -1;
				qboolean isHardStrafe = (abs(rawDir) == 2);
				const char* keyStr = isHardStrafe ? (sDir == 1 ? "Just D" : "Just A") : (sDir == 1 ? "W+D" : "W+A");

				trap->Print("[%s] FATALITY (STUCK) %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime, bot2_states[clientNum].tele_takeoffSpd,
					aDist, pMoveDist, aZDrop, pm_ZDrop, cPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], cPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr);
			}
			G_Kill(ent); return;
		}
		else if (level.time - bot2_states[clientNum].stuck_timer > 3000) {
			if (level.time - bot2_states[clientNum].unstuck_phase_timer > 400) { bot2_states[clientNum].unstuck_phase = rand() % 5; bot2_states[clientNum].unstuck_phase_timer = level.time; }
			int ph = bot2_states[clientNum].unstuck_phase;
			const char fwd[] = { -127, 127, 127, -127, 127 }, upm[] = { 127, -127, -127, 127, 127 };
			ucmd->forwardmove = fwd[ph]; ucmd->rightmove = (ph == 3) ? ((rand() % 2 == 0) ? 127 : -127) : ((ph == 1) ? 127 : ((ph == 2) ? -127 : 0)); ucmd->upmove = upm[ph];
			ucmd->angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW] + (ph * ((ph % 2 == 0) ? 15.0f : -15.0f))) - ent->client->ps.delta_angles[YAW];
			bot2_states[clientNum].state = 0;
		}
	}
	else bot2_states[clientNum].stuck_timer = level.time;
}

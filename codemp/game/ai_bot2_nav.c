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

void BotBreadcrumb(const char *format, ...) {
	va_list argptr;
	char string[1024];
	FILE *f;

	va_start(argptr, format);
	vsprintf(string, format, argptr);
	va_end(argptr);

	f = fopen("bot_breadcrumbs.txt", "a");
	if (f) {
		fprintf(f, "%s\n", string);
		fclose(f);
	}
}

// ==============================================================================
// Phantom Pmove Simulation Engine
// ==============================================================================
qboolean SimulatePmoveTrajectory(gentity_t* ent, float start_yaw, int strafeDir, float max_run_speed, vec3_t out_pmove_land_pos) {
	if (!ent || !ent->inuse || !ent->client || level.intermissiontime || level.time == 0) return qfalse;
	if (!g_entities) return qfalse;

	BotBreadcrumb("SimulatePmoveTrajectory START - ClientNum: %d", ent->s.number);

	// ==============================================================================
	// TRIPWIRE 1: Validates the new DLL is actually loaded by the game engine.
	// ==============================================================================
	// G_Error("TRIPWIRE 1: New DLL successfully loaded and SimulatePmove was reached!");

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
	VectorCopy(ent->r.mins, sim_pm.mins);
	VectorCopy(ent->r.maxs, sim_pm.maxs);
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

		// ==============================================================================
		// TRIPWIRE 2: Proves the setup survives and Pmove is about to be called safely.
		// ==============================================================================
		// G_Error("TRIPWIRE 2: Survived setup. About to run Pmove tick %d", i);

		BotBreadcrumb("Calling Pmove for tick %d...", i);
		Pmove(&sim_pm);
		BotBreadcrumb("Pmove tick %d completed.", i);

		// ==============================================================================
		// TRIPWIRE 3: Proves Pmove survived the internal engine math!
		// ==============================================================================
		// G_Error("TRIPWIRE 3: Pmove survived tick %d!", i);

		if (i > 2 && dummy_ps.groundEntityNum != ENTITYNUM_NONE) {
			landed = qtrue;
			BotBreadcrumb("Landed on tick %d.", i);
			break;
		}
		if (start_z - dummy_ps.origin[2] > 800.0f) {
			BotBreadcrumb("Fell too far on tick %d.", i);
			break;
		}
		vec3_t p_mins = { -15, -15, -24 }, p_maxs = { 15, 15, 32 };
		if (CheckForTriggerHurt(ent, dummy_ps.origin, dummy_ps.origin, p_mins, p_maxs)) {
			BotBreadcrumb("Hit trigger_hurt on tick %d.", i);
			break;
		}
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
	trace_t tr; vec3_t p_mins = { -15, -15, -24 }, p_maxs = { 15, 15, 32 }, zeroVec = { 0,0,0 };

#define TRACE_SOL(out, tr_start, tr_end) trap->Trace(&(out), tr_start, p_mins, p_maxs, tr_end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0)
#define TRACE_LINE(out, tr_start, tr_end) trap->Trace(&(out), tr_start, zeroVec, zeroVec, tr_end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0)

	vec3_t slopeEnd; VectorCopy(start, slopeEnd); slopeEnd[2] -= 64.0f;
	TRACE_LINE(tr, start, slopeEnd);
	float slopeAngle = (tr.fraction < 1.0f) ? acos(tr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	vec3_t sStart, sEnd, fwd;
	vec3_t vel_angles = { 0, vel_yaw, 0 };
	AngleVectors(vel_angles, fwd, NULL, NULL);
	VectorMA(start, 32.0f, fwd, sStart); VectorCopy(sStart, sEnd); sEnd[2] -= 64.0f;
	trace_t trAhead;
	trap->Trace(&trAhead, sStart, p_mins, p_maxs, sEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (trAhead.fraction < 1.0f && trAhead.plane.normal[2] > 0.7f) {
		float sd = trAhead.endpos[2] - start[2], ha = acos(trAhead.plane.normal[2]) * (180.0f / M_PI);
		if (ha < 5.0f && (sd > 4.0f || (sd < -4.0f && sd > -32.0f))) slopeAngle = 45.0f;
	}

	float pAir = 0.925f, rDot = 0.0f;
	if (slopeAngle > 5.0f && tr.fraction < 1.0f) {
		vec3_t flat = { cos(vel_yaw * (M_PI / 180.0f)), sin(vel_yaw * (M_PI / 180.0f)), 0 };
		rDot = (flat[0] * tr.plane.normal[0]) + (flat[1] * tr.plane.normal[1]);
	}

	int actualDir = (testDir > 0) ? 1 : -1;
	qboolean isHardStrafe = (abs(testDir) == 2);

	float jumpZ = 400.0f * pAir, jumpC = 112500.0f * pow(max_run_speed / 250.0f, 2);
	float pLandSpd = sqrt((current_speed * current_speed) + jumpC);
	float avgSpd = max((current_speed + pLandSpd) / 2.0f, max_run_speed);

	float curveBias = 1.0f + max(0.0f, (800.0f - current_speed) * 0.000045f);
	float dSec = max(26.5f - ((current_speed - max_run_speed) * 0.007f), 5.0f);

	if (isHardStrafe) {
		dSec *= 1.75f;
		avgSpd *= 0.85f;
	}

	float baseDist = avgSpd * pAir * curveBias;

	// Probe the arc in 4 segments
	vec3_t segStart; VectorCopy(start, segStart);
	segStart[2] += 18.0f;

	for (int i = 1; i <= 4; i++) {
		float fr = i * 0.20f;
		float pd = baseDist * fr; // Arc Length
		float segTime = pd / avgSpd;
		float segTurnDeg = dSec * segTime * actualDir;
		float segTurnRad = fabs(segTurnDeg) * (M_PI / 180.0f);

		// Compress Arc Length into Straight-Line Chord!
		float segChord = pd;
		if (segTurnRad > 0.01f) segChord = 2.0f * (pd / segTurnRad) * sin(segTurnRad / 2.0f);

		// The angle of the chord is exactly half the total turn angle
		float segChordAngle = (vel_yaw - (segTurnDeg * 0.5f)) * (M_PI / 180.0f);

		vec3_t p = { start[0] + cos(segChordAngle) * segChord, start[1] + sin(segChordAngle) * segChord, start[2] + 18.0f + (jumpZ * segTime) - (400.0f * segTime * segTime) };

		TRACE_SOL(tr, segStart, p);
		if (tr.fraction < 1.0f) {
			if (tr.plane.normal[2] < 0.7f) {
				if (failReason) Com_sprintf(failReason, 128, "Clipped: fr=%.2f, nZ=%.2f, startSolid=%d", tr.fraction, tr.plane.normal[2], tr.startsolid);
				return qfalse;
			}
			else { VectorCopy(tr.endpos, segStart); break; }
		}
		else { VectorCopy(p, segStart); }
	}

	// Base Prediction (Flat Ground)
	float tBase = baseDist / avgSpd;
	float bTurnDeg = dSec * tBase * actualDir;
	float bTurnRad = fabs(bTurnDeg) * (M_PI / 180.0f);

	float bChord = baseDist;
	if (bTurnRad > 0.01f) bChord = 2.0f * (baseDist / bTurnRad) * sin(bTurnRad / 2.0f);
	float bChordAngle = (vel_yaw - (bTurnDeg * 0.5f)) * (M_PI / 180.0f);

	vec3_t testP;
	testP[0] = start[0] + cos(bChordAngle) * bChord;
	testP[1] = start[1] + sin(bChordAngle) * bChord;
	testP[2] = start[2] + 18.0f + (jumpZ * tBase) - (400.0f * tBase * tBase);
	TRACE_SOL(tr, segStart, testP);

	vec3_t dnS, dnE; VectorCopy(tr.endpos, dnS); VectorCopy(dnS, dnE); dnE[2] -= 2048.0f;
	TRACE_SOL(tr, dnS, dnE);

	float drop = start[2] - tr.endpos[2], tdDist = baseDist, eDrop = 0.0f, eAir = pAir;
	float dAvgSpd = avgSpd;

	if (fabs(drop) > 8.0f) {
		eDrop = max(min(drop, 800.0f), -((pow(jumpZ, 2) / 1600.0f) - 1.0f));
		eAir = (pAir / 2.0f) + sqrt(((pow(jumpZ, 2) / 1600.0f) + eDrop) / 400.0f);
		dAvgSpd = max((current_speed + sqrt(pow(current_speed, 2) + (jumpC * (eAir / pAir)))) / 2.0f, max_run_speed);
		tdDist = (dAvgSpd * eAir) * curveBias;
	}

	float tF = eAir;

	// ANALYTICAL SLOPE INTERSECTION MATH
	if (slopeAngle > 5.0f && rDot < -0.1f) {
		float slopeRad = slopeAngle * (M_PI / 180.0f);
		float floorRiseRate = dAvgSpd * (fabs(rDot) / cos(slopeRad));

		if (jumpZ > floorRiseRate) {
			tF = (jumpZ - floorRiseRate) / 400.0f;
			tdDist = (dAvgSpd * tF) * curveBias;
		}
	}

	// Final Projected Chord Calculation
	float fTurnDeg = dSec * tF * actualDir;
	float fTurnRad = fabs(fTurnDeg) * (M_PI / 180.0f);
	float fChord = tdDist;
	if (fTurnRad > 0.01f) fChord = 2.0f * (tdDist / fTurnRad) * sin(fTurnRad / 2.0f);
	float fChordAngle = (vel_yaw - (fTurnDeg * 0.5f)) * (M_PI / 180.0f);

	float dxF = cos(fChordAngle);
	float dyF = sin(fChordAngle);

	testP[0] = start[0] + dxF * fChord;
	testP[1] = start[1] + dyF * fChord;
	testP[2] = start[2] + 18.0f + (jumpZ * tF) - (400.0f * tF * tF);

	TRACE_SOL(tr, segStart, testP);
	if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) {
		if (failReason) Com_sprintf(failReason, 128, "Final Spot Clipped: fr=%.2f, nZ=%.2f, startSolid=%d", tr.fraction, tr.plane.normal[2], tr.startsolid);
		return qfalse;
	}

	// ==========================================
	// 1. TRUE LANDING Z (Standard Box)
	// ==========================================
	VectorCopy(tr.endpos, dnS);
	VectorCopy(dnS, dnE); dnE[2] -= 2048.0f;

	trace_t trueTr;
	trap->Trace(&trueTr, dnS, p_mins, p_maxs, dnE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float flrZ = trueTr.endpos[2];
	float fDrop = start[2] - flrZ;
	float pLandSlope = (trueTr.fraction < 1.0f) ? acos(trueTr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	if (trueTr.fraction == 1.0f || fDrop > 450.0f || CheckForTriggerHurt(ent, dnS, trueTr.endpos, p_mins, p_maxs)) {
		if (failReason) Q_strncpyz(failReason, "Center landing spot is a pit or trigger_hurt", 128);
		return qfalse;
	}

	if (!NavMesh_IsPointOnMesh(trueTr.endpos)) {
		if (failReason) Q_strncpyz(failReason, "Landing spot is solid, but NOT on the NavMesh", 128);
		return qfalse;
	}

	// ==========================================
	// 1.5 KINEMATIC REALITY CHECK (The Hallucination Fix)
	// ==========================================
	float true_tF = (pAir / 2.0f) + sqrt(((pow(jumpZ, 2) / 1600.0f) + max(0.0f, fDrop)) / 400.0f);
	float true_dist = (dAvgSpd * true_tF) * curveBias;

	// tdDist is the Arc Length. We compare it to the max physical Arc Length (true_dist)
	if (tdDist > true_dist + 32.0f) {
		if (failReason) Com_sprintf(failReason, 128, "Kinematic Fall Short: Target at %.0f dist, hit Z at %.0f", tdDist, true_dist);
		return qfalse;
	}

	if (!(slopeAngle > 5.0f && rDot < -0.1f)) {
		tF = true_tF;

		// Recalculate the final chord displacement for telemetry so it perfectly aligns with reality!
		float realTurnDeg = dSec * tF * actualDir;
		float realTurnRad = fabs(realTurnDeg) * (M_PI / 180.0f);
		fChord = true_dist;
		if (realTurnRad > 0.01f) fChord = 2.0f * (true_dist / realTurnRad) * sin(realTurnRad / 2.0f);
		fChordAngle = (vel_yaw - (realTurnDeg * 0.5f)) * (M_PI / 180.0f);
		dxF = cos(fChordAngle);
		dyF = sin(fChordAngle);
	}

	// ==========================================
	// 2. TRUE LATERAL DRIFT PROBES (Swept Volume)
	// ==========================================
	float perpX = -dyF;
	float perpY = dxF;
	float driftMargin = 64.0f;
	float stepSize = 32.0f;

	for (float offset = stepSize; offset <= driftMargin; offset += stepSize) {
		vec3_t l_dnS, r_dnS, l_dnE, r_dnE;
		VectorCopy(dnS, l_dnS);
		VectorCopy(dnS, r_dnS);

		l_dnS[0] += perpX * offset; l_dnS[1] += perpY * offset;
		r_dnS[0] -= perpX * offset; r_dnS[1] -= perpY * offset;

		VectorCopy(l_dnS, l_dnE); l_dnE[2] -= 2048.0f;
		VectorCopy(r_dnS, r_dnE); r_dnE[2] -= 2048.0f;

		trace_t trL, trR;
		trap->Trace(&trL, l_dnS, p_mins, p_maxs, l_dnE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		trap->Trace(&trR, r_dnS, p_mins, p_maxs, r_dnE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		float dropL = start[2] - trL.endpos[2];
		float dropR = start[2] - trR.endpos[2];

		if (trL.fraction == 1.0f || dropL > 450.0f || CheckForTriggerHurt(ent, l_dnS, trL.endpos, p_mins, p_maxs)) {
			if (failReason) Com_sprintf(failReason, 128, "Lateral Sweep Danger: Left pit/hurt at offset %.0f", offset);
			return qfalse;
		}
		if (trR.fraction == 1.0f || dropR > 450.0f || CheckForTriggerHurt(ent, r_dnS, trR.endpos, p_mins, p_maxs)) {
			if (failReason) Com_sprintf(failReason, 128, "Lateral Sweep Danger: Right pit/hurt at offset %.0f", offset);
			return qfalse;
		}
	}

	// ==========================================
	// 3. FORWARD OVERSHOOT SWEEP (Swept Volume)
	// ==========================================
	for (float fwdOffset = 32.0f; fwdOffset <= 64.0f; fwdOffset += 32.0f) {
		vec3_t f_dnS, f_dnE;
		VectorCopy(dnS, f_dnS);
		f_dnS[0] += dxF * fwdOffset; f_dnS[1] += dyF * fwdOffset;

		VectorCopy(f_dnS, f_dnE); f_dnE[2] -= 2048.0f;

		trace_t trF;
		trap->Trace(&trF, f_dnS, p_mins, p_maxs, f_dnE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		float dropF = start[2] - trF.endpos[2];

		if (trF.fraction == 1.0f || dropF > 450.0f || CheckForTriggerHurt(ent, f_dnS, trF.endpos, p_mins, p_maxs)) {
			if (failReason) Com_sprintf(failReason, 128, "Overshoot Sweep Danger: Front pit/hurt at +%.0f", fwdOffset);
			return qfalse;
		}
	}

	if (out_land_pos) VectorCopy(tr.endpos, out_land_pos); if (out_land_speed) *out_land_speed = avgSpd;

	bot2_states[clientNum].tele_jumpSeq++; bot2_states[clientNum].tele_jumpStartTime = level.time;
	bot2_states[clientNum].tele_groundSlope = slopeAngle; bot2_states[clientNum].tele_crossedZ = qfalse;
	bot2_states[clientNum].tele_predDir = testDir; bot2_states[clientNum].tele_midAirTime = level.time + (int)((eAir * 1000.0f) * 0.5f);
	bot2_states[clientNum].tele_midAirLogged = qfalse; bot2_states[clientNum].tele_rampDot = rDot;
	bot2_states[clientNum].tele_secant = dSec; bot2_states[clientNum].tele_effDrop = eDrop;
	bot2_states[clientNum].tele_predLandSlope = pLandSlope; bot2_states[clientNum].tele_takeoffYaw = vel_yaw;
	bot2_states[clientNum].tele_predAirTime = (int)(tF * 1000.0f);

	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos); VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);

	// TELEMETRY IS NOW DRIVEN BY CHORD DISTANCE (Straight Line Displacement)
	bot2_states[clientNum].tele_predPos[0] = start[0] + (dxF * fChord);
	bot2_states[clientNum].tele_predPos[1] = start[1] + (dyF * fChord);
	bot2_states[clientNum].tele_predPos[2] = flrZ;
	bot2_states[clientNum].tele_predDist = fChord;
	bot2_states[clientNum].tele_takeoffSpd = current_speed; bot2_states[clientNum].tele_inAir = 1;

	// --- PHANTOM PMOVE SIMULATION ---
	vec3_t pmove_land;
	if (SimulatePmoveTrajectory(ent, vel_yaw, testDir, max_run_speed, pmove_land)) {
		VectorCopy(pmove_land, bot2_states[clientNum].tele_pmovePredPos);
	}
	else {
		VectorClear(bot2_states[clientNum].tele_pmovePredPos);
	}

#undef TRACE_SOL
#undef TRACE_LINE
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

				trap->Print("[%s] Z-CROSS %d | Act T: %dms (Pred: %dms) | Act D: %.1f (Math: %.1f, PM: %.1f) | Math Err: %+.1f, %+.1f | PM Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq,
					level.time - bot2_states[clientNum].tele_jumpStartTime,
					bot2_states[clientNum].tele_predAirTime,
					aDist, bot2_states[clientNum].tele_predDist, pMoveDist,
					cPos[0] - bot2_states[clientNum].tele_predPos[0], cPos[1] - bot2_states[clientNum].tele_predPos[1],
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
				float pZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_predPos[2];

				float yawD = vel_yaw - bot2_states[clientNum].tele_takeoffYaw;
				while (yawD > 180.0f) yawD -= 360.0f; while (yawD < -180.0f) yawD += 360.0f;

				trap->Print("[%s] JUMP %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Math: %.1f, PM: %.1f) | Act Z: %.1f (Math: %.1f, PM: %.1f) | Math Err: %+.1f, %+.1f | PM Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq,
					level.time - bot2_states[clientNum].tele_jumpStartTime,
					bot2_states[clientNum].tele_takeoffSpd,
					aDist, bot2_states[clientNum].tele_predDist, pMoveDist,
					aZDrop, pZDrop, pm_ZDrop,
					aLPos[0] - bot2_states[clientNum].tele_predPos[0], aLPos[1] - bot2_states[clientNum].tele_predPos[1],
					aLPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], aLPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr
				);
			}

			int nDir = (lSlope > 5.0f) ? EvaluateStrafeDir(ent, base_target_yaw) : sDir * -1; qboolean cJmp = qfalse;
			float aD = vel_yaw - base_target_yaw; while (aD > 180.0f) aD -= 360.0f; while (aD < -180.0f) aD += 360.0f; aD = fabs(aD);

			if (wantsSpeed) {
				if (current_speed < (max_run_speed - 30.0f)) {
					if (bot2_states[clientNum].tele_jumpSeq > 0) {
						bot2_states[clientNum].lastFailReason[0] = '\0';
						cJmp = qtrue;
					}
					else {
						Q_strncpyz(bot2_states[clientNum].lastFailReason, "Landed from drop; walking to build speed", sizeof(bot2_states[clientNum].lastFailReason));
					}
				}
				else if (distToTarget <= 500.0f) {
					Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Target too close (< 500 units)", sizeof(bot2_states[clientNum].lastFailReason));
				}
				else if (aD > 135.0f) {
					Q_strncpyz(bot2_states[clientNum].lastFailReason, "Chain Jump: Spun around too far (aD > 135)", sizeof(bot2_states[clientNum].lastFailReason));
				}
				else {
					int tD[] = { nDir, nDir * 2, nDir * -1, nDir * -2 };
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
		if (level.time - bot2_states[clientNum].stuck_timer > 5000) { G_Kill(ent); return; }
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

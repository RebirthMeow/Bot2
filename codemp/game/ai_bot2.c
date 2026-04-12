// ==============================================================================
// ai_bot2.c - Advanced CTF Bot Engine for Jedi Academy
// Features: Dynamic role switching, predictive jump physics, anti-snag pathing,
// predictive leading for projectiles, and granular mid-air telemetry logging.
// ==============================================================================

#include "g_local.h"
#include "bg_local.h" // Requires Pmove definitions for engine constants
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

extern void G_Kill(gentity_t* ent);

// ==============================================================================
// Enums & Structs
// ==============================================================================
// High-level CTF roles assigned dynamically based on team composition
typedef enum { ROLE_OFFENSE, ROLE_CHASE, ROLE_BASE } botRole_t;

// Macro states dictate the bot's current objective (pathing target and behavior)
typedef enum {
	MACRO_FETCH_FLAG, MACRO_RETURN_FLAG, MACRO_HUNT_TRIPMINES, MACRO_CAMP_REGEN,
	MACRO_GET_WEAPON, MACRO_DEFEND_STAND, MACRO_CHASE_THIEF, MACRO_ESCORT_FC, MACRO_SURVIVAL
} macroState_t;

// Human-readable names for telemetry prints
static const char* macroNames[] = {
	"FETCH_FLAG", "RETURN_FLAG", "HUNT_TRIPMINES", "CAMP_REGEN", "GET_WEAPON",
	"DEFEND_STAND", "CHASE_THIEF", "ESCORT_FC", "SURVIVAL"
};

// ==============================================================================
// State Machine Tracker
// ==============================================================================
static struct {
	// --- MACRO TACTICS ---
	botRole_t role;
	macroState_t macroState;
	int targetEntNum;
	vec3_t macroTargetOrigin;
	int abilityTimer;       // Throttle force powers
	int chargeTimer;        // Throttle weapon charging (e.g., Bryar)

	// --- MICRO STATE MACHINE ---
	// state: 0 = Walk/Run, 2 = Airborne/Falling, 3 = Trigger Push / Jump Pad
	int state, stateTimer, strafeDir, spawnCooldown, ledgeEvading;
	float targetYaw;

	// --- ANTI-SNAG ENGINE ---
	vec3_t stuck_pos;
	int stuck_timer, unstuck_phase, unstuck_phase_timer;

	// --- MID-AIR TELEMETRY ENGINE ---
	int tele_inAir, tele_jumpSeq, tele_jumpStartTime, tele_midAirTime, tele_predDir, tele_actDir;
	int tele_deadLogged, tele_hasFlag;
	float tele_takeoffSpd, tele_prevSpeed, tele_predDist, tele_groundSlope, tele_rampDot, tele_secant;
	float tele_effDrop, tele_predLandSlope, tele_takeoffYaw;
	vec3_t tele_startPos, tele_predPos, tele_pmovePredPos, tele_midPredPos, tele_prevPos, tele_crossPos;
	qboolean tele_crossedZ, tele_midAirLogged;

	int diagTimer; char lastFailReason[128];
} bot2_states[MAX_CLIENTS] = { 0 };

// ==============================================================================
// Environment & Entity Helpers
// ==============================================================================

// Assigns Offense, Chase, or Base roles evenly across the team
static botRole_t GetBotRole(int myClientNum, int myTeam) {
	int myRank = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == myTeam) {
			if (i == myClientNum) break;
			myRank++;
		}
	}
	int mod = myRank % 4;
	return (mod == 1) ? ROLE_CHASE : ((mod == 3) ? ROLE_BASE : ROLE_OFFENSE);
}

// Scans for the nearest item matching an array of classnames
static gentity_t* GetNearestItem(vec3_t pos, const char** classnames, int numClasses, float maxDist) {
	gentity_t* bestItem = NULL; float bestDist = maxDist;
	for (int i = 0; i < numClasses; i++) {
		gentity_t* found = NULL;
		while ((found = G_Find(found, FOFS(classname), classnames[i])) != NULL) {
			if (found->r.linked && Distance(pos, found->s.origin) < bestDist) {
				bestDist = Distance(pos, found->s.origin); bestItem = found;
			}
		}
	}
	return bestItem;
}

// Scans for the nearest living enemy player
static gentity_t* GetNearestEnemy(vec3_t pos, int myTeam, float maxDist) {
	gentity_t* bestEnemy = NULL; float bestDist = maxDist;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		gentity_t* ent = &g_entities[i];
		if (ent->inuse && ent->client && ent->health > 0 && ent->client->sess.sessionTeam != myTeam && ent->client->sess.sessionTeam != TEAM_SPECTATOR) {
			float d = Distance(pos, ent->client->ps.origin);
			if (d < bestDist) { bestDist = d; bestEnemy = ent; }
		}
	}
	return bestEnemy;
}

// Maps engine weapon enum to ammo array index
static int Bot2_GetAmmo(gentity_t* ent, int weapon) {
	switch (weapon) {
	case WP_BLASTER: case WP_BRYAR_PISTOL: return ent->client->ps.ammo[2];
	case WP_DISRUPTOR: case WP_BOWCASTER: case WP_DEMP2: return ent->client->ps.ammo[3];
	case WP_REPEATER: case WP_FLECHETTE: case WP_CONCUSSION: return ent->client->ps.ammo[4];
	case WP_ROCKET_LAUNCHER: return ent->client->ps.ammo[5];
	case WP_TRIP_MINE: return ent->client->ps.ammo[8];
	}
	return 0;
}

// Prevents bots from jumping into death pits
static qboolean CheckForTriggerHurt(gentity_t* ent, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs) {
	vec3_t cs; VectorCopy(start, cs);
	while (cs[2] > end[2]) {
		trace_t tr; trap->Trace(&tr, cs, mins, maxs, end, ent->s.number, CONTENTS_TRIGGER, qfalse, 0, 0);
		if (tr.fraction == 1.0f) return qfalse;
		if (tr.entityNum < ENTITYNUM_WORLD && Q_stricmp(g_entities[tr.entityNum].classname, "trigger_hurt") == 0) return qtrue;
		VectorCopy(tr.endpos, cs); cs[2] -= 8.0f;
	}
	return qfalse;
}

// Returns a "score" for floor safety based on tracing ahead and down
static float TraceFloorScore(gentity_t* ent, vec3_t start, float yaw, float dist, qboolean* hitWall) {
	vec3_t forward, end, downEnd, tStart; trace_t tr, trDrop;
	vec3_t mins = { -15, -15, -24 }, maxs = { 15, 15, 32 };
	AngleVectors((vec3_t) { 0, yaw, 0 }, forward, NULL, NULL);

	VectorCopy(start, tStart); tStart[2] += 24.0f;
	VectorMA(tStart, dist, forward, end);
	trap->Trace(&tr, tStart, mins, maxs, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (hitWall) *hitWall = (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f);

	float actual_dist = dist * tr.fraction;
	for (int i = 1; i <= 3; i++) {
		vec3_t probe; VectorMA(tStart, actual_dist * (i / 3.0f), forward, probe);
		VectorCopy(probe, downEnd); downEnd[2] -= 1024.0f;
		trap->Trace(&trDrop, probe, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		// If the drop is too far, or into a trigger hurt, it's unsafe (-1 score)
		if (trDrop.fraction == 1.0f || (probe[2] - trDrop.endpos[2]) > 800.0f || CheckForTriggerHurt(ent, probe, trDrop.endpos, mins, maxs)) return -1.0f;
	}
	return tr.fraction;
}

// Finds an alternate path if the direct path leads off a cliff
static qboolean GetSafeEscapeYaw(gentity_t* ent, float danger_yaw, float* safe_yaw) {
	float escape_yaw = AngleMod(danger_yaw + 180.0f); float best_score = -1.0f, best_yaw = escape_yaw;
	// Test 8 angles radiating out from the direct opposite direction
	for (int i = 0; i < 8; i++) {
		float test_yaw = AngleMod(escape_yaw + ((i % 2 == 0 ? 1.0f : -1.0f) * ((i + 1) / 2) * 45.0f));
		float score = TraceFloorScore(ent, ent->client->ps.origin, test_yaw, 128.0f, NULL);
		if (score > best_score) { best_score = score; best_yaw = test_yaw; if (best_score == 1.0f) break; }
	}
	if (best_score >= 0.0f) { if (safe_yaw) *safe_yaw = best_yaw; return qtrue; }
	return qfalse;
}

// Decide whether to strafe left (-1) or right (+1) based on floor geometry
static int EvaluateStrafeDir(gentity_t* ent, float base_target_yaw) {
	float scoreR = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw - 30.0f), 256.0f, NULL);
	float scoreL = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw + 30.0f), 256.0f, NULL);
	return (scoreL > scoreR) ? -1 : ((scoreR > scoreL) ? 1 : ((Q_irand(0, 1) == 0) ? 1 : -1));
}

// Bounding box check for jump pads
static qboolean IsInTriggerPush(gentity_t* ent) {
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
// Kinematic Math & Jump Arc Prediction
// ==============================================================================
static qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed) {
	trace_t tr; vec3_t p_mins = { -15, -15, -24 }, p_maxs = { 15, 15, 32 }, zeroVec = { 0,0,0 };

#define TRACE_SOL(out, tr_start, tr_end) trap->Trace(&(out), tr_start, p_mins, p_maxs, tr_end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0)
#define TRACE_LINE(out, tr_start, tr_end) trap->Trace(&(out), tr_start, zeroVec, zeroVec, tr_end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0)

	vec3_t slopeEnd; VectorCopy(start, slopeEnd); slopeEnd[2] -= 64.0f;
	TRACE_LINE(tr, start, slopeEnd);
	float slopeAngle = (tr.fraction < 1.0f) ? acos(tr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	vec3_t sStart, sEnd, fwd; AngleVectors((vec3_t) { 0, vel_yaw, 0 }, fwd, NULL, NULL);
	VectorMA(start, 32.0f, fwd, sStart); VectorCopy(sStart, sEnd); sEnd[2] -= 64.0f;
	TRACE_SOL(tr, sStart, sEnd);
	if (tr.fraction < 1.0f && tr.plane.normal[2] > 0.7f) {
		float sd = tr.endpos[2] - start[2], ha = acos(tr.plane.normal[2]) * (180.0f / M_PI);
		if (ha < 5.0f && (sd > 4.0f || (sd < -4.0f && sd > -32.0f))) slopeAngle = 45.0f;
	}

	float pAir = 0.925f, rDot = 0.0f;
	if (slopeAngle > 5.0f && tr.fraction < 1.0f) {
		vec3_t flat = { cos(vel_yaw * (M_PI / 180.0f)), sin(vel_yaw * (M_PI / 180.0f)), 0 };
		rDot = (flat[0] * tr.plane.normal[0]) + (flat[1] * tr.plane.normal[1]);
		pAir += (rDot < -0.1f) ? min(0.30f, slopeAngle * 0.01f * fabs(rDot)) : ((rDot > 0.1f) ? -min(0.15f, slopeAngle * 0.005f * rDot) : 0);
	}
	if (current_speed > 600.0f) pAir -= 0.02f;

	float jumpZ = 400.0f * pAir, jumpC = 112500.0f * pow(max_run_speed / 250.0f, 2);
	float pLandSpd = sqrt((current_speed * current_speed) + jumpC);
	float avgSpd = max((current_speed + pLandSpd) / 2.0f, max_run_speed);
	float baseDist = (avgSpd * pAir) * 1.025f;
	float dSec = max(28.0f - ((current_speed - max_run_speed) * 0.01f), 5.0f);

	// Probe the arc in 4 segments to ensure we don't hit our head
	vec3_t segStart; VectorCopy(start, segStart);
	segStart[2] += 18.0f; // Lift start point out of the floor

	for (int i = 1; i <= 4; i++) {
		float fr = i * 0.20f, pd = baseDist * fr, pr = (vel_yaw - (dSec * testDir * fr)) * (M_PI / 180.0f), t = pd / avgSpd;

		// Add 18.0f to the parabolic Z calculation
		vec3_t p = { start[0] + cos(pr) * pd, start[1] + sin(pr) * pd, start[2] + 18.0f + (jumpZ * t) - (400.0f * t * t) }, pDn;

		TRACE_SOL(tr, segStart, p);
		if (tr.fraction < 1.0f) {
			if (tr.plane.normal[2] < 0.7f) {
				if (failReason) {
					Com_sprintf(failReason, 128, "Clipped: fr=%.2f, nZ=%.2f, startSolid=%d", tr.fraction, tr.plane.normal[2], tr.startsolid);
				}
				return qfalse;
			} else {
				VectorCopy(tr.endpos, segStart);
				break;
			}
		} else {
			VectorCopy(p, segStart);
		}

		VectorCopy(segStart, pDn); pDn[2] -= 2048.0f;
		TRACE_SOL(tr, segStart, pDn);
		if (tr.fraction == 1.0f || (start[2] - tr.endpos[2]) > 450.0f || CheckForTriggerHurt(ent, segStart, tr.endpos, p_mins, p_maxs)) {
			if (failReason) Q_strncpyz(failReason, "Arc over death pit or trigger_hurt", 128);
			return qfalse;
		}
	}

	float bRad = (vel_yaw - ((dSec * 0.55f) * testDir)) * (M_PI / 180.0f);
	float tBase = baseDist / avgSpd;
	// Add 18.0f to testP
	vec3_t testP = { start[0] + cos(bRad) * baseDist, start[1] + sin(bRad) * baseDist, start[2] + 18.0f + (jumpZ * tBase) - (400.0f * tBase * tBase) };
	TRACE_SOL(tr, segStart, testP);

	vec3_t dnS, dnE; VectorCopy(tr.endpos, dnS); VectorCopy(dnS, dnE); dnE[2] -= 2048.0f;
	TRACE_SOL(tr, dnS, dnE);
	if (tr.fraction == 1.0f || CheckForTriggerHurt(ent, dnS, tr.endpos, p_mins, p_maxs)) {
		if (failReason) Q_strncpyz(failReason, "Base projection over death pit or trigger_hurt", 128);
		return qfalse;
	}

	float drop = start[2] - tr.endpos[2], tdDist = baseDist, eDrop = 0.0f, eAir = pAir, dxF = cos(bRad), dyF = sin(bRad);
	if (fabs(drop) > 8.0f) {
		eDrop = max(min(drop, 800.0f), -((pow(jumpZ, 2) / 1600.0f) - 1.0f));
		eAir = (pAir / 2.0f) + sqrt(((pow(jumpZ, 2) / 1600.0f) + eDrop) / 400.0f) + (eDrop * 0.0006f);
		float dAvgSpd = max((current_speed + sqrt(pow(current_speed, 2) + (jumpC * (eAir / pAir)))) / 2.0f, max_run_speed);
		tdDist = (dAvgSpd * eAir) * (1.025f + max(eAir - pAir, 0.0f) * 0.1f);

		float fRad = (vel_yaw - ((dSec * (eAir / pAir) * 0.55f) * testDir)) * (M_PI / 180.0f);
		dxF = cos(fRad); dyF = sin(fRad);
	}

	float tF = tdDist / avgSpd;
	testP[0] = start[0] + dxF * tdDist;
	testP[1] = start[1] + dyF * tdDist;
	// Add 18.0f to testP[2]
	testP[2] = start[2] + 18.0f + (jumpZ * tF) - (400.0f * tF * tF);
	TRACE_SOL(tr, segStart, testP);
	if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) {
		if (failReason) {
			Com_sprintf(failReason, 128, "Final Spot Clipped: fr=%.2f, nZ=%.2f, startSolid=%d",
				tr.fraction, tr.plane.normal[2], tr.startsolid);
		}
		return qfalse;
	}

	VectorCopy(tr.endpos, dnS); VectorCopy(dnS, dnE); dnE[2] -= 2048.0f;
	TRACE_SOL(tr, dnS, dnE);
	float fDrop = start[2] - tr.endpos[2], flrZ = tr.endpos[2], pLandSlope = (tr.fraction < 1.0f) ? acos(tr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	if ((eDrop > 64.0f && fDrop < (eDrop - 128.0f)) || tr.fraction == 1.0f || fDrop > 450.0f || CheckForTriggerHurt(ent, dnS, tr.endpos, p_mins, p_maxs)) {
		if (failReason) Q_strncpyz(failReason, "Final landing spot is lethal (Drop/Hurt)", 128);
		return qfalse;
	}

	vec3_t oP = { start[0] + dxF * (tdDist + 50.0f), start[1] + dyF * (tdDist + 50.0f), flrZ + 24.0f }, oDnE, lP;
	VectorCopy(tr.endpos, lP); lP[2] += 24.0f;
	TRACE_SOL(tr, lP, oP); VectorCopy(tr.endpos, dnS); VectorCopy(dnS, oDnE); oDnE[2] -= 2048.0f;
	TRACE_SOL(tr, dnS, oDnE);

	if (tr.fraction == 1.0f || start[2] - tr.endpos[2] > 450.0f) {
		if (failReason) Q_strncpyz(failReason, "Landing too close to cliff edge (+50 unit check)", 128);
		return qfalse;
	}
	if (-fDrop > 0.0f && tr.plane.normal[2] > 0.99f && (((jumpZ * tF) - (400.0f * tF * tF)) + 18.0f) < -fDrop) {
		if (failReason) Q_strncpyz(failReason, "Failed ledge step-up height math", 128);
		return qfalse;
	}

	if (out_land_pos) VectorCopy(tr.endpos, out_land_pos); if (out_land_speed) *out_land_speed = avgSpd;

	bot2_states[clientNum].tele_jumpSeq++; bot2_states[clientNum].tele_jumpStartTime = level.time;
	bot2_states[clientNum].tele_groundSlope = slopeAngle; bot2_states[clientNum].tele_crossedZ = qfalse;
	bot2_states[clientNum].tele_predDir = testDir; bot2_states[clientNum].tele_midAirTime = level.time + (int)((eAir * 1000.0f) * 0.5f);
	bot2_states[clientNum].tele_midAirLogged = qfalse; bot2_states[clientNum].tele_rampDot = rDot;
	bot2_states[clientNum].tele_secant = dSec; bot2_states[clientNum].tele_effDrop = eDrop;
	bot2_states[clientNum].tele_predLandSlope = pLandSlope; bot2_states[clientNum].tele_takeoffYaw = vel_yaw;

	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos); VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);
	bot2_states[clientNum].tele_predPos[0] = start[0] + (dxF * tdDist); bot2_states[clientNum].tele_predPos[1] = start[1] + (dyF * tdDist);
	bot2_states[clientNum].tele_predPos[2] = flrZ;
	bot2_states[clientNum].tele_predDist = tdDist; bot2_states[clientNum].tele_takeoffSpd = current_speed; bot2_states[clientNum].tele_inAir = 1;

#undef TRACE_SOL
#undef TRACE_LINE
	return qtrue;
}

static void Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos) {
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
// Main Bot Think Execution Engine
// ==============================================================================
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum]; usercmd_t ucmd; char serverCmd[1024];

	if (!ent || !ent->inuse || !ent->client || !botstates[clientNum]) return;
	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {}
	ent->client->inactivityTime = level.time + 1000000;

	// --- FATALITIES & RECOVERY ---
	if (ent->health <= 0) {
		bot2_states[clientNum].spawnCooldown = level.time; memset(&ucmd, 0, sizeof(ucmd)); ucmd.serverTime = time; ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		if (!bot2_states[clientNum].tele_deadLogged) {
			trap->Print("[%s] STATE Transition: Bot Killed / Respawning.\n", ent->client->pers.netname);
			bot2_states[clientNum].tele_deadLogged = 1; bot2_states[clientNum].tele_inAir = 0; bot2_states[clientNum].tele_jumpSeq = 0;
			bot2_states[clientNum].state = 0; bot2_states[clientNum].tele_hasFlag = 0; bot2_states[clientNum].stuck_timer = level.time;
			VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		}

		if (ent->client->ps.velocity[2] < -300.0f || ent->client->ps.fallingToDeath) { ent->client->respawnTime = level.time; ent->client->ps.pm_time = 0; }
		botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd); return;
	}

	bot2_states[clientNum].tele_deadLogged = 0; if (ent->client->ps.fallingToDeath) G_Kill(ent);

	// --- ROLE TAGGING ---
	bot2_states[clientNum].role = GetBotRole(clientNum, ent->client->sess.sessionTeam);
	const char* roleTags[] = { "[OFFENSE]", "[CHASE]", "[BASE]" }; char userinfo[MAX_INFO_STRING];
	trap->GetUserinfo(clientNum, userinfo, sizeof(userinfo)); char* name = Info_ValueForKey(userinfo, "name");

	if (name && !strstr(name, roleTags[bot2_states[clientNum].role]) && level.time - bot2_states[clientNum].abilityTimer > 2000) {
		char newName[MAX_NETNAME], cleanName[MAX_NETNAME]; Q_strncpyz(cleanName, name, sizeof(cleanName));
		for (int i = 0; i < 3; i++) { char* tag = strstr(cleanName, roleTags[i]); if (tag) *tag = '\0'; }
		Com_sprintf(newName, sizeof(newName), "%s%s", cleanName, roleTags[bot2_states[clientNum].role]);
		Info_SetValueForKey(userinfo, "name", newName); trap->SetUserinfo(clientNum, userinfo);
		trap->Print("[%s] Assigned New Role: %s\n", ent->client->pers.netname, newName);
	}

	// --- SYSTEM PRE-PROCESSING ---
	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
	float vel_x = ent->client->ps.velocity[0], vel_y = ent->client->ps.velocity[1], vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));

	ucmd = botstates[clientNum]->lastucmd; ucmd.serverTime = time; ucmd.buttons = 0;

	if (level.time - ent->client->pers.enterTime < 200) {
		bot2_states[clientNum].spawnCooldown = bot2_states[clientNum].stuck_timer = level.time; VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		ucmd.forwardmove = ucmd.rightmove = ucmd.upmove = 0; ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd); return;
	}

	ucmd.weapon = WP_BRYAR_PISTOL; ent->client->ps.stats[STAT_WEAPONS] |= (1 << WP_BRYAR_PISTOL); if (ent->client->ps.ammo[2] < 10) ent->client->ps.ammo[2] = 100;
	if (bot2_states[clientNum].role != ROLE_BASE) {
		const int wpn_prio[] = { WP_ROCKET_LAUNCHER, WP_FLECHETTE, WP_REPEATER, WP_CONCUSSION, WP_BOWCASTER, WP_BLASTER, WP_DEMP2 };
		for (int i = 0; i < 7; i++) {
			if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << wpn_prio[i])) && Bot2_GetAmmo(ent, wpn_prio[i]) > 0) { ucmd.weapon = wpn_prio[i]; break; }
		}
	}

	int botTeam = ent->client->sess.sessionTeam, enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
	int hasFlag = (ent->client->ps.powerups[enemyFlagItem] != 0) ? 1 : 0;
	if (bot2_states[clientNum].tele_hasFlag && !hasFlag) { bot2_states[clientNum].tele_hasFlag = 0; G_Kill(ent); return; }
	bot2_states[clientNum].tele_hasFlag = hasFlag;

	// --- MACRO TACTICS ENGINE ---
	macroState_t oldMacro = bot2_states[clientNum].macroState;
	gentity_t* redFlag = G_Find(NULL, FOFS(classname), "team_CTF_redflag"), * blueFlag = G_Find(NULL, FOFS(classname), "team_CTF_blueflag");
	gentity_t* myFlag = (botTeam == TEAM_RED) ? redFlag : blueFlag, * enemyFlag = (botTeam == TEAM_RED) ? blueFlag : redFlag;

	gentity_t* droppedMyFlag = NULL, * fSearch = NULL;
	while ((fSearch = G_Find(fSearch, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag")) != NULL) {
		if ((fSearch->flags & FL_DROPPED_ITEM) && fSearch->r.linked) { droppedMyFlag = fSearch; break; }
	}

	qboolean enemyHasOurFlag = qfalse;
	for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { enemyHasOurFlag = qtrue; break; }

	qboolean myFlagAtBase = (myFlag && myFlag->r.linked && (myFlag->flags & FL_DROPPED_ITEM) == 0 && myFlag->parent == NULL && !enemyHasOurFlag && !droppedMyFlag);
	qboolean enemyFlagAtBase = (enemyFlag && enemyFlag->r.linked && (enemyFlag->flags & FL_DROPPED_ITEM) == 0 && enemyFlag->parent == NULL);

	if (hasFlag) bot2_states[clientNum].macroState = MACRO_RETURN_FLAG;
	else if (bot2_states[clientNum].role == ROLE_OFFENSE) bot2_states[clientNum].macroState = enemyFlagAtBase ? MACRO_FETCH_FLAG : MACRO_CAMP_REGEN;
	else if (bot2_states[clientNum].role == ROLE_CHASE) bot2_states[clientNum].macroState = !myFlagAtBase ? MACRO_CHASE_THIEF : (!(ent->client->ps.stats[STAT_WEAPONS] & ((1 << WP_ROCKET_LAUNCHER) | (1 << WP_FLECHETTE) | (1 << WP_REPEATER))) ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND);
	else {
		gentity_t* alliedFC = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == botTeam && g_entities[i].client->ps.powerups[enemyFlagItem]) { alliedFC = &g_entities[i]; break; }
		if (alliedFC && myFlag && Distance(alliedFC->client->ps.origin, myFlag->s.origin) < 1500.0f) { bot2_states[clientNum].macroState = MACRO_ESCORT_FC; bot2_states[clientNum].targetEntNum = alliedFC->s.number; }
		else if (ent->health < 40) bot2_states[clientNum].macroState = MACRO_SURVIVAL;
		else bot2_states[clientNum].macroState = !(ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND;
	}

	if (bot2_states[clientNum].macroState != oldMacro) trap->Print("[%s] Mode changed to: %s\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);

	if (bot2_states[clientNum].macroState == MACRO_FETCH_FLAG && enemyFlag && Distance(ent->client->ps.origin, enemyFlag->s.origin) < 800.0f) {
		gentity_t* mine = NULL;
		while ((mine = G_Find(mine, FOFS(classname), "tripmine")) != NULL) {
			if (mine->r.linked && mine->s.otherEntityNum2 != botTeam && Distance(mine->s.origin, enemyFlag->s.origin) < 500.0f) {
				bot2_states[clientNum].macroState = MACRO_HUNT_TRIPMINES; bot2_states[clientNum].targetEntNum = mine->s.number;
				trap->Print("[%s | %s] Trip Mine spotted! Clearing.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
				break;
			}
		}
	}

	vec3_t targetOrigin = { 0,0,0 }, aimDir; qboolean lockAim = qfalse, wantsToShoot = qfalse; int currentMineCount = 0;
	if (myFlag) { gentity_t* m = NULL; while ((m = G_Find(m, FOFS(classname), "tripmine")) != NULL) if (Distance(m->s.origin, myFlag->s.origin) < 200.0f) currentMineCount++; }

	switch (bot2_states[clientNum].macroState) {
	case MACRO_FETCH_FLAG: if (enemyFlag) VectorCopy(enemyFlag->s.origin, targetOrigin); break;
	case MACRO_RETURN_FLAG: if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin); break;
	case MACRO_DEFEND_STAND: if (myFlag) {
		if (bot2_states[clientNum].role == ROLE_BASE) {
			ucmd.weapon = WP_BRYAR_PISTOL;
			if (level.time % 2000 < 50) trap->Print("[%s] Base Defense | Dist to Flag: %.0f | Tripmines: %d\n", ent->client->pers.netname, Distance(ent->client->ps.origin, myFlag->s.origin), Bot2_GetAmmo(ent, WP_TRIP_MINE));

			if (!GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f) && (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) && Bot2_GetAmmo(ent, WP_TRIP_MINE) > 0 && Distance(ent->client->ps.origin, myFlag->s.origin) < 150.0f && currentMineCount < 4) {
				ucmd.weapon = WP_TRIP_MINE; lockAim = wantsToShoot = qtrue; VectorCopy(myFlag->s.origin, aimDir); aimDir[2] -= 32.0f;
				float ao = currentMineCount * 90.0f * (M_PI / 180.0f); aimDir[0] += cos(ao) * 64.0f; aimDir[1] += sin(ao) * 64.0f; VectorSubtract(aimDir, ent->client->ps.origin, aimDir);
			}
		}
		float pRad = (bot2_states[clientNum].role == ROLE_CHASE) ? 400.0f : ((bot2_states[clientNum].role == ROLE_BASE && currentMineCount > 0) ? 250.0f : 0.0f);
		if (pRad > 0.0f) { float pa = (level.time / 1500.0f) + (clientNum * 0.5f); targetOrigin[0] = myFlag->s.origin[0] + cos(pa) * pRad; targetOrigin[1] = myFlag->s.origin[1] + sin(pa) * pRad; targetOrigin[2] = myFlag->s.origin[2]; }
		else VectorCopy(myFlag->s.origin, targetOrigin);
	} break;
	case MACRO_CHASE_THIEF: {
		gentity_t* thief = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { thief = &g_entities[i]; break; }
		if (thief) {
			VectorCopy(thief->client->ps.origin, targetOrigin); vec3_t myEye = { ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2] + ent->client->ps.viewheight };
			trace_t tr; trap->Trace(&tr, myEye, (vec3_t) { 0, 0, 0 }, (vec3_t) { 0, 0, 0 }, thief->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			if (tr.fraction == 1.0f || tr.entityNum == thief->s.number) { vec3_t lead; Bot2_GetLeadOrigin(ent, thief, lead); lockAim = wantsToShoot = qtrue; VectorSubtract(lead, myEye, aimDir); }
		}
		else if (droppedMyFlag) {
			VectorCopy(droppedMyFlag->s.origin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Thief dead, fetching DROPPED flag.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
		else if (enemyFlag) {
			VectorCopy(enemyFlag->s.origin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Target lost, intercepting at enemy base.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
	} break;
	case MACRO_ESCORT_FC: if (bot2_states[clientNum].targetEntNum >= 0) VectorCopy(g_entities[bot2_states[clientNum].targetEntNum].client->ps.origin, targetOrigin); break;
	case MACRO_GET_WEAPON: {
		const char* heavy[] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater" }, * trips[] = { "weapon_trip_mine" };
		gentity_t* item = GetNearestItem(ent->client->ps.origin, (bot2_states[clientNum].role == ROLE_CHASE) ? heavy : trips, (bot2_states[clientNum].role == ROLE_CHASE) ? 3 : 1, 99999.0f);
		if (item) VectorCopy(item->s.origin, targetOrigin);
	} break;
	case MACRO_CAMP_REGEN: {
		const char* allW[] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater", "weapon_bowcaster", "weapon_blaster", "weapon_disruptor", "weapon_demp2", "weapon_concussion_rifle" };
		gentity_t* wpn = GetNearestItem(ent->client->ps.origin, allW, 8, 2000.0f);
		if (wpn) VectorCopy(wpn->s.origin, targetOrigin);
		else if (enemyFlag) { float pa = (level.time / 1500.0f) + (clientNum * 0.5f); targetOrigin[0] = enemyFlag->s.origin[0] + cos(pa) * 450.0f; targetOrigin[1] = enemyFlag->s.origin[1] + sin(pa) * 450.0f; targetOrigin[2] = enemyFlag->s.origin[2]; }

		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f);
		if (enemy) { vec3_t lead; Bot2_GetLeadOrigin(ent, enemy, lead); lockAim = wantsToShoot = qtrue; VectorSubtract(lead, ent->client->ps.origin, aimDir); aimDir[2] -= ent->client->ps.viewheight; }
	} break;
	case MACRO_SURVIVAL: {
		const char* sups[] = { "item_medpak_instant", "item_shield_sm_instant", "item_shield_lrg_instant" };
		gentity_t* hp = GetNearestItem(ent->client->ps.origin, sups, 3, 3000.0f);
		if (hp) VectorCopy(hp->s.origin, targetOrigin); else if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin);

		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1500.0f);
		if (enemy && hp) { vec3_t rDir; VectorSubtract(ent->client->ps.origin, enemy->client->ps.origin, rDir); VectorNormalize(rDir); VectorMA(ent->client->ps.origin, 500.0f, rDir, targetOrigin); }
	} break;
	case MACRO_HUNT_TRIPMINES: {
		gentity_t* mine = (bot2_states[clientNum].targetEntNum >= MAX_CLIENTS) ? &g_entities[bot2_states[clientNum].targetEntNum] : NULL;
		if (mine && mine->inuse && mine->r.linked) { VectorCopy(mine->s.origin, targetOrigin); lockAim = wantsToShoot = qtrue; VectorSubtract(mine->s.origin, ent->client->ps.origin, aimDir); }
		else bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
	} break;
	}

	if (!lockAim && !(bot2_states[clientNum].role == ROLE_OFFENSE && bot2_states[clientNum].macroState != MACRO_CAMP_REGEN)) {
		vec3_t myEye = { ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2] + ent->client->ps.viewheight };
		gentity_t* bTarget = GetNearestEnemy(ent->client->ps.origin, botTeam, 2500.0f);
		if (bTarget) {
			trace_t tr; trap->Trace(&tr, myEye, (vec3_t) { 0, 0, 0 }, (vec3_t) { 0, 0, 0 }, bTarget->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			if (tr.fraction == 1.0f || tr.entityNum == bTarget->s.number) { vec3_t lead; Bot2_GetLeadOrigin(ent, bTarget, lead); VectorSubtract(lead, myEye, aimDir); lockAim = wantsToShoot = qtrue; }
		}
	}

	if (wantsToShoot) {
		if (ucmd.weapon == WP_BRYAR_PISTOL) { if (level.time - bot2_states[clientNum].chargeTimer < 1500) ucmd.buttons |= BUTTON_ALT_ATTACK; else if (level.time - bot2_states[clientNum].chargeTimer >= 1600) bot2_states[clientNum].chargeTimer = level.time; }
		else if (ucmd.weapon == WP_REPEATER) ucmd.buttons |= (Bot2_GetAmmo(ent, WP_REPEATER) >= 30) ? BUTTON_ALT_ATTACK : BUTTON_ATTACK;
		else ucmd.buttons |= (ucmd.weapon == WP_FLECHETTE || ucmd.weapon == WP_TRIP_MINE) ? BUTTON_ALT_ATTACK : BUTTON_ATTACK;
	}
	else bot2_states[clientNum].chargeTimer = level.time;

	qboolean isSpamWeapon = (ucmd.weapon == WP_REPEATER || ucmd.weapon == WP_FLECHETTE || ucmd.weapon == WP_BLASTER || ucmd.weapon == WP_BRYAR_PISTOL);
	int currentAttackButtons = (ucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));
	int lastAttackButtons = (botstates[clientNum]->lastucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));

	if (currentAttackButtons && !lastAttackButtons) {
		if (!isSpamWeapon && ucmd.weapon != WP_TRIP_MINE) {
			vec3_t forward, end, start; trace_t tr;
			AngleVectors(ent->client->ps.viewangles, forward, NULL, NULL);
			VectorCopy(ent->client->ps.origin, start); start[2] += ent->client->ps.viewheight;
			VectorMA(start, 4096.0f, forward, end);
			vec3_t zeroVec = { 0,0,0 };
			trap->Trace(&tr, start, zeroVec, zeroVec, end, ent->s.number, MASK_SHOT, qfalse, 0, 0);

			if (tr.entityNum < MAX_CLIENTS && g_entities[tr.entityNum].inuse && g_entities[tr.entityNum].client && g_entities[tr.entityNum].client->sess.sessionTeam != botTeam) {
				trap->Print("[%s | FIRED %d] Hit Predicted on %s!\n", ent->client->pers.netname, ucmd.weapon, g_entities[tr.entityNum].client->pers.netname);
			}
			else {
				trap->Print("[%s | FIRED %d] Missed / Hit Geometry.\n", ent->client->pers.netname, ucmd.weapon);
			}
		}
	}

	ent->client->ps.fd.forcePowersKnown |= (1 << FP_SPEED) | (1 << FP_ABSORB) | (1 << FP_TEAM_FORCE); ent->client->ps.fd.forcePowerLevel[FP_SPEED] = ent->client->ps.fd.forcePowerLevel[FP_ABSORB] = ent->client->ps.fd.forcePowerLevel[FP_TEAM_FORCE] = 3;
	qboolean wantsSpeed = !(bot2_states[clientNum].macroState == MACRO_CAMP_REGEN || bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES);
	if (ent->client->ps.fd.forcePower >= 50 && !ent->client->ps.fd.forceButtonNeedRelease) {
		if (bot2_states[clientNum].role == ROLE_BASE && ent->client->ps.fd.forcePower >= 75 && (level.time - bot2_states[clientNum].abilityTimer > 5000)) { ucmd.forcesel = FP_TEAM_FORCE; ucmd.buttons |= BUTTON_FORCEPOWER; bot2_states[clientNum].abilityTimer = level.time; }
		else if ((wantsSpeed && !(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) || (!wantsSpeed && (ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED)))) { ucmd.forcesel = FP_SPEED; ucmd.buttons |= BUTTON_FORCEPOWER; }
	}

	vec3_t dir, angles, nextWp = { 0,0,0 }; qboolean validPath = qfalse; float base_target_yaw;
	float distToTarget = VectorLength(targetOrigin) > 1.0f ? Distance(targetOrigin, ent->client->ps.origin) : 999999.0f;

	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		ucmd.forwardmove = ucmd.rightmove = ucmd.upmove = 0; vectoangles(aimDir, angles);
		ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW]; ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd); return;
	}

	if (VectorLength(targetOrigin) > 1.0f) {
		if (NavMesh_GetNextWaypoint(ent->s.number, ent->client->ps.origin, targetOrigin, nextWp) && (VectorSubtract(nextWp, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) validPath = qtrue;
		else if (bot2_states[clientNum].role == ROLE_CHASE && bot2_states[clientNum].macroState == MACRO_CHASE_THIEF && enemyFlag) {
			VectorCopy(enemyFlag->s.origin, targetOrigin);
			if (NavMesh_GetNextWaypoint(ent->s.number, ent->client->ps.origin, targetOrigin, nextWp) && (VectorSubtract(nextWp, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) validPath = qtrue;
		}
	}

	base_target_yaw = validPath ? (vectoangles(dir, angles), angles[YAW]) : bot2_states[clientNum].targetYaw;
	float distToWp = validPath ? Distance(ent->client->ps.origin, nextWp) : distToTarget;

	if (IsInTriggerPush(ent) && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 3; bot2_states[clientNum].stateTimer = level.time;
		trap->Print("[%s] STATE Transition: Entered Trigger Push / Jump Pad (State 3)\n", ent->client->pers.netname);
	}
	if (ent->client->ps.groundEntityNum == ENTITYNUM_NONE && bot2_states[clientNum].state != 2 && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 2; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw; bot2_states[clientNum].strafeDir = EvaluateStrafeDir(ent, base_target_yaw);
		trap->Print("[%s] STATE Transition: Walking -> Falling/Airborne (State 2)\n", ent->client->pers.netname);
	}

	// --- STATE 3: JUMP PAD / PUSHED ---
	if (bot2_states[clientNum].state == 3) {
		float lookYaw = base_target_yaw;
		if (lockAim) { vectoangles(aimDir, angles); lookYaw = angles[YAW]; ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH]; }
		else ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
		ucmd.angles[YAW] = ANGLE2SHORT(lookYaw) - ent->client->ps.delta_angles[YAW]; ucmd.forwardmove = 127; ucmd.rightmove = ucmd.upmove = 0;
		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 500)) {
			bot2_states[clientNum].state = 0;
			trap->Print("[%s] STATE Transition: Landed from Jump Pad -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname);
		}
	}
	// --- STATE 0: WALK/IDLE & RUNNING JUMPS ---
	else if (bot2_states[clientNum].state == 0) {
		bot2_states[clientNum].tele_jumpSeq = 0; float bDist = 128.0f + (current_speed * 0.0f), dYaw = base_target_yaw; qboolean lDang = qfalse;

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
			else if (distToTarget <= 300.0f) {
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
				int tD[] = { pDir, pDir * -1 }; // Test preferred direction first, then opposite

				for (int d = 0; d < 2; d++) {
					vec3_t predLandPos;

					// Pass the failure buffer into the kinematic simulator
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
							bot2_states[clientNum].strafeDir = tD[d]; // Lock in whichever direction succeeded!
							ucmd.upmove = 127; jExec = qtrue;
							bot2_states[clientNum].lastFailReason[0] = '\0'; // Clear buffer on success
							trap->Print("[%s] STATE Transition: Walking -> Executing Running Jump (State 2)\n", ent->client->pers.netname);
							break; // Found a safe jump, stop testing!
						}
					}
				}
			}

			// Throttled Diagnostic Print
			if (!jExec && level.time - bot2_states[clientNum].diagTimer > 1000) {
				trap->Print("[%s] Jump Refused: %s\n", ent->client->pers.netname, bot2_states[clientNum].lastFailReason);
				bot2_states[clientNum].diagTimer = level.time;
			}
		}

		if (!jExec) {
			float safeYaw; qboolean hasEsc = lDang ? GetSafeEscapeYaw(ent, dYaw, &safeYaw) : qfalse;
			if (lockAim) {
				float mYaw = lDang ? (hasEsc ? safeYaw : dYaw) : base_target_yaw; vectoangles(aimDir, angles);
				ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW]; ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
				if (lDang && !hasEsc) ucmd.forwardmove = ucmd.rightmove = 0; else if (current_speed <= max_run_speed + 10.0f) { float df = (mYaw - angles[YAW]) * (M_PI / 180.0f); ucmd.forwardmove = (char)max(-127, min(127, 127.0f * cos(df))); ucmd.rightmove = (char)max(-127, min(127, 127.0f * -sin(df))); }
				else ucmd.forwardmove = ucmd.rightmove = 0;
			}
			else {
				ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
				if (lDang) { ucmd.angles[YAW] = ANGLE2SHORT(hasEsc ? safeYaw : dYaw) - ent->client->ps.delta_angles[YAW]; ucmd.forwardmove = hasEsc ? 127 : 0; ucmd.rightmove = 0; }
				else { ucmd.angles[YAW] = ANGLE2SHORT(base_target_yaw) - ent->client->ps.delta_angles[YAW]; ucmd.forwardmove = (current_speed > max_run_speed + 10.0f) ? 0 : 127; ucmd.rightmove = 0; }
			}
			ucmd.upmove = 0;
		}
	}
	// --- STATE 2: AIRBORNE ---
	else if (bot2_states[clientNum].state == 2) {
		int sDir = bot2_states[clientNum].strafeDir;
		float mA = (current_speed > max_run_speed - 15.0f) ? acos(max(-1.0f, min(1.0f, (max_run_speed - 15.0f) / current_speed))) * (180.0f / M_PI) : 0.0f;
		float tYaw = AngleMod(vel_yaw - ((mA - 45.0f) * sDir));

		if (lockAim) { vectoangles(aimDir, angles); ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH]; ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW]; float tDiff = (tYaw - angles[YAW]) * (M_PI / 180.0f); ucmd.forwardmove = (char)(127.0f * cos(tDiff)); ucmd.rightmove = (char)(127.0f * -sin(tDiff)); }
		else { ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH]; ucmd.angles[YAW] = ANGLE2SHORT(tYaw) - ent->client->ps.delta_angles[YAW]; ucmd.forwardmove = 127; ucmd.rightmove = 127 * sDir; }

		ucmd.upmove = (level.time - bot2_states[clientNum].stateTimer < 100) ? 127 : 0; // Hold jump to clear ledge

		// Landing Logic
		qboolean minAirTimeMet = (level.time - bot2_states[clientNum].stateTimer > 250);
		qboolean wasJustADrop = (bot2_states[clientNum].tele_jumpSeq == 0); // 0 means we fell/spawned, we didn't jump

		// If we actually jumped, enforce the 250ms buffer. If we just dropped, land instantly!
		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (minAirTimeMet || wasJustADrop)) {
			trace_t lTr; vec3_t lE, zV = { 0,0,0 }; VectorCopy(ent->client->ps.origin, lE); lE[2] -= 64.0f; trap->Trace(&lTr, ent->client->ps.origin, zV, zV, lE, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			float lSlope = (lTr.fraction < 1.0f) ? acos(lTr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

			if (bot2_states[clientNum].tele_inAir && ent->client) {
				bot2_states[clientNum].tele_inAir = 0;
				vec3_t aLPos; VectorCopy(ent->client->ps.origin, aLPos);
				float dx = aLPos[0] - bot2_states[clientNum].tele_startPos[0], dy = aLPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy)), aZDrop = bot2_states[clientNum].tele_startPos[2] - aLPos[2];
				float pZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_predPos[2];
				float yawD = vel_yaw - bot2_states[clientNum].tele_takeoffYaw;
				while (yawD > 180.0f) yawD -= 360.0f; while (yawD < -180.0f) yawD += 360.0f;

				trap->Print("[%s] JUMP %d | T: %dms | Spd: %.0f | D: %.1f (Err %+.1f) | Z: %.1f (Err %+.1f) | XY Err: %+.1f, %+.1f | Turn: %+.1f deg\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime,
					bot2_states[clientNum].tele_takeoffSpd, aDist, aDist - bot2_states[clientNum].tele_predDist,
					aZDrop, aZDrop - pZDrop, aLPos[0] - bot2_states[clientNum].tele_predPos[0], aLPos[1] - bot2_states[clientNum].tele_predPos[1], yawD
				);
			}

			int nDir = (lSlope > 5.0f) ? EvaluateStrafeDir(ent, base_target_yaw) : sDir * -1; qboolean cJmp = qfalse;
			float aD = vel_yaw - base_target_yaw; while (aD > 180.0f) aD -= 360.0f; while (aD < -180.0f) aD += 360.0f; aD = fabs(aD);

			// --- DIAGNOSTIC GATEKEEPER (STATE 2 CHAIN JUMPS) ---
			if (wantsSpeed) {
				if (current_speed < (max_run_speed - 30.0f)) {
					// Only force a recovery hop if we lost speed DURING an active jump chain
					if (bot2_states[clientNum].tele_jumpSeq > 0) {
						bot2_states[clientNum].lastFailReason[0] = '\0';
						cJmp = qtrue;
					}
					else {
						// We spawned or dropped off a ledge. Walk to build speed normally.
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
					int tD[] = { nDir, nDir * -1 };
					for (int d = 0; d < 2; d++) {
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

				// Throttled Diagnostic Print
				if (!cJmp && level.time - bot2_states[clientNum].diagTimer > 1000) {
					trap->Print("[%s] Chain Jump Refused: %s\n", ent->client->pers.netname, bot2_states[clientNum].lastFailReason);
					bot2_states[clientNum].diagTimer = level.time;
				}
			}

			if (cJmp) { bot2_states[clientNum].strafeDir = nDir; ucmd.upmove = 127; trap->Print("[%s] STATE Event: Executing Chain Jump (State 2)\n", ent->client->pers.netname); }
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
			ucmd.forwardmove = fwd[ph]; ucmd.rightmove = (ph == 3) ? ((rand() % 2 == 0) ? 127 : -127) : ((ph == 1) ? 127 : ((ph == 2) ? -127 : 0)); ucmd.upmove = upm[ph];
			ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW] + (ph * ((ph % 2 == 0) ? 15.0f : -15.0f))) - ent->client->ps.delta_angles[YAW];
			bot2_states[clientNum].state = 0;
		}
	}
	else bot2_states[clientNum].stuck_timer = level.time;

	botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd);
}

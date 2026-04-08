// no speed caps
#include "g_local.h"
#include "bg_local.h" // Requires Pmove definitions
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

extern void G_Kill(gentity_t* ent);

// ==============================================================================
// Enums & Structs
// ==============================================================================
typedef enum {
	ROLE_OFFENSE,
	ROLE_CHASE,
	ROLE_BASE
} botRole_t;

typedef enum {
	MACRO_FETCH_FLAG,
	MACRO_RETURN_FLAG,
	MACRO_HUNT_TRIPMINES,
	MACRO_CAMP_REGEN,
	MACRO_GET_WEAPON,
	MACRO_DEFEND_STAND,
	MACRO_CHASE_THIEF,
	MACRO_ESCORT_FC,
	MACRO_SURVIVAL
} macroState_t;

// Global array for enhanced macro state telemetry
static const char* macroNames[] = {
	"FETCH_FLAG",
	"RETURN_FLAG",
	"HUNT_TRIPMINES",
	"CAMP_REGEN",
	"GET_WEAPON",
	"DEFEND_STAND",
	"CHASE_THIEF",
	"ESCORT_FC",
	"SURVIVAL"
};

// ==============================================================================
// State Machine Tracker
// ==============================================================================
static struct {
	// MACRO TACTICS
	botRole_t role;
	macroState_t macroState;
	int targetEntNum;
	vec3_t macroTargetOrigin;
	int abilityTimer;
	int chargeTimer;

	// STATE MACHINE
	int state;           // 0 = Walk, 2 = Airborne, 3 = Trigger Push
	int stateTimer;
	float targetYaw;
	int strafeDir;
	int spawnCooldown;
	int ledgeEvading;

	// ANTI-SNAG ENGINE
	vec3_t stuck_pos;
	int stuck_timer;
	int unstuck_phase;
	int unstuck_phase_timer;

	// MID-AIR TELEMETRY ENGINE
	int tele_inAir;
	int tele_jumpSeq;
	int tele_jumpStartTime;
	int tele_midAirTime;
	int tele_predDir;
	int tele_actDir;
	int tele_deadLogged;
	int tele_hasFlag;

	float tele_takeoffSpd;
	float tele_prevSpeed;
	float tele_predDist;
	float tele_groundSlope;

	float tele_rampDot;
	float tele_secant;
	float tele_effDrop;
	float tele_predLandSlope;
	float tele_takeoffYaw;

	vec3_t tele_startPos;
	vec3_t tele_predPos;
	vec3_t tele_pmovePredPos;
	vec3_t tele_midPredPos;
	vec3_t tele_prevPos;
	vec3_t tele_crossPos;

	qboolean tele_crossedZ;
	qboolean tele_midAirLogged;

	// DIAGNOSTICS & THROTTLES
	int diagTimer;
	char lastFailReason[128];
} bot2_states[MAX_CLIENTS] = { 0 };

// ==============================================================================
// Helper: Team Roles
// ==============================================================================
static botRole_t GetBotRole(int myClientNum, int myTeam) {
	int teamClients[MAX_CLIENTS];
	int count = 0;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == myTeam) {
			teamClients[count++] = i;
		}
	}

	int myRank = 0;
	for (int i = 0; i < count; i++) {
		if (teamClients[i] == myClientNum) {
			myRank = i;
			break;
		}
	}

	// 0: Offense, 1: Chase, 2: Offense, 3: Base, (Loop)
	if (myRank % 4 == 1) return ROLE_CHASE;
	if (myRank % 4 == 3) return ROLE_BASE;
	return ROLE_OFFENSE;
}

// ==============================================================================
// Helper: Nearest Item/Enemy Search
// ==============================================================================
static gentity_t* GetNearestItem(vec3_t pos, const char** classnames, int numClasses, float maxDist) {
	gentity_t* bestItem = NULL;
	float bestDist = maxDist;

	for (int i = 0; i < numClasses; i++) {
		gentity_t* found = NULL;
		while ((found = G_Find(found, FOFS(classname), classnames[i])) != NULL) {
			if (found->r.linked) {
				float d = Distance(pos, found->s.origin);
				if (d < bestDist) {
					bestDist = d;
					bestItem = found;
				}
			}
		}
	}
	return bestItem;
}

static gentity_t* GetNearestEnemy(vec3_t pos, int myTeam, float maxDist) {
	gentity_t* bestEnemy = NULL;
	float bestDist = maxDist;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		gentity_t* ent = &g_entities[i];

		if (ent->inuse && ent->client && ent->health > 0 &&
			ent->client->sess.sessionTeam != myTeam &&
			ent->client->sess.sessionTeam != TEAM_SPECTATOR) {

			float d = Distance(pos, ent->client->ps.origin);
			if (d < bestDist) {
				bestDist = d;
				bestEnemy = ent;
			}
		}
	}
	return bestEnemy;
}

// ==============================================================================
// Helper: Ammo Index Mapper
// ==============================================================================
static int Bot2_GetAmmo(gentity_t* ent, int weapon) {
	int ammoIndex = 0;
	switch (weapon) {
	case WP_BLASTER: case WP_BRYAR_PISTOL: ammoIndex = 2; break; // AMMO_BLASTER
	case WP_DISRUPTOR: case WP_BOWCASTER: case WP_DEMP2: ammoIndex = 3; break; // AMMO_POWERCELL
	case WP_REPEATER: case WP_FLECHETTE: case WP_CONCUSSION: ammoIndex = 4; break; // AMMO_METAL_BOLTS
	case WP_ROCKET_LAUNCHER: ammoIndex = 5; break; // AMMO_ROCKETS
	case WP_TRIP_MINE: ammoIndex = 8; break; // AMMO_TRIPMINE
	}
	return ent->client->ps.ammo[ammoIndex];
}

// ==============================================================================
// Environment / Tracing Helpers
// ==============================================================================
static qboolean CheckForTriggerHurt(gentity_t* ent, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs) {
	vec3_t currentStart;
	VectorCopy(start, currentStart);

	while (1) {
		trace_t tr;
		trap->Trace(&tr, currentStart, mins, maxs, end, ent->s.number, CONTENTS_TRIGGER, qfalse, 0, 0);

		if (tr.fraction == 1.0f) return qfalse;

		if (tr.entityNum < ENTITYNUM_WORLD) {
			gentity_t* hitEnt = &g_entities[tr.entityNum];
			if (hitEnt && hitEnt->inuse && hitEnt->classname) {
				if (Q_stricmp(hitEnt->classname, "trigger_hurt") == 0) return qtrue;
			}
		}

		VectorCopy(tr.endpos, currentStart);
		currentStart[2] -= 8.0f;
		if (currentStart[2] <= end[2]) break;
	}
	return qfalse;
}

static float TraceFloorScore(gentity_t* ent, vec3_t start, float yaw, float dist, qboolean* hitWall) {
	vec3_t forward, end, downEnd, traceStart;
	trace_t tr, trDrop;
	vec3_t mins = { -15.0f, -15.0f, -24.0f };
	vec3_t maxs = { 15.0f, 15.0f, 32.0f };

	AngleVectors((vec3_t) { 0, yaw, 0 }, forward, NULL, NULL);

	VectorCopy(start, traceStart);
	traceStart[2] += 24.0f;
	VectorMA(traceStart, dist, forward, end);

	trap->Trace(&tr, traceStart, mins, maxs, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (hitWall) *hitWall = (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f);

	float actual_dist = dist * tr.fraction;
	for (int i = 1; i <= 3; i++) {
		float f = (float)i / 3.0f;
		vec3_t probe_pos;
		VectorMA(traceStart, actual_dist * f, forward, probe_pos);

		VectorCopy(probe_pos, downEnd);
		downEnd[2] -= 1024.0f;
		trap->Trace(&trDrop, probe_pos, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		if (trDrop.fraction == 1.0f || (probe_pos[2] - trDrop.endpos[2]) > 800.0f || CheckForTriggerHurt(ent, probe_pos, trDrop.endpos, mins, maxs)) {
			return -1.0f;
		}
	}
	return tr.fraction;
}

static qboolean GetSafeEscapeYaw(gentity_t* ent, float danger_yaw, float* safe_yaw) {
	float escape_yaw = AngleMod(danger_yaw + 180.0f);
	float best_score = -1.0f;
	float best_yaw = escape_yaw;

	for (int i = 0; i < 8; i++) {
		float offset = (i % 2 == 0 ? 1.0f : -1.0f) * ((i + 1) / 2) * 45.0f;
		float test_yaw = AngleMod(escape_yaw + offset);
		float score = TraceFloorScore(ent, ent->client->ps.origin, test_yaw, 128.0f, NULL);

		if (score > best_score) {
			best_score = score;
			best_yaw = test_yaw;
			if (best_score == 1.0f) break;
		}
	}

	if (best_score >= 0.0f) {
		if (safe_yaw) *safe_yaw = best_yaw;
		return qtrue;
	}
	return qfalse;
}

static int EvaluateStrafeDir(gentity_t* ent, float base_target_yaw) {
	float scoreRight = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw - 30.0f), 256.0f, NULL);
	float scoreLeft = TraceFloorScore(ent, ent->client->ps.origin, AngleMod(base_target_yaw + 30.0f), 256.0f, NULL);

	if (scoreLeft > scoreRight) return -1;
	if (scoreRight > scoreLeft) return 1;
	// ENGINE FIX: Use Q_irand instead of standard rand()
	return (Q_irand(0, 1) == 0) ? 1 : -1;
}

static qboolean IsInTriggerPush(gentity_t* ent) {
	gentity_t* push = NULL;
	while ((push = G_Find(push, FOFS(classname), "trigger_push")) != NULL) {
		if (push->r.linked) {
			vec3_t mins, maxs;
			VectorAdd(ent->client->ps.origin, ent->r.mins, mins);
			VectorAdd(ent->client->ps.origin, ent->r.maxs, maxs);

			// AABB intersection check
			if (maxs[0] >= push->r.absmin[0] && mins[0] <= push->r.absmax[0] &&
				maxs[1] >= push->r.absmin[1] && mins[1] <= push->r.absmax[1] &&
				maxs[2] >= push->r.absmin[2] && mins[2] <= push->r.absmax[2]) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

// ==============================================================================
// Helper: Physics Trace Prediction (Kinematic Math)
// ==============================================================================
static qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start_origin, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed) {
	trace_t tr;
	vec3_t start, test_pos;
	vec3_t player_mins = { -15.0f, -15.0f, -24.0f };
	vec3_t player_maxs = { 15.0f, 15.0f, 32.0f };
	vec3_t zeroVec = { 0, 0, 0 };

	VectorCopy(start_origin, start);

	trace_t trSlope;
	vec3_t slopeEnd;
	VectorCopy(start, slopeEnd);
	slopeEnd[2] -= 64.0f;
	trap->Trace(&trSlope, start, zeroVec, zeroVec, slopeEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float slopeAngle = (trSlope.fraction < 1.0f) ? acos(trSlope.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	trace_t stairTr;
	vec3_t stairStart, stairEnd;
	vec3_t flatAngles = { 0, vel_yaw, 0 };
	vec3_t forward;
	AngleVectors(flatAngles, forward, NULL, NULL);
	VectorMA(start, 32.0f, forward, stairStart);
	VectorCopy(stairStart, stairEnd);
	stairEnd[2] -= 64.0f;
	trap->Trace(&stairTr, stairStart, player_mins, player_maxs, stairEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	if (stairTr.fraction < 1.0f && stairTr.plane.normal[2] > 0.7f) {
		float stepDiff = stairTr.endpos[2] - start[2];
		float hitSlopeAngle = acos(stairTr.plane.normal[2]) * (180.0f / M_PI);
		if (hitSlopeAngle < 5.0f && (stepDiff > 4.0f || (stepDiff < -4.0f && stepDiff > -32.0f))) slopeAngle = 45.0f;
	}

	float predicted_airtime = 0.925f;
	float ramp_dot = 0.0f;

	if (slopeAngle > 5.0f && trSlope.fraction < 1.0f) {
		vec3_t flat_dir = { cos(vel_yaw * (M_PI / 180.0f)), sin(vel_yaw * (M_PI / 180.0f)), 0.0f };
		ramp_dot = (flat_dir[0] * trSlope.plane.normal[0]) + (flat_dir[1] * trSlope.plane.normal[1]);

		if (ramp_dot < -0.1f) {
			float slope_bonus = slopeAngle * 0.01f * fabs(ramp_dot);
			predicted_airtime += (slope_bonus > 0.30f) ? 0.30f : slope_bonus;
		}
		else if (ramp_dot > 0.1f) {
			float slope_penalty = slopeAngle * 0.005f * ramp_dot;
			predicted_airtime -= (slope_penalty > 0.15f) ? 0.15f : slope_penalty;
		}
	}

	if (current_speed > 600.0f) predicted_airtime -= 0.02f;

	float jump_z_vel = 400.0f * predicted_airtime;
	float speed_scale = max_run_speed / 250.0f;
	float jump_constant = 112500.0f * (speed_scale * speed_scale);

	float predicted_land_speed = sqrt((current_speed * current_speed) + jump_constant);
	float avg_speed = (current_speed + predicted_land_speed) / 2.0f;
	if (avg_speed < max_run_speed) avg_speed = max_run_speed;

	// --- FLAT JUMP BASELINE SCALAR FIX ---
	float base_dist = (avg_speed * predicted_airtime) * 1.025f;

	// --- TELEMETRY SECANT CURVE UPDATE (28.0 deg baseline) ---
	float dynamic_secant = 28.0f - ((current_speed - max_run_speed) * 0.01f);
	if (dynamic_secant < 5.0f) dynamic_secant = 5.0f;

	vec3_t segment_start;
	VectorCopy(start, segment_start);
	segment_start[2] += 32.0f;

	for (int i = 1; i <= 4; i++) {
		float frac = i * 0.20f;
		float probe_dist = base_dist * frac;
		float point_yaw = vel_yaw - (dynamic_secant * testDir * frac);
		float point_rad = point_yaw * (M_PI / 180.0f);
		float t_probe = probe_dist / avg_speed;
		float arc_z_probe = (jump_z_vel * t_probe) - (400.0f * t_probe * t_probe);

		vec3_t probe_pos, probeDownStart, probeDownEnd;
		probe_pos[0] = start[0] + (cos(point_rad) * probe_dist);
		probe_pos[1] = start[1] + (sin(point_rad) * probe_dist);
		probe_pos[2] = start[2] + 32.0f + arc_z_probe;

		trap->Trace(&tr, segment_start, player_mins, player_maxs, probe_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) return qfalse;

		VectorCopy(probe_pos, segment_start);
		VectorCopy(probe_pos, probeDownStart);
		VectorCopy(probeDownStart, probeDownEnd);
		probeDownEnd[2] -= 2048.0f;

		trap->Trace(&tr, probeDownStart, player_mins, player_maxs, probeDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		if (tr.fraction == 1.0f) return qfalse;
		else if ((start[2] - tr.endpos[2]) > 450.0f) return qfalse;
		else if (CheckForTriggerHurt(ent, probeDownStart, tr.endpos, player_mins, player_maxs)) return qfalse;
	}

	// --- BASE CHORD PROJECTION FIX ---
	// Calculate the average angle of the arc based on dynamic_secant to fix X/Y drift
	float base_chord_yaw = vel_yaw - ((dynamic_secant * 0.55f) * testDir);
	float base_chord_rad = base_chord_yaw * (M_PI / 180.0f);

	float trace_dx_base = cos(base_chord_rad);
	float trace_dy_base = sin(base_chord_rad);

	float t_base = base_dist / avg_speed;
	float arc_z_base = (jump_z_vel * t_base) - (400.0f * t_base * t_base);

	test_pos[0] = start[0] + trace_dx_base * base_dist;
	test_pos[1] = start[1] + trace_dy_base * base_dist;
	test_pos[2] = start[2] + 32.0f + arc_z_base;

	trap->Trace(&tr, segment_start, player_mins, player_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	vec3_t downStart, downEnd;
	VectorCopy(tr.endpos, downStart);
	VectorCopy(downStart, downEnd);
	downEnd[2] -= 2048.0f;
	trap->Trace(&tr, downStart, player_mins, player_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	if (tr.fraction == 1.0f || CheckForTriggerHurt(ent, downStart, tr.endpos, player_mins, player_maxs)) return qfalse;

	float dropHeight = start[2] - tr.endpos[2];
	float touchdown_dist = base_dist;
	float effective_drop = 0.0f;
	float expected_airtime = predicted_airtime;

	// Initial fallback for finals
	float trace_dx_final = trace_dx_base;
	float trace_dy_final = trace_dy_base;

	// --- MICRO-DROP THRESHOLD FIX (Reduced to 8.0f) ---
	if (dropHeight > 8.0f || dropHeight < -8.0f) {
		effective_drop = dropHeight;
		float max_jump_height = (jump_z_vel * jump_z_vel) / 1600.0f;
		if (effective_drop < -(max_jump_height - 1.0f)) effective_drop = -(max_jump_height - 1.0f);
		if (effective_drop > 800.0f) effective_drop = 800.0f;

		float fall_time = sqrt(((jump_z_vel * jump_z_vel) / 1600.0f + effective_drop) / 400.0f);
		expected_airtime = (predicted_airtime / 2.0f) + fall_time;
		expected_airtime += (effective_drop * 0.0006f);

		// --- THE FIX: Scale speed by extra airtime ---
		float airtime_ratio = expected_airtime / predicted_airtime;
		float dynamic_jump_constant = jump_constant * airtime_ratio;
		float adjusted_land_speed = sqrt((current_speed * current_speed) + dynamic_jump_constant);

		float drop_avg_speed = (current_speed + adjusted_land_speed) / 2.0f;
		if (drop_avg_speed < max_run_speed) drop_avg_speed = max_run_speed;

		// --- THE FIX: Q3 Exponential Air-Accel Compensator ---
		float extra_time = expected_airtime - predicted_airtime;
		float distance_scalar = 1.025f + (extra_time > 0.0f ? extra_time * 0.1f : 0.0f);

		touchdown_dist = (drop_avg_speed * expected_airtime) * distance_scalar;

		// --- THE FIX: Dynamic Chord Angle for Drops ---
		float final_curve_degrees = dynamic_secant * airtime_ratio;
		float final_chord_yaw = vel_yaw - ((final_curve_degrees * 0.55f) * testDir);
		float final_chord_rad = final_chord_yaw * (M_PI / 180.0f);

		trace_dx_final = cos(final_chord_rad);
		trace_dy_final = sin(final_chord_rad);
	}

	float t_final = touchdown_dist / avg_speed;
	float arc_z_final = (jump_z_vel * t_final) - (400.0f * t_final * t_final);

	test_pos[0] = start[0] + trace_dx_final * touchdown_dist;
	test_pos[1] = start[1] + trace_dy_final * touchdown_dist;
	test_pos[2] = start[2] + 32.0f + arc_z_final;

	trap->Trace(&tr, segment_start, player_mins, player_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) return qfalse;

	VectorCopy(tr.endpos, downStart);
	VectorCopy(downStart, downEnd);
	downEnd[2] -= 2048.0f;
	trap->Trace(&tr, downStart, player_mins, player_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float finalDropHeight = start[2] - tr.endpos[2];
	float floor_z = tr.endpos[2];
	float predicted_land_slope = (tr.fraction < 1.0f) ? acos(tr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	if (effective_drop > 64.0f && finalDropHeight < (effective_drop - 128.0f)) return qfalse;
	if (tr.fraction == 1.0f || finalDropHeight > 450.0f || CheckForTriggerHurt(ent, downStart, tr.endpos, player_mins, player_maxs)) return qfalse;

	vec3_t over_pos, overDownStart, overDownEnd, land_pos;
	float over_dist = touchdown_dist + 50.0f;

	over_pos[0] = start[0] + trace_dx_final * over_dist;
	over_pos[1] = start[1] + trace_dy_final * over_dist;
	over_pos[2] = floor_z + 24.0f;

	VectorCopy(tr.endpos, land_pos);
	land_pos[2] += 24.0f;

	trap->Trace(&tr, land_pos, player_mins, player_maxs, over_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	VectorCopy(tr.endpos, overDownStart);
	VectorCopy(overDownStart, overDownEnd);
	overDownEnd[2] -= 2048.0f;
	trap->Trace(&tr, overDownStart, player_mins, player_maxs, overDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	if (tr.fraction == 1.0f || start[2] - tr.endpos[2] > 450.0f) return qfalse;

	float z_increase = -finalDropHeight;
	if (z_increase > 0.0f && tr.plane.normal[2] > 0.99f) {
		float t_reach = touchdown_dist / avg_speed;
		float arc_height = (jump_z_vel * t_reach) - (400.0f * t_reach * t_reach);
		if ((arc_height + 18.0f) < z_increase) return qfalse;
	}

	if (out_land_pos) VectorCopy(tr.endpos, out_land_pos);
	if (out_land_speed) *out_land_speed = avg_speed;

	bot2_states[clientNum].tele_jumpSeq++;
	bot2_states[clientNum].tele_jumpStartTime = level.time;
	bot2_states[clientNum].tele_groundSlope = slopeAngle;
	bot2_states[clientNum].tele_crossedZ = qfalse;
	bot2_states[clientNum].tele_predDir = testDir;
	bot2_states[clientNum].tele_midAirTime = level.time + (int)((expected_airtime * 1000.0f) * 0.5f);
	bot2_states[clientNum].tele_midAirLogged = qfalse;

	bot2_states[clientNum].tele_rampDot = ramp_dot;
	bot2_states[clientNum].tele_secant = dynamic_secant;
	bot2_states[clientNum].tele_effDrop = effective_drop;
	bot2_states[clientNum].tele_predLandSlope = predicted_land_slope;
	bot2_states[clientNum].tele_takeoffYaw = vel_yaw; // Capture precise angle at execution

	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);

	bot2_states[clientNum].tele_predPos[0] = start[0] + (trace_dx_final * touchdown_dist);
	bot2_states[clientNum].tele_predPos[1] = start[1] + (trace_dy_final * touchdown_dist);
	bot2_states[clientNum].tele_predPos[2] = tr.endpos[2];

	bot2_states[clientNum].tele_predDist = touchdown_dist;
	bot2_states[clientNum].tele_takeoffSpd = current_speed;
	bot2_states[clientNum].tele_inAir = 1;

	return qtrue;
}

// ==============================================================================
// Helper: Advanced Shooting (Physics-Based Leading)
// ==============================================================================
static void Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos) {
	if (!target || !target->client) {
		VectorCopy(target->s.origin, out_leadPos);
		return;
	}

	vec3_t myEye, targetHead;
	VectorCopy(ent->client->ps.origin, myEye);
	myEye[2] += ent->client->ps.viewheight;

	VectorCopy(target->client->ps.origin, targetHead);
	targetHead[2] += target->client->ps.viewheight;

	float projVel = 0;
	switch (ent->client->ps.weapon) {
	case WP_BLASTER: projVel = 2300.0f; break;
	case WP_BOWCASTER: projVel = 1300.0f; break;
	case WP_REPEATER: projVel = 1600.0f; break;
	case WP_DEMP2: projVel = 1800.0f; break;
	case WP_ROCKET_LAUNCHER: projVel = 900.0f; break;
	case WP_FLECHETTE: projVel = 3500.0f; break;
	case WP_CONCUSSION: projVel = 3000.0f; break;
	default: projVel = 0; break; // Instant hit or melee
	}

	if (projVel <= 0) {
		VectorCopy(targetHead, out_leadPos);
		return;
	}

	float dist = Distance(myEye, targetHead);
	float timeToImpact = dist / projVel;

	// Predict position: P = P0 + V*t
	VectorMA(targetHead, timeToImpact, target->client->ps.velocity, out_leadPos);
}

// ==============================================================================
// Bot2_Think Core Engine
// ==============================================================================
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum];
	usercmd_t ucmd;
	char serverCmd[1024];

	// --- THE GATEKEEPER: Prevent connection/drop crashes ---
	if (!ent || !ent->inuse || !ent->client || !botstates[clientNum]) {
		return;
	}

	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {}
	ent->client->inactivityTime = level.time + 1000000;

	// --- FATALITIES & RECOVERY ---
	if (ent->health <= 0) {
		bot2_states[clientNum].spawnCooldown = level.time;
		memset(&ucmd, 0, sizeof(ucmd));
		ucmd.serverTime = time;
		ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		if (!bot2_states[clientNum].tele_deadLogged) {
			trap->Print("[%s] STATE Transition: Bot Killed / Respawning.\n", ent->client->pers.netname);
			bot2_states[clientNum].tele_deadLogged = 1;
			bot2_states[clientNum].tele_inAir = 0;
			bot2_states[clientNum].tele_jumpSeq = 0;
			bot2_states[clientNum].state = 0;
			bot2_states[clientNum].tele_hasFlag = 0;
			bot2_states[clientNum].stuck_timer = level.time;
			VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		}

		if (ent->client->ps.velocity[2] < -300.0f || ent->client->ps.fallingToDeath) {
			ent->client->respawnTime = level.time;
			ent->client->ps.pm_time = 0;
		}

		botstates[clientNum]->lastucmd = ucmd;
		trap->BotUserCommand(ent->s.number, &ucmd);
		return;
	}

	bot2_states[clientNum].tele_deadLogged = 0;
	if (ent->client->ps.fallingToDeath) G_Kill(ent);

	// --- ROLE TAGGING ---
	bot2_states[clientNum].role = GetBotRole(clientNum, ent->client->sess.sessionTeam);
	const char* roleTags[] = { "[OFFENSE]", "[CHASE]", "[BASE]" };
	char userinfo[MAX_INFO_STRING];
	trap->GetUserinfo(clientNum, userinfo, sizeof(userinfo));
	char* name = Info_ValueForKey(userinfo, "name");
	if (name && !strstr(name, roleTags[bot2_states[clientNum].role])) {
		if (level.time - bot2_states[clientNum].abilityTimer > 2000) { // Throttle name changes
			char newName[MAX_NETNAME];
			char cleanName[MAX_NETNAME];
			Q_strncpyz(cleanName, name, sizeof(cleanName));
			for (int i = 0; i < 3; i++) {
				char* tag = strstr(cleanName, roleTags[i]);
				if (tag) *tag = '\0';
			}
			Com_sprintf(newName, sizeof(newName), "%s%s", cleanName, roleTags[bot2_states[clientNum].role]);
			Info_SetValueForKey(userinfo, "name", newName);
			trap->SetUserinfo(clientNum, userinfo);
			trap->Print("[%s] Assigned New Role: %s\n", ent->client->pers.netname, newName);
		}
	}

	// --- SYSTEM OVERRIDES ---
	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
	float vel_x = ent->client->ps.velocity[0];
	float vel_y = ent->client->ps.velocity[1];
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));
	float vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);

	ucmd = botstates[clientNum]->lastucmd;
	ucmd.serverTime = time;
	ucmd.buttons = 0;

	if (level.time - ent->client->pers.enterTime < 200) {
		bot2_states[clientNum].spawnCooldown = level.time;
		bot2_states[clientNum].stuck_timer = level.time;
		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		ucmd.forwardmove = 0; ucmd.rightmove = 0; ucmd.upmove = 0;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd;
		trap->BotUserCommand(ent->s.number, &ucmd);
		return;
	}

	// --- WEAPON SELECTION PRE-PROCESSING ---
	ucmd.weapon = WP_BRYAR_PISTOL;
	ent->client->ps.stats[STAT_WEAPONS] |= (1 << WP_BRYAR_PISTOL);
	if (ent->client->ps.ammo[2] < 10) {
		ent->client->ps.ammo[2] = 100;
	}

	if (bot2_states[clientNum].role != ROLE_BASE) {
		if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_ROCKET_LAUNCHER)) && Bot2_GetAmmo(ent, WP_ROCKET_LAUNCHER) > 0) ucmd.weapon = WP_ROCKET_LAUNCHER;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_FLECHETTE)) && Bot2_GetAmmo(ent, WP_FLECHETTE) > 0) ucmd.weapon = WP_FLECHETTE;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_REPEATER)) && Bot2_GetAmmo(ent, WP_REPEATER) > 0) ucmd.weapon = WP_REPEATER;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_CONCUSSION)) && Bot2_GetAmmo(ent, WP_CONCUSSION) > 0) ucmd.weapon = WP_CONCUSSION;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_BOWCASTER)) && Bot2_GetAmmo(ent, WP_BOWCASTER) > 0) ucmd.weapon = WP_BOWCASTER;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_BLASTER)) && Bot2_GetAmmo(ent, WP_BLASTER) > 0) ucmd.weapon = WP_BLASTER;
		else if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_DEMP2)) && Bot2_GetAmmo(ent, WP_DEMP2) > 0) ucmd.weapon = WP_DEMP2;
	}

	// --- CTF FLAG DROP/CAP DETECTION ---
	int botTeam = ent->client->sess.sessionTeam;
	int enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
	int hasFlag = (ent->client->ps.powerups[enemyFlagItem] != 0) ? 1 : 0;

	if (bot2_states[clientNum].tele_hasFlag && !hasFlag) {
		bot2_states[clientNum].tele_hasFlag = 0;
		G_Kill(ent);
		return;
	}
	bot2_states[clientNum].tele_hasFlag = hasFlag;

	// ==============================================================================
	// MACRO TACTICS ENGINE
	// ==============================================================================
	macroState_t oldMacro = bot2_states[clientNum].macroState;
	gentity_t* redFlag = G_Find(NULL, FOFS(classname), "team_CTF_redflag");
	gentity_t* blueFlag = G_Find(NULL, FOFS(classname), "team_CTF_blueflag");

	gentity_t* myFlag = (botTeam == TEAM_RED) ? redFlag : blueFlag;
	gentity_t* enemyFlag = (botTeam == TEAM_RED) ? blueFlag : redFlag;

	// Search for Dropped Flags
	gentity_t* droppedMyFlag = NULL;
	gentity_t* fSearch = NULL;
	while ((fSearch = G_Find(fSearch, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag")) != NULL) {
		if ((fSearch->flags & FL_DROPPED_ITEM) && fSearch->r.linked) {
			droppedMyFlag = fSearch;
			break;
		}
	}

	qboolean enemyHasOurFlag = qfalse;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) {
			enemyHasOurFlag = qtrue;
			break;
		}
	}

	qboolean myFlagAtBase = (myFlag && myFlag->r.linked && (myFlag->flags & FL_DROPPED_ITEM) == 0 && myFlag->parent == NULL && !enemyHasOurFlag && !droppedMyFlag);
	qboolean enemyFlagAtBase = (enemyFlag && enemyFlag->r.linked && (enemyFlag->flags & FL_DROPPED_ITEM) == 0 && enemyFlag->parent == NULL);

	float distToRed = redFlag ? Distance(ent->client->ps.origin, redFlag->s.origin) : 9999;
	float distToBlue = blueFlag ? Distance(ent->client->ps.origin, blueFlag->s.origin) : 9999;
	qboolean inEnemyBase = (botTeam == TEAM_RED) ? (distToBlue < distToRed) : (distToRed < distToBlue);

	// Evaluate Macro State
	if (hasFlag) {
		bot2_states[clientNum].macroState = MACRO_RETURN_FLAG;
	}
	else if (bot2_states[clientNum].role == ROLE_OFFENSE) {
		if (enemyFlagAtBase) bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
		else bot2_states[clientNum].macroState = MACRO_CAMP_REGEN;
	}
	else if (bot2_states[clientNum].role == ROLE_CHASE) {
		qboolean hasHeavyWeapon = (ent->client->ps.stats[STAT_WEAPONS] & ((1 << WP_ROCKET_LAUNCHER) | (1 << WP_FLECHETTE) | (1 << WP_REPEATER)));
		if (!myFlagAtBase) bot2_states[clientNum].macroState = MACRO_CHASE_THIEF;
		else if (!hasHeavyWeapon) bot2_states[clientNum].macroState = MACRO_GET_WEAPON;
		else bot2_states[clientNum].macroState = MACRO_DEFEND_STAND;
	}
	else if (bot2_states[clientNum].role == ROLE_BASE) {
		qboolean hasTrips = (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE));
		gentity_t* alliedFC = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == botTeam && g_entities[i].client->ps.powerups[enemyFlagItem]) {
				alliedFC = &g_entities[i]; break;
			}
		}

		if (alliedFC && myFlag && Distance(alliedFC->client->ps.origin, myFlag->s.origin) < 1500.0f) {
			bot2_states[clientNum].macroState = MACRO_ESCORT_FC;
			bot2_states[clientNum].targetEntNum = alliedFC->s.number;
		}
		else if (ent->health < 40) {
			bot2_states[clientNum].macroState = MACRO_SURVIVAL;
		}
		else if (!hasTrips) {
			bot2_states[clientNum].macroState = MACRO_GET_WEAPON;
		}
		else {
			bot2_states[clientNum].macroState = MACRO_DEFEND_STAND;
		}
	}

	if (bot2_states[clientNum].macroState != oldMacro) {
		trap->Print("[%s] Mode changed to: %s\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
	}

	// Tripmine Clearance Override
	if (bot2_states[clientNum].macroState == MACRO_FETCH_FLAG && enemyFlag && Distance(ent->client->ps.origin, enemyFlag->s.origin) < 800.0f) {
		gentity_t* mine = NULL;
		while ((mine = G_Find(mine, FOFS(classname), "wpn_tripmine")) != NULL) {
			if (mine->r.linked && mine->s.otherEntityNum2 != botTeam && Distance(mine->s.origin, enemyFlag->s.origin) < 500.0f) {
				bot2_states[clientNum].macroState = MACRO_HUNT_TRIPMINES;
				bot2_states[clientNum].targetEntNum = mine->s.number;
				trap->Print("[%s | %s] Trip Mine spotted! Clearing.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
				break;
			}
		}
	}

	// --- TRANSLATE MACRO TO MICRO TARGET ---
	vec3_t targetOrigin = { 0,0,0 };
	qboolean lockAim = qfalse;
	qboolean wantsToShoot = qfalse;
	vec3_t aimDir;
	int currentMineCount = 0;

	if (myFlag) {
		gentity_t* m = NULL;
		while ((m = G_Find(m, FOFS(classname), "tripmine")) != NULL) {
			if (Distance(m->s.origin, myFlag->s.origin) < 200.0f) currentMineCount++;
		}
	}

	switch (bot2_states[clientNum].macroState) {
	case MACRO_FETCH_FLAG:
		if (enemyFlag) VectorCopy(enemyFlag->s.origin, targetOrigin);
		break;
	case MACRO_RETURN_FLAG:
		if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin);
		break;
	case MACRO_DEFEND_STAND:
		if (myFlag) {
			VectorCopy(myFlag->s.origin, targetOrigin);
			if (bot2_states[clientNum].role == ROLE_BASE) {
				ucmd.weapon = WP_BRYAR_PISTOL;

				if (level.time % 2000 < 50) {
					trap->Print("[%s] Base Defense | Dist to Flag: %.0f | Tripmines: %d\n", ent->client->pers.netname, Distance(ent->client->ps.origin, myFlag->s.origin), Bot2_GetAmmo(ent, WP_TRIP_MINE));
				}

				gentity_t* immediateThreat = GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f);

				if (!immediateThreat && (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) && Bot2_GetAmmo(ent, WP_TRIP_MINE) > 0) {
					if (Distance(ent->client->ps.origin, myFlag->s.origin) < 150.0f) {
						if (currentMineCount < 4) {
							ucmd.weapon = WP_TRIP_MINE;
							lockAim = qtrue;
							wantsToShoot = qtrue;
							VectorCopy(myFlag->s.origin, aimDir);
							aimDir[2] -= 32.0f;
							float angleOffset = currentMineCount * 90.0f * (M_PI / 180.0f);
							aimDir[0] += cos(angleOffset) * 64.0f;
							aimDir[1] += sin(angleOffset) * 64.0f;
							VectorSubtract(aimDir, ent->client->ps.origin, aimDir);
						}
					}
				}
			}

			float patrolRadius = 0.0f;
			if (bot2_states[clientNum].role == ROLE_CHASE) patrolRadius = 400.0f;
			else if (bot2_states[clientNum].role == ROLE_BASE && currentMineCount > 0) patrolRadius = 250.0f;

			if (patrolRadius > 0.0f) {
				float patrolAngle = (level.time / 1500.0f) + (clientNum * 0.5f);
				targetOrigin[0] = myFlag->s.origin[0] + cos(patrolAngle) * patrolRadius;
				targetOrigin[1] = myFlag->s.origin[1] + sin(patrolAngle) * patrolRadius;
				targetOrigin[2] = myFlag->s.origin[2];
			}
			else {
				VectorCopy(myFlag->s.origin, targetOrigin);
			}
		}
		break;
	case MACRO_CHASE_THIEF:
	{
		gentity_t* thief = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) {
				thief = &g_entities[i];
				VectorCopy(thief->client->ps.origin, targetOrigin);

				vec3_t myEye;
				VectorCopy(ent->client->ps.origin, myEye);
				myEye[2] += ent->client->ps.viewheight;

				trace_t tr;
				vec3_t zeroVec = { 0, 0, 0 };
				trap->Trace(&tr, myEye, zeroVec, zeroVec, thief->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (tr.fraction == 1.0f || tr.entityNum == thief->s.number) {
					vec3_t leadPos;
					Bot2_GetLeadOrigin(ent, thief, leadPos);
					lockAim = qtrue;
					wantsToShoot = qtrue;
					VectorSubtract(leadPos, myEye, aimDir);
				}
				break;
			}
		}

		if (!thief) {
			if (droppedMyFlag) {
				VectorCopy(droppedMyFlag->s.origin, targetOrigin);
				if (level.time % 2000 < 50) trap->Print("[%s | %s] Thief dead, fetching DROPPED flag.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
			}
			else if (enemyFlag) {
				VectorCopy(enemyFlag->s.origin, targetOrigin);
				if (level.time % 2000 < 50) trap->Print("[%s | %s] Target lost, intercepting at enemy base.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
			}
		}
		break;
	}
	case MACRO_ESCORT_FC:
		if (bot2_states[clientNum].targetEntNum >= 0) {
			VectorCopy(g_entities[bot2_states[clientNum].targetEntNum].client->ps.origin, targetOrigin);
		}
		break;
	case MACRO_GET_WEAPON:
		if (bot2_states[clientNum].role == ROLE_CHASE) {
			const char* heavy[3] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater" };
			gentity_t* item = GetNearestItem(ent->client->ps.origin, heavy, 3, 99999.0f);
			if (item) VectorCopy(item->s.origin, targetOrigin);
		}
		else if (bot2_states[clientNum].role == ROLE_BASE) {
			const char* trips[1] = { "weapon_trip_mine" };
			gentity_t* item = GetNearestItem(ent->client->ps.origin, trips, 1, 99999.0f);
			if (item) VectorCopy(item->s.origin, targetOrigin);
		}
		break;
	case MACRO_CAMP_REGEN:
	{
		const char* allWeapons[10] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater", "weapon_bowcaster", "weapon_blaster", "weapon_disruptor", "weapon_demp2", "weapon_concussion_rifle" };
		gentity_t* wpn = GetNearestItem(ent->client->ps.origin, allWeapons, 8, 2000.0f);
		if (wpn) {
			VectorCopy(wpn->s.origin, targetOrigin);
		}
		else if (enemyFlag) {
			float patrolAngle = (level.time / 1500.0f) + (clientNum * 0.5f);
			targetOrigin[0] = enemyFlag->s.origin[0] + cos(patrolAngle) * 450.0f;
			targetOrigin[1] = enemyFlag->s.origin[1] + sin(patrolAngle) * 450.0f;
			targetOrigin[2] = enemyFlag->s.origin[2];
		}

		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f);
		if (enemy) {
			vec3_t leadPos;
			Bot2_GetLeadOrigin(ent, enemy, leadPos);
			lockAim = qtrue;
			wantsToShoot = qtrue;
			VectorSubtract(leadPos, ent->client->ps.origin, aimDir);
			aimDir[2] -= ent->client->ps.viewheight;
		}
		break;
	}
	case MACRO_SURVIVAL:
	{
		const char* supplies[3] = { "item_medpak_instant", "item_shield_sm_instant", "item_shield_lrg_instant" };
		gentity_t* hp = GetNearestItem(ent->client->ps.origin, supplies, 3, 3000.0f);
		if (hp) VectorCopy(hp->s.origin, targetOrigin);
		else if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin);

		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1500.0f);
		if (enemy && hp) {
			vec3_t runDir;
			VectorSubtract(ent->client->ps.origin, enemy->client->ps.origin, runDir);
			VectorNormalize(runDir);
			VectorMA(ent->client->ps.origin, 500.0f, runDir, targetOrigin);
		}
		break;
	}
	case MACRO_HUNT_TRIPMINES:
		if (bot2_states[clientNum].targetEntNum >= MAX_CLIENTS) {
			gentity_t* mine = &g_entities[bot2_states[clientNum].targetEntNum];
			if (mine->inuse && mine->r.linked) {
				VectorCopy(mine->s.origin, targetOrigin);
				lockAim = qtrue;
				wantsToShoot = qtrue;
				VectorSubtract(mine->s.origin, ent->client->ps.origin, aimDir);
			}
			else {
				bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
			}
		}
		break;
	}

	// ==============================================================================
	// UNIVERSAL MICRO COMBAT ENGINE
	// ==============================================================================
	qboolean ignoreCombat = (bot2_states[clientNum].role == ROLE_OFFENSE && bot2_states[clientNum].macroState != MACRO_CAMP_REGEN);

	if (!lockAim && !ignoreCombat) {
		vec3_t myEye;
		VectorCopy(ent->client->ps.origin, myEye);
		myEye[2] += ent->client->ps.viewheight;

		gentity_t* bestTarget = GetNearestEnemy(ent->client->ps.origin, botTeam, 2500.0f);
		if (bestTarget) {
			trace_t tr;
			vec3_t zeroVec = { 0, 0, 0 };
			trap->Trace(&tr, myEye, zeroVec, zeroVec, bestTarget->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

			if (tr.fraction == 1.0f || tr.entityNum == bestTarget->s.number) {
				vec3_t leadPos;
				Bot2_GetLeadOrigin(ent, bestTarget, leadPos);
				VectorSubtract(leadPos, myEye, aimDir);
				lockAim = qtrue;
				wantsToShoot = qtrue;
			}
		}
	}

	// --- WEAPON FIRE MODES & TELEMETRY ---
	if (wantsToShoot) {
		if (ucmd.weapon == WP_ROCKET_LAUNCHER) {
			ucmd.buttons |= BUTTON_ATTACK;
		}
		else if (ucmd.weapon == WP_BRYAR_PISTOL) {
			if (level.time - bot2_states[clientNum].chargeTimer < 1500) {
				ucmd.buttons |= BUTTON_ALT_ATTACK; // Hold charge
			}
			else if (level.time - bot2_states[clientNum].chargeTimer < 1600) {
				// Release button to fire the charged shot
			}
			else {
				bot2_states[clientNum].chargeTimer = level.time; // Reset cycle
			}
		}
		else if (ucmd.weapon == WP_REPEATER) {
			if (Bot2_GetAmmo(ent, WP_REPEATER) >= 30) {
				ucmd.buttons |= BUTTON_ALT_ATTACK;
			}
			else {
				ucmd.buttons |= BUTTON_ATTACK;
			}
		}
		else if (ucmd.weapon == WP_FLECHETTE) {
			ucmd.buttons |= BUTTON_ALT_ATTACK;
		}
		else if (ucmd.weapon == WP_TRIP_MINE) {
			ucmd.buttons |= BUTTON_ALT_ATTACK; // Area/Proximity version
		}
		else {
			ucmd.buttons |= BUTTON_ATTACK;
		}

	}
	else {
		bot2_states[clientNum].chargeTimer = level.time; // Keep timer reset when not shooting
	}

	// Hit Prediction Telemetry
	qboolean isSpamWeapon = (ucmd.weapon == WP_REPEATER || ucmd.weapon == WP_FLECHETTE || ucmd.weapon == WP_BLASTER || ucmd.weapon == WP_BRYAR_PISTOL);
	int currentAttackButtons = (ucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));
	int lastAttackButtons = (botstates[clientNum]->lastucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));

	if (currentAttackButtons && !lastAttackButtons) {
		if (!isSpamWeapon && ucmd.weapon != WP_TRIP_MINE) {
			vec3_t forward, end, start;
			trace_t tr;
			AngleVectors(ent->client->ps.viewangles, forward, NULL, NULL);
			VectorCopy(ent->client->ps.origin, start);
			start[2] += ent->client->ps.viewheight;
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

	// --- FORCE OVERRIDES (SPEED / ABSORB / ENERGIZE) ---
	ent->client->ps.fd.forcePowersKnown |= (1 << FP_SPEED) | (1 << FP_ABSORB) | (1 << FP_TEAM_FORCE);
	ent->client->ps.fd.forcePowerLevel[FP_SPEED] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_ABSORB] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_TEAM_FORCE] = 3;

	qboolean wantsSpeed = qtrue;
	if (bot2_states[clientNum].macroState == MACRO_CAMP_REGEN || bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		wantsSpeed = qfalse;
	}

	if (ent->client->ps.fd.forcePower >= 50 && !ent->client->ps.fd.forceButtonNeedRelease) {
		if (bot2_states[clientNum].role == ROLE_BASE && ent->client->ps.fd.forcePower >= 75 && (level.time - bot2_states[clientNum].abilityTimer > 5000)) {
			ucmd.forcesel = FP_TEAM_FORCE;
			ucmd.buttons |= BUTTON_FORCEPOWER;
			bot2_states[clientNum].abilityTimer = level.time;
		}
		else if (wantsSpeed && !(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) {
			ucmd.forcesel = FP_SPEED;
			ucmd.buttons |= BUTTON_FORCEPOWER;
		}
		else if (!wantsSpeed && (ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) {
			ucmd.forcesel = FP_SPEED;
			ucmd.buttons |= BUTTON_FORCEPOWER;
		}
	}

	// --- MICRO NAV/PATHING ---
	vec3_t dir, angles, nextWp = { 0,0,0 };
	qboolean validPath = qfalse;
	float base_target_yaw;
	float distToTarget = VectorLength(targetOrigin) > 1.0f ? Distance(targetOrigin, ent->client->ps.origin) : 999999.0f;

	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		// Stop moving
		ucmd.forwardmove = 0; ucmd.rightmove = 0; ucmd.upmove = 0;
		vectoangles(aimDir, angles);
		ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd;
		trap->BotUserCommand(ent->s.number, &ucmd);
		return;
	}

	if (VectorLength(targetOrigin) > 1.0f) {
		qboolean isChasing = (bot2_states[clientNum].role == ROLE_CHASE && bot2_states[clientNum].macroState == MACRO_CHASE_THIEF);

		if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)targetOrigin, (float*)nextWp)) {
			VectorSubtract(nextWp, ent->client->ps.origin, dir);
			if (VectorLength(dir) > 0.1f) validPath = qtrue;
		}
		else if (isChasing && enemyFlag) {
			// Path to aerial/off-mesh thief failed. Intercept at enemy flag stand!
			VectorCopy(enemyFlag->s.origin, targetOrigin);
			if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)targetOrigin, (float*)nextWp)) {
				VectorSubtract(nextWp, ent->client->ps.origin, dir);
				if (VectorLength(dir) > 0.1f) validPath = qtrue;
			}
		}
	}

	if (!validPath) base_target_yaw = bot2_states[clientNum].targetYaw;
	else { vectoangles(dir, angles); base_target_yaw = angles[YAW]; }

	// --- TRIGGER PUSH / JUMP PAD DETECTION ---
	if (IsInTriggerPush(ent) && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 3;
		bot2_states[clientNum].stateTimer = level.time;
		trap->Print("[%s] STATE Transition: Entered Trigger Push / Jump Pad (State 3)\n", ent->client->pers.netname);
	}

	if (ent->client->ps.groundEntityNum == ENTITYNUM_NONE && bot2_states[clientNum].state != 2 && bot2_states[clientNum].state != 3) {
		bot2_states[clientNum].state = 2;
		bot2_states[clientNum].stateTimer = level.time;
		bot2_states[clientNum].targetYaw = base_target_yaw;
		bot2_states[clientNum].strafeDir = EvaluateStrafeDir(ent, base_target_yaw);
		trap->Print("[%s] STATE Transition: Walking -> Falling/Airborne (State 2)\n", ent->client->pers.netname);
	}

	// --- STATE 3: JUMP PAD / PUSHED ---
	if (bot2_states[clientNum].state == 3) {
		float lookYaw = base_target_yaw;

		// Exact compensation against engine view snapping
		if (lockAim) {
			vectoangles(aimDir, angles);
			lookYaw = angles[YAW];
			ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		}
		else {
			ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
		}
		ucmd.angles[YAW] = ANGLE2SHORT(lookYaw) - ent->client->ps.delta_angles[YAW];

		ucmd.forwardmove = 127; // Force +W
		ucmd.rightmove = 0;
		ucmd.upmove = 0;

		// Exit condition: landed and at least 500ms have passed
		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 500)) {
			bot2_states[clientNum].state = 0;
			trap->Print("[%s] STATE Transition: Landed from Jump Pad -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname);
		}
	}
	// --- STATE 0: WALK/IDLE & RUNNING JUMPS ---
	else if (bot2_states[clientNum].state == 0) {
		bot2_states[clientNum].tele_jumpSeq = 0;

		float brake_dist = 64.0f + (current_speed * 0.5f);
		float danger_yaw = base_target_yaw;
		qboolean ledgeDanger = qfalse;

		if (current_speed > 100.0f && TraceFloorScore(ent, ent->client->ps.origin, vel_yaw, brake_dist, NULL) < 0.0f) {
			ledgeDanger = qtrue;
			danger_yaw = vel_yaw;
		}
		else if (TraceFloorScore(ent, ent->client->ps.origin, base_target_yaw, brake_dist, NULL) < 0.0f) {
			ledgeDanger = qtrue;
			danger_yaw = base_target_yaw;
		}

		if (ledgeDanger) {
			if (!bot2_states[clientNum].ledgeEvading) {
				bot2_states[clientNum].ledgeEvading = 1;
				trap->Print("[%s] STATE Event: Ledge Danger Detected! Evading...\n", ent->client->pers.netname);
			}
		}
		else {
			if (bot2_states[clientNum].ledgeEvading) {
				bot2_states[clientNum].ledgeEvading = 0;
				trap->Print("[%s] STATE Event: Ledge Clear. Resuming normal pathing.\n", ent->client->pers.netname);
			}
		}

		float align_delta = vel_yaw - base_target_yaw;
		while (align_delta > 180.0f) align_delta -= 360.0f;
		while (align_delta < -180.0f) align_delta += 360.0f;
		if (align_delta < 0.0f) align_delta = -align_delta;

		qboolean wantsToJump = (wantsSpeed && current_speed >= max_run_speed - 20.0f && distToTarget > 300.0f && !ledgeDanger && align_delta < 45.0f);
		qboolean jumpExecuted = qfalse;

		if (wantsToJump) {
			int proposedDir = EvaluateStrafeDir(ent, base_target_yaw);
			if (IsSafeToJump(ent, clientNum, ent->client->ps.origin, current_speed, vel_yaw, proposedDir, max_run_speed, NULL, NULL, NULL, NULL)) {
				bot2_states[clientNum].state = 2;
				bot2_states[clientNum].stateTimer = level.time;
				bot2_states[clientNum].targetYaw = base_target_yaw;
				bot2_states[clientNum].strafeDir = proposedDir;
				ucmd.upmove = 127;
				jumpExecuted = qtrue;
				trap->Print("[%s] STATE Transition: Walking -> Executing Running Jump (State 2)\n", ent->client->pers.netname);
			}
		}

		if (!jumpExecuted) {
			if (lockAim) {
				float moveYaw = base_target_yaw;
				qboolean hasEscape = qfalse;

				if (ledgeDanger) {
					float safe_yaw;
					hasEscape = GetSafeEscapeYaw(ent, danger_yaw, &safe_yaw);
					if (hasEscape) {
						moveYaw = safe_yaw;
					}
					else {
						moveYaw = danger_yaw;
					}
				}

				vectoangles(aimDir, angles);
				ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW];
				ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];

				if (ledgeDanger && !hasEscape) {
					ucmd.forwardmove = 0; ucmd.rightmove = 0;
				}
				else if (current_speed <= max_run_speed + 10.0f) {
					float diff = (moveYaw - angles[YAW]) * (M_PI / 180.0f);
					float fwd = 127.0f * cos(diff);
					float rgt = 127.0f * -sin(diff);

					if (fwd > 127.0f) fwd = 127.0f; if (fwd < -127.0f) fwd = -127.0f;
					if (rgt > 127.0f) rgt = 127.0f; if (rgt < -127.0f) rgt = -127.0f;

					ucmd.forwardmove = (char)fwd;
					ucmd.rightmove = (char)rgt;
				}
				else {
					ucmd.forwardmove = 0; ucmd.rightmove = 0;
				}
			}
			else {
				ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

				if (ledgeDanger) {
					float safe_yaw;
					if (GetSafeEscapeYaw(ent, danger_yaw, &safe_yaw)) {
						ucmd.angles[YAW] = ANGLE2SHORT(safe_yaw) - ent->client->ps.delta_angles[YAW];
						ucmd.forwardmove = 127; ucmd.rightmove = 0;
					}
					else {
						ucmd.angles[YAW] = ANGLE2SHORT(danger_yaw) - ent->client->ps.delta_angles[YAW];
						ucmd.forwardmove = 0; ucmd.rightmove = 0;
					}
				}
				else {
					ucmd.angles[YAW] = ANGLE2SHORT(base_target_yaw) - ent->client->ps.delta_angles[YAW];
					if (current_speed > max_run_speed + 10.0f) ucmd.forwardmove = 0;
					else ucmd.forwardmove = 127;
					ucmd.rightmove = 0;
				}
			}
		}

		if (!jumpExecuted) ucmd.upmove = 0;
	}
	// --- STATE 2: AIRBORNE ---
	else if (bot2_states[clientNum].state == 2) {
		int sDir = bot2_states[clientNum].strafeDir;
		float magic_angle = 0.0f;

		if (current_speed > max_run_speed - 15.0f) {
			float acos_val = (max_run_speed - 15.0f) / current_speed;
			magic_angle = acos((acos_val < -1.0f) ? -1.0f : ((acos_val > 1.0f) ? 1.0f : acos_val)) * (180.0f / M_PI);
		}

		float thrust_yaw = AngleMod(vel_yaw - ((magic_angle - 45.0f) * sDir));

		if (lockAim) {
			vectoangles(aimDir, angles);
			float lookYaw = angles[YAW];
			ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
			ucmd.angles[YAW] = ANGLE2SHORT(lookYaw) - ent->client->ps.delta_angles[YAW];

			float thrust_diff = (thrust_yaw - lookYaw) * (M_PI / 180.0f);
			ucmd.forwardmove = (char)(127.0f * cos(thrust_diff));
			ucmd.rightmove = (char)(127.0f * -sin(thrust_diff));
		}
		else {
			float lookYaw = thrust_yaw;
			ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
			ucmd.angles[YAW] = ANGLE2SHORT(lookYaw) - ent->client->ps.delta_angles[YAW];

			ucmd.forwardmove = 127;
			ucmd.rightmove = 127 * sDir;
		}

		ucmd.upmove = (level.time - bot2_states[clientNum].stateTimer < 100) ? 127 : 0;

		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 250)) {

			// Calculate actual landing slope FIRST so telemetry can read it
			trace_t landTr;
			vec3_t landEnd, zeroVec = { 0, 0, 0 };
			VectorCopy(ent->client->ps.origin, landEnd); landEnd[2] -= 64.0f;
			trap->Trace(&landTr, ent->client->ps.origin, zeroVec, zeroVec, landEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			float landSlope = (landTr.fraction < 1.0f) ? acos(landTr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

			// --- PREDICTED VS ACTUAL TELEMETRY ---
			if (bot2_states[clientNum].tele_inAir && ent->client) {
				bot2_states[clientNum].tele_inAir = 0;

				vec3_t actualLandPos;
				VectorCopy(ent->client->ps.origin, actualLandPos);

				// Calculate actual 2D distance traveled
				float dx = actualLandPos[0] - bot2_states[clientNum].tele_startPos[0];
				float dy = actualLandPos[1] - bot2_states[clientNum].tele_startPos[1];
				float actualDist = sqrt((dx * dx) + (dy * dy));

				// Calculate Z-drop (Height difference)
				float actualZDrop = bot2_states[clientNum].tele_startPos[2] - actualLandPos[2];
				float predZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_predPos[2];

				// Deltas
				float distError = actualDist - bot2_states[clientNum].tele_predDist;
				float zError = actualZDrop - predZDrop;
				float xError = actualLandPos[0] - bot2_states[clientNum].tele_predPos[0];
				float yError = actualLandPos[1] - bot2_states[clientNum].tele_predPos[1];
				int jumpTime = level.time - bot2_states[clientNum].tele_jumpStartTime;

				// Calculate the actual degrees the bot rotated while in the air
				float yaw_delta = vel_yaw - bot2_states[clientNum].tele_takeoffYaw;
				while (yaw_delta > 180.0f) yaw_delta -= 360.0f;
				while (yaw_delta < -180.0f) yaw_delta += 360.0f;

				trap->Print("[%s] JUMP %d | T: %dms | Spd: %.0f | D: %.1f (Err %+.1f) | Z: %.1f (Err %+.1f) | XY Err: %+.1f, %+.1f | Turn: %+.1f deg\n",
					ent->client->pers.netname,
					bot2_states[clientNum].tele_jumpSeq,
					jumpTime,
					bot2_states[clientNum].tele_takeoffSpd,   // Restored Speed output
					actualDist, distError,
					actualZDrop, zError,
					xError, yError,
					yaw_delta                                  // New turn output
				);
			}

			int nextDirToUse = (landSlope > 5.0f) ? EvaluateStrafeDir(ent, base_target_yaw) : sDir * -1;

			qboolean safeToChainJump = qfalse;
			float angle_delta = vel_yaw - base_target_yaw;
			while (angle_delta > 180.0f) angle_delta -= 360.0f;
			while (angle_delta < -180.0f) angle_delta += 360.0f;
			if (angle_delta < 0.0f) angle_delta = -angle_delta;

			if (wantsSpeed && distToTarget > 500.0f && angle_delta <= 135.0f && current_speed > (max_run_speed * 0.2f)) {
				int testDirs[2] = { nextDirToUse, nextDirToUse * -1 };
				for (int d = 0; d < 2; d++) {
					if (IsSafeToJump(ent, clientNum, ent->client->ps.origin, current_speed, vel_yaw, testDirs[d], max_run_speed, NULL, NULL, NULL, NULL)) {
						safeToChainJump = qtrue; nextDirToUse = testDirs[d]; break;
					}
				}
			}
			else if (wantsSpeed && current_speed <= (max_run_speed * 0.2f)) safeToChainJump = qtrue;

			if (safeToChainJump) {
				bot2_states[clientNum].strafeDir = nextDirToUse;
				ucmd.upmove = 127;
				trap->Print("[%s] STATE Event: Executing Chain Jump (State 2)\n", ent->client->pers.netname);
			}
			else {
				bot2_states[clientNum].state = 0;
				trap->Print("[%s] STATE Transition: Landed -> Resuming Walk/Run (State 0)\n", ent->client->pers.netname);
			}

			bot2_states[clientNum].stateTimer = level.time;
		}
	}

	// --- ANTI-SNAG ENGINE ---
	float dx = ent->client->ps.origin[0] - bot2_states[clientNum].stuck_pos[0];
	float dy = ent->client->ps.origin[1] - bot2_states[clientNum].stuck_pos[1];
	float dz = ent->client->ps.origin[2] - bot2_states[clientNum].stuck_pos[2];

	if ((dx * dx) + (dy * dy) + (dz * dz) > 625.0f) {
		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		bot2_states[clientNum].stuck_timer = level.time;
	}
	else if (distToTarget > 150.0f) {
		int stuck_duration = level.time - bot2_states[clientNum].stuck_timer;
		if (stuck_duration > 5000) { G_Kill(ent); return; }
		else if (stuck_duration > 3000) {
			if (level.time - bot2_states[clientNum].unstuck_phase_timer > 400) {
				bot2_states[clientNum].unstuck_phase = rand() % 5;
				bot2_states[clientNum].unstuck_phase_timer = level.time;
			}
			if (bot2_states[clientNum].unstuck_phase == 0) { ucmd.forwardmove = -127; ucmd.rightmove = 0; ucmd.upmove = 127; }
			else if (bot2_states[clientNum].unstuck_phase == 1) { ucmd.forwardmove = 127; ucmd.rightmove = 127; ucmd.upmove = -127; }
			else if (bot2_states[clientNum].unstuck_phase == 2) { ucmd.forwardmove = 127; ucmd.rightmove = -127; ucmd.upmove = -127; }
			else if (bot2_states[clientNum].unstuck_phase == 3) { ucmd.forwardmove = -127; ucmd.rightmove = (rand() % 2 == 0) ? 127 : -127; ucmd.upmove = 127; }
			else { ucmd.forwardmove = 127; ucmd.rightmove = 0; ucmd.upmove = 127; }

			float wobble = (bot2_states[clientNum].unstuck_phase % 2 == 0) ? 15.0f : -15.0f;
			ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW] + (bot2_states[clientNum].unstuck_phase * wobble)) - ent->client->ps.delta_angles[YAW];
			bot2_states[clientNum].state = 0;
		}
	}
	else bot2_states[clientNum].stuck_timer = level.time;

	botstates[clientNum]->lastucmd = ucmd;
	trap->BotUserCommand(ent->s.number, &ucmd);
}

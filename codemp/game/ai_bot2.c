// no speed caps
#include "g_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

extern void G_Kill(gentity_t* ent);

// ==============================================================================
// State Machine Tracker
// ==============================================================================
static struct {
	// STATE MACHINE
	int state;           // 0 = Walk, 1 = Wind-up, 2 = Airborne
	int stateTimer;      // Tracks when the current state started
	float targetYaw;     // The original heading to the waypoint
	int strafeDir;       // 1 = Right, -1 = Left
	int spawnCooldown;
	int hadFlag;         // Tracks flag possession for suicide on cap

	// MID-AIR TELEMETRY ENGINE
	int tele_inAir;
	int tele_jumpSeq;
	int tele_jumpStartTime;
	int tele_midAirTime;
	int tele_predDir;
	int tele_actDir;

	float tele_takeoffSpd;
	float tele_predDist;
	float tele_groundSlope;

	vec3_t tele_startPos;
	vec3_t tele_predPos;
	vec3_t tele_midPredPos;
	vec3_t tele_prevPos;
	vec3_t tele_crossPos;

	qboolean tele_crossedZ;
	qboolean tele_midAirLogged;

	// DIAGNOSTICS & THROTTLES
	int forceTimer;
	int diagTimer;
	char lastFailReason[128];
} bot2_states[MAX_CLIENTS] = { 0 };

// ==============================================================================
// Helper: Target Acquisition
// ==============================================================================
static qboolean GetBotTarget(gentity_t* ent, vec3_t targetOrigin) {
	if (g_gametype.integer == GT_CTF) {
		int botTeam = ent->client->sess.sessionTeam;
		int enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
		char* targetClass = (ent->client->ps.powerups[enemyFlagItem]) ?
			((botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag") :
			((botTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag");

		gentity_t* flagEnt = G_Find(NULL, FOFS(classname), targetClass);
		if (flagEnt) {
			VectorCopy(flagEnt->s.origin, targetOrigin);
			return qtrue;
		}
	}

	gentity_t* player = &g_entities[0];
	if (player && player->inuse && player->client && player != ent) {
		VectorCopy(player->client->ps.origin, targetOrigin);
		return qtrue;
	}
	return qfalse;
}

// ==============================================================================
// Helper: Trigger Hurt Scanner
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
				if (strcmp(hitEnt->classname, "trigger_hurt") == 0) return qtrue;
			}
		}

		VectorCopy(tr.endpos, currentStart);
		currentStart[2] -= 8.0f;
		if (currentStart[2] <= end[2]) break;
	}
	return qfalse;
}

// ==============================================================================
// Helper: Initial Strafe Direction Evaluation
// ==============================================================================
static int EvaluateStrafeDir(gentity_t* ent, float base_target_yaw) {
	vec3_t flatAngles = { 0, base_target_yaw, 0 };
	vec3_t forward, right;
	AngleVectors(flatAngles, forward, right, NULL);

	vec3_t start, forwardPos, endRight, endLeft;
	VectorCopy(ent->client->ps.origin, start);
	VectorMA(start, 64.0f, forward, forwardPos);
	VectorMA(forwardPos, 64.0f, right, endRight);
	VectorMA(forwardPos, -64.0f, right, endLeft);

	trace_t tr;
	vec3_t mins = { -15, -15, -24 };
	vec3_t maxs = { 15, 15, 32 };
	float scoreRight = 0.0f, scoreLeft = 0.0f;

	// Right Trace
	trap->Trace(&tr, start, mins, maxs, endRight, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (tr.fraction < 1.0f) scoreRight -= 10.0f;
	else {
		vec3_t downEnd;
		VectorCopy(tr.endpos, downEnd);
		downEnd[2] -= 450.0f;
		trap->Trace(&tr, tr.endpos, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		if (tr.fraction == 1.0f || CheckForTriggerHurt(ent, tr.endpos, tr.endpos, mins, maxs)) scoreRight -= 1000.0f;
	}

	// Left Trace
	trap->Trace(&tr, start, mins, maxs, endLeft, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (tr.fraction < 1.0f) scoreLeft -= 10.0f;
	else {
		vec3_t downEnd;
		VectorCopy(tr.endpos, downEnd);
		downEnd[2] -= 450.0f;
		trap->Trace(&tr, tr.endpos, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		if (tr.fraction == 1.0f || CheckForTriggerHurt(ent, tr.endpos, tr.endpos, mins, maxs)) scoreLeft -= 1000.0f;
	}

	if (scoreLeft > scoreRight) return -1;
	if (scoreRight > scoreLeft) return 1;
	return (rand() % 2 == 0) ? 1 : -1;
}

// ==============================================================================
// Helper: Physics Trace Prediction (CURVED KINEMATICS VERSION)
// ==============================================================================
static qboolean IsSafeToJump(gentity_t* ent, int clientNum, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, qboolean isSimulation) {
	trace_t tr;
	vec3_t start, test_pos;
	vec3_t player_mins = { -15.0f, -15.0f, -24.0f };
	vec3_t player_maxs = { 15.0f, 15.0f, 32.0f };

	VectorCopy(ent->client->ps.origin, start);

	// --- PHASE 1: ENVIRONMENTAL SLOPE & STAIR SCANS ---
	trace_t trSlope;
	vec3_t slopeEnd;
	VectorCopy(start, slopeEnd);
	slopeEnd[2] -= 64.0f;
	trap->Trace(&trSlope, start, vec3_origin, vec3_origin, slopeEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float slopeAngle = (trSlope.fraction < 1.0f) ? acos(trSlope.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

	// The Universal Stair Scanner
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
		if (stepDiff > 4.0f || (stepDiff < -4.0f && stepDiff > -32.0f)) {
			slopeAngle = 45.0f; // Force slope bans to prevent Bermuda Triangle deaths
		}
	}

	// --- PHASE 2: ENERGY KINEMATICS ---
	float predicted_airtime = 0.925f;
	if (slopeAngle > 5.0f) {
		float slope_bonus = slopeAngle * 0.01f;
		predicted_airtime += (slope_bonus > 0.30f) ? 0.30f : slope_bonus;
	}
	if (current_speed > 600.0f) predicted_airtime -= 0.02f;

	float jump_z_vel = 400.0f * predicted_airtime;
	float speed_scale = max_run_speed / 250.0f;
	float jump_constant = 112500.0f * (speed_scale * speed_scale);

	float predicted_land_speed = sqrt((current_speed * current_speed) + jump_constant);
	float avg_speed = (current_speed + predicted_land_speed) / 2.0f;
	if (avg_speed < max_run_speed) avg_speed = max_run_speed;

	float base_dist = (avg_speed * predicted_airtime) * 1.02f;
	float dynamic_secant = 12.5f - ((current_speed - max_run_speed) * 0.0066f);
	if (dynamic_secant < 5.0f) dynamic_secant = 5.0f;

	// --- PHASE 3: POINT-TO-POINT ARC TUNNEL SWEEP ---
	vec3_t segment_start;
	VectorCopy(start, segment_start);
	segment_start[2] += 32.0f; // Elevate origin slightly to ride the arc

	for (int i = 1; i <= 3; i++) {
		float frac = i * 0.25f;
		float probe_dist = base_dist * frac;

		float point_yaw = vel_yaw - (dynamic_secant * testDir * frac);
		float point_rad = point_yaw * (M_PI / 180.0f);

		float t_probe = probe_dist / avg_speed;
		float arc_z_probe = (jump_z_vel * t_probe) - (400.0f * t_probe * t_probe);

		vec3_t probe_pos, probeDownStart, probeDownEnd;
		probe_pos[0] = start[0] + (cos(point_rad) * probe_dist);
		probe_pos[1] = start[1] + (sin(point_rad) * probe_dist);
		probe_pos[2] = start[2] + 32.0f + arc_z_probe;

		// Tunnel Segment Trace
		trap->Trace(&tr, segment_start, player_mins, player_maxs, probe_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
		if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) {
			if (failReason) sprintf(failReason, "Mid-curve wall hit (Seg %d)", i);
			return qfalse;
		}

		VectorCopy(probe_pos, segment_start); // Advance the tunnel start point
		VectorCopy(probe_pos, probeDownStart);
		VectorCopy(probeDownStart, probeDownEnd);
		probeDownEnd[2] -= 2048.0f;

		// Drop Segment Trace
		trap->Trace(&tr, probeDownStart, player_mins, player_maxs, probeDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		int pct = i * 25;
		if (tr.fraction == 1.0f) {
			if (warningString && warningString[0] == '\0') sprintf(warningString, "Pit at %d%% {%.0f, %.0f, %.0f}", pct, probeDownStart[0], probeDownStart[1], probeDownStart[2]);
			if (slopeAngle > 5.0f) { if (failReason) sprintf(failReason, "Aborted: Mid-air drop while on slope (%d%%)", pct); return qfalse; }
		}
		else if ((start[2] - tr.endpos[2]) > 450.0f) {
			if (warningString && warningString[0] == '\0') sprintf(warningString, "Drop %.1f at %d%%", (start[2] - tr.endpos[2]), pct);
			if (slopeAngle > 5.0f) { if (failReason) sprintf(failReason, "Aborted: Mid-air drop while on slope (%d%%)", pct); return qfalse; }
		}
		else if (CheckForTriggerHurt(ent, probeDownStart, tr.endpos, player_mins, player_maxs)) {
			if (warningString && warningString[0] == '\0') sprintf(warningString, "Hurt at %d%%", pct);
			if (slopeAngle > 5.0f) { if (failReason) sprintf(failReason, "Aborted: Mid-air hurt while on slope (%d%%)", pct); return qfalse; }
		}
	}

	// --- PHASE 4: FINAL CHORD & DISTANCE PREDICTION ---
	float final_rad = (vel_yaw - (dynamic_secant * testDir)) * (M_PI / 180.0f);
	float trace_dx_final = cos(final_rad);
	float trace_dy_final = sin(final_rad);

	test_pos[0] = start[0] + trace_dx_final * base_dist;
	test_pos[1] = start[1] + trace_dy_final * base_dist;
	test_pos[2] = start[2] + 24.0f;

	trap->Trace(&tr, segment_start, player_mins, player_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	vec3_t downStart, downEnd;
	VectorCopy(tr.endpos, downStart);
	VectorCopy(downStart, downEnd);
	downEnd[2] -= 2048.0f;
	trap->Trace(&tr, downStart, player_mins, player_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	if (tr.fraction == 1.0f && warningString && warningString[0] == '\0') sprintf(warningString, "Base drop pit");
	else if (CheckForTriggerHurt(ent, downStart, tr.endpos, player_mins, player_maxs) && warningString && warningString[0] == '\0') sprintf(warningString, "Base drop hurt");

	float dropHeight = start[2] - tr.endpos[2];
	float touchdown_dist = base_dist;
	float effective_drop = 0.0f;
	float expected_airtime = predicted_airtime;

	if (dropHeight > 16.0f || dropHeight < -16.0f) {
		effective_drop = dropHeight;
		if (effective_drop < -45.0f) effective_drop = -45.0f;
		if (effective_drop > 800.0f) effective_drop = 800.0f;

		float fall_time = sqrt(((jump_z_vel * jump_z_vel) / 1600.0f + effective_drop) / 400.0f);
		expected_airtime = (predicted_airtime / 2.0f) + fall_time;
		touchdown_dist = avg_speed * expected_airtime;
	}

	// Tornado Drift Guard
	if (expected_airtime > 1.25f) {
		if (failReason) sprintf(failReason, "Aborted: Free-fall exceeds 1.25s (Tornado Drift Risk)");
		return qfalse;
	}

	// --- PHASE 5: TOUCHDOWN & OVERSHOOT GUARDS ---
	test_pos[0] = start[0] + trace_dx_final * touchdown_dist;
	test_pos[1] = start[1] + trace_dy_final * touchdown_dist;
	test_pos[2] = start[2] + 24.0f;

	trap->Trace(&tr, segment_start, player_mins, player_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	if (tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f) {
		if (failReason) sprintf(failReason, "Final touchdown wall smash");
		return qfalse;
	}

	VectorCopy(tr.endpos, downStart);
	VectorCopy(downStart, downEnd);
	downEnd[2] -= 2048.0f;
	trap->Trace(&tr, downStart, player_mins, player_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	float finalDropHeight = start[2] - tr.endpos[2];

	if (effective_drop > 64.0f && finalDropHeight < (effective_drop - 128.0f)) { if (failReason) sprintf(failReason, "Hallucinated Distance (Mismatch)"); return qfalse; }
	if (tr.fraction == 1.0f) { if (failReason) sprintf(failReason, "Final touchdown pit"); return qfalse; }
	if (finalDropHeight > 450.0f) { if (failReason) sprintf(failReason, "Final drop too deep (%.1f)", finalDropHeight); return qfalse; }
	if (CheckForTriggerHurt(ent, downStart, tr.endpos, player_mins, player_maxs)) { if (failReason) sprintf(failReason, "Final touchdown trigger_hurt"); return qfalse; }

	// Overshoot Trace (Static 50 units)
	vec3_t over_pos, overDownStart, overDownEnd;
	float over_dist = touchdown_dist + 50.0f;
	over_pos[0] = start[0] + trace_dx_final * over_dist;
	over_pos[1] = start[1] + trace_dy_final * over_dist;
	over_pos[2] = start[2] + 24.0f;

	trap->Trace(&tr, start, player_mins, player_maxs, over_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
	VectorCopy(tr.endpos, overDownStart);
	VectorCopy(overDownStart, overDownEnd);
	overDownEnd[2] -= 2048.0f;
	trap->Trace(&tr, overDownStart, player_mins, player_maxs, overDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

	if (tr.fraction == 1.0f) { if (failReason) sprintf(failReason, "Overshoot Danger (Pit)"); return qfalse; }
	if (start[2] - tr.endpos[2] > 450.0f) { if (failReason) sprintf(failReason, "Overshoot Danger (Deep Drop)"); return qfalse; }

	// Parabolic Arc Check
	float z_increase = -finalDropHeight;
	if (z_increase > 0.0f) {
		float t_reach = touchdown_dist / avg_speed;
		float arc_height = (jump_z_vel * t_reach) - (400.0f * t_reach * t_reach);
		if ((arc_height + 18.0f) < z_increase) {
			if (failReason) sprintf(failReason, "Parabolic Faceplant (Need: %.1f, Arc: %.1f)", z_increase, arc_height);
			return qfalse;
		}
	}

	// --- PHASE 6: MIDAIR TELEMETRY REGISTRATION ---
	// Only apply state changes if this is a real commitment to jump, not a pre-check
	if (!isSimulation) {
		bot2_states[clientNum].tele_jumpSeq++;
		bot2_states[clientNum].tele_jumpStartTime = level.time;
		bot2_states[clientNum].tele_groundSlope = slopeAngle;
		bot2_states[clientNum].tele_crossedZ = qfalse;
		bot2_states[clientNum].tele_predDir = testDir;
		bot2_states[clientNum].tele_midAirTime = level.time + (int)((expected_airtime * 1000.0f) * 0.5f);
		bot2_states[clientNum].tele_midAirLogged = qfalse;

		// Accelerative Mid-Air Tracking
		float mid_t = expected_airtime * 0.5f;
		float accel = (predicted_land_speed - current_speed) / expected_airtime;
		float mid_dist = (current_speed * mid_t) + (0.5f * accel * mid_t * mid_t);
		float mid_rad = (vel_yaw - (dynamic_secant * testDir * 0.5f)) * (M_PI / 180.0f);

		bot2_states[clientNum].tele_midPredPos[0] = start[0] + (cos(mid_rad) * mid_dist);
		bot2_states[clientNum].tele_midPredPos[1] = start[1] + (sin(mid_rad) * mid_dist);
		bot2_states[clientNum].tele_midPredPos[2] = start[2] + ((jump_z_vel * mid_t) - (400.0f * mid_t * mid_t));

		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);
		bot2_states[clientNum].tele_predPos[0] = start[0] + (trace_dx_final * touchdown_dist);
		bot2_states[clientNum].tele_predPos[1] = start[1] + (trace_dy_final * touchdown_dist);
		bot2_states[clientNum].tele_predPos[2] = tr.endpos[2];
		bot2_states[clientNum].tele_predDist = touchdown_dist;
		bot2_states[clientNum].tele_takeoffSpd = current_speed;
		bot2_states[clientNum].tele_inAir = 1;
	}

	return qtrue;
}

// ==============================================================================
// Bot2_Think Core Engine
// ==============================================================================
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum];
	usercmd_t ucmd;
	char serverCmd[1024];

	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {}
	ent->client->inactivityTime = level.time + 1000000;

	// --- Z-CROSS TRACKER (ELEVATED) ---
	if (bot2_states[clientNum].tele_inAir && !bot2_states[clientNum].tele_crossedZ) {
		if (level.time > bot2_states[clientNum].tele_jumpStartTime + 100) {
			if (bot2_states[clientNum].tele_prevPos[2] >= bot2_states[clientNum].tele_predPos[2] &&
				ent->client->ps.origin[2] <= bot2_states[clientNum].tele_predPos[2]) {

				float z_diff_total = ent->client->ps.origin[2] - bot2_states[clientNum].tele_prevPos[2];
				if (z_diff_total == 0.0f) {
					VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_crossPos);
				}
				else {
					float t = (bot2_states[clientNum].tele_predPos[2] - bot2_states[clientNum].tele_prevPos[2]) / z_diff_total;
					bot2_states[clientNum].tele_crossPos[0] = bot2_states[clientNum].tele_prevPos[0] + (t * (ent->client->ps.origin[0] - bot2_states[clientNum].tele_prevPos[0]));
					bot2_states[clientNum].tele_crossPos[1] = bot2_states[clientNum].tele_prevPos[1] + (t * (ent->client->ps.origin[1] - bot2_states[clientNum].tele_prevPos[1]));
					bot2_states[clientNum].tele_crossPos[2] = bot2_states[clientNum].tele_predPos[2];
				}
				bot2_states[clientNum].tele_crossedZ = qtrue;
			}
		}
		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
	}

	// --- FATALITIES & RECOVERY ---
	if (ent->health <= 0) {
		bot2_states[clientNum].spawnCooldown = level.time;
		memset(&ucmd, 0, sizeof(ucmd));
		ucmd.serverTime = time;
		ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		if (bot2_states[clientNum].tele_inAir && ent->client) {
			float miss_x, miss_y, cleared_dist = 0.0f;
			char crossStr[128] = "Never Crossed Z (Died above pad)";

			if (bot2_states[clientNum].tele_crossedZ) {
				miss_x = bot2_states[clientNum].tele_crossPos[0] - bot2_states[clientNum].tele_predPos[0];
				miss_y = bot2_states[clientNum].tele_crossPos[1] - bot2_states[clientNum].tele_predPos[1];
				float cx = bot2_states[clientNum].tele_crossPos[0] - bot2_states[clientNum].tele_startPos[0];
				float cy = bot2_states[clientNum].tele_crossPos[1] - bot2_states[clientNum].tele_startPos[1];
				cleared_dist = sqrt((cx * cx) + (cy * cy));
				sprintf(crossStr, "CrossZ: {%.0f, %.0f, %.0f} (Cleared: %.1f units)", bot2_states[clientNum].tele_crossPos[0], bot2_states[clientNum].tele_crossPos[1], bot2_states[clientNum].tele_crossPos[2], cleared_dist);
			}
			else {
				miss_x = ent->client->ps.origin[0] - bot2_states[clientNum].tele_predPos[0];
				miss_y = ent->client->ps.origin[1] - bot2_states[clientNum].tele_predPos[1];
			}

			trap->Print(va("[Bot2 FATAL] Time: %d | Seq: %d | PIT / DEATH!\n"
				"  -> 2D Horiz Miss: %.1f units from predicted pad\n"
				"  -> %s\n"
				"  -> Start: {%.0f, %.0f, %.0f} (Slope: %.1f deg)\n"
				"  -> Pred:  {%.0f, %.0f, %.0f} (Exp Drop: %.1f)\n"
				"  -> Dead:  {%.0f, %.0f, %.0f} (Act Drop: %.1f)\n",
				level.time, bot2_states[clientNum].tele_jumpSeq, sqrt((miss_x * miss_x) + (miss_y * miss_y)), crossStr,
				(double)bot2_states[clientNum].tele_startPos[0], (double)bot2_states[clientNum].tele_startPos[1], (double)bot2_states[clientNum].tele_startPos[2], (double)bot2_states[clientNum].tele_groundSlope,
				(double)bot2_states[clientNum].tele_predPos[0], (double)bot2_states[clientNum].tele_predPos[1], (double)bot2_states[clientNum].tele_predPos[2], (double)(bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_predPos[2]),
				(double)ent->client->ps.origin[0], (double)ent->client->ps.origin[1], (double)ent->client->ps.origin[2], (double)(bot2_states[clientNum].tele_startPos[2] - ent->client->ps.origin[2])));

			bot2_states[clientNum].tele_inAir = 0;
			bot2_states[clientNum].tele_jumpSeq = 0;
		}

		if (ent->client && (ent->client->ps.velocity[2] < -300.0f || ent->client->ps.fallingToDeath)) {
			ent->client->respawnTime = level.time;
			ent->client->ps.pm_time = 0;
		}

		botstates[clientNum]->lastucmd = ucmd;
		trap->BotUserCommand(ent->s.number, &ucmd);
		return;
	}

	if (ent->client->ps.fallingToDeath) G_Kill(ent);

	// --- SYSTEM OVERRIDES (Capping & Buffs) ---
	int botTeam = ent->client->sess.sessionTeam;
	int enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
	qboolean currentlyHasFlag = (ent->client->ps.powerups[enemyFlagItem] != 0);

	if (bot2_states[clientNum].hadFlag && !currentlyHasFlag && ent->health > 0) { bot2_states[clientNum].hadFlag = 0; G_Kill(ent); return; }
	bot2_states[clientNum].hadFlag = currentlyHasFlag;

	ucmd = botstates[clientNum]->lastucmd;
	ucmd.serverTime = time;
	ucmd.buttons = 0;

	if (level.time - ent->client->pers.enterTime < 200) {
		bot2_states[clientNum].spawnCooldown = level.time;
		ucmd.forwardmove = 0; ucmd.rightmove = 0; ucmd.upmove = 0;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd;
		trap->BotUserCommand(ent->s.number, &ucmd);
		return;
	}

	if (ent->client->ps.weapon != WP_BRYAR_PISTOL && ent->client->ps.weaponTime <= 0) {
		if (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_BRYAR_PISTOL)) ucmd.weapon = WP_BRYAR_PISTOL;
	}

	ent->client->ps.fd.forcePowersKnown |= (1 << FP_SPEED);
	ent->client->ps.fd.forcePowerLevel[FP_SPEED] = 3;
	if (!(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) {
		if (ent->client->ps.fd.forcePower >= 50) {
			ent->client->ps.fd.forcePowersActive |= (1 << FP_SPEED);
			ent->client->ps.fd.forcePower -= 50;
		}
	}

	// --- TARGET ACQUISITION & PATHING ---
	vec3_t targetOrigin = { 0,0,0 };
	vec3_t dir, angles, nextWp = { 0,0,0 };
	qboolean validPath = qfalse;
	float base_target_yaw;

	qboolean hasTarget = GetBotTarget(ent, targetOrigin);
	float distToTarget = 999999.0f;

	if (hasTarget && VectorLength(targetOrigin) > 1.0f) {
		vec3_t tDir;
		VectorSubtract(targetOrigin, ent->client->ps.origin, tDir);
		distToTarget = VectorLength(tDir); // 3D distance to the ultimate objective

		if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)targetOrigin, (float*)nextWp)) {
			VectorSubtract(nextWp, ent->client->ps.origin, dir);
			if (VectorLength(dir) > 0.1f) validPath = qtrue;
		}
	}

	if (!validPath) {
		if (bot2_states[clientNum].state == 2) {
			base_target_yaw = bot2_states[clientNum].targetYaw; // Mid-Air Amnesia Fix
		}
		else {
			bot2_states[clientNum].state = 0;

			// Idle Wall-Bump & Circle Jump Guard
			vec3_t start, end, forward;
			trace_t trWall;
			vec3_t player_mins = { -15.0f, -15.0f, -24.0f };
			vec3_t player_maxs = { 15.0f, 15.0f, 32.0f };

			AngleVectors((vec3_t) { 0, ent->client->ps.viewangles[YAW], 0 }, forward, NULL, NULL);
			VectorCopy(ent->client->ps.origin, start);
			VectorMA(start, 64.0f, forward, end);
			trap->Trace(&trWall, start, player_mins, player_maxs, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

			ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

			if (trWall.fraction < 1.0f && trWall.plane.normal[2] < 0.7f) {
				ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
				ucmd.forwardmove = -127; ucmd.rightmove = 0; ucmd.upmove = 0;
			}
			else {
				int jumpStrafeDir = ((level.time / 1000) % 2 == 0) ? 1 : -1;
				ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW] + (jumpStrafeDir * -15.0f)) - ent->client->ps.delta_angles[YAW];
				ucmd.forwardmove = 127; ucmd.rightmove = 127 * jumpStrafeDir; ucmd.upmove = 127;
			}

			botstates[clientNum]->lastucmd = ucmd;
			trap->BotUserCommand(ent->s.number, &ucmd);
			return;
		}
	}
	else {
		vectoangles(dir, angles);
		base_target_yaw = angles[YAW];
	}

	ucmd.angles[ROLL] = ANGLE2SHORT(angles[ROLL]) - ent->client->ps.delta_angles[ROLL];

	float vel_x = ent->client->ps.velocity[0];
	float vel_y = ent->client->ps.velocity[1];
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));
	float vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;

	// --- STATE 0: WALK/IDLE ---
	if (bot2_states[clientNum].state == 0) {
		bot2_states[clientNum].tele_jumpSeq = 0;

		vec3_t start, end, forward, downStart, downEnd;
		trace_t ledgeTr;
		vec3_t player_mins = { -15.0f, -15.0f, -24.0f };
		vec3_t player_maxs = { 15.0f, 15.0f, 32.0f };

		AngleVectors((vec3_t) { 0, base_target_yaw, 0 }, forward, NULL, NULL);
		VectorCopy(ent->client->ps.origin, start);
		VectorMA(start, 64.0f, forward, end);
		trap->Trace(&ledgeTr, start, player_mins, player_maxs, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		qboolean ledgeDanger = qfalse;
		VectorCopy(ledgeTr.endpos, downStart);
		VectorCopy(downStart, downEnd);
		downEnd[2] -= 450.0f;
		trap->Trace(&ledgeTr, downStart, player_mins, player_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

		if (ledgeTr.fraction == 1.0f || (downStart[2] - ledgeTr.endpos[2]) > 400.0f || CheckForTriggerHurt(ent, downStart, ledgeTr.endpos, player_mins, player_maxs)) {
			ledgeDanger = qtrue;
		}

		if (current_speed > max_run_speed + 10.0f) {
			ucmd.angles[YAW] = ANGLE2SHORT(vel_yaw) - ent->client->ps.delta_angles[YAW];
			ucmd.forwardmove = -127;
			bot2_states[clientNum].stateTimer = level.time;
		}
		else {
			ucmd.angles[YAW] = ANGLE2SHORT(base_target_yaw) - ent->client->ps.delta_angles[YAW];
			ucmd.forwardmove = ledgeDanger ? 0 : 127;

			if (level.time - bot2_states[clientNum].stateTimer > 100) {

				if (distToTarget <= 500.0f && !ledgeDanger) {
					if (level.time - bot2_states[clientNum].diagTimer > 500) {
						trap->Print(va("[Bot2] Time: %d | STATE 0: Target close (%.1f), walking\n", level.time, distToTarget));
						bot2_states[clientNum].diagTimer = level.time;
					}
				}
				else {
					// THE PRE-CHECK: Simulate a jump before twisting the bot's body
					int proposedDir = EvaluateStrafeDir(ent, base_target_yaw);
					char failReason[128] = "";
					char warningStr[128] = "";

					// Run simulation using a standard 425 takeoff speed
					if (IsSafeToJump(ent, clientNum, 425.0f, base_target_yaw, proposedDir, max_run_speed, failReason, warningStr, qtrue)) {
						bot2_states[clientNum].state = 1;
						bot2_states[clientNum].stateTimer = level.time;
						bot2_states[clientNum].targetYaw = base_target_yaw;
						bot2_states[clientNum].strafeDir = proposedDir;
					}
					else {
						// Pre-check failed. Walk forward and try again later.
						bot2_states[clientNum].stateTimer = level.time;
						if (level.time - bot2_states[clientNum].diagTimer > 500) {
							trap->Print(va("[Bot2] Time: %d | STATE 0: Jump pre-check failed (%s), keeping Walk state\n", level.time, failReason));
							bot2_states[clientNum].diagTimer = level.time;
						}
					}
				}
			}
		}

		ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
		ucmd.rightmove = 0;
		ucmd.upmove = 0;
	}
	// --- STATE 1: WIND-UP ---
	else if (bot2_states[clientNum].state == 1) {
		int sDir = bot2_states[clientNum].strafeDir;
		ucmd.forwardmove = 127;
		ucmd.rightmove = 127 * sDir;
		ucmd.upmove = 0;

		float windup_yaw = AngleMod(bot2_states[clientNum].targetYaw + (60.0f * sDir));
		ucmd.angles[YAW] = ANGLE2SHORT(windup_yaw) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

		if (level.time - bot2_states[clientNum].stateTimer > 150) {
			if (current_speed > (max_run_speed * 0.2f)) {
				char failReason[128] = "";
				char warningStr[128] = "";

				// Actual commitment check using real speed and real yaw
				if (IsSafeToJump(ent, clientNum, current_speed, vel_yaw, sDir, max_run_speed, failReason, warningStr, qfalse)) {
					trap->Print(va("[Bot2] Time: %d | STATE 1 -> 2 (JUMP)%s%s\n", level.time, warningStr[0] ? " | Warn: " : "", warningStr));
					bot2_states[clientNum].tele_actDir = sDir;
					ucmd.upmove = 127;
					bot2_states[clientNum].state = 2;
				}
				else {
					trap->Print(va("[Bot2] Time: %d | STATE 1 -> 0 (ABORT) | Reason: %s\n", level.time, failReason));
					bot2_states[clientNum].state = 0;
				}
				bot2_states[clientNum].stateTimer = level.time;
			}
		}
	}
	// --- STATE 2: AIRBORNE ---
	else if (bot2_states[clientNum].state == 2) {
		int sDir = bot2_states[clientNum].strafeDir;
		float magic_angle = 0.0f;

		if (current_speed > max_run_speed - 15.0f) {
			float acos_val = (max_run_speed - 15.0f) / current_speed;
			magic_angle = acos((acos_val < -1.0f) ? -1.0f : ((acos_val > 1.0f) ? 1.0f : acos_val)) * (180.0f / M_PI);
		}

		ucmd.forwardmove = 127;
		ucmd.rightmove = 127 * sDir;
		ucmd.angles[YAW] = ANGLE2SHORT(AngleMod(vel_yaw - ((magic_angle - 45.0f) * sDir))) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];
		ucmd.upmove = (level.time - bot2_states[clientNum].stateTimer < 100) ? 127 : 0;

		// Mid-Air Telemetry Drop
		if (bot2_states[clientNum].tele_inAir && !bot2_states[clientNum].tele_midAirLogged) {
			if (level.time >= bot2_states[clientNum].tele_midAirTime) {
				bot2_states[clientNum].tele_midAirLogged = qtrue;
				float errX = ent->client->ps.origin[0] - bot2_states[clientNum].tele_midPredPos[0];
				float errY = ent->client->ps.origin[1] - bot2_states[clientNum].tele_midPredPos[1];
				trap->Print(va("[Bot2 Mid-Air] Time: %d | Seq: %d | Err 2D: %.1f | Err Z: %+.1f\n  -> Pred 50%%: {%.0f, %.0f, %.0f}\n  -> Act 50%%:  {%.0f, %.0f, %.0f} | Act Yaw: %.0f\n",
					level.time, bot2_states[clientNum].tele_jumpSeq, (double)sqrt((errX * errX) + (errY * errY)), (double)(ent->client->ps.origin[2] - bot2_states[clientNum].tele_midPredPos[2]),
					(double)bot2_states[clientNum].tele_midPredPos[0], (double)bot2_states[clientNum].tele_midPredPos[1], (double)bot2_states[clientNum].tele_midPredPos[2],
					(double)ent->client->ps.origin[0], (double)ent->client->ps.origin[1], (double)ent->client->ps.origin[2], (double)ent->client->ps.viewangles[YAW]));
			}
		}

		// Touchdown Logic
		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 250)) {
			if (bot2_states[clientNum].tele_inAir && ent->client) {
				float dx = ent->client->ps.origin[0] - bot2_states[clientNum].tele_startPos[0];
				float dy = ent->client->ps.origin[1] - bot2_states[clientNum].tele_startPos[1];
				float actualDist = sqrt((dx * dx) + (dy * dy));

				trap->Print(va("[Bot2] Time: %d | Seq: %d | Err: %+.1f | Slope: %.1f deg\n  -> Dir - Pred: %d | Act: %d\n  -> TakeoffSpd: %.0f | LandSpd: %.0f | Yaw: %.0f | AirTime: %dms\n  -> Start: {%.0f, %.0f, %.0f}\n  -> Pred:  {%.0f, %.0f, %.0f} (Dist: %.1f)\n  -> Act:   {%.0f, %.0f, %.0f} (Dist: %.1f)\n",
					level.time, bot2_states[clientNum].tele_jumpSeq, (double)(actualDist - bot2_states[clientNum].tele_predDist), (double)bot2_states[clientNum].tele_groundSlope,
					bot2_states[clientNum].tele_predDir, bot2_states[clientNum].tele_actDir, (double)bot2_states[clientNum].tele_takeoffSpd, (double)current_speed, (double)vel_yaw, level.time - bot2_states[clientNum].tele_jumpStartTime,
					(double)bot2_states[clientNum].tele_startPos[0], (double)bot2_states[clientNum].tele_startPos[1], (double)bot2_states[clientNum].tele_startPos[2],
					(double)bot2_states[clientNum].tele_predPos[0], (double)bot2_states[clientNum].tele_predPos[1], (double)bot2_states[clientNum].tele_predPos[2], (double)bot2_states[clientNum].tele_predDist,
					(double)ent->client->ps.origin[0], (double)ent->client->ps.origin[1], (double)ent->client->ps.origin[2], (double)actualDist));

				bot2_states[clientNum].tele_inAir = 0;
			}

			// --- THE RAMP WHISKER BIAS ---
			trace_t landTr;
			vec3_t landEnd, landStart;
			VectorCopy(ent->client->ps.origin, landEnd);
			landEnd[2] -= 64.0f;
			trap->Trace(&landTr, ent->client->ps.origin, vec3_origin, vec3_origin, landEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

			float landSlope = (landTr.fraction < 1.0f) ? acos(landTr.plane.normal[2]) * (180.0f / M_PI) : 0.0f;

			// Universal Stair Scanner for Touchdown
			vec3_t flatAngles = { 0, vel_yaw, 0 };
			vec3_t forward;
			AngleVectors(flatAngles, forward, NULL, NULL);
			VectorMA(ent->client->ps.origin, 32.0f, forward, landStart);
			VectorCopy(landStart, landEnd);
			landEnd[2] -= 64.0f;
			trap->Trace(&landTr, landStart, (vec3_t) { -15, -15, -24 }, (vec3_t) { 15, 15, 32 }, landEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

			if (landTr.fraction < 1.0f && landTr.plane.normal[2] > 0.7f) {
				float stepDiff = landTr.endpos[2] - ent->client->ps.origin[2];
				if (stepDiff > 4.0f || (stepDiff < -4.0f && stepDiff > -32.0f)) landSlope = 45.0f;
			}

			int nextDirToUse = (landSlope > 5.0f) ? EvaluateStrafeDir(ent, vel_yaw) : sDir * -1;

			qboolean safeToChainJump = qfalse;
			char finalWarning[128] = "";

			float angle_delta = vel_yaw - base_target_yaw;
			while (angle_delta > 180.0f) angle_delta -= 360.0f;
			while (angle_delta < -180.0f) angle_delta += 360.0f;
			if (angle_delta < 0.0f) angle_delta = -angle_delta;

			if (distToTarget <= 500.0f) {
				sprintf(bot2_states[clientNum].lastFailReason, "Target proximity brake (%.1f)", distToTarget);
			}
			else if (angle_delta > 135.0f) {
				strcpy(bot2_states[clientNum].lastFailReason, "Missed the turn (Angle Delta > 170)");
			}
			else if (current_speed > (max_run_speed * 0.2f)) {
				int testDirs[2] = { nextDirToUse, nextDirToUse * -1 };
				for (int d = 0; d < 2; d++) {
					char failReason[128] = "", warningStr[128] = "";
					if (IsSafeToJump(ent, clientNum, current_speed, vel_yaw, testDirs[d], max_run_speed, failReason, warningStr, qfalse)) {
						safeToChainJump = qtrue;
						nextDirToUse = testDirs[d];
						strcpy(finalWarning, warningStr);
						break;
					}
					else {
						strcpy(bot2_states[clientNum].lastFailReason, failReason);
					}
				}
			}
			else safeToChainJump = qtrue;

			if (safeToChainJump) {
				trap->Print(va("[Bot2] Time: %d | STATE 2 -> 2 (CHAIN)%s%s\n", level.time, finalWarning[0] ? " | Warn: " : "", finalWarning));
				bot2_states[clientNum].strafeDir = nextDirToUse;
				bot2_states[clientNum].tele_actDir = nextDirToUse;
				ucmd.upmove = 127;
			}
			else {
				trap->Print(va("[Bot2] Time: %d | STATE 2 -> 0 (LANDED) | Reason: %s\n", level.time, bot2_states[clientNum].lastFailReason));
				bot2_states[clientNum].state = 0;
			}
			bot2_states[clientNum].stateTimer = level.time;
		}
	}

	botstates[clientNum]->lastucmd = ucmd;
	trap->BotUserCommand(ent->s.number, &ucmd);
}

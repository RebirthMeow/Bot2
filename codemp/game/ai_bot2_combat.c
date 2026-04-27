// ==============================================================================
// ai_bot2_combat.c - Combat aim assist for Advanced CTF Bot
//
//   * Bot2_GetLeadOrigin   - projectile / hitscan lead computation with two-pass
//                            iterative prediction, future-floor clamping, and
//                            ballistic compensation for arc weapons (alt-fire
//                            repeater, alt-fire flechette, thermal det).
//   * Bot2_ApplySmoothing  - clamped angle blending used by every state in the
//                            movement state machine and by combat firing.  The
//                            single source of truth for the bot's "humanized"
//                            view rate (1200 deg/s).
// ==============================================================================

#include "g_local.h"
#include "bg_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "ai_bot2_internal.h"

qboolean Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos, qboolean doTelemetry, float* out_timeOfFlight) {
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
	if (out_timeOfFlight) *out_timeOfFlight = timeOfFlight;
	return synced;
}

void Bot2_ApplySmoothing(int clientNum, vec3_t desiredAngles, usercmd_t* ucmd, qboolean lockAim) {
	gentity_t* ent = &g_entities[clientNum];
	bot2_state_t* bs = &bot2_states[clientNum];

	if (bs->lastAimTime == 0) {
		VectorCopy(ent->client->ps.viewangles, bs->lastViewAngles);
	}

	vec3_t finalAngles;

	if (lockAim) {
		// Turn speed: degrees per second.
		float turnSpeed = 1200.0f; // Fast, humanized flick
		float frameTime = (level.time - bs->lastAimTime) / 1000.0f;
		if (frameTime > 0.1f) frameTime = 0.1f;
		if (frameTime <= 0) frameTime = 0.001f;

		float maxDelta = turnSpeed * frameTime;

		// Pitch
		float pDelta = AngleDelta(desiredAngles[PITCH], bs->lastViewAngles[PITCH]);
		if (pDelta > maxDelta) pDelta = maxDelta;
		else if (pDelta < -maxDelta) pDelta = -maxDelta;
		finalAngles[PITCH] = AngleNormalize360(bs->lastViewAngles[PITCH] + pDelta);

		// Yaw
		float yDelta = AngleDelta(desiredAngles[YAW], bs->lastViewAngles[YAW]);
		if (yDelta > maxDelta) yDelta = maxDelta;
		else if (yDelta < -maxDelta) yDelta = -maxDelta;
		finalAngles[YAW] = AngleNormalize360(bs->lastViewAngles[YAW] + yDelta);
	} else {
		// BASE CASE: Instant Snap for movement/strafing
		finalAngles[PITCH] = AngleNormalize360(desiredAngles[PITCH]);
		finalAngles[YAW] = AngleNormalize360(desiredAngles[YAW]);
	}

	finalAngles[ROLL] = 0;

	ucmd->angles[PITCH] = ANGLE2SHORT(finalAngles[PITCH]) - ent->client->ps.delta_angles[PITCH];
	ucmd->angles[YAW] = ANGLE2SHORT(finalAngles[YAW]) - ent->client->ps.delta_angles[YAW];

	VectorCopy(finalAngles, bs->lastViewAngles);
	bs->lastAimTime = level.time;
}

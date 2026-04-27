// ==============================================================================
// ai_bot2_movement.c - Movement state machine for Advanced CTF Bot
//
// Hosts the on-the-ground driver that turns macro-level intent (targetOrigin,
// aimDir, lockAim, wantsSpeed) into per-tick usercmd_t input.  Internally
// runs an 8-state machine:
//
//   0  WALK       - normal navmesh locomotion + running-jump diagnostics
//   1  ESCAPE     - dedicated stuck-recovery jump executor
//   2  AIRBORNE   - generic free-strafe while in the air
//   3  JUMPPAD    - trigger_push handling with cached upper-ledge target
//   4  ELEVATOR   - func_plat / func_door vertical mover handling
//   5  DROP       - off-mesh drop-down across a ledge
//   6  JUMP       - off-mesh basic-jump fallback
//   7  WALLRUN    - off-mesh wallrun execution (6-phase sequence)
//
// Geometry probing helpers (CheckForTriggerHurt, TraceFloorScore, etc.) and
// elevator detection helpers (FindNearbyElevator etc.) live here too because
// the driver is their primary consumer.  ai_bot2_pmove.c re-uses the geometry
// helpers via ai_bot2_internal.h.
//
// Pmove simulation, combat aim assist, and wallrun simulators were extracted
// to ai_bot2_pmove.c, ai_bot2_combat.c, and ai_bot2_wallrun.c respectively.
// ==============================================================================

#include "g_local.h"
#include "bg_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "ai_bot2_internal.h"
#include "g_navmesh.h"

#include <stdio.h>

// ==============================================================================
// Floor & Geometry Helpers
// (also used by ai_bot2_pmove.c — declared in ai_bot2_internal.h)
// ==============================================================================

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
// Elevator Detection Helpers
// (only used by Bot2_ExecuteMovement, kept static)
// ==============================================================================

// Shared XY footprint test: both the bot and the far waypoint must fall inside
// the entity's current XY bounds (stable for any mover that only travels in Z).
static qboolean ElevatorFootprintMatch(gentity_t* mover, float botX, float botY,
                                        float wpX, float wpY, float margin) {
	if (botX < mover->r.absmin[0] - margin || botX > mover->r.absmax[0] + margin) return qfalse;
	if (botY < mover->r.absmin[1] - margin || botY > mover->r.absmax[1] + margin) return qfalse;
	if (wpX  < mover->r.absmin[0] - margin || wpX  > mover->r.absmax[0] + margin) return qfalse;
	if (wpY  < mover->r.absmin[1] - margin || wpY  > mover->r.absmax[1] + margin) return qfalse;
	return qtrue;
}

static qboolean WpInElevatorFootprint(gentity_t* mover, float wpX, float wpY, float margin) {
	if (wpX < mover->r.absmin[0] - margin || wpX > mover->r.absmax[0] + margin) return qfalse;
	if (wpY < mover->r.absmin[1] - margin || wpY > mover->r.absmax[1] + margin) return qfalse;
	return qtrue;
}

static qboolean IsApproachingElevator(gentity_t* ent, vec3_t wp) {
	float wpX = wp[0], wpY = wp[1], wpZ = wp[2];
	float botZ = ent->client->ps.origin[2];

	if (fabs(wpZ - botZ) < 80.0f) return qfalse;

	gentity_t* plat = NULL;
	while ((plat = G_Find(plat, FOFS(classname), "func_plat")) != NULL) {
		if (!plat->r.linked) continue;
		if (WpInElevatorFootprint(plat, wpX, wpY, 20.0f)) return qtrue;
	}

	gentity_t* door = NULL;
	while ((door = G_Find(door, FOFS(classname), "func_door")) != NULL) {
		if (!door->r.linked) continue;
		float dz  = fabs(door->pos2[2] - door->pos1[2]);
		float dxy = sqrt((door->pos2[0] - door->pos1[0]) * (door->pos2[0] - door->pos1[0]) +
		                 (door->pos2[1] - door->pos1[1]) * (door->pos2[1] - door->pos1[1]));
		if (dz < 80.0f || dxy > 32.0f) continue;
		if (WpInElevatorFootprint(door, wpX, wpY, 64.0f)) return qtrue;
	}
	return qfalse;
}

// Find a vertical elevator near the bot — either a func_plat or a func_door whose
// travel vector is primarily in Z (common map trick for vertical lifts).
// Both the bot and the next NavMesh waypoint must be inside the entity's XY footprint.
// Returns the elevator entity on match, NULL otherwise.
// outTargetZ is set to the waypoint Z the bot is trying to reach.
static gentity_t* FindNearbyElevator(gentity_t* ent, vec3_t nextWp, float* outTargetZ) {
	float botZ = ent->client->ps.origin[2];
	float wpZ  = nextWp[2];

	// Ignore small Z differences — stairs and ramps are handled by normal pathing.
	if (fabs(wpZ - botZ) < 80.0f) return NULL;

	float botX = ent->client->ps.origin[0], botY = ent->client->ps.origin[1];
	float wpX  = nextWp[0],                 wpY  = nextWp[1];
	const float margin = 20.0f;

	// --- func_plat: always travels in Z, no direction check needed ---
	gentity_t* plat = NULL;
	while ((plat = G_Find(plat, FOFS(classname), "func_plat")) != NULL) {
		if (!plat->r.linked) continue;
		if (!ElevatorFootprintMatch(plat, botX, botY, wpX, wpY, margin)) continue;

		float topZ = (plat->pos1[2] > plat->pos2[2]) ? plat->pos1[2] : plat->pos2[2];
		float bottomZ = (plat->pos1[2] < plat->pos2[2]) ? plat->pos1[2] : plat->pos2[2];

		if (wpZ > botZ && topZ <= botZ + 20.0f) continue; // Exhausted: can't take us higher
		if (wpZ < botZ && bottomZ >= botZ - 20.0f) continue; // Exhausted: can't take us lower

		if (outTargetZ) *outTargetZ = wpZ;
		return plat;
	}

	// --- func_door used as a vertical lift ---
	// Many maps build elevators from func_door rather than func_plat.
	// We accept one only when its travel vector is predominantly vertical:
	// significant Z movement and negligible XY drift between pos1 and pos2.
	//
	// func_door gets a larger footprint margin (64 vs 20) so State 4 engages
	// before the bot reaches the door frame geometry.  Without this, the bot
	// is still in State 0 when it bumps the threshold and the jump logic fires,
	// causing it to hop around instead of walking onto the platform.
	gentity_t* door = NULL;
	while ((door = G_Find(door, FOFS(classname), "func_door")) != NULL) {
		if (!door->r.linked) continue;

		float dz  = fabs(door->pos2[2] - door->pos1[2]);
		float dxy = sqrt((door->pos2[0] - door->pos1[0]) * (door->pos2[0] - door->pos1[0]) +
		                 (door->pos2[1] - door->pos1[1]) * (door->pos2[1] - door->pos1[1]));

		if (dz  < 80.0f) continue;  // not enough vertical travel to be a lift
		if (dxy > 32.0f) continue;  // moves too much laterally — it's a normal door

		if (!ElevatorFootprintMatch(door, botX, botY, wpX, wpY, 64.0f)) continue;

		float topZ = (door->pos1[2] > door->pos2[2]) ? door->pos1[2] : door->pos2[2];
		float bottomZ = (door->pos1[2] < door->pos2[2]) ? door->pos1[2] : door->pos2[2];

		if (wpZ > botZ && topZ <= botZ + 20.0f) continue; // Exhausted: can't take us higher
		if (wpZ < botZ && bottomZ >= botZ - 20.0f) continue; // Exhausted: can't take us lower

		if (outTargetZ) *outTargetZ = wpZ;
		return door;
	}

	return NULL;
}

// ==============================================================================
// Escape Jump Scanner
// Sweeps 360° in 22° steps using IsSafeToJump pmove simulation.  For each
// valid arc, checks that the predicted landing has a working navmesh path to
// targetOrigin.  Returns the best (closest-to-goal) escape yaw/landing pos.
// Called by the general stuck-recovery handler and the wallrun abort handler.
// ==============================================================================
static qboolean Bot2_FindEscapeJump(gentity_t *ent, int clientNum,
                                     vec3_t targetOrigin, float max_run_speed,
                                     vec3_t aimDir, float *out_escapeYaw,
                                     vec3_t out_landPos) {
	float bestPathDist = 999999.0f;
	*out_escapeYaw = -1.0f;
	out_landPos[0] = out_landPos[1] = out_landPos[2] = 0.0f;

	// When stuck the bot is typically at near-zero speed, so a simulation using
	// the real ps produces a tiny arc that lands right next to the stuck position.
	// Instead, build a "boosted" ps with full forward velocity in each scan
	// direction so we find destinations a genuine running-jump would actually reach.
	// The BOT_STATE_ESCAPE handler then runs the bot forward first to build real
	// speed before firing the jump, so the actual arc matches the simulation.
	playerState_t boostBase;
	memcpy(&boostBase, &ent->client->ps, sizeof(playerState_t));

	int deg;
	for (deg = 0; deg < 360; deg += 45) {
		vec3_t landPos = { 0.0f, 0.0f, 0.0f };
		playerState_t pmove_ps;

		// Override velocity to full run speed in this scan direction so the
		// simulated arc represents what a non-stuck bot could achieve.
		playerState_t boostPS;
		memcpy(&boostPS, &boostBase, sizeof(playerState_t));
		float radYaw = (float)deg * (M_PI / 180.0f);
		boostPS.velocity[0] = cosf(radYaw) * max_run_speed;
		boostPS.velocity[1] = sinf(radYaw) * max_run_speed;
		boostPS.velocity[2] = 0.0f;

		if (!SimulatePmoveTrajectory(ent, &boostPS, (float)deg, 0, 0.0f, max_run_speed,
		                             landPos, &pmove_ps, qfalse, aimDir))
			continue;

		// Must land further than one body-length from current pos — a landing right
		// next to the stuck spot won't help.
		float escapeDist = Distance(ent->client->ps.origin, landPos);
		if (escapeDist < 96.0f)
			continue;

		// Landing must be on the navmesh
		if (!NavMesh_IsPointOnMesh(landPos))
			continue;

		// Verify navmesh connectivity from landing to goal
		NavMeshWaypoint_t escInfo;
		memset(&escInfo, 0, sizeof(escInfo));
		if (!NavMesh_GetNextWaypointEx(ent->s.number, landPos, targetOrigin,
		                               &escInfo, 0))
			continue;

		// Use true topological path distance, NOT Euclidean straight-line distance
		float pathDist = NavMesh_GetPathDistance(ent->s.number, landPos, targetOrigin);
		if (pathDist < bestPathDist) {
			bestPathDist = pathDist;
			VectorCopy(landPos, out_landPos);
			*out_escapeYaw = (float)deg;
		}
	}

	return (*out_escapeYaw >= 0.0f) ? qtrue : qfalse;
}

// ==============================================================================
// Driver: Executes Macro Intent via Physical Engine
// ==============================================================================
void Bot2_ExecuteMovement(int clientNum, usercmd_t* ucmd, vec3_t targetOrigin, vec3_t aimDir, qboolean lockAim, qboolean wantsSpeed, qboolean hasFallback, vec3_t fallbackOrigin) {
	gentity_t* ent = &g_entities[clientNum];
	if (!ent || !ent->inuse || !ent->client || level.intermissiontime || level.time == 0) return;

	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
	float vel_x = ent->client->ps.velocity[0], vel_y = ent->client->ps.velocity[1], vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));

	vec3_t dir, angles, nextWp = { 0,0,0 }; qboolean validPath = qfalse; float base_target_yaw;
	float distToTarget = VectorLength(targetOrigin) > 1.0f ? Distance(targetOrigin, ent->client->ps.origin) : 999999.0f;

	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
		vectoangles(aimDir, angles);
		Bot2_ApplySmoothing(clientNum, angles, ucmd, qtrue);
		return;
	}

	// Extended waypoint query — also surfaces any off-mesh connection in the path.
	NavMeshWaypoint_t wpInfo;
	wpInfo.position[0] = wpInfo.position[1] = wpInfo.position[2] = 0.0f;
	wpInfo.offmeshType = 0;
	wpInfo.offmeshStart[0] = wpInfo.offmeshStart[1] = wpInfo.offmeshStart[2] = 0.0f;
	wpInfo.offmeshEnd[0]   = wpInfo.offmeshEnd[1]   = wpInfo.offmeshEnd[2]   = 0.0f;

	// Don't issue a new navmesh query while executing off-mesh traversal — the bot
	// is off the mesh mid-flight and the query would fail or return garbage.
	qboolean inOffmeshState = (bot2_states[clientNum].state == BOT_STATE_DROP  ||
	                            bot2_states[clientNum].state == BOT_STATE_JUMP  ||
	                            bot2_states[clientNum].state == BOT_STATE_WALLRUN);

	// Wallruns and elevators are only used when no other route exists.
	// The high penalty cost makes Detour prefer any alternative; if none exists it
	// still routes through them, so the flag goal is always reachable.
#define OFFMESH_AVOID_MASK ((1 << OFFMESH_AREA_WALLRUN) | (1 << OFFMESH_AREA_ELEVATOR))

	if (!inOffmeshState && VectorLength(targetOrigin) > 1.0f) {
		if (NavMesh_GetNextWaypointEx(ent->s.number, ent->client->ps.origin, targetOrigin, &wpInfo, OFFMESH_AVOID_MASK)
		    && (VectorSubtract(wpInfo.position, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) {
			VectorCopy(wpInfo.position, nextWp);
			validPath = qtrue;
		}
		else if (hasFallback) {
			VectorCopy(fallbackOrigin, targetOrigin);
			if (NavMesh_GetNextWaypointEx(ent->s.number, ent->client->ps.origin, targetOrigin, &wpInfo, OFFMESH_AVOID_MASK)
			    && (VectorSubtract(wpInfo.position, ent->client->ps.origin, dir), VectorLength(dir) > 0.1f)) {
				VectorCopy(wpInfo.position, nextWp);
				validPath = qtrue;
			}
		}
	}

	// Convenient aliases used throughout the state machine below.
	int    offmeshType  = wpInfo.offmeshType;
	float* offmeshStart = wpInfo.offmeshStart;
	float* offmeshEnd   = wpInfo.offmeshEnd;

	base_target_yaw = validPath ? (vectoangles(dir, angles), angles[YAW]) : bot2_states[clientNum].targetYaw;
	float distToWp = validPath ? Distance(ent->client->ps.origin, nextWp) : distToTarget;

	// =========================================================================
	// UNIVERSAL STUCK DETECTION
	// Two complementary signals are combined so neither alone can be bypassed:
	//
	//   snapshot_moved  — Distance(origin, stuck_pos): a single snapshot can be
	//     fooled if the bot oscillates and happens to be at the far end of its
	//     jitter range when the sample fires (the classic Q3 oscillation bypass).
	//
	//   stuck_dist_acc  — XY ground speed accumulated every frame: a bot that
	//     is physically wedged will have near-zero accumulated speed regardless
	//     of where the snapshot lands.  Threshold is 40 % of theoretical max
	//     over 1 s; genuine forward movement comfortably exceeds this.
	//
	// Detecting a stuck condition requires EITHER signal to trip:
	//   • snapshot_moved < 48 u   (didn't end up far from where it started), OR
	//   • stuck_dist_acc < max_run_speed * 8  (barely moved in total path length)
	//
	// Excluded: jumppad, elevator, off-mesh traversal states, and wallrun
	// staging (offmeshType==WALLRUN while in WALK) all have their own handling.
	// =========================================================================
	{
		int botState = bot2_states[clientNum].state;
		// Exclude AIRBORNE: a bot mid-jump was not stuck when it left the ground,
		// and jump arcs that return near their origin cause false positives when
		// the 1-second snapshot window happens to straddle takeoff and landing.
		qboolean locomotionState  = (botState == BOT_STATE_WALK);
		qboolean wallrunStaging   = (offmeshType == OFFMESH_AREA_WALLRUN &&
		                             botState == BOT_STATE_WALK);

		// Per-frame accumulation: add XY ground speed (u/s) every think call.
		// Using XY only so legitimate jumps and falls don't mask a ground-stuck.
		{
			float vx = ent->client->ps.velocity[0];
			float vy = ent->client->ps.velocity[1];
			bot2_states[clientNum].stuck_dist_acc += sqrtf(vx*vx + vy*vy);
		}

		if (level.time - bot2_states[clientNum].stuck_timer >= 1000) {
			float snapshot_moved = Distance(ent->client->ps.origin,
			                               bot2_states[clientNum].stuck_pos);
			// stuck_dist_acc is raw u/s summed over ~20 ticks at 50 ms each.
			// At max speed 250 u/s that's ~5000 per second; 40 % threshold = 2000.
			float accThreshold   = max_run_speed * 8.0f; // ≈ 40 % of 20-tick budget
			qboolean lowSnapshot = (snapshot_moved < 48.0f);
			qboolean lowAcc      = (bot2_states[clientNum].stuck_dist_acc < accThreshold);

			if ((lowSnapshot || lowAcc) && locomotionState && !wallrunStaging
			    && distToTarget > 128.0f) {
				bot2_states[clientNum].unstuck_phase++;
				Bot2_PrintTelemetry(1,
					"[%s] STUCK phase=%d: snap=%.0f u acc=%.0f (thresh=%.0f) state=%d\n",
					ent->client->pers.netname,
					bot2_states[clientNum].unstuck_phase,
					snapshot_moved, bot2_states[clientNum].stuck_dist_acc,
					accThreshold, botState);
			} else {
				bot2_states[clientNum].unstuck_phase = 0;
			}

			VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
			bot2_states[clientNum].stuck_timer    = level.time;
			bot2_states[clientNum].stuck_dist_acc = 0.0f; // reset for next window
		}

		// Fire recovery after 2 consecutive stuck-seconds; 3 s inter-scan cooldown.
		if (bot2_states[clientNum].unstuck_phase >= 2 && locomotionState
		    && !wallrunStaging
		    && (level.time - bot2_states[clientNum].unstuck_phase_timer >= 3000)) {
			bot2_states[clientNum].unstuck_phase_timer = level.time;

			Bot2_PrintTelemetry(1,
				"[%s] STUCK RECOVERY: sweeping 360° escape scan (state=%d)\n",
				ent->client->pers.netname, botState);

			float  escYaw = -1.0f;
			vec3_t escLand = { 0.0f, 0.0f, 0.0f };
			if (Bot2_FindEscapeJump(ent, clientNum, targetOrigin, max_run_speed,
			                        aimDir, &escYaw, escLand)) {
				bot2_states[clientNum].state = BOT_STATE_ESCAPE;
				bot2_states[clientNum].stateTimer = level.time;
				bot2_states[clientNum].targetYaw = escYaw;
				bot2_states[clientNum].unstuck_phase = 0;
				Bot2_PrintTelemetry(1,
					"[%s] STUCK RECOVERY: escape jump yaw=%.0f -> {%.0f %.0f %.0f}\n",
					ent->client->pers.netname, escYaw,
					escLand[0], escLand[1], escLand[2]);
				// Fall through to let the newly set BOT_STATE_ESCAPE execute this tick
			} else {
				// No valid jump found — keep phase at 2 so we retry next window
				bot2_states[clientNum].unstuck_phase = 2;
				Bot2_PrintTelemetry(1,
					"[%s] STUCK RECOVERY: no escape jump found, will retry\n",
					ent->client->pers.netname);
			}
		}
	}

	// --- JUMPPAD APPROACH CAPTURE ---
	// While the bot is still walking toward a jumppad, the path query returns an
	// off-mesh connection (OFFMESH_AREA_JUMPPAD) whose offmeshEnd is the upper-
	// ledge landing waypoint placed by the navmesh generator.  We cache that end
	// every tick during approach so that the *most recent* known-good landing
	// waypoint is preserved at the moment the trigger fires — by the next think,
	// the bot is airborne and Detour may issue a path that re-evaluates around
	// a different "better" waypoint (e.g. when the value function shifts cost
	// and a closer alternative wins).  Once we're locked into State 3 the cache
	// is no longer updated; tele_predPos is the source of truth for steering.
	#define JUMPPAD_CACHE_FRESH_MS 1000  // honor the cache only if updated <= 1s ago
	if (bot2_states[clientNum].state == BOT_STATE_WALK
	    && offmeshType == OFFMESH_AREA_JUMPPAD
	    && (offmeshEnd[0] != 0.0f || offmeshEnd[1] != 0.0f || offmeshEnd[2] != 0.0f)) {
		VectorCopy(offmeshEnd, bot2_states[clientNum].jumppadCachedEnd);
		bot2_states[clientNum].jumppadCachedTime = level.time;
		bot2_states[clientNum].jumppadEndCached  = qtrue;
	}

	if (IsInTriggerPush(ent) && bot2_states[clientNum].state != BOT_STATE_JUMPPAD) {
		bot2_states[clientNum].state = BOT_STATE_JUMPPAD;
		bot2_states[clientNum].stateTimer = level.time;

		// Freshness gate: a cache older than JUMPPAD_CACHE_FRESH_MS may belong to
		// a previous jumppad on a previous goal — don't trust it for this trigger.
		qboolean cacheFresh = bot2_states[clientNum].jumppadEndCached
		                    && (level.time - bot2_states[clientNum].jumppadCachedTime
		                        <= JUMPPAD_CACHE_FRESH_MS);

		// PRIMARY: cached offmeshEnd from the approach window.  This is the
		// upper-ledge landing waypoint the navmesh generator placed for this
		// specific jumppad — the only waypoint that's physically reachable from
		// the trajectory the trigger_push imparts.  We use it even when Detour's
		// value function would rate some other waypoint as a "better" continuation:
		// once airborne the bot cannot redirect, so any "better" waypoint not on
		// this jumppad's pair is a route we can no longer reach mid-arc.
		const char* lockSrc;
		if (cacheFresh) {
			VectorCopy(bot2_states[clientNum].jumppadCachedEnd, bot2_states[clientNum].tele_predPos);
			lockSrc = "cached offmeshEnd";
		}
		// FALLBACK 1: same-tick offmeshEnd.  Covers the edge case where the
		// trigger fires on the very first tick the path query saw the offmesh
		// connection (no prior approach tick to populate the cache).
		else if (offmeshType == OFFMESH_AREA_JUMPPAD
		         && (offmeshEnd[0] != 0.0f || offmeshEnd[1] != 0.0f || offmeshEnd[2] != 0.0f)) {
			VectorCopy(offmeshEnd, bot2_states[clientNum].tele_predPos);
			lockSrc = "current-tick offmeshEnd";
		}
		// FALLBACK 2: no offmesh data — face the macro goal (e.g. the flag) and
		// hold W.  This is intentionally the ONLY fallback: trigger_push
		// target_position is often straight overhead on vertical pads (useless
		// for steering), and Detour's post-jumppad waypoint is the very thing
		// whose value-function drift caused the original bug.  Aiming at the
		// end goal at least gives the bot a coherent direction to lean toward
		// for whatever in-air control is available, and it's correct on average
		// even when the offmesh connection is missing.
		else if (VectorLength(targetOrigin) > 1.0f) {
			VectorCopy(targetOrigin, bot2_states[clientNum].tele_predPos);
			lockSrc = "macro goal (no offmesh)";
		}
		// LAST RESORT: no goal either (shouldn't happen in normal play).  Keep
		// whatever tele_predPos was already set to so we don't snap to <0,0,0>.
		else {
			lockSrc = "preserved (no goal)";
		}

		Bot2_PrintTelemetry(1,"[%s] STATE Transition: Entered Jump Pad (State 3) - Locked Target {%.0f, %.0f, %.0f} (src=%s)\n",
			ent->client->pers.netname, bot2_states[clientNum].tele_predPos[0], bot2_states[clientNum].tele_predPos[1], bot2_states[clientNum].tele_predPos[2], lockSrc);
	}

	// --- ELEVATOR DETECTION (State 0 only) ---
	// When the NavMesh routes the bot into the XY footprint of a func_plat with a
	// significant Z delta to the next waypoint, hand off to the elevator state machine.
	// This must run after the jump-pad check (State 3 takes priority) but before the
	// airborne check so we don't accidentally launch a jump toward the upper waypoint.
	if (bot2_states[clientNum].state == BOT_STATE_WALK && validPath) {
		float elevTargetZ = 0.0f;
		gentity_t* elevEnt = FindNearbyElevator(ent, nextWp, &elevTargetZ);
		if (elevEnt) {
			bot2_states[clientNum].state          = 4;
			bot2_states[clientNum].stateTimer     = level.time;
			bot2_states[clientNum].elevatorEntNum = elevEnt->s.number;
			bot2_states[clientNum].elevatorPhase  = 0;
			// Reuse tele_predPos to store the destination: XY from the waypoint, Z from the plat's
			// pos2 (the exact top endpoint the engine will stop at, not just the waypoint approximation).
			bot2_states[clientNum].tele_predPos[0] = nextWp[0];
			bot2_states[clientNum].tele_predPos[1] = nextWp[1];
			bot2_states[clientNum].tele_predPos[2] = (elevTargetZ > ent->client->ps.origin[2])
			                                         ? elevEnt->pos2[2]   // going up  -> top stop
			                                         : elevEnt->pos1[2];  // going down -> bottom stop
			Bot2_PrintTelemetry(1, "[%s] STATE Transition: Entering Elevator (State 4) - Target Z %.0f\n",
				ent->client->pers.netname, bot2_states[clientNum].tele_predPos[2]);
		}
	}

	if (ent->client->ps.groundEntityNum == ENTITYNUM_NONE &&
	    bot2_states[clientNum].state != BOT_STATE_AIRBORNE &&
	    bot2_states[clientNum].state != BOT_STATE_ESCAPE   &&
	    bot2_states[clientNum].state != BOT_STATE_JUMPPAD  &&
	    bot2_states[clientNum].state != BOT_STATE_ELEVATOR &&
	    bot2_states[clientNum].state != BOT_STATE_DROP     &&
	    bot2_states[clientNum].state != BOT_STATE_JUMP     &&
	    bot2_states[clientNum].state != BOT_STATE_WALLRUN) {
		bot2_states[clientNum].state = BOT_STATE_AIRBORNE;
		bot2_states[clientNum].stateTimer = level.time;
		bot2_states[clientNum].targetYaw = base_target_yaw;
		bot2_states[clientNum].strafeDir = EvaluateStrafeDir(ent, base_target_yaw);
		Bot2_PrintTelemetry(1,"[%s] STATE Transition: Walking -> Falling/Airborne (State 2)\n", ent->client->pers.netname);
	}

	// --- STATE 3: JUMP PAD / PUSHED ---
	if (bot2_states[clientNum].state == BOT_STATE_JUMPPAD) {

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

		Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd, lockAim);

		ucmd->forwardmove = 127;
		ucmd->rightmove = ucmd->upmove = 0;

		if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 500)) {
			bot2_states[clientNum].state = BOT_STATE_WALK;
			// Clear the approach cache so the NEXT jumppad gets a fresh capture
			// rather than reusing this one's stale upper-ledge waypoint.
			bot2_states[clientNum].jumppadEndCached    = qfalse;
			bot2_states[clientNum].jumppadCachedTime   = 0;
			bot2_states[clientNum].jumppadCachedEnd[0] = 0.0f;
			bot2_states[clientNum].jumppadCachedEnd[1] = 0.0f;
			bot2_states[clientNum].jumppadCachedEnd[2] = 0.0f;
			Bot2_PrintTelemetry(1,"[%s] STATE Transition: Landed from Jump Pad -> Walk\n", ent->client->pers.netname);
		}
	}
	// --- STATE 4: ELEVATOR ---
	else if (bot2_states[clientNum].state == BOT_STATE_ELEVATOR) {
		int elevNum = bot2_states[clientNum].elevatorEntNum;

		// Sanity: entity must still be alive
		if (elevNum < 0 || elevNum >= ENTITYNUM_WORLD || !g_entities[elevNum].inuse) {
			bot2_states[clientNum].state = BOT_STATE_WALK;
			Bot2_PrintTelemetry(1, "[%s] STATE Transition: Elevator entity invalid -> Walk\n",
				ent->client->pers.netname);
		}
		// Hard safety timeout — give up after 10 seconds to prevent permanent stalls
		else if (level.time - bot2_states[clientNum].stateTimer > 10000) {
			bot2_states[clientNum].state = BOT_STATE_WALK;
			Bot2_PrintTelemetry(1, "[%s] STATE Transition: Elevator timeout -> Walk\n",
				ent->client->pers.netname);
		}
		else {
			gentity_t* elev    = &g_entities[elevNum];
			float      targetZ = bot2_states[clientNum].tele_predPos[2];
			float      botZ    = ent->client->ps.origin[2];

			// Determine travel direction once — used by both phases.
			// midZ splits the travel range so we know which endpoint is the destination.
			float    midZ    = elev->pos1[2] + (elev->pos2[2] - elev->pos1[2]) * 0.5f;
			qboolean goingUp = (targetZ > midZ);

			// Is the elevator currently moving toward our destination?
			// MOVER_1TO2 = moving toward pos2 (up), MOVER_2TO1 = moving toward pos1 (down).
			qboolean movingToTarget = goingUp
				? (elev->moverState == MOVER_1TO2)
				: (elev->moverState == MOVER_2TO1);

			// Has the elevator fully arrived at our destination endpoint?
			qboolean arrivedAtTarget = goingUp
				? (elev->moverState == MOVER_POS2)
				: (elev->moverState == MOVER_POS1);

			// ---- PHASE 0: WAITING FOR THE ELEVATOR TO START MOVING ----
			// The platform surface at MOVER_POS1 is flush with world geometry, so
			// groundEntityNum stays ENTITYNUM_WORLD and can't be used to detect boarding.
			// Instead, transition to Phase 1 the moment the elevator begins moving toward
			// the target — at that point it is physically carrying the bot regardless of
			// what groundEntityNum reports.
			if (bot2_states[clientNum].elevatorPhase == 0) {

				if (movingToTarget || arrivedAtTarget) {
					bot2_states[clientNum].elevatorPhase = 1;
					Bot2_PrintTelemetry(1, "[%s] STATE Elevator: Platform moving - RIDING to Z %.0f (moverState=%d)\n",
						ent->client->pers.netname, targetZ, elev->moverState);
				}

				// Only walk toward the centre when the elevator is at boarding level.
				// If it is at the far end or still travelling back toward us, hold
				// position — walking under a descending platform gets the bot crushed.
				qboolean safeToBoard = goingUp
					? (elev->moverState == MOVER_POS1)
					: (elev->moverState == MOVER_POS2);

				vec3_t elevCentre;
				elevCentre[0] = (elev->r.absmin[0] + elev->r.absmax[0]) * 0.5f;
				elevCentre[1] = (elev->r.absmin[1] + elev->r.absmax[1]) * 0.5f;
				elevCentre[2] = ent->client->ps.origin[2];

				vec3_t toCenter;
				VectorSubtract(elevCentre, ent->client->ps.origin, toCenter);
				float distXY = sqrt(toCenter[0]*toCenter[0] + toCenter[1]*toCenter[1]);

				vec3_t desiredAngles = { 0, 0, 0 };
				if (lockAim) {
					vectoangles(aimDir, desiredAngles);
				} else {
					vectoangles(toCenter, desiredAngles);
				}
				Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd, lockAim);

				ucmd->forwardmove = (safeToBoard && distXY > 20.0f) ? 64 : 0;
				ucmd->rightmove   = ucmd->upmove = 0;
			}
			// ---- PHASE 1: RIDING ----
			// Hold completely still and let the mover carry the bot.
			// Exit the moment moverState reaches the destination endpoint.
			else {
				if (arrivedAtTarget) {
					bot2_states[clientNum].state = BOT_STATE_WALK;
					Bot2_PrintTelemetry(1, "[%s] STATE Transition: Elevator arrived at Z %.0f -> Walk\n",
						ent->client->pers.netname, botZ);
				}

				// Face the macro target while riding so the bot is already oriented
				// when it steps off at the destination
				vec3_t rideAngles = { 0, base_target_yaw, 0 };
				if (lockAim) vectoangles(aimDir, rideAngles);
				Bot2_ApplySmoothing(clientNum, rideAngles, ucmd, lockAim);
				ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
			}
		}
	}
	// --- STATE 0: WALK/IDLE & RUNNING JUMPS ---
	else if (bot2_states[clientNum].state == BOT_STATE_WALK) {
		bot2_states[clientNum].tele_jumpSeq = 0; float bDist = 32.0f, dYaw = base_target_yaw; qboolean lDang = qfalse;

		if (current_speed > 100.0f && TraceFloorScore(ent, ent->client->ps.origin, vel_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = vel_yaw; }
		else if (TraceFloorScore(ent, ent->client->ps.origin, base_target_yaw, bDist, NULL) < 0.0f) { lDang = qtrue; dYaw = base_target_yaw; }

		// Suppress ledge avoidance when the navmesh is actively routing through a
		// non-wallrun off-mesh connection (drop, basic jump) — the bot must walk
		// straight to the connection start unimpeded.
		// Wallrun is excluded: its outer zone (64-250 u) relies on normal
		// navmesh locomotion which needs ledge avoidance to stay safe.
		if (offmeshType != OFFMESH_AREA_NONE && offmeshType != OFFMESH_AREA_WALLRUN) {
			lDang = qfalse;
			bot2_states[clientNum].ledgeEvading = 0;
		}

		if (lDang && !bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 1; Bot2_PrintTelemetry(1,"[%s] STATE Event: Ledge Danger Detected! Evading...\n", ent->client->pers.netname); }
		else if (!lDang && bot2_states[clientNum].ledgeEvading) { bot2_states[clientNum].ledgeEvading = 0; Bot2_PrintTelemetry(1,"[%s] STATE Event: Ledge Clear. Resuming normal pathing.\n", ent->client->pers.netname); }

		// --- OFF-MESH ACTIVATION (DROPS & WALLRUNS — high priority) ---
		// Drops activate immediately at the connection start (48 u).
		// Wallruns use a wide staging zone (250 u) that steals all locomotion
		// control, walks the bot flush against the wall, then activates State 7.
		// Basic jumps are handled AFTER the wantsSpeed block so the strafe-jump
		// system gets first refusal — see the fallback block below.
		float offmeshDistToStart2D = 999999.0f;

		// ---- DROP ----
		if (offmeshType == OFFMESH_AREA_JUMP_DROP) {
			float dxOM = ent->client->ps.origin[0] - offmeshStart[0];
			float dyOM = ent->client->ps.origin[1] - offmeshStart[1];
			offmeshDistToStart2D = sqrtf(dxOM*dxOM + dyOM*dyOM);

			// Cooldown: don't re-activate within 1.5 s of the last offmesh exit.
			// Z guard: only activate if the bot is at or above the drop start —
			// if it has already fallen below, the connection is behind it.
			qboolean dropCooldownOk = (level.time - bot2_states[clientNum].offmeshExitTime > 1500);
			qboolean dropAboveStart = (ent->client->ps.origin[2] >= offmeshStart[2] - 32.0f);

			if (offmeshDistToStart2D <= 48.0f && dropCooldownOk && dropAboveStart) {
				bot2_states[clientNum].offmeshType = offmeshType;
				VectorCopy(offmeshEnd, bot2_states[clientNum].tele_predPos);
				VectorCopy(offmeshStart, bot2_states[clientNum].tele_startPos);
				bot2_states[clientNum].stateTimer = level.time;
				bot2_states[clientNum].tele_inAir = 0; // 0 = walking to ledge, 1 = falling

				// Dynamically find the ledge by tracing around the bot
				float bestYaw = -1.0f;
				float bestDist = 999999.0f;

				for (int deg = 0; deg < 360; deg += 15) {
					float angle = deg * (M_PI / 180.0f);
					vec3_t fwd = { cos(angle), sin(angle), 0.0f };

					// Trace forward 64 units to see if there's a clear path to the edge
					trace_t trFwd;
					vec3_t endFwd;
					VectorMA(ent->client->ps.origin, 64.0f, fwd, endFwd);
					trap->Trace(&trFwd, ent->client->ps.origin, ent->r.mins, ent->r.maxs, endFwd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

					// Only consider directions where we can walk at least 16 units before hitting a wall
					if (trFwd.fraction * 64.0f > 16.0f) {
						// Trace down from the forward point to see where we would land
						vec3_t startDown;
						VectorMA(ent->client->ps.origin, (trFwd.fraction * 64.0f) + 1.0f, fwd, startDown);
						startDown[2] += 16.0f; // step up slightly to clear bumps

						vec3_t endDown;
						VectorCopy(startDown, endDown);
						endDown[2] -= 8192.0f;

						trace_t trDown;
						trap->Trace(&trDown, startDown, ent->r.mins, ent->r.maxs, endDown, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

						// A ledge is found if the drop is significant (e.g. > 32 units)
						float dropDist = startDown[2] - trDown.endpos[2];
						if (dropDist > 32.0f) {
							// Score this drop based on how close the landing spot is to offmeshEnd
							float distToEnd = Distance(trDown.endpos, offmeshEnd);
							if (distToEnd < bestDist) {
								bestDist = distToEnd;
								bestYaw = deg;
							}
						}
					}
				}

				if (bestYaw >= 0.0f) {
					bot2_states[clientNum].tele_takeoffYaw = bestYaw;
				} else {
					// Fallback: just look at the destination
					vec3_t toEnd;
					VectorSubtract(offmeshEnd, ent->client->ps.origin, toEnd);
					toEnd[2] = 0.0f;
					if (VectorLength(toEnd) > 0.1f) {
						bot2_states[clientNum].tele_takeoffYaw = atan2(toEnd[1], toEnd[0]) * (180.0f / M_PI);
					} else {
						bot2_states[clientNum].tele_takeoffYaw = ent->client->ps.viewangles[YAW];
					}
				}

				vec3_t dropAngles = { 0.0f, bot2_states[clientNum].tele_takeoffYaw, 0.0f };
				if (lockAim) vectoangles(aimDir, dropAngles);
				Bot2_ApplySmoothing(clientNum, dropAngles, ucmd, lockAim);

				ucmd->forwardmove = 127;
				ucmd->rightmove   = 0;
				bot2_states[clientNum].state = BOT_STATE_DROP;
				ucmd->upmove = 0;
				Bot2_PrintTelemetry(1, "[%s] STATE Transition: Off-Mesh Drop -> {%.0f %.0f %.0f} | LedgeYaw: %.1f\n",
					ent->client->pers.netname, offmeshEnd[0], offmeshEnd[1], offmeshEnd[2], bot2_states[clientNum].tele_takeoffYaw);
				return;
			}
		}

		// ---- WALLRUN STAGING ----
		// Two-zone approach:
		//   Outer zone (250 u): proactive saber equip only — navmesh locomotion
		//     continues normally so the bot follows the safe path without falling
		//     into pits or off ledges on the way to the connection start.
		//   Inner zone (64 u): take locomotion control — face the wall, walk
		//     straight in, run flush/height/pmove pre-validation, then activate.
		else if (offmeshType == OFFMESH_AREA_WALLRUN) {
			float dxOM = ent->client->ps.origin[0] - offmeshStart[0];
			float dyOM = ent->client->ps.origin[1] - offmeshStart[1];
			offmeshDistToStart2D = sqrtf(dxOM*dxOM + dyOM*dyOM);

			// Outer zone: equip saber now so the animation finishes long before
			// the bot reaches the inner zone.  No locomotion override here.
			if (offmeshDistToStart2D <= 250.0f && ent->client->ps.weapon != WP_SABER) {
				if (bot2_states[clientNum].savedWeapon == 0)
					bot2_states[clientNum].savedWeapon = ent->client->ps.weapon;
				ucmd->weapon = WP_SABER;
			}

			// Inner zone: steal locomotion control only in the last 64 u.
			if (offmeshDistToStart2D <= 64.0f
			    && (level.time - bot2_states[clientNum].wallrunCooldown >= 3000)) {
				// Cooldown of 3 s applies after each failed/escaped attempt.
				// When cooling down this block is skipped entirely and normal
				// locomotion runs, letting the bot reposition or land from
				// the escape jump executed by the recovery scanner.

				// Find the correct wall face with a 360° sweep centred on offmeshStart.
				// offmeshStart is placed at the base of the intended wall, so the nearest
				// near-vertical surface from that point is always the right face — corner
				// geometry on adjacent sides will be further away and won't win.
				vec3_t wallDir = { 0.0f, 0.0f, 0.0f };
				vec3_t bestWallPoint = { 0.0f, 0.0f, 0.0f };
				float bestWallDist = 999999.0f;

				// Raise the sweep origin to eye height to avoid startsolid on the floor.
				vec3_t traceOrigin;
				traceOrigin[0] = offmeshStart[0];
				traceOrigin[1] = offmeshStart[1];
				traceOrigin[2] = offmeshStart[2] + 24.0f;

				for (int i = 0; i < 360; i += 15) {
					float angle = (float)i * (M_PI / 180.0f);
					vec3_t dir = { cosf(angle), sinf(angle), 0.0f };
					vec3_t end;
					VectorMA(traceOrigin, 128.0f, dir, end);
					trace_t tr;
					trap->Trace(&tr, traceOrigin, NULL, NULL, end, ent->s.number, MASK_SOLID, qfalse, 0, 0);
					if (!tr.startsolid && tr.fraction < 1.0f && fabsf(tr.plane.normal[2]) < 0.7f) {
						float dist = tr.fraction * 128.0f;
						if (dist < bestWallDist) {
							bestWallDist = dist;
							VectorScale(tr.plane.normal, -1.0f, wallDir);
							wallDir[2] = 0.0f;
							VectorNormalize(wallDir);
							VectorCopy(tr.endpos, bestWallPoint);
							bestWallPoint[2] = ent->client->ps.origin[2];
						}
					}
				}

				if (level.time - bot2_states[clientNum].diagTimer > 400) {
					Bot2_PrintTelemetry(1, "[%s] WALLRUN TRACE: found=%d dist=%.1f\n",
						ent->client->pers.netname, (int)(bestWallDist < 999999.0f), bestWallDist);
				}

				float stageYaw;
				if (bestWallDist < 999999.0f) {
					vec3_t wa; vectoangles(wallDir, wa); stageYaw = wa[YAW];
				} else {
					stageYaw = ent->client->ps.viewangles[YAW];
					VectorCopy(offmeshStart, bestWallPoint);
				}

				// ---- FACE IDENTITY CHECKS (cheapest first, short-circuit on failure) ----
				// These confirm the bot is actually adjacent to side A before we steal
				// locomotion.  If any fail, we fall through to normal navmesh movement
				// so pathfinding can route the bot around the corner first.

				// Check 1: half-space — bot must be in front of side A's plane.
				// wallDir points INTO the wall; vector from bot to bestWallPoint dotted
				// with wallDir is positive iff the bot is on the approach side.
				float botToWallX = bestWallPoint[0] - ent->client->ps.origin[0];
				float botToWallY = bestWallPoint[1] - ent->client->ps.origin[1];
				qboolean inFrontOfSideA = (botToWallX*wallDir[0] + botToWallY*wallDir[1]) > 0.0f;

				// Check 2: normal alignment — the nearest wall from the BOT must be side A.
				// Trace from chest height toward side A; if something else (side B, a column)
				// intercepts first its normal won't align with side A's and the dot is low.
				qboolean nearestWallIsSideA = qfalse;
				{
					vec3_t normStart, normEnd;
					VectorCopy(ent->client->ps.origin, normStart);
					normStart[2] += 24.0f;
					VectorMA(normStart, 128.0f, wallDir, normEnd);
					trace_t normTr;
					trap->Trace(&normTr, normStart, NULL, NULL, normEnd, ent->s.number, MASK_SOLID, qfalse, 0, 0);
					if (!normTr.startsolid && normTr.fraction < 1.0f && fabsf(normTr.plane.normal[2]) < 0.7f) {
						// side A outward normal = -wallDir; dot against hit normal should be ~1
						float nd = -(normTr.plane.normal[0]*wallDir[0] + normTr.plane.normal[1]*wallDir[1]);
						nearestWallIsSideA = (nd > 0.85f);
					}
				}

				// Check 3: bbox LOS — nothing should block the bot's hull from reaching
				// the flush position in front of side A.  Pull the endpoint back from
				// the wall surface by 20 u (roughly hull half-width) so the trace target
				// is not embedded in solid geometry — tracing to the surface itself will
				// always appear blocked by the wall's own face.
				qboolean hasLosToSideA = qfalse;
				{
					vec3_t losStart, losEnd;
					VectorCopy(ent->client->ps.origin, losStart);
					losStart[2] += 24.0f;
					// wallDir points INTO the wall, so subtract it to stand in front of it.
					losEnd[0] = bestWallPoint[0] - wallDir[0] * 20.0f;
					losEnd[1] = bestWallPoint[1] - wallDir[1] * 20.0f;
					losEnd[2] = losStart[2];
					trace_t losTr;
					trap->Trace(&losTr, losStart, ent->r.mins, ent->r.maxs, losEnd,
					            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
					float dx = losEnd[0]-losStart[0], dy = losEnd[1]-losStart[1];
					float totalDist = sqrtf(dx*dx + dy*dy);
					float remaining = (1.0f - losTr.fraction) * totalDist;
					hasLosToSideA = (remaining < 16.0f);
					if (!hasLosToSideA && level.time - bot2_states[clientNum].diagTimer > 400) {
						Bot2_PrintTelemetry(1, "[%s] WALLRUN LOS BLOCKED: remaining=%.0f blocker normal {%.2f %.2f %.2f}\n",
							ent->client->pers.netname, remaining,
							losTr.plane.normal[0], losTr.plane.normal[1], losTr.plane.normal[2]);
					}
				}

				qboolean onCorrectFace = inFrontOfSideA && nearestWallIsSideA && hasLosToSideA;

				if (level.time - bot2_states[clientNum].diagTimer > 400) {
					bot2_states[clientNum].diagTimer = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN STAGING: dist=%.0f yaw=%.1f onFace=%d (hs=%d nm=%d los=%d)\n",
						ent->client->pers.netname, offmeshDistToStart2D, stageYaw,
						(int)onCorrectFace, (int)inFrontOfSideA,
						(int)nearestWallIsSideA, (int)hasLosToSideA);
				}

				if (!onCorrectFace) {
					// Wrong face or corner occluded — let navmesh route us around.
					// Do not return; fall through to normal locomotion below.
				}
				else {
					// On the correct face — take locomotion control.

					// Force saber every tick so the combat system can't override it.
					if (bot2_states[clientNum].savedWeapon == 0 && ent->client->ps.weapon != WP_SABER)
						bot2_states[clientNum].savedWeapon = ent->client->ps.weapon;
					ucmd->weapon = WP_SABER;

					// Face the wall and steer toward the contact point.
					vec3_t stageAngles = { 0.0f, stageYaw, 0.0f };
					Bot2_ApplySmoothing(clientNum, stageAngles, ucmd, qfalse);

					vec3_t toWallPoint;
					VectorSubtract(bestWallPoint, ent->client->ps.origin, toWallPoint);
					toWallPoint[2] = 0.0f;
					float distToWallPoint = VectorLength(toWallPoint);
					float toPointYaw = atan2f(toWallPoint[1], toWallPoint[0]) * (180.0f / M_PI);
					float yawDiff = (toPointYaw - stageAngles[YAW]) * (M_PI / 180.0f);
					ucmd->forwardmove = (char)max(-127, min(127, 127.0f * cosf(yawDiff)));
					ucmd->rightmove   = (char)max(-127, min(127, 127.0f * -sinf(yawDiff)));
					if (distToWallPoint <= 16.0f)
						ucmd->forwardmove = 127;
					ucmd->upmove = 0;

					// Flush check: point-trace 25 u forward along stageYaw.
					vec3_t stageFwd = { 0.0f, 0.0f, 0.0f };
					AngleVectors(stageAngles, stageFwd, NULL, NULL);
					vec3_t stageTraceEnd;
					VectorMA(ent->client->ps.origin, 25.0f, stageFwd, stageTraceEnd);
					trace_t stageTr;
					trap->Trace(&stageTr, ent->client->ps.origin, NULL, NULL, stageTraceEnd,
					            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
					qboolean stageGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);
					qboolean stageFlush    = (stageTr.fraction < 0.9f);

					// Mount vs jump-off: destination behind the wall face = jump-off.
					float wallFwdX = cosf(stageYaw * (M_PI / 180.0f));
					float wallFwdY = sinf(stageYaw * (M_PI / 180.0f));
					float dxEnd = offmeshEnd[0] - offmeshStart[0];
					float dyEnd = offmeshEnd[1] - offmeshStart[1];
					qboolean shouldTopJump = (dxEnd*wallFwdX + dyEnd*wallFwdY) < 0.0f;

					// Activate: flush, grounded, saber.
					if (stageFlush && stageGrounded && ent->client->ps.weapon == WP_SABER) {
						bot2_states[clientNum].wallrunFaceYaw    = stageYaw;
						bot2_states[clientNum].wallrunTopJump    = shouldTopJump;
						bot2_states[clientNum].wallrunPhase3Time = 0;
						bot2_states[clientNum].wallrunRetries    = 0;
						bot2_states[clientNum].wallrunCooldown   = 0;
						if (bot2_states[clientNum].savedWeapon == 0)
							bot2_states[clientNum].savedWeapon = ent->client->ps.weapon;
						bot2_states[clientNum].wallrunPhase = 1;
						bot2_states[clientNum].offmeshType  = offmeshType;
						VectorCopy(offmeshEnd, bot2_states[clientNum].tele_predPos);
						bot2_states[clientNum].stateTimer   = level.time;
						bot2_states[clientNum].state        = BOT_STATE_WALLRUN;
						Bot2_PrintTelemetry(1, "[%s] WALLRUN ACTIVATE: flush (tr=%.2f) | faceYaw=%.1f topJump=%d savedWeapon=%d\n",
							ent->client->pers.netname, stageTr.fraction, stageYaw,
							(int)bot2_states[clientNum].wallrunTopJump,
							bot2_states[clientNum].savedWeapon);
					}
					// Staging owns the tick — return regardless of whether we activated.
					return;
				}
			}
			// Cooldown active or distance > 64 u: fall through to normal locomotion.
		}

		else if (offmeshType == OFFMESH_AREA_JUMP_BASIC) {
			float dxOM = ent->client->ps.origin[0] - offmeshStart[0];
			float dyOM = ent->client->ps.origin[1] - offmeshStart[1];
			offmeshDistToStart2D = sqrtf(dxOM*dxOM + dyOM*dyOM);
		}

		qboolean jExec = qfalse;

		// --- DIAGNOSTIC GATEKEEPER (STATE 0) ---
		if (wantsSpeed) {
			vec3_t waypointAfter;
			qboolean hasAfter = validPath ? NavMesh_GetNextWaypoint(ent->s.number, nextWp, targetOrigin, waypointAfter) : qfalse;

			if (current_speed < max_run_speed - 35.0f) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Speed too low for running jump", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else if (distToTarget <= 400.0f) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Target too close (< 300 units)", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else if (lDang) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Ledge danger detected ahead", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else if (validPath && (IsApproachingElevator(ent, nextWp) || (hasAfter && IsApproachingElevator(ent, waypointAfter)))) {
				Q_strncpyz(bot2_states[clientNum].lastFailReason, "Approaching elevator (func_plat / func_door)", sizeof(bot2_states[clientNum].lastFailReason));
			}
			else {
				int pDir = EvaluateStrafeDir(ent, base_target_yaw);
				int tD[] = { pDir, pDir * -1 };

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
						bot2_states[clientNum].state = BOT_STATE_AIRBORNE; bot2_states[clientNum].stateTimer = level.time; bot2_states[clientNum].targetYaw = base_target_yaw;
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
							Bot2_PrintTelemetry(1,"[%s] CHAIN AngleFrac Hist | boundary=%.0fpct optimal=%.0fpct | counts: %d %d\n",
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

		// --- OFF-MESH BASIC JUMP FALLBACK ---
		// The strafe-jump system has now had its chance.  If it didn't fire (jExec is
		// false) and the bot is within activation range of a basic-jump connection,
		// engage State 6 — a slower but reliable direct jump over the gap.
		if (!jExec && offmeshType == OFFMESH_AREA_JUMP_BASIC && offmeshDistToStart2D <= 48.0f) {
			bot2_states[clientNum].offmeshType = OFFMESH_AREA_JUMP_BASIC;
			VectorCopy(offmeshEnd, bot2_states[clientNum].tele_predPos);
			bot2_states[clientNum].stateTimer = level.time;
			bot2_states[clientNum].state      = BOT_STATE_JUMP;

			vec3_t toEnd, toEndAngles;
			VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, toEnd);
			vectoangles(toEnd, toEndAngles);
			if (lockAim) vectoangles(aimDir, toEndAngles);
			Bot2_ApplySmoothing(clientNum, toEndAngles, ucmd, lockAim);
			ucmd->forwardmove = 127;
			ucmd->rightmove   = 0;
			ucmd->upmove      = 127;
			Bot2_PrintTelemetry(1, "[%s] STATE Transition: Off-Mesh Jump (fallback) -> {%.0f %.0f %.0f}\n",
				ent->client->pers.netname, offmeshEnd[0], offmeshEnd[1], offmeshEnd[2]);
			return;
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
			Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd, lockAim);
			ucmd->upmove = 0;
		}
	}
	// --- STATE 1: ESCAPE JUMP (STUCK RECOVERY) ---
	else if (bot2_states[clientNum].state == BOT_STATE_ESCAPE) {
		int escElapsed = level.time - bot2_states[clientNum].stateTimer;
		qboolean escGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);

		vec3_t escAngles = { 0.0f, bot2_states[clientNum].targetYaw, 0.0f };
		Bot2_ApplySmoothing(clientNum, escAngles, ucmd, qfalse);
		ucmd->forwardmove = 127;
		ucmd->rightmove   = 0;

		// Two-phase escape:
		//   Phase A (0-300 ms): run forward to build speed — the scan found a
		//     destination at full-speed jump range, so we need real velocity before
		//     firing the jump or the arc will be too short.
		//   Phase B (300-450 ms): hold jump for 150 ms to achieve full height.
		//   Phase C (450 ms+): release jump, wait to land.
		if (escElapsed < 300) {
			ucmd->upmove = 0; // build speed first
		} else if (escElapsed < 450) {
			ucmd->upmove = 127; // jump
		} else {
			ucmd->upmove = 0;
		}

		// Exit: grounded after the jump phase has had time to land
		if (escGrounded && escElapsed > 500) {
			bot2_states[clientNum].state = BOT_STATE_WALK;
			bot2_states[clientNum].stateTimer = level.time;
			Bot2_PrintTelemetry(1, "[%s] STUCK RECOVERY: landed from escape jump (elapsed=%dms spd=%.0f)\n",
				ent->client->pers.netname, escElapsed, current_speed);
		}

		// Hard timeout
		if (escElapsed > 3000) {
			bot2_states[clientNum].state = BOT_STATE_WALK;
			bot2_states[clientNum].stateTimer = level.time;
			Bot2_PrintTelemetry(1, "[%s] STUCK RECOVERY: escape timeout\n", ent->client->pers.netname);
		}
	}
	// --- STATE 2: AIRBORNE ---
	else if (bot2_states[clientNum].state == BOT_STATE_AIRBORNE) {
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
			ucmd->rightmove   = (char)(127.0f * -sin(tDiff));
		}
		else {
			desiredAngles[PITCH] = 0;
			desiredAngles[YAW] = tYaw;
			ucmd->forwardmove = isHardStrafe ? 0 : 127;
			ucmd->rightmove = 127 * sDir;
		}
		Bot2_ApplySmoothing(clientNum, desiredAngles, ucmd, lockAim);

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
			bot2_states[clientNum].state = BOT_STATE_WALK;
			bot2_states[clientNum].stateTimer = level.time;
		}
	}

	// ==========================================================================
	// STATE 5: OFF-MESH DROP — walk/fall off a ledge to a lower platform
	// ==========================================================================
	else if (bot2_states[clientNum].state == BOT_STATE_DROP) {
		int dropElapsed = level.time - bot2_states[clientNum].stateTimer;
		qboolean dropGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);

		if (!bot2_states[clientNum].tele_inAir && !dropGrounded) {
			bot2_states[clientNum].tele_inAir = 1;
			Bot2_PrintTelemetry(1, "[%s] DROP EVENT: airborne\n", ent->client->pers.netname);
		}

		if (dropElapsed > 3000) {
			bot2_states[clientNum].state         = BOT_STATE_WALK;
			bot2_states[clientNum].offmeshType   = 0;
			bot2_states[clientNum].offmeshExitTime = level.time;
			Bot2_PrintTelemetry(1, "[%s] DROP TIMEOUT %dms | pos {%.0f %.0f %.0f} target {%.0f %.0f %.0f}\n",
				ent->client->pers.netname, dropElapsed,
				ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2],
				bot2_states[clientNum].tele_predPos[0], bot2_states[clientNum].tele_predPos[1], bot2_states[clientNum].tele_predPos[2]);
		}
		else {
			vec3_t toEndAngles;
			if (!bot2_states[clientNum].tele_inAir) {
				// Walk off the exact ledge we found during activation
				toEndAngles[PITCH] = 0.0f;
				toEndAngles[YAW]   = bot2_states[clientNum].tele_takeoffYaw;
				toEndAngles[ROLL]  = 0.0f;
			} else {
				// Once airborne, steer directly toward the landing spot
				vec3_t toEnd;
				VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, toEnd);
				toEnd[2] = 0.0f; // Walk in XY direction
				vectoangles(toEnd, toEndAngles);
			}

			if (lockAim) vectoangles(aimDir, toEndAngles);
			Bot2_ApplySmoothing(clientNum, toEndAngles, ucmd, lockAim);
			ucmd->forwardmove = 127;
			ucmd->rightmove   = 0;
			ucmd->upmove      = 0;

			if (level.time - bot2_states[clientNum].diagTimer > 500) {
				bot2_states[clientNum].diagTimer = level.time;
				float dz = ent->client->ps.origin[2] - bot2_states[clientNum].tele_predPos[2];
				Bot2_PrintTelemetry(1, "[%s] DROP %dms | grounded=%d inAir=%d | Z %.0f -> %.0f (gap %.0f) | vel_z %.0f\n",
					ent->client->pers.netname, dropElapsed,
					(int)dropGrounded, bot2_states[clientNum].tele_inAir,
					ent->client->ps.origin[2], bot2_states[clientNum].tele_predPos[2], dz,
					ent->client->ps.velocity[2]);
			}

			// Only consider the drop finished if we have actually left the ground first
			if (bot2_states[clientNum].tele_inAir && dropGrounded && dropElapsed > 150) {
				float dx = ent->client->ps.origin[0] - bot2_states[clientNum].tele_predPos[0];
				float dy = ent->client->ps.origin[1] - bot2_states[clientNum].tele_predPos[1];
				float dz = ent->client->ps.origin[2] - bot2_states[clientNum].tele_predPos[2];
				bot2_states[clientNum].state         = BOT_STATE_WALK;
				bot2_states[clientNum].stateTimer    = level.time;
				bot2_states[clientNum].offmeshType   = 0;
				bot2_states[clientNum].offmeshExitTime = level.time;
				Bot2_PrintTelemetry(1, "[%s] DROP LAND %dms | err {%+.0f %+.0f %+.0f}\n",
					ent->client->pers.netname, dropElapsed, dx, dy, dz);
			}
		}
	}

	// ==========================================================================
	// STATE 6: OFF-MESH JUMP — running jump across a gap
	// ==========================================================================
	else if (bot2_states[clientNum].state == BOT_STATE_JUMP) {
		int jumpElapsed = level.time - bot2_states[clientNum].stateTimer;
		qboolean jumpGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);

		if (jumpElapsed > 3000) {
			bot2_states[clientNum].state      = BOT_STATE_WALK;
			bot2_states[clientNum].offmeshType = 0;
			Bot2_PrintTelemetry(1, "[%s] JUMP TIMEOUT %dms | pos {%.0f %.0f %.0f} target {%.0f %.0f %.0f}\n",
				ent->client->pers.netname, jumpElapsed,
				ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2],
				bot2_states[clientNum].tele_predPos[0], bot2_states[clientNum].tele_predPos[1], bot2_states[clientNum].tele_predPos[2]);
		}
		else {
			vec3_t toEnd, toEndAngles;
			VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, toEnd);
			vectoangles(toEnd, toEndAngles);
			if (lockAim) vectoangles(aimDir, toEndAngles);
			Bot2_ApplySmoothing(clientNum, toEndAngles, ucmd, lockAim);
			ucmd->forwardmove = 127;
			ucmd->rightmove   = 0;
			ucmd->upmove = (jumpElapsed < 150) ? 127 : 0;

			if (level.time - bot2_states[clientNum].diagTimer > 500) {
				bot2_states[clientNum].diagTimer = level.time;
				float dx = bot2_states[clientNum].tele_predPos[0] - ent->client->ps.origin[0];
				float dy = bot2_states[clientNum].tele_predPos[1] - ent->client->ps.origin[1];
				float distXY = sqrtf(dx*dx + dy*dy);
				float dz = ent->client->ps.origin[2] - bot2_states[clientNum].tele_predPos[2];
				Bot2_PrintTelemetry(1, "[%s] JUMP %dms | grounded=%d | distXY %.0f | Z gap %+.0f | vel {%.0f %.0f %.0f}\n",
					ent->client->pers.netname, jumpElapsed,
					(int)jumpGrounded, distXY, dz,
					ent->client->ps.velocity[0], ent->client->ps.velocity[1], ent->client->ps.velocity[2]);
			}

			if (jumpGrounded && jumpElapsed > 300) {
				float dx = ent->client->ps.origin[0] - bot2_states[clientNum].tele_predPos[0];
				float dy = ent->client->ps.origin[1] - bot2_states[clientNum].tele_predPos[1];
				float dz = ent->client->ps.origin[2] - bot2_states[clientNum].tele_predPos[2];
				bot2_states[clientNum].state      = BOT_STATE_WALK;
				bot2_states[clientNum].stateTimer  = level.time;
				bot2_states[clientNum].offmeshType = 0;
				Bot2_PrintTelemetry(1, "[%s] JUMP LAND %dms | err {%+.0f %+.0f %+.0f}\n",
					ent->client->pers.netname, jumpElapsed, dx, dy, dz);
			}
		}
	}

	// ==========================================================================
	// STATE 7: OFF-MESH WALLRUN
	// ==========================================================================
	// Phase sequence:
	//   0 — (unused at runtime; staging activates at phase 1)
	//   1 — first jump: press jump until airborne
	//   2 — release: upmove=0 until PMF_JUMP_HELD clears
	//   3 — second jump: single tick upmove=127 fires BOTH_FORCEWALLRUNFLIP_START
	//   4 — ascending: upmove=0; if wallrunTopJump, press jump at apex
	//   5 — post-top-jump: falling toward far-side dest, wait to land
	// Exit: grounded at/above target Z (phase >= 4), or hard timeout at 5 s.
	else if (bot2_states[clientNum].state == BOT_STATE_WALLRUN) {
		int      wrElapsed  = level.time - bot2_states[clientNum].stateTimer;
		qboolean wrGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);
		float    wrBotZ     = ent->client->ps.origin[2];
		float    wrEndZ     = bot2_states[clientNum].tele_predPos[2];
		int     *phase      = &bot2_states[clientNum].wallrunPhase;
		qboolean jumpHeld   = (ent->client->ps.pm_flags & PMF_JUMP_HELD) != 0;
		float    wrFaceYaw  = bot2_states[clientNum].wallrunFaceYaw;

		// Helper macro — restores saved weapon on every exit path
#define WALLRUN_EXIT() \
		do { \
			if (bot2_states[clientNum].savedWeapon > 0) { \
				ucmd->weapon = bot2_states[clientNum].savedWeapon; \
				bot2_states[clientNum].savedWeapon = 0; \
			} \
		} while(0)

		if (wrElapsed > 5000) {
			WALLRUN_EXIT();
			bot2_states[clientNum].state      = BOT_STATE_WALK;
			bot2_states[clientNum].offmeshType = 0;
			Bot2_PrintTelemetry(1, "[%s] WALLRUN TIMEOUT %dms | phase=%d | Z %.0f / %.0f\n",
				ent->client->pers.netname, wrElapsed, *phase, wrBotZ, wrEndZ);
		}
		else {
			// WP_SABER required by pmove outer gate (bg_pmove.c:2089).
			ucmd->weapon = WP_SABER;

			// Force the bot to have Force Jump Level 3, otherwise the JKA engine
			// will silently refuse to execute the wallrun animation.
			ent->client->ps.fd.forcePowerLevel[FP_LEVITATION] = FORCE_LEVEL_3;
			ent->client->ps.fd.forcePowersKnown |= (1 << FP_LEVITATION);

			// Check if the engine is currently executing the ascent animation
			int currentAnim = ent->client->ps.legsAnim;
			qboolean isWallrunning = (currentAnim == BOTH_FORCEWALLRUNFLIP_START);

			switch (*phase) {
			case 1: // Phase 1: Jump off the ground
				ucmd->upmove = 127;
				if (!wrGrounded) {
					*phase = 2;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 2: airborne, releasing jump\n", ent->client->pers.netname);
				}
				break;
			case 2: // Phase 2: Ensure JUMP_HELD is fully cleared before we latch
				ucmd->upmove = 0;
				if (!jumpHeld) {
					*phase = 3;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 3: JUMP_HELD clear, latching to wall\n", ent->client->pers.netname);
				}
				break;
			case 3: // Phase 3: Press jump to latch onto the wall
				ucmd->upmove = 127;
				if (isWallrunning) {
					*phase = 4;
					bot2_states[clientNum].wallrunPhase3Time = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 4: engine accepted wallrun\n", ent->client->pers.netname);
				}
				break;
			case 4: { // Phase 4: Ascending (RELEASE JUMP KEY)
				ucmd->upmove = 0; // Must release jump so we can press it again at the apex!
				int   p3Elapsed = level.time - bot2_states[clientNum].wallrunPhase3Time;
				float vzNow     = ent->client->ps.velocity[2];
				float wrBotZ4   = ent->client->ps.origin[2];
				float wrEndZ4   = bot2_states[clientNum].tele_predPos[2];

				// Player recording shows the kickflip fires while vz is still strongly
				// positive (~297 u/s) — NOT at the apex. The player fires when they have
				// enough combined velocity to coast to the destination:
				//
				//   (vz + kickflip_boost)² >= 2 * gravity * (targetZ - currentZ)
				//
				// Measured from recording: kickflip adds ~200 u/s vz (297→497).
				// JKA default gravity: 800 u/s².  Both triggers require isWallrunning
				// so jump is pressed while anim=872 is active — pressing jump into a
				// dead animation does nothing useful.
#define WR_KICK_BOOST 200.0f
#define WR_GRAVITY    800.0f
				float heightNeeded  = wrEndZ4 - wrBotZ4;
				float boostTotal    = vzNow + WR_KICK_BOOST;
				// physicsReady: boost is sufficient to reach target Z. 300 ms minimum
				// guards against firing during the initial wallrun impulse spike.
				qboolean physicsReady = isWallrunning && (p3Elapsed >= 300)
				    && (boostTotal * boostTotal >= 2.0f * WR_GRAVITY * heightNeeded);

				// animEnding: fire at 1100 ms if physicsReady never triggered — still
				// inside the ~1350 ms animation window, giving ~250 ms of jump presses.
				qboolean animEnding = isWallrunning && (p3Elapsed >= 1100);

				// timerApex: last resort if animation already ended somehow.
				qboolean timerApex = !isWallrunning && (p3Elapsed >= 1500 && vzNow < 100.0f);

				if (bot2_states[clientNum].wallrunTopJump && (physicsReady || animEnding || timerApex)) {
					// Press jump THIS tick — isWallrunning is confirmed true right now.
					// If we wait until case 5 next tick, the animation may have ended in
					// the 50 ms gap and the engine won't register the kickflip.
					ucmd->upmove = 127;
					*phase = 5;
					const char* reason = physicsReady ? "physics" : animEnding ? "anim-ending" : "timer";
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 5: kickflip p3+%dms vz=%.0f Z=%.0f/%.0f (%s)\n",
						ent->client->pers.netname, p3Elapsed, vzNow, wrBotZ4, wrEndZ4, reason);
				}
				break;
			}
			case 5: // Phase 5: Manual kickflip (PRESS JUMP KEY)
				// Jump input is handled here; steering is handled below in the STEERING block.
				ucmd->upmove = 127;
				if (!isWallrunning) {
					// The engine has completed the ascent and executed the flip!
					*phase = 6;
					bot2_states[clientNum].wallrunPhase5Time = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 6: flip executed, steering to destination\n", ent->client->pers.netname);
				}
				break;
			case 6: // Phase 6: Post-flip — wait to land
				ucmd->upmove = 0;
				break;
			default:
				ucmd->upmove = 0;
				break;
			}

			// --- STEERING ---
			if (*phase == 5 && bot2_states[clientNum].wallrunTopJump) {
				// Jump-off phase 5: kick away from the wall toward the far-side destination.
				// PM_AdjustAngleForWallRunUp requires forwardmove > 0 to apply the kickflip
				// velocity boost — using S (-127) suppresses the boost entirely.
				// Both branches hold W (+127).  In the non-lockAim case we keep facing the
				// wall (wrFaceYaw) so the engine still fires the wallrun flip geometry, then
				// phase 6 steers to the destination once the flip completes.
				if (lockAim) {
					vec3_t toEnd, toEndAngles;
					VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, toEnd);
					toEnd[2] = 0.0f;
					vectoangles(toEnd, toEndAngles);
					vectoangles(aimDir, toEndAngles);
					Bot2_ApplySmoothing(clientNum, toEndAngles, ucmd, qtrue);
					ucmd->forwardmove = 127;
				} else {
					vec3_t wrFaceAngles = { 0.0f, wrFaceYaw, 0.0f };
					Bot2_ApplySmoothing(clientNum, wrFaceAngles, ucmd, qfalse);
					ucmd->forwardmove = 127;  // W — required by PM_AdjustAngleForWallRunUp
				}
				ucmd->rightmove = 0;
			} else if (*phase < 6) {
				// Phases 1-4 (and phase 5 for mount): face the wall and hold W
				vec3_t wrFaceAngles = { 0.0f, wrFaceYaw, 0.0f };
				Bot2_ApplySmoothing(clientNum, wrFaceAngles, ucmd, qfalse);
				ucmd->forwardmove = 127;
				ucmd->rightmove   = 0;
			} else {
				// Phase 6: The flip has completed. Steer towards the landing destination!
				vec3_t toEnd, toEndAngles;
				VectorSubtract(bot2_states[clientNum].tele_predPos, ent->client->ps.origin, toEnd);
				toEnd[2] = 0.0f;
				vectoangles(toEnd, toEndAngles);
				if (lockAim) vectoangles(aimDir, toEndAngles);
				Bot2_ApplySmoothing(clientNum, toEndAngles, ucmd, lockAim);
				ucmd->forwardmove = 127;
				ucmd->rightmove   = 0;
			}

			// If we haven't left the ground after 1.5 s, or if we failed to attach, recover.
			if (*phase <= 3 && wrElapsed > 1500) {
				int retries = bot2_states[clientNum].wallrunRetries;

				if (retries < 2) {
					WALLRUN_EXIT();
					bot2_states[clientNum].wallrunRetries++;
					bot2_states[clientNum].state      = BOT_STATE_WALK;
					bot2_states[clientNum].offmeshType = 0;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN RETRY %d/2: back to staging\n",
						ent->client->pers.netname, bot2_states[clientNum].wallrunRetries);
				}
				else {
					Bot2_PrintTelemetry(1, "[%s] WALLRUN ESCAPE SCAN (retry %d): sweeping 360\n",
						ent->client->pers.netname, retries);

					float  escYaw = -1.0f;
					vec3_t escLand = { 0.0f, 0.0f, 0.0f };
					qboolean escFound = Bot2_FindEscapeJump(ent, clientNum, targetOrigin,
					                                        max_run_speed, aimDir,
					                                        &escYaw, escLand);

					WALLRUN_EXIT();
					bot2_states[clientNum].wallrunCooldown = level.time;
					bot2_states[clientNum].wallrunRetries  = 0;
					bot2_states[clientNum].state      = BOT_STATE_WALK;
					bot2_states[clientNum].offmeshType = 0;

					if (escFound) {
						bot2_states[clientNum].state = BOT_STATE_ESCAPE;
						bot2_states[clientNum].stateTimer = level.time;
						bot2_states[clientNum].targetYaw = escYaw;
						Bot2_PrintTelemetry(1,
							"[%s] WALLRUN ESCAPE: escape jump yaw=%.0f -> {%.0f %.0f %.0f}\n",
						ent->client->pers.netname, escYaw,
						escLand[0], escLand[1], escLand[2]);
					} else {
						Bot2_PrintTelemetry(1, "[%s] WALLRUN ESCAPE: no valid jump found, hard abort\n",
							ent->client->pers.netname);
					}
				}
			}

			// Periodic telemetry
			if (level.time - bot2_states[clientNum].diagTimer > 300) {
				bot2_states[clientNum].diagTimer = level.time;
				Bot2_PrintTelemetry(1, "[%s] WALLRUN %dms phase=%d | Z %.0f/%.0f | grnd=%d jHeld=%d | vz %.0f\n",
					ent->client->pers.netname, wrElapsed, *phase,
					wrBotZ, wrEndZ,
					(int)wrGrounded, (int)jumpHeld,
					ent->client->ps.velocity[2]);
			}

			// Success exit: grounded at or above target Z, wallrun completed (phase >= 4)
			if (wrGrounded && *phase >= 4 && wrElapsed > 500) {
				if (wrBotZ >= wrEndZ - 64.0f) {
					WALLRUN_EXIT();
					bot2_states[clientNum].state           = BOT_STATE_WALK;
					bot2_states[clientNum].stateTimer      = level.time;
					bot2_states[clientNum].offmeshType     = 0;
					bot2_states[clientNum].offmeshExitTime = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN COMPLETE %dms | Z %.0f / %.0f\n",
						ent->client->pers.netname, wrElapsed, wrBotZ, wrEndZ);
				}
				else if (wrElapsed > 3000) {
					WALLRUN_EXIT();
					bot2_states[clientNum].state           = BOT_STATE_WALK;
					bot2_states[clientNum].stateTimer      = level.time;
					bot2_states[clientNum].offmeshType     = 0;
					bot2_states[clientNum].offmeshExitTime = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN INCOMPLETE %dms | Z %.0f / %.0f\n",
						ent->client->pers.netname, wrElapsed, wrBotZ, wrEndZ);
				}
			}
		}
#undef WALLRUN_EXIT
	}
}

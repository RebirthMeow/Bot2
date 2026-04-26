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

// Resolve the apex/landing waypoint of the jumppad the bot is currently inside.
//
// A trigger_push entity QUAKED-spec mandates a target pointing at a
// target_position which is the apex of the leap (see g_trigger.c, AimAtTarget
// and the trigger_push QUAKED block).  That target_position IS the secondary
// waypoint of the jumppad pair, and once airborne the bot cannot steer, so it
// is the only correct camera lock for State 3.
//
// We deliberately bypass NavMesh_GetNextWaypoint here.  Detour's path query
// returns whatever waypoint its cost / value function rates best from the
// pre-jump position, which can drift to a different route entirely if the path
// costs have been retuned — producing the "camera locked on the wrong waypoint"
// bug after stepping onto a jumppad.  The trigger's own target_position is
// stable and physically meaningful regardless of pathfinding state.
//
// Returns qtrue on success and writes the target_position's origin into
// outTargetPos.  Returns qfalse if the bot is not inside a trigger_push, the
// trigger has no target, or the target entity cannot be resolved.
qboolean GetTriggerPushTargetOrigin(gentity_t* ent, vec3_t outTargetPos) {
	gentity_t* push = NULL;
	while ((push = G_Find(push, FOFS(classname), "trigger_push")) != NULL) {
		if (!push->r.linked) continue;

		vec3_t mins, maxs;
		VectorAdd(ent->client->ps.origin, ent->r.mins, mins);
		VectorAdd(ent->client->ps.origin, ent->r.maxs, maxs);
		if (maxs[0] < push->r.absmin[0] || mins[0] > push->r.absmax[0]) continue;
		if (maxs[1] < push->r.absmin[1] || mins[1] > push->r.absmax[1]) continue;
		if (maxs[2] < push->r.absmin[2] || mins[2] > push->r.absmax[2]) continue;

		// Found the trigger_push enclosing the bot.  Look up its target_position.
		if (!push->target || !push->target[0]) return qfalse;
		gentity_t* tgt = G_PickTarget(push->target);
		if (!tgt) return qfalse;

		// target_position calls G_SetOrigin at spawn, so r.currentOrigin is
		// authoritative.  Fall back to s.origin if currentOrigin is unset
		// (defensive — shouldn't happen for a properly spawned target_position).
		if (tgt->r.currentOrigin[0] != 0.0f ||
		    tgt->r.currentOrigin[1] != 0.0f ||
		    tgt->r.currentOrigin[2] != 0.0f) {
			VectorCopy(tgt->r.currentOrigin, outTargetPos);
		} else {
			VectorCopy(tgt->s.origin, outTargetPos);
		}
		return qtrue;
	}
	return qfalse;
}

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
// Safely Wrapped Syscalls for Pmove Simulation
// ==============================================================================
static void Bot2_PMTrace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentMask) {
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

static void Bot2_ApplySmoothing(int clientNum, vec3_t desiredAngles, usercmd_t* ucmd, qboolean lockAim) {
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
static qboolean Bot2_SimulateWallrunEx(gentity_t *ent, float faceYaw, vec3_t out_landPos) {
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
static qboolean Bot2_SimulateWallrun(gentity_t *ent, float faceYaw) {
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
	#define WALLRUN_SCAN_MAX_FLOORS 8
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
			// This ensures walls on lower levels (e.g. the lower floor of ctf4) are
			// sampled — a single downward trace would only find the topmost floor.
			//
			// Step-down math: when a bbox trace lands at endpos[2], the physical
			// floor surface is at endpos[2] + s_playerMins[2] = endpos[2] - 24.
			// To start the next trace entirely below that surface, the bbox top
			// (center + 32) must clear it: center < (endpos[2]-24) - 32 - margin.
			// Using margin=8 gives scanTop = endpos[2] - 64.
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
					// scanTop landed inside a floor brush — the bbox is embedded in the
					// solid mass between two floor levels (which can be arbitrarily thick).
					// Bbox traces from inside solid have inconsistent BSP behaviour, so
					// walk downward with PointContents (point query, not bbox — always
					// unambiguous) until we exit the solid, then set scanTop just below
					// the exit point and retry the bbox trace at that depth.
					// This does NOT consume a floor-level slot (flv-- + continue).
					const float SOLID_STEP  = 32.0f;
					const int   MAX_SOLID_STEPS = 512; // cap: ~16 384 u max slab, sub-ms cost
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
						break; // slab extends to map bottom or hit step cap — done

					// Drop one extra bbox-half so the next trace's maxs[2] doesn't
					// clip back into the slab we just exited.
					scanTop = probeZ - 32.0f;
					flv--; // retry at same floor depth — don't bill this as a floor level
					continue;
				}

				if (groundTr.allsolid)
					break; // degenerate — shouldn't normally occur after startsolid check

				// Advance scanTop for the next floor level regardless of walkability,
				// so non-walkable ledges don't block discovery of floors below them.
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

				// Ray-trace from mid-body height in probe direction.
				// Using a ray (no box) so we hit the wall face precisely.
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
				// The wallrun trigger in bg_pmove requires the player to be adjacent to
				// the wall during the double-jump tick.  Starting the simulation at the
				// raw grid point (which may be far from the wall) fails the trigger check.
				//
				// Instead: step back from the wall hit point by one player half-width (15u)
				// plus 1u clearance along the wall normal to get the flush position in XY,
				// then drop a bbox trace downward to find the actual ground Z.  This is the
				// same position the bot occupies when it presses into the wall in-game.
				//
				// The sidecar uses this same position — it is already ~16u from the wall
				// face, matching the navmesh generator's START_SETBACK convention, and
				// sits comfortably within the builder's 48u XZ snap radius.
				vec3_t flushXY;
				VectorMA(wallTr.endpos, 16.0f, wallTr.plane.normal, flushXY);

				vec3_t flushGroundFrom = { flushXY[0], flushXY[1], flushXY[2] + 64.0f };
				vec3_t flushGroundTo   = { flushXY[0], flushXY[1], flushXY[2] - 128.0f };
				trace_t flushGnd;
				trap->Trace(&flushGnd, flushGroundFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					flushGroundTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (flushGnd.allsolid || flushGnd.startsolid || flushGnd.fraction >= 1.0f)
					continue; // no solid ground at flush position
				if (flushGnd.plane.normal[2] < 0.7f)
					continue; // slope is too steep — not navmesh-walkable

				vec3_t simStart;
				VectorCopy(flushGnd.endpos, simStart);

				// Reject immediately if the flush position isn't on the navmesh —
				// the builder would drop this connection anyway (findNearestPoly fails),
				// and we save the cost of 7 Pmove sweeps per false candidate.
				if (!NavMesh_IsPointOnMesh(simStart)) { floorFiltered++; continue; }

				// faceYaw = probeYaw (bot faces the wall = probe direction)
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

				// Verify the landing position is reachable on the navmesh too — physics
				// can deposit the bot on geometry too narrow or too small to be included in
				// the navmesh (thin ledges, clip-only surfaces, etc.).  The builder would
				// also drop such connections, so discard them here.
				if (!NavMesh_IsPointOnMesh(landPos)) continue;

				passCount++;

				// Deduplicate: skip if we already have a connection within 32u of this start+end.
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
	// Append so multiple local scans accumulate into one file.
	// If the file is new, write a header first.
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
	#define WALLRUN_SCAN_MAX_FLOORS 8
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
			// This ensures walls on lower levels (e.g. the lower floor of ctf4) are
			// sampled — a single downward trace would only find the topmost floor.
			//
			// Step-down math: when a bbox trace lands at endpos[2], the physical
			// floor surface is at endpos[2] + s_playerMins[2] = endpos[2] - 24.
			// To start the next trace entirely below that surface, the bbox top
			// (center + 32) must clear it: center < (endpos[2]-24) - 32 - margin.
			// Using margin=8 gives scanTop = endpos[2] - 64.
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
					// scanTop landed inside a floor brush — the bbox is embedded in the
					// solid mass between two floor levels (which can be arbitrarily thick).
					// Bbox traces from inside solid have inconsistent BSP behaviour, so
					// walk downward with PointContents (point query, not bbox — always
					// unambiguous) until we exit the solid, then set scanTop just below
					// the exit point and retry the bbox trace at that depth.
					// This does NOT consume a floor-level slot (flv-- + continue).
					const float SOLID_STEP  = 32.0f;
					const int   MAX_SOLID_STEPS = 512; // cap: ~16 384 u max slab, sub-ms cost
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
						break; // slab extends to map bottom or hit step cap — done

					// Drop one extra bbox-half so the next trace's maxs[2] doesn't
					// clip back into the slab we just exited.
					scanTop = probeZ - 32.0f;
					flv--; // retry at same floor depth — don't bill this as a floor level
					continue;
				}

				if (groundTr.allsolid)
					break; // degenerate — shouldn't normally occur after startsolid check

				// Advance scanTop for the next floor level regardless of walkability,
				// so non-walkable ledges don't block discovery of floors below them.
				scanTop = groundTr.endpos[2] - 64.0f;

				if (groundTr.plane.normal[2] < 0.7f)
					continue; // steep slope — not walkable, keep looking below

			floorSamples++;
			floorLevel[flv < WALLRUN_SCAN_MAX_FLOORS ? flv : WALLRUN_SCAN_MAX_FLOORS-1]++;

			// Bot stands with feet at groundTr.endpos.
			vec3_t groundPos;
			VectorCopy(groundTr.endpos, groundPos);

			// NavMesh Pre-Filtering: if the floor itself isn't on the navmesh, skip 16 probes!
			vec3_t testGround;
			VectorCopy(groundPos, testGround);
			testGround[2] += 2.0f;
			if (!NavMesh_IsPointOnMesh(testGround)) { floorFiltered++; continue; }

			// Probe horizontally in WALLRUN_SCAN_PROBE_DIRS directions.
			for (int d = 0; d < WALLRUN_SCAN_PROBE_DIRS; d++) {
				float probeYaw = d * (360.0f / WALLRUN_SCAN_PROBE_DIRS);
				vec3_t probeAngles = { 0, probeYaw, 0 };
				vec3_t probeFwd;
				AngleVectors(probeAngles, probeFwd, NULL, NULL);

				// Ray-trace from mid-body height in probe direction.
				// Using a ray (no box) so we hit the wall face precisely.
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
				// The wallrun trigger in bg_pmove requires the player to be adjacent to
				// the wall during the double-jump tick.  Starting the simulation at the
				// raw grid point (which may be far from the wall) fails the trigger check.
				//
				// Instead: step back from the wall hit point by one player half-width (15u)
				// plus 1u clearance along the wall normal to get the flush position in XY,
				// then drop a bbox trace downward to find the actual ground Z.  This is the
				// same position the bot occupies when it presses into the wall in-game.
				//
				// The sidecar uses this same position — it is already ~16u from the wall
				// face, matching the navmesh generator's START_SETBACK convention, and
				// sits comfortably within the builder's 48u XZ snap radius.
				vec3_t flushXY;
				VectorMA(wallTr.endpos, 16.0f, wallTr.plane.normal, flushXY);

				vec3_t flushGroundFrom = { flushXY[0], flushXY[1], flushXY[2] + 64.0f };
				vec3_t flushGroundTo   = { flushXY[0], flushXY[1], flushXY[2] - 128.0f };
				trace_t flushGnd;
				trap->Trace(&flushGnd, flushGroundFrom, (float*)s_playerMins, (float*)s_playerMaxs,
					flushGroundTo, tracePass, MASK_PLAYERSOLID, qfalse, 0, 0);

				if (flushGnd.allsolid || flushGnd.startsolid || flushGnd.fraction >= 1.0f)
					continue; // no solid ground at flush position
				if (flushGnd.plane.normal[2] < 0.7f)
					continue; // slope is too steep — not navmesh-walkable

				vec3_t simStart;
				VectorCopy(flushGnd.endpos, simStart);

				// Reject immediately if the flush position isn't on the navmesh —
				// the builder would drop this connection anyway (findNearestPoly fails),
				// and we save the cost of 7 Pmove sweeps per false candidate.
				if (!NavMesh_IsPointOnMesh(simStart)) { floorFiltered++; continue; }

				// faceYaw = probeYaw (bot faces the wall = probe direction)
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

				// Verify the landing position is reachable on the navmesh too — physics
				// can deposit the bot on geometry too narrow or too small to be included in
				// the navmesh (thin ledges, clip-only surfaces, etc.).  The builder would
				// also drop such connections, so discard them here.
				if (!NavMesh_IsPointOnMesh(landPos)) continue;

				passCount++;

				// Deduplicate: skip if we already have a connection within 32u of this start+end.
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
	// Append so multiple local scans accumulate into one file.
	// If the file is new, write a header first.
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
	// Clamp pitch to 0 so we always shoot horizontally regardless of
	// where the player is looking vertically.
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

	// ---- Report raw hit info ----
	trap->Print("[WR-CHECK] Surface hit at %.1f u | normal (%.2f %.2f %.2f) | contents 0x%X\n",
		hitDist,
		tr.plane.normal[0], tr.plane.normal[1], tr.plane.normal[2],
		tr.surfaceFlags);

	// ---- Verticality check (mirrors bg_pmove's own gate) ----
	// bg_pmove requires the wall normal to be nearly horizontal:
	// it dot-tests the normal against (0,0,1) and rejects if |z| is too large.
	// Empirically, surfaces with |normal.z| > 0.3 rarely wallrun.
	float normalZ = fabsf(tr.plane.normal[2]);
	if (normalZ > 0.3f) {
		trap->Print("[WR-CHECK] FAIL: surface too sloped for wallrun (|normal.z|=%.2f, threshold 0.3).\n",
			normalZ);
		return;
	}
	trap->Print("[WR-CHECK] Verticality OK (|normal.z|=%.2f).\n", normalZ);

	// ---- Wall height trace ----
	// Bot2_SimulateWallrun already checks this internally, but report it here
	// so the developer can see which sub-check is rejecting.
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
	// Note: Bot2_SimulateWallrunEx internally calls Bot2_PrintTelemetry which
	// will also emit [WR-SIM] lines to the telemetry log.
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
		// FALLBACK 2: trigger_push's own target_position.  Reasonable for
		// horizontal toss-style pads (where target_position == apex);
		// degrades gracefully for vertical pads (camera locks straight up,
		// but the body is going straight up anyway, so it doesn't matter).
		else {
			vec3_t padTarget;
			if (GetTriggerPushTargetOrigin(ent, padTarget)) {
				VectorCopy(padTarget, bot2_states[clientNum].tele_predPos);
				lockSrc = "trigger_push target_position";
			}
			// FALLBACK 3: post-jumppad waypoint from the path.  This is the
			// pre-fix behaviour and is still subject to the value-function
			// drift the cache was built to defeat — last-resort only.
			else if (validPath) {
				vec3_t waypointAfter;
				if (NavMesh_GetNextWaypoint(ent->s.number, nextWp, targetOrigin, waypointAfter)) {
					VectorCopy(waypointAfter, bot2_states[clientNum].tele_predPos);
					lockSrc = "navmesh waypoint-after";
				}
				else {
					VectorCopy(nextWp, bot2_states[clientNum].tele_predPos);
					lockSrc = "navmesh nextWp";
				}
			}
			// FALLBACK 4: no path at all — aim at the macro goal.
			else {
				VectorCopy(targetOrigin, bot2_states[clientNum].tele_predPos);
				lockSrc = "macro targetOrigin";
			}
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

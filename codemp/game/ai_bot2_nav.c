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
#define OFFMESH_AVOID_MASK ((1 << OFFMESH_AREA_WALLRUN) | (1 << 10))  // wallrun + elevator (area 10)

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

	if (IsInTriggerPush(ent) && bot2_states[clientNum].state != BOT_STATE_JUMPPAD) {
		bot2_states[clientNum].state = BOT_STATE_JUMPPAD;
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

				// Compute wall-face direction once per staging tick.
				// Find the nearest wall around the offmeshStart position using radial traces.
				vec3_t wallDir = { 0.0f, 0.0f, 0.0f };
				vec3_t bestWallPoint = { 0.0f, 0.0f, 0.0f };
				float bestWallDist = 999999.0f;
				
				vec3_t traceOrigin;
				VectorCopy(ent->client->ps.origin, traceOrigin);
				traceOrigin[2] += 24.0f; // Raise trace above floor to prevent startsolid
				
				int startSolidCount = 0;
				int validWallCount = 0;

				for (int i = 0; i < 360; i += 15) {
					vec3_t dir, end;
					float angle = (float)i * M_PI / 180.0f;
					dir[0] = cos(angle);
					dir[1] = sin(angle);
					dir[2] = 0.0f;
					
					VectorMA(traceOrigin, 128.0f, dir, end); // Trace 128 units out
					
					trace_t tr;
					// Use a POINT trace instead of a box trace. NavMesh points are often close to the wall,
					// so a 30x30 box trace would instantly startsolid against the wall itself.
					trap->Trace(&tr, traceOrigin, NULL, NULL, end, ent->s.number, MASK_SOLID, qfalse, 0, 0);
					
					if (tr.startsolid) {
						startSolidCount++;
					} else if (tr.fraction < 1.0f) {
						// Only consider steep slopes/vertical surfaces as valid walls
						if (fabs(tr.plane.normal[2]) < 0.7f) {
							validWallCount++;
							float dist = tr.fraction * 128.0f;
							// If this is the closest wall we've found, save its normal
							// The direction INTO the wall is the opposite of the wall's normal
							if (dist < bestWallDist) {
								bestWallDist = dist;
								VectorScale(tr.plane.normal, -1.0f, wallDir);
								wallDir[2] = 0.0f; // Ensure it's purely horizontal
								VectorNormalize(wallDir);
								VectorCopy(tr.endpos, bestWallPoint);
								bestWallPoint[2] = ent->client->ps.origin[2]; // Project back to bot's height for pathing
							}
						}
					}
				}
				
				if (level.time - bot2_states[clientNum].diagTimer > 400) {
					Bot2_PrintTelemetry(1, "[%s] WALLRUN TRACE: startSolid=%d validWalls=%d bestDist=%.1f\n",
						ent->client->pers.netname, startSolidCount, validWallCount, bestWallDist);
				}

				float stageYaw;
				if (bestWallDist < 999999.0f) {
					vec3_t wa; vectoangles(wallDir, wa); stageYaw = wa[YAW];
				} else {
					stageYaw = ent->client->ps.viewangles[YAW]; // fallback if no wall found (shouldn't happen near a link)
					VectorCopy(offmeshStart, bestWallPoint);
				}

				// Always force saber in the inner zone — unconditionally set every tick so
				// the combat system (which runs after Bot2_ExecuteMovement returns) cannot
				// override it back to the previous weapon and cause oscillation.
				if (bot2_states[clientNum].savedWeapon == 0 && ent->client->ps.weapon != WP_SABER)
					bot2_states[clientNum].savedWeapon = ent->client->ps.weapon;
				ucmd->weapon = WP_SABER;

				// Face the wall.
				vec3_t stageAngles = { 0.0f, stageYaw, 0.0f };
				Bot2_ApplySmoothing(clientNum, stageAngles, ucmd, qfalse);
				
				// Move directly towards the physical point on the wall nearest to offmeshStart.
				vec3_t toWallPoint;
				VectorSubtract(bestWallPoint, ent->client->ps.origin, toWallPoint);
				toWallPoint[2] = 0.0f;
				float distToWallPoint = VectorLength(toWallPoint);

				float toPointYaw = atan2(toWallPoint[1], toWallPoint[0]) * 180.0f / M_PI;
				float yawDiff = (toPointYaw - stageAngles[YAW]) * (M_PI / 180.0f);
				ucmd->forwardmove = (char)max(-127, min(127, 127.0f * cos(yawDiff)));
				ucmd->rightmove   = (char)max(-127, min(127, 127.0f * -sin(yawDiff)));

				// Once very close to the ideal point, override forwardmove to press straight 
				// into the wall to ensure the flush contact trace passes, while maintaining 
				// rightmove correction to prevent sliding away from the ideal point.
				if (distToWallPoint <= 16.0f) {
					ucmd->forwardmove = 127;
				}
				ucmd->upmove = 0;

				// Check flush: point-trace forward 25 u; wall contact = fraction < 0.9
				// (0.9 allows contact up to 22.5 u — the radial scan finds walls at ~21 u
				// which would fail the old 0.8 threshold of 20 u.)
				vec3_t stageFwd = { 0.0f, 0.0f, 0.0f };
				AngleVectors(stageAngles, stageFwd, NULL, NULL);
				vec3_t stageTraceEnd;
				VectorMA(ent->client->ps.origin, 25.0f, stageFwd, stageTraceEnd);
				trace_t stageTr;
				trap->Trace(&stageTr, ent->client->ps.origin, NULL, NULL, stageTraceEnd,
				            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
				qboolean stageGrounded = (ent->client->ps.groundEntityNum != ENTITYNUM_NONE);
				qboolean stageFlush    = (stageTr.fraction < 0.9f);

				// Determine if we should push off the wall (flip-jump) or just ride it up and mantle.
				// If the offmesh destination is horizontally far away from the start, we are trying to clear a gap.
				// If it's horizontally close (e.g. within 80 units), we just want to go straight up and mantle.
				float dxEnd = offmeshEnd[0] - offmeshStart[0];
				float dyEnd = offmeshEnd[1] - offmeshStart[1];
				float distToEndXY = sqrtf(dxEnd*dxEnd + dyEnd*dyEnd);
				qboolean shouldTopJump = (distToEndXY > 80.0f);

				// --- PMOVE PRE-VALIDATION (only when flush so traces are meaningful) ---
				// Run before activation to confirm the wall is a valid wallrun surface.
				qboolean wallrunValid = qtrue;
				if (stageFlush) {
					// Check 1: wall height — the wall must extend >= 200 u above the flush
					// position to sustain the full ascent.
					vec3_t heightOrg;
					VectorCopy(ent->client->ps.origin, heightOrg);
					heightOrg[2] += 200.0f;
					vec3_t heightEnd;
					VectorMA(heightOrg, 30.0f, stageFwd, heightEnd);
					trace_t heightTr;
					trap->Trace(&heightTr, heightOrg, NULL, NULL, heightEnd,
					            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
					if (heightTr.fraction >= 0.9f) {
						wallrunValid = qfalse;
						Bot2_PrintTelemetry(1, "[%s] WALLRUN PRECHECK FAIL: wall too short (height tr=%.2f)\n",
							ent->client->pers.netname, heightTr.fraction);
					}

					// Check 2: ceiling clearance — bot needs >= 72 u of headroom to jump.
					vec3_t ceilEnd;
					VectorCopy(ent->client->ps.origin, ceilEnd);
					ceilEnd[2] += 72.0f;
					trace_t ceilTr;
					trap->Trace(&ceilTr, ent->client->ps.origin, NULL, NULL, ceilEnd,
					            ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
					if (ceilTr.fraction < 0.9f) {
						wallrunValid = qfalse;
						Bot2_PrintTelemetry(1, "[%s] WALLRUN PRECHECK FAIL: ceiling too low (ceil tr=%.2f)\n",
							ent->client->pers.netname, ceilTr.fraction);
					}
				}

				if (level.time - bot2_states[clientNum].diagTimer > 400) {
					bot2_states[clientNum].diagTimer = level.time;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN STAGING: dist=%.0f flush=%d grnd=%d weapon=%d valid=%d yaw=%.1f\n",
						ent->client->pers.netname, offmeshDistToStart2D,
						(int)stageFlush, (int)stageGrounded, ent->client->ps.weapon,
						(int)wallrunValid, stageYaw);
				}

				// Activate: flush + grounded + saber + pmove validation passed
				if (stageFlush && stageGrounded && ent->client->ps.weapon == WP_SABER && wallrunValid) {
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
				// Always return — staging owns the tick regardless of activation.
				return;
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
				
				// Wait for the bot to reach the true apex of its wallrun before kicking off.
				// velApex fires when vz <= 0 (actually at or past the peak) with a 350ms
				// minimum so it can't trigger during the initial ascent impulse.
				// timerApex is a fallback for walls where vz stays odd (e.g. angled surface).
				qboolean timerApex = (p3Elapsed >= 800);
				qboolean velApex   = (p3Elapsed >= 350 && vzNow <= 0.0f);

				if (bot2_states[clientNum].wallrunTopJump && (timerApex || velApex)) {
					*phase = 5;
					Bot2_PrintTelemetry(1, "[%s] WALLRUN phase 5: kickflip p3+%dms vz=%.0f (%s)\n",
						ent->client->pers.netname, p3Elapsed, vzNow,
						(velApex && !timerApex) ? "vel-apex" : "timer");
				}
				break;
			}
			case 5: // Phase 5: Manual kickflip (PRESS JUMP KEY)
				// We MUST keep looking at the wall while pressing jump so the engine's 
				// forward trace hits the wall and grants the velocity boost.
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
			if (*phase < 6) {
				// Phases 1-5: Always face the wall and press forward to ascend/trigger flip
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

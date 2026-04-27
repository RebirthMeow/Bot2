#ifndef __AI_BOT2_H__
#define __AI_BOT2_H__

// High-level CTF roles assigned dynamically based on team composition
typedef enum { ROLE_OFFENSE, ROLE_CHASE, ROLE_BASE } botRole_t;

// Macro states dictate the bot's current objective (pathing target and behavior)
typedef enum {
	MACRO_FETCH_FLAG, MACRO_RETURN_FLAG, MACRO_HUNT_TRIPMINES, MACRO_CAMP_REGEN,
	MACRO_GET_WEAPON, MACRO_DEFEND_STAND, MACRO_CHASE_THIEF, MACRO_ESCORT_FC, MACRO_SURVIVAL
} macroState_t;

typedef struct bot2_state_s {
	// --- MACRO TACTICS ---
	botRole_t role;
	macroState_t macroState;
	int targetEntNum;
	vec3_t macroTargetOrigin;
	int abilityTimer;       // Throttle force powers
	int chargeTimer;        // Throttle weapon charging (e.g., Bryar)

	// --- MICRO STATE MACHINE ---
	// state values — use BOT_STATE_* constants below
	int state, stateTimer, strafeDir, spawnCooldown, ledgeEvading;
	float targetYaw;

	// --- OFF-MESH CONNECTION TRAVERSAL (States 5-7) ---
	// offmeshType mirrors the OFFMESH_AREA_* value that triggered the state.
	// The landing target is stored in tele_predPos (shared with jump-pad state).
	int offmeshType;

	// --- ELEVATOR STATE (State 4) ---
	int elevatorEntNum;   // entity number of the func_plat being used
	int elevatorPhase;    // 0 = waiting to board, 1 = riding to destination

	// --- JUMPPAD APPROACH CACHE (State 3 prep) ---
	// Captured every tick during BOT_STATE_WALK whenever the path query reports
	// an OFFMESH_AREA_JUMPPAD connection ahead.  The trigger_push's own
	// target_position is often straight overhead (vertical-launch pads), which
	// is useless for in-air steering — the actual upper-ledge landing waypoint
	// is the offmesh connection's END, and that's what the bot must lean toward
	// while airborne.  Locked into tele_predPos at the moment the bot enters
	// the trigger so Detour cannot re-route mid-flight.
	// jumppadCachedTime gates freshness: if the most recent capture is older
	// than JUMPPAD_CACHE_FRESH_MS, the cache is treated as stale (path changed
	// targets, bot wandered off, etc.) and the fallbacks take over.
	vec3_t   jumppadCachedEnd;
	int      jumppadCachedTime;
	qboolean jumppadEndCached;

	// --- WALLRUN STATE (State 7) ---
	// Phase 0: approach wall — face wallrunFaceYaw, forwardmove until flush
	// Phase 1: press jump to leave ground
	// Phase 2: release jump, wait for PMF_JUMP_HELD to clear
	// Phase 3: press jump again — this fires the wallrun animation (BOTH_FORCEWALLRUNFLIP_START)
	// Phase 4: ascending — upmove=0; if wallrunTopJump, transitions to phase 5 at apex
	// Phase 5: kickflip (jump-off only) — hold S (or W facing dest if lockAim) + jump; wait for anim to clear
	// Phase 6: falling to destination — steer toward tele_predPos, wait to land
	//
	// wallrunTopJump = destination is BEHIND the wall face (dot(end-start, wallFwd) < 0) → jump-off
	// wallrunTopJump = false                                                               → mount (ride up)
	int wallrunPhase;
	float    wallrunFaceYaw;     // yaw toward the wall face (XY of offmeshEnd - offmeshStart)
	qboolean wallrunTopJump;     // true = press jump at top to reach far-side destination
	int      wallrunPhase3Time;  // level.time when phase 3 fired (for top-jump timing)
	int      wallrunPhase5Time;  // level.time when phase 5 fired (post-flip)
	int      wallrunRetries;     // number of failed wallrun attempts at this connection
	int      wallrunCooldown;    // level.time of last abort; staging is blocked for 3 s
	int      offmeshExitTime;    // level.time of last offmesh state exit; blocks re-activation for 1.5 s

	// --- ANTI-SNAG ENGINE ---
	vec3_t stuck_pos;
	int   stuck_timer, unstuck_phase, unstuck_phase_timer;
	float stuck_dist_acc;  // XY ground-speed accumulated since last 1-s check (u/s units, not u)

	// --- ROUTING TEST HARNESS ---
	qboolean test_active;
	int test_variation;
	int test_run_idx;
	int test_retries;
	int test_start_time;
	qboolean test_had_flag;
	qboolean test_waiting_for_teleport;
	float test_results[10][5]; // 10 variations, 5 runs each

	// --- MID-AIR TELEMETRY ENGINE ---
	int tele_inAir, tele_jumpSeq, tele_jumpStartTime, tele_midAirTime, tele_predDir, tele_actDir;
	int tele_deadLogged, tele_hasFlag, tele_predAirTime;
	float tele_takeoffSpd, tele_prevSpeed, tele_predDist, tele_groundSlope, tele_rampDot, tele_secant;
	float tele_effDrop, tele_predLandSlope, tele_takeoffYaw;
	vec3_t tele_startPos, tele_predPos, tele_pmovePredPos, tele_midPredPos, tele_prevPos, tele_crossPos;
	qboolean tele_crossedZ, tele_midAirLogged;
	int tele_lastChainFrac;        // fraction index (0-1) that won the last chain eval: 0=boundary, 1=optimal
	int tele_chainFracCounts[2];   // cumulative wins per fraction: {0.0=boundary, 1.0=optimal}

	int diagTimer, jumpRetryTimer; char lastFailReason[128];

	// --- WALLRUN WEAPON SAVE ---
	int savedWeapon;  // weapon held before entering BOT_STATE_WALLRUN; 0 = none saved

	// --- AIMING & HUMANIZATION ---
	vec3_t lastViewAngles;
	int lastAimTime, combatIntentTime, combatAimHoldTime;
	char tele_lastAimString[256];
	int tele_lastFireTime;
	int tele_lastHits;

	// Miss-detection timeout: armed by the fire path, disarmed when a hit
	// registers; the hit-detection block at end of Bot2_Think logs a MISS
	// telemetry line if the deadline expires with no hit.  Lets us track
	// shot outcomes without instrumenting g_missile.c.
	qboolean tele_missCheckPending;
	int tele_missCheckDeadline;
	} bot2_state_t;
extern bot2_state_t bot2_states[MAX_CLIENTS];

// bot2_state_t.state values
#define BOT_STATE_WALK        0   // ground locomotion and running-jump logic
#define BOT_STATE_ESCAPE      1   // dedicated stuck-recovery jump execution
#define BOT_STATE_AIRBORNE    2   // generic airborne / free-strafe
#define BOT_STATE_JUMPPAD     3   // trigger_push / jump pad
#define BOT_STATE_ELEVATOR    4   // func_plat / func_door elevator
#define BOT_STATE_DROP        5   // off-mesh: walk off ledge, no jump needed
#define BOT_STATE_JUMP        6   // off-mesh: running jump across gap
#define BOT_STATE_WALLRUN     7   // off-mesh: wallrun up near-vertical surface

void Bot2_ClearState(int clientNum);
void Bot2_Think(int clientNum, int time);
void Bot2_UpdateManagedBots(void);

// Navigation & Physics Helpers
qboolean SimulatePmoveTrajectory(gentity_t* ent, playerState_t* in_ps, float start_yaw, int strafeDir, float angle_fraction, float max_run_speed, vec3_t out_pmove_land_pos, playerState_t* out_ps, qboolean lockAim, vec3_t aimDir);
qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed, vec3_t targetOrigin, float* out_chain_dist, int* out_chain_frac, qboolean lockAim, vec3_t aimDir);
qboolean Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos, qboolean doTelemetry, float* out_timeOfFlight);
void Bot2_ExecuteMovement(int clientNum, usercmd_t *ucmd, vec3_t targetOrigin, vec3_t aimDir, qboolean lockAim, qboolean wantsSpeed, qboolean hasFallback, vec3_t fallbackOrigin);
void Bot2_WallrunCheck(gentity_t *ent);
void Bot2_ScanWallruns(int gridStep, float radius, int centerClient);
void Bot2_ScanWallrunsHeadless(int gridStep, float radius, int centerClient);
void Bot2_PrintTelemetry(int mode, const char* format, ...);

#endif
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               
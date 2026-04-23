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
	// state: 0 = Walk/Run, 2 = Airborne/Falling, 3 = Trigger Push / Jump Pad
	int state, stateTimer, strafeDir, spawnCooldown, ledgeEvading;
	float targetYaw;

	// --- ANTI-SNAG ENGINE ---
	vec3_t stuck_pos;
	int stuck_timer, unstuck_phase, unstuck_phase_timer;

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

	// --- AIMING & HUMANIZATION ---
	vec3_t lastViewAngles;
	int lastAimTime, combatIntentTime;
	char tele_lastAimString[256];
	int tele_lastFireTime;
	int tele_lastHits;
	} bot2_state_t;
extern bot2_state_t bot2_states[MAX_CLIENTS];

void Bot2_ClearState(int clientNum);
void Bot2_Think(int clientNum, int time);
void Bot2_UpdateManagedBots(void);

// Navigation & Physics Helpers
qboolean SimulatePmoveTrajectory(gentity_t* ent, playerState_t* in_ps, float start_yaw, int strafeDir, float angle_fraction, float max_run_speed, vec3_t out_pmove_land_pos, playerState_t* out_ps, qboolean lockAim, vec3_t aimDir);
qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed, vec3_t targetOrigin, float* out_chain_dist, int* out_chain_frac, qboolean lockAim, vec3_t aimDir);
qboolean Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos, qboolean doTelemetry);
void Bot2_ExecuteMovement(int clientNum, usercmd_t *ucmd, vec3_t targetOrigin, vec3_t aimDir, qboolean lockAim, qboolean wantsSpeed, qboolean hasFallback, vec3_t fallbackOrigin);
void Bot2_PrintTelemetry(int mode, const char* format, ...);

#endif
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               
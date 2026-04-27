// ==============================================================================
// ai_bot2_internal.h - Private header shared between ai_bot2_*.c source files.
//
// Public API lives in ai_bot2.h.  This header exposes helpers that need to be
// callable across the bot2 source files (pmove, combat, wallrun, movement)
// without polluting the public bot2 surface.  Don't include this from outside
// the ai_bot2_* TU set.
// ==============================================================================
#ifndef __AI_BOT2_INTERNAL_H__
#define __AI_BOT2_INTERNAL_H__

#include "ai_bot2.h"

// --- Geometry & floor probing (ai_bot2_movement.c) ---
qboolean CheckForTriggerHurt(gentity_t* ent, vec3_t start, vec3_t end, vec3_t mins, vec3_t maxs);
float    TraceFloorScore(gentity_t* ent, vec3_t start, float yaw, float dist, qboolean* hitWall);
qboolean GetSafeEscapeYaw(gentity_t* ent, float danger_yaw, float* safe_yaw);
int      EvaluateStrafeDir(gentity_t* ent, float base_target_yaw);
qboolean IsInTriggerPush(gentity_t* ent);

// --- Phantom Pmove syscall wrappers (ai_bot2_pmove.c) ---
// Used by SimulatePmoveTrajectory and the wallrun simulators; see definition
// for water-splash + landing-dust suppression behaviour.
void Bot2_PMTrace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentMask);
int  Bot2_PMPointContents(const vec3_t point, int passEntityNum);
void BotBreadcrumb(const char* format, ...);

// --- View smoothing (ai_bot2_combat.c) ---
// Single source of truth for clamped angle blending used by both the movement
// state machine and combat aiming.
void Bot2_ApplySmoothing(int clientNum, vec3_t desiredAngles, usercmd_t* ucmd, qboolean lockAim);

// --- Wallrun simulator (ai_bot2_wallrun.c) ---
// Currently only consumed inside ai_bot2_wallrun.c; declared here so external
// callers (e.g. a future state-7 pre-check, or a developer command) can use
// the same code path the WallrunCheck diagnostic does without poking at the
// public ai_bot2.h surface.
qboolean Bot2_SimulateWallrun(gentity_t *ent, float faceYaw);
qboolean Bot2_SimulateWallrunEx(gentity_t *ent, float faceYaw, vec3_t out_landPos);

#endif

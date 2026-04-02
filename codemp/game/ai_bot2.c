#include "g_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

/*
==============
Bot2_Think
==============
*/
void Bot2_Think(int clientNum, int time) {
	gentity_t *ent = &g_entities[clientNum];
	usercmd_t ucmd;
	char serverCmd[1024];

	// 1. UNCONDITIONAL MAILBOX DRAIN
	// We MUST read these every frame to acknowledge reliable server commands and prevent overflows.
	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {
		// Read and discard.
	}

	// 2. UNCONDITIONAL HEARTBEAT
	// Prevent inactivity timeout and sync time.
	ent->client->inactivityTime = level.time + 1000000;

	// 3. STATE BRANCHING
	if (ent->health <= 0) {
		// DEAD: Just spam attack to respawn and clear movement
		memset(&ucmd, 0, sizeof(ucmd));
		ucmd.serverTime = time;
		ucmd.buttons = BUTTON_ATTACK;
		// Maintain current look direction
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
	} else {
		// ALIVE: Handle navigation and steering
		ucmd = botstates[clientNum]->lastucmd;
		ucmd.serverTime = time;
		ucmd.buttons |= BUTTON_ATTACK;

		// 0. Physics Settle
		if (level.time - ent->client->pers.enterTime < 2000) {
			ucmd.forwardmove = 0;
			ucmd.rightmove = 0;
			ucmd.upmove = 0;
			ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
			ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		} else if (level.time % 100 <= 10) { // Thinking Throttle (Every 100ms)
			vec3_t targetOrigin = { 0,0,0 };
			qboolean hasTarget = qfalse;
			vec3_t nextWp = { 0,0,0 };

			// 1. CTF Target Selection
			if (g_gametype.integer == GT_CTF) {
				int botTeam = ent->client->sess.sessionTeam;
				int enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
				char *targetClass = (ent->client->ps.powerups[enemyFlagItem]) ?
					((botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag") :
					((botTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag");

				gentity_t *flagEnt = G_Find(NULL, FOFS(classname), targetClass);
				if (flagEnt) {
					VectorCopy(flagEnt->s.origin, targetOrigin);
					hasTarget = qtrue;
				}
			}

			// 2. Fallback Target Selection
			if (!hasTarget) {
				gentity_t *player = &g_entities[0];
				if (player && player->inuse && player->client && player != ent) {
					VectorCopy(player->client->ps.origin, targetOrigin);
					hasTarget = qtrue;
				}
			}

			// 3. Validation & Navigation
			if (hasTarget && VectorLength(targetOrigin) > 1.0f) {
				if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)targetOrigin, (float*)nextWp)) {
					vec3_t dir, angles;
					VectorSubtract(nextWp, ent->client->ps.origin, dir);
					if (VectorLength(dir) > 0.1f) {
						vectoangles(dir, angles);
						ucmd.angles[YAW] = ANGLE2SHORT(angles[YAW]) - ent->client->ps.delta_angles[YAW];
						ucmd.angles[PITCH] = ANGLE2SHORT(angles[PITCH]) - ent->client->ps.delta_angles[PITCH];
						ucmd.angles[ROLL] = ANGLE2SHORT(angles[ROLL]) - ent->client->ps.delta_angles[ROLL];
						ucmd.forwardmove = 127;
					} else {
						ucmd.forwardmove = 0;
					}
				} else {
					ucmd.forwardmove = 0;
					ucmd.rightmove = 0;
				}
			} else {
				ucmd.forwardmove = 0;
				ucmd.rightmove = 0;
			}
		}
	}

	// 4. UNCONDITIONAL SEND
	botstates[clientNum]->lastucmd = ucmd;
	trap->BotUserCommand(ent->s.number, &ucmd);
}

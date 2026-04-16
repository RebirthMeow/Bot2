// ==============================================================================
// ai_bot2.c - Advanced CTF Bot Engine for Jedi Academy
// Features: Dynamic role switching, predictive jump physics, anti-snag pathing,
// predictive leading for projectiles, and granular mid-air telemetry logging.
// ==============================================================================

#include "g_local.h"
#include "bg_local.h" // Requires Pmove definitions for engine constants
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

extern void G_Kill(gentity_t* ent);

// Human-readable names for telemetry prints
static const char* macroNames[] = {
	"FETCH_FLAG", "RETURN_FLAG", "HUNT_TRIPMINES", "CAMP_REGEN", "GET_WEAPON",
	"DEFEND_STAND", "CHASE_THIEF", "ESCORT_FC", "SURVIVAL"
};

// ==============================================================================
// State Machine Tracker
// ==============================================================================
bot2_state_t bot2_states[MAX_CLIENTS] = { 0 };

// ==============================================================================
// Environment & Entity Helpers
// ==============================================================================

// Assigns Offense, Chase, or Base roles evenly across the team
static botRole_t GetBotRole(int myClientNum, int myTeam) {
	int myRank = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == myTeam) {
			if (i == myClientNum) break;
			myRank++;
		}
	}
	int mod = myRank % 4;
	return (mod == 1) ? ROLE_CHASE : ((mod == 3) ? ROLE_BASE : ROLE_OFFENSE);
}

// Scans for the nearest item matching an array of classnames
static gentity_t* GetNearestItem(vec3_t pos, const char** classnames, int numClasses, float maxDist) {
	gentity_t* bestItem = NULL; float bestDist = maxDist;
	for (int i = 0; i < numClasses; i++) {
		gentity_t* found = NULL;
		while ((found = G_Find(found, FOFS(classname), classnames[i])) != NULL) {
			if (found->r.linked && Distance(pos, found->s.origin) < bestDist) {
				bestDist = Distance(pos, found->s.origin); bestItem = found;
			}
		}
	}
	return bestItem;
}

// Scans for the nearest living enemy player
static gentity_t* GetNearestEnemy(vec3_t pos, int myTeam, float maxDist) {
	gentity_t* bestEnemy = NULL; float bestDist = maxDist;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		gentity_t* ent = &g_entities[i];
		if (ent->inuse && ent->client && ent->health > 0 && ent->client->sess.sessionTeam != myTeam && ent->client->sess.sessionTeam != TEAM_SPECTATOR) {
			float d = Distance(pos, ent->client->ps.origin);
			if (d < bestDist) { bestDist = d; bestEnemy = ent; }
		}
	}
	return bestEnemy;
}

// Maps engine weapon enum to ammo array index
static int Bot2_GetAmmo(gentity_t* ent, int weapon) {
	switch (weapon) {
	case WP_BLASTER: case WP_BRYAR_PISTOL: return ent->client->ps.ammo[2];
	case WP_DISRUPTOR: case WP_BOWCASTER: case WP_DEMP2: return ent->client->ps.ammo[3];
	case WP_REPEATER: case WP_FLECHETTE: case WP_CONCUSSION: return ent->client->ps.ammo[4];
	case WP_ROCKET_LAUNCHER: return ent->client->ps.ammo[5];
	case WP_TRIP_MINE: return ent->client->ps.ammo[8];
	}
	return 0;
}

// ==============================================================================
// Main Bot Think Execution Engine
// ==============================================================================
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum]; usercmd_t ucmd; char serverCmd[1024];

	if (!ent || !ent->inuse || !ent->client || !botstates[clientNum]) return;
	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {}
	ent->client->inactivityTime = level.time + 1000000;

	// --- FATALITIES & RECOVERY ---
	if (ent->health <= 0) {
		bot2_states[clientNum].spawnCooldown = level.time; memset(&ucmd, 0, sizeof(ucmd)); ucmd.serverTime = time; ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		if (!bot2_states[clientNum].tele_deadLogged) {
			trap->Print("[%s] STATE Transition: Bot Killed / Respawning.\n", ent->client->pers.netname);
			bot2_states[clientNum].tele_deadLogged = 1; bot2_states[clientNum].tele_inAir = 0; bot2_states[clientNum].tele_jumpSeq = 0;
			bot2_states[clientNum].state = 0; bot2_states[clientNum].tele_hasFlag = 0; bot2_states[clientNum].stuck_timer = level.time;
			VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		}

		if (ent->client->ps.velocity[2] < -300.0f || ent->client->ps.fallingToDeath) { ent->client->respawnTime = level.time; ent->client->ps.pm_time = 0; }
		botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd); return;
	}

	bot2_states[clientNum].tele_deadLogged = 0; if (ent->client->ps.fallingToDeath) G_Kill(ent);

	// --- ROLE TAGGING ---
	bot2_states[clientNum].role = GetBotRole(clientNum, ent->client->sess.sessionTeam);
	const char* roleTags[] = { "[OFFENSE]", "[CHASE]", "[BASE]" }; char userinfo[MAX_INFO_STRING];
	trap->GetUserinfo(clientNum, userinfo, sizeof(userinfo)); char* name = Info_ValueForKey(userinfo, "name");

	if (name && !strstr(name, roleTags[bot2_states[clientNum].role]) && level.time - bot2_states[clientNum].abilityTimer > 2000) {
		char newName[MAX_NETNAME], cleanName[MAX_NETNAME]; Q_strncpyz(cleanName, name, sizeof(cleanName));
		for (int i = 0; i < 3; i++) { char* tag = strstr(cleanName, roleTags[i]); if (tag) *tag = '\0'; }
		Com_sprintf(newName, sizeof(newName), "%s%s", cleanName, roleTags[bot2_states[clientNum].role]);
		Info_SetValueForKey(userinfo, "name", newName); trap->SetUserinfo(clientNum, userinfo);
		trap->Print("[%s] Assigned New Role: %s\n", ent->client->pers.netname, newName);
	}

	// --- SYSTEM PRE-PROCESSING ---
	float max_run_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
	float vel_x = ent->client->ps.velocity[0], vel_y = ent->client->ps.velocity[1], vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
	float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));

	ucmd = botstates[clientNum]->lastucmd; ucmd.serverTime = time; ucmd.buttons = 0;

	if (level.time - ent->client->pers.enterTime < 200) {
		bot2_states[clientNum].spawnCooldown = bot2_states[clientNum].stuck_timer = level.time; VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		ucmd.forwardmove = ucmd.rightmove = ucmd.upmove = 0; ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd); return;
	}

	ucmd.weapon = WP_BRYAR_PISTOL; ent->client->ps.stats[STAT_WEAPONS] |= (1 << WP_BRYAR_PISTOL); if (ent->client->ps.ammo[2] < 10) ent->client->ps.ammo[2] = 100;
	if (bot2_states[clientNum].role != ROLE_BASE) {
		const int wpn_prio[] = { WP_ROCKET_LAUNCHER, WP_FLECHETTE, WP_REPEATER, WP_CONCUSSION, WP_BOWCASTER, WP_BLASTER, WP_DEMP2 };
		for (int i = 0; i < 7; i++) {
			if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << wpn_prio[i])) && Bot2_GetAmmo(ent, wpn_prio[i]) > 0) { ucmd.weapon = wpn_prio[i]; break; }
		}
	}

	int botTeam = ent->client->sess.sessionTeam, enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
	int hasFlag = (ent->client->ps.powerups[enemyFlagItem] != 0) ? 1 : 0;
	if (bot2_states[clientNum].tele_hasFlag && !hasFlag) { bot2_states[clientNum].tele_hasFlag = 0; G_Kill(ent); return; }
	bot2_states[clientNum].tele_hasFlag = hasFlag;

	// --- MACRO TACTICS ENGINE ---
	macroState_t oldMacro = bot2_states[clientNum].macroState;
	gentity_t* redFlag = G_Find(NULL, FOFS(classname), "team_CTF_redflag"), * blueFlag = G_Find(NULL, FOFS(classname), "team_CTF_blueflag");
	gentity_t* myFlag = (botTeam == TEAM_RED) ? redFlag : blueFlag, * enemyFlag = (botTeam == TEAM_RED) ? blueFlag : redFlag;

	// Calculate contextual distance metrics globally for state transitions
	float totalFlagDist = (myFlag && enemyFlag) ? Distance(myFlag->s.origin, enemyFlag->s.origin) : 5000.0f;
	float baseRadius = totalFlagDist * 0.40f;

	gentity_t* droppedMyFlag = NULL, * fSearch = NULL;
	while ((fSearch = G_Find(fSearch, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag")) != NULL) {
		if ((fSearch->flags & FL_DROPPED_ITEM) && fSearch->r.linked) { droppedMyFlag = fSearch; break; }
	}

	qboolean enemyHasOurFlag = qfalse;
	for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { enemyHasOurFlag = qtrue; break; }

	qboolean myFlagAtBase = (myFlag && myFlag->r.linked && (myFlag->flags & FL_DROPPED_ITEM) == 0 && myFlag->parent == NULL && !enemyHasOurFlag && !droppedMyFlag);
	qboolean enemyFlagAtBase = (enemyFlag && enemyFlag->r.linked && (enemyFlag->flags & FL_DROPPED_ITEM) == 0 && enemyFlag->parent == NULL);

	if (hasFlag) {
		// Offense Survival Trigger: We have the flag, we are in our base radius, but our flag is missing. Turtle and evade.
		if (bot2_states[clientNum].role == ROLE_OFFENSE && !myFlagAtBase && myFlag && Distance(ent->client->ps.origin, myFlag->s.origin) < baseRadius) {
			bot2_states[clientNum].macroState = MACRO_SURVIVAL;
		}
		else {
			bot2_states[clientNum].macroState = MACRO_RETURN_FLAG;
		}
	}
	else if (bot2_states[clientNum].role == ROLE_OFFENSE) bot2_states[clientNum].macroState = enemyFlagAtBase ? MACRO_FETCH_FLAG : MACRO_CAMP_REGEN;
	else if (bot2_states[clientNum].role == ROLE_CHASE) bot2_states[clientNum].macroState = !myFlagAtBase ? MACRO_CHASE_THIEF : (!(ent->client->ps.stats[STAT_WEAPONS] & ((1 << WP_ROCKET_LAUNCHER) | (1 << WP_FLECHETTE) | (1 << WP_REPEATER))) ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND);
	else {
		gentity_t* alliedFC = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == botTeam && g_entities[i].client->ps.powerups[enemyFlagItem]) { alliedFC = &g_entities[i]; break; }

		// Base player escorts using the dynamic 40% threshold instead of a hardcoded 1500 units
		if (alliedFC && myFlag && Distance(alliedFC->client->ps.origin, myFlag->s.origin) < baseRadius) { bot2_states[clientNum].macroState = MACRO_ESCORT_FC; bot2_states[clientNum].targetEntNum = alliedFC->s.number; }
		else bot2_states[clientNum].macroState = !(ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND;
	}

	if (bot2_states[clientNum].macroState != oldMacro) trap->Print("[%s] Mode changed to: %s\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);

	if (bot2_states[clientNum].macroState == MACRO_FETCH_FLAG && enemyFlag && Distance(ent->client->ps.origin, enemyFlag->s.origin) < 800.0f) {
		gentity_t* mine = NULL;
		while ((mine = G_Find(mine, FOFS(classname), "tripmine")) != NULL) {
			if (mine->r.linked && mine->s.otherEntityNum2 != botTeam && Distance(mine->s.origin, enemyFlag->s.origin) < 500.0f) {
				bot2_states[clientNum].macroState = MACRO_HUNT_TRIPMINES; bot2_states[clientNum].targetEntNum = mine->s.number;
				trap->Print("[%s | %s] Trip Mine spotted! Clearing.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
				break;
			}
		}
	}

	vec3_t targetOrigin = { 0,0,0 }, aimDir;
	qboolean lockAim = qfalse, wantsToShoot = qfalse, isShootingMine = qfalse;
	gentity_t* combatTarget = NULL;
	int currentMineCount = 0;

	if (myFlag) { gentity_t* m = NULL; while ((m = G_Find(m, FOFS(classname), "tripmine")) != NULL) if (Distance(m->s.origin, myFlag->s.origin) < 200.0f) currentMineCount++; }

	switch (bot2_states[clientNum].macroState) {
	case MACRO_FETCH_FLAG: if (enemyFlag) VectorCopy(enemyFlag->s.origin, targetOrigin); break;
	case MACRO_RETURN_FLAG: if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin); break;
	case MACRO_DEFEND_STAND: if (myFlag) {
		float pRad = (bot2_states[clientNum].role == ROLE_CHASE) ? 400.0f : ((bot2_states[clientNum].role == ROLE_BASE && currentMineCount > 0) ? 250.0f : 0.0f);
		if (pRad > 0.0f) { float pa = (level.time / 1500.0f) + (clientNum * 0.5f); targetOrigin[0] = myFlag->s.origin[0] + cos(pa) * pRad; targetOrigin[1] = myFlag->s.origin[1] + sin(pa) * pRad; targetOrigin[2] = myFlag->s.origin[2]; }
		else VectorCopy(myFlag->s.origin, targetOrigin);
	} break;
	case MACRO_CHASE_THIEF: {
		gentity_t* thief = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { thief = &g_entities[i]; break; }

		// Tactical Self-Kill to refresh Force Pool / Speed for the chase
		if (thief && ent->client->ps.fd.forcePower < 20 && !(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) {
			trap->Print("[%s] Tactical self-kill to restore Force Pool for Chase.\n", ent->client->pers.netname);
			G_Kill(ent);
			return;
		}

		if (thief) {
			VectorCopy(thief->client->ps.origin, targetOrigin);
			combatTarget = thief; // Target acquired, aiming decoupled until trigger pull
		}
		else if (droppedMyFlag) {
			VectorCopy(droppedMyFlag->s.origin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Thief dead, fetching DROPPED flag.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
		else if (enemyFlag) {
			VectorCopy(enemyFlag->s.origin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Target lost, intercepting at enemy base.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
	} break;
	case MACRO_ESCORT_FC: if (bot2_states[clientNum].targetEntNum >= 0) VectorCopy(g_entities[bot2_states[clientNum].targetEntNum].client->ps.origin, targetOrigin); break;
	case MACRO_GET_WEAPON: {
		const char* heavy[] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater" }, * trips[] = { "weapon_trip_mine" };
		gentity_t* item = GetNearestItem(ent->client->ps.origin, (bot2_states[clientNum].role == ROLE_CHASE) ? heavy : trips, (bot2_states[clientNum].role == ROLE_CHASE) ? 3 : 1, 99999.0f);
		if (item) VectorCopy(item->s.origin, targetOrigin);
	} break;
	case MACRO_CAMP_REGEN: {
		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f); // TWEAK: Tighter Loiter range
		if (enemy) {
			VectorCopy(enemy->client->ps.origin, targetOrigin);
			combatTarget = enemy;
		}
		else {
			const char* allW[] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater", "weapon_bowcaster", "weapon_blaster", "weapon_disruptor", "weapon_demp2", "weapon_concussion_rifle" };
			gentity_t* wpn = GetNearestItem(ent->client->ps.origin, allW, 8, 2000.0f);
			if (wpn) {
				VectorCopy(wpn->s.origin, targetOrigin);
			}
			else if (enemyFlag) {
				float pa = (level.time / 1500.0f) + (clientNum * 0.5f);
				targetOrigin[0] = enemyFlag->s.origin[0] + cos(pa) * 450.0f;
				targetOrigin[1] = enemyFlag->s.origin[1] + sin(pa) * 450.0f;
				targetOrigin[2] = enemyFlag->s.origin[2];
			}
		}
	} break;
	case MACRO_SURVIVAL: {
		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1200.0f); // TWEAK: Adjusted evasion scan
		if (myFlag && enemyFlag) {
			if (enemy) {
				vec3_t bestEvasionSpot;
				float bestDistance = -1.0f;
				qboolean foundValidSpot = qfalse;

				for (int i = 0; i < 8; i++) {
					float angle = (i * 45.0f) * (M_PI / 180.0f);
					vec3_t testPos;

					testPos[0] = myFlag->s.origin[0] + cos(angle) * baseRadius;
					testPos[1] = myFlag->s.origin[1] + sin(angle) * baseRadius;
					testPos[2] = myFlag->s.origin[2];

					vec3_t floorTraceEnd;
					VectorCopy(testPos, floorTraceEnd);
					floorTraceEnd[2] -= 512.0f;

					trace_t floorTr;
					trap->Trace(&floorTr, testPos, NULL, NULL, floorTraceEnd, ENTITYNUM_NONE, MASK_SOLID, qfalse, 0, 0);
					VectorCopy(floorTr.endpos, testPos);
					testPos[2] += 24.0f;

					trace_t solidTr;
					vec3_t mins = { -15, -15, -24 };
					vec3_t maxs = { 15, 15, 32 };
					trap->Trace(&solidTr, testPos, mins, maxs, testPos, ENTITYNUM_NONE, MASK_PLAYERSOLID, qfalse, 0, 0);

					qboolean isOnMesh = NavMesh_IsPointOnMesh(testPos);
					qboolean isGeometryClear = (!solidTr.startsolid && !solidTr.allsolid);

					if (isGeometryClear && isOnMesh) {
						float distFromEnemy = Distance(testPos, enemy->client->ps.origin);
						if (distFromEnemy > bestDistance) {
							bestDistance = distFromEnemy;
							VectorCopy(testPos, bestEvasionSpot);
							foundValidSpot = qtrue;
						}
					}
				}

				if (foundValidSpot) {
					VectorCopy(bestEvasionSpot, targetOrigin);
					if (level.time % 1000 < 50) trap->Print("[%s] FC Turtling - evading to valid node %.0f units from enemy.\n", ent->client->pers.netname, bestDistance);
				}
				else {
					VectorCopy(myFlag->s.origin, targetOrigin);
				}
			}
			else {
				VectorCopy(myFlag->s.origin, targetOrigin);
			}
		}
	} break;
	case MACRO_HUNT_TRIPMINES: break; // Handled below in targeting block
	}

	// --- TARGETING & WEAPON FIRE LOGIC ---
	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		gentity_t* mine = (bot2_states[clientNum].targetEntNum >= MAX_CLIENTS) ? &g_entities[bot2_states[clientNum].targetEntNum] : NULL;
		if (mine && mine->inuse && mine->r.linked) {
			isShootingMine = qtrue; lockAim = wantsToShoot = qtrue;
			VectorSubtract(mine->s.origin, ent->client->ps.origin, aimDir);
			ucmd.buttons |= BUTTON_ATTACK;
		}
		else bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
	}
	else if (bot2_states[clientNum].macroState == MACRO_DEFEND_STAND && bot2_states[clientNum].role == ROLE_BASE && (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) && Bot2_GetAmmo(ent, WP_TRIP_MINE) > 0 && currentMineCount < 4 && !GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f)) {
		// Plant Tripmine logic
		ucmd.weapon = WP_TRIP_MINE; lockAim = wantsToShoot = qtrue; ucmd.buttons |= BUTTON_ALT_ATTACK;
		VectorCopy(myFlag->s.origin, aimDir); aimDir[2] -= 32.0f;
		float ao = currentMineCount * 90.0f * (M_PI / 180.0f); aimDir[0] += cos(ao) * 64.0f; aimDir[1] += sin(ao) * 64.0f;
		VectorSubtract(aimDir, ent->client->ps.origin, aimDir);
	}
	else {
		// General combat scanning
		if (!combatTarget && !(bot2_states[clientNum].role == ROLE_OFFENSE && bot2_states[clientNum].macroState != MACRO_CAMP_REGEN)) {
			combatTarget = GetNearestEnemy(ent->client->ps.origin, botTeam, 1200.0f); // TWEAK: Tighter global engagement range
		}

		if (combatTarget) {
			vec3_t myEye = { ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2] + ent->client->ps.viewheight };
			trace_t tr; trap->Trace(&tr, myEye, NULL, NULL, combatTarget->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			if (tr.fraction == 1.0f || tr.entityNum == combatTarget->s.number) wantsToShoot = qtrue;
		}

		if (wantsToShoot) {
			qboolean triggerPulled = qfalse;

			// Handle charge-ups and fire modes
			if (ucmd.weapon == WP_BRYAR_PISTOL) {
				if (level.time - bot2_states[clientNum].chargeTimer < 1500) { ucmd.buttons |= BUTTON_ALT_ATTACK; } // Charging (Silent aim)
				else if (level.time - bot2_states[clientNum].chargeTimer >= 1600) { bot2_states[clientNum].chargeTimer = level.time; triggerPulled = qtrue; }
			}
			else if (ucmd.weapon == WP_REPEATER) {
				if (Bot2_GetAmmo(ent, WP_REPEATER) >= 30) ucmd.buttons |= BUTTON_ALT_ATTACK; else ucmd.buttons |= BUTTON_ATTACK;
				triggerPulled = qtrue;
			}
			else {
				if (ucmd.weapon == WP_FLECHETTE) ucmd.buttons |= BUTTON_ALT_ATTACK; else ucmd.buttons |= BUTTON_ATTACK;
				triggerPulled = qtrue;
			}

			// TWEAK: Only SNAP aim on the exact frame the bot fires the weapon
			if (triggerPulled && combatTarget) {
				vec3_t myEye = { ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2] + ent->client->ps.viewheight };
				vec3_t lead; Bot2_GetLeadOrigin(ent, combatTarget, lead);

				// TWEAK: Splash Weapon Prediction (Aim at ground)
				qboolean isSplash = (ucmd.weapon == WP_ROCKET_LAUNCHER || ucmd.weapon == WP_CONCUSSION || ucmd.weapon == WP_FLECHETTE || (ucmd.weapon == WP_REPEATER && (ucmd.buttons & BUTTON_ALT_ATTACK)));
				if (isSplash) {
					vec3_t traceEnd; VectorCopy(lead, traceEnd); traceEnd[2] -= 1024.0f;
					trace_t floorTr; trap->Trace(&floorTr, lead, NULL, NULL, traceEnd, combatTarget->s.number, MASK_SOLID, qfalse, 0, 0);

					if (floorTr.fraction < 1.0f && (lead[2] - floorTr.endpos[2]) < 250.0f) {
						VectorCopy(floorTr.endpos, lead); // Snap crosshair to the ground beneath them
					}
					else {
						lead[2] -= 24.0f; // Target is high in the air, aim at their feet instead
					}
				}

				lockAim = qtrue;
				VectorSubtract(lead, myEye, aimDir);
			}
		}
		else {
			bot2_states[clientNum].chargeTimer = level.time;
		}
	}

	int currentAttackButtons = (ucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));
	int lastAttackButtons = (botstates[clientNum]->lastucmd.buttons & (BUTTON_ATTACK | BUTTON_ALT_ATTACK));

	if (currentAttackButtons && !lastAttackButtons) {
		if (!(ucmd.weapon == WP_REPEATER || ucmd.weapon == WP_FLECHETTE || ucmd.weapon == WP_BLASTER || ucmd.weapon == WP_BRYAR_PISTOL) && ucmd.weapon != WP_TRIP_MINE) {
			vec3_t forward, end, start; trace_t tr;
			AngleVectors(ent->client->ps.viewangles, forward, NULL, NULL);
			VectorCopy(ent->client->ps.origin, start); start[2] += ent->client->ps.viewheight;
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

	ent->client->ps.fd.forcePowersKnown |= (1 << FP_SPEED) | (1 << FP_ABSORB) | (1 << FP_TEAM_FORCE) | (1 << FP_TEAM_HEAL);
	ent->client->ps.fd.forcePowerLevel[FP_SPEED] = ent->client->ps.fd.forcePowerLevel[FP_ABSORB] = ent->client->ps.fd.forcePowerLevel[FP_TEAM_FORCE] = ent->client->ps.fd.forcePowerLevel[FP_TEAM_HEAL] = 3;

	qboolean wantsSpeed = !(bot2_states[clientNum].macroState == MACRO_CAMP_REGEN || bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES);

	if (ent->client->ps.fd.forcePower >= 50 && !ent->client->ps.fd.forceButtonNeedRelease) {
		if (bot2_states[clientNum].role == ROLE_BASE && ent->client->ps.fd.forcePower >= 75 && (level.time - bot2_states[clientNum].abilityTimer > 5000)) {
			int needsHeal = 0;
			for (int i = 0; i < MAX_CLIENTS; i++) {
				gentity_t* ally = &g_entities[i];
				if (ally->inuse && ally->client && ally->health > 0 && ally->client->sess.sessionTeam == botTeam && i != clientNum) {
					if (Distance(ent->client->ps.origin, ally->client->ps.origin) < 1000.0f && ally->health < 50) {
						needsHeal = 1; break;
					}
				}
			}

			ucmd.forcesel = needsHeal ? FP_TEAM_HEAL : FP_TEAM_FORCE;
			ucmd.buttons |= BUTTON_FORCEPOWER;
			bot2_states[clientNum].abilityTimer = level.time;
		}
		else if ((wantsSpeed && !(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) || (!wantsSpeed && (ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED)))) {
			ucmd.forcesel = FP_SPEED; ucmd.buttons |= BUTTON_FORCEPOWER;
		}
	}

	qboolean hasFallback = qfalse; vec3_t fallbackOrigin;
	if (bot2_states[clientNum].role == ROLE_CHASE && bot2_states[clientNum].macroState == MACRO_CHASE_THIEF && enemyFlag) {
		hasFallback = qtrue; VectorCopy(enemyFlag->s.origin, fallbackOrigin);
	}

	Bot2_ExecuteMovement(clientNum, &ucmd, targetOrigin, aimDir, lockAim, wantsSpeed, hasFallback, fallbackOrigin);

	botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd);
}

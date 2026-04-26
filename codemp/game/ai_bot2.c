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

void Bot2_ClearState(int clientNum) {
	if (clientNum >= 0 && clientNum < MAX_CLIENTS) {
		memset(&bot2_states[clientNum], 0, sizeof(bot2_state_t));
		// targetEntNum=0 after memset is entity 0 (world), which has no client.
		// Explicitly set to -1 so any accidental deref is caught by our guards.
		bot2_states[clientNum].targetEntNum = -1;
		bot2_states[clientNum].lastAimTime = 0;
	}
}

// Scans all connected clients and assigns the Bot2_Think hook to any bot named *_v2
void Bot2_UpdateManagedBots(void) {
	for (int i = 0; i < MAX_CLIENTS; i++) {
		gentity_t* ent = &g_entities[i];
		// Make sure they are a bot and actively connected
		if (ent->inuse && ent->client && (ent->r.svFlags & SVF_BOT)) {
			if (botstates[i] && botstates[i]->customThink != Bot2_Think) {
				char userinfo[MAX_INFO_STRING];
				trap->GetUserinfo(i, userinfo, sizeof(userinfo));
				char* name = Info_ValueForKey(userinfo, "name");
				
				// Identify Bot2 via naming convention or cvar override
				int forceBot2 = trap->Cvar_VariableIntegerValue("bot_forcebot2");
				if (forceBot2 > 0 || (name && strstr(name, "_v2"))) {
					botstates[i]->customThink = Bot2_Think;
					trap->Print("Bot2 AI initialized for %s (Slot %d)\n", name ? name : "Unknown", i);
				}
			}
		}
	}
}

// Telemetry Queue Filter (Bitmask via bot_telemetry CVAR)
// Bit 1 (1) = Movement/Jump, Bit 2 (2) = Combat/Aiming
void Bot2_PrintTelemetry(int mode, const char* format, ...) {
	int cvar_mode = trap->Cvar_VariableIntegerValue("bot_telemetry");
	if (!(cvar_mode & mode)) return;

	va_list argptr;
	char string[1024];

	va_start(argptr, format);
	Q_vsnprintf(string, sizeof(string), format, argptr);
	va_end(argptr);

	trap->Print(string);
}

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
			// Skip items that are currently respawning or otherwise disabled
			if (!found->r.linked || (found->s.eFlags & EF_NODRAW) || (found->s.eFlags & EF_ITEMPLACEHOLDER)) {
				continue;
			}
			
			if (Distance(pos, found->s.origin) < bestDist) {
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

// Builds a prioritized list of weapons and ammo to search for based on current inventory
static int Bot2_PopulateItemSearch(gentity_t* ent, const char** outList, int maxItems, qboolean heavyOnly) {
	int count = 0;
	int weapons = ent->client->ps.stats[STAT_WEAPONS];

	#define ADD_SEARCH(wpn, wClass, aClass, minAmmo) \
		if (count < maxItems) { \
			if (!(weapons & (1 << wpn))) { outList[count++] = wClass; } \
			else if (aClass && Bot2_GetAmmo(ent, wpn) < minAmmo) { outList[count++] = aClass; } \
		}

	// High priority heavy weapons
	ADD_SEARCH(WP_ROCKET_LAUNCHER, "weapon_rocket_launcher", "ammo_rockets", 10);
	ADD_SEARCH(WP_FLECHETTE, "weapon_flechette", "ammo_metallic_bolts", 60);
	ADD_SEARCH(WP_REPEATER, "weapon_repeater", "ammo_metallic_bolts", 90);
	
	if (!heavyOnly) {
		ADD_SEARCH(WP_CONCUSSION, "weapon_concussion_rifle", "ammo_metallic_bolts", 20);
		ADD_SEARCH(WP_BOWCASTER, "weapon_bowcaster", "ammo_powercell", 100);
		ADD_SEARCH(WP_BLASTER, "weapon_blaster", "ammo_blaster", 100);
		ADD_SEARCH(WP_DEMP2, "weapon_demp2", "ammo_powercell", 100);
	}
	
	#undef ADD_SEARCH
	return count;
}

// ==============================================================================
// Main Bot Think Execution Engine
// ==============================================================================
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum]; usercmd_t ucmd; char serverCmd[1024];
	memset(&ucmd, 0, sizeof(ucmd));

	if (!ent || !ent->inuse || !ent->client || !botstates[clientNum]) return;
	if (level.intermissiontime || level.time == 0) return;
	if (ent->client->pers.connected != CON_CONNECTED) return;

	// Defensive: if macroState is out of range the struct was never properly cleared
	// (e.g. bot wasn't inuse during BotAILoadMap on the previous map change).
	// Reset rather than crash on a stale targetEntNum dereference.
	if ((int)bot2_states[clientNum].macroState < 0 || (int)bot2_states[clientNum].macroState >= 9) {
		Bot2_ClearState(clientNum);
	}

	int botTeam = ent->client->sess.sessionTeam, enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
	int hasFlag = (ent->client->ps.powerups[enemyFlagItem] != 0) ? 1 : 0;
	gentity_t* myFlag = G_Find(NULL, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag");

	// --- ROUTING TEST HARNESS (Capture Check - Absolute Priority) ---
	if (bot2_states[clientNum].test_active && bot2_states[clientNum].test_had_flag && !hasFlag) {
		gentity_t* homeFlag = NULL;
		while ((homeFlag = G_Find(homeFlag, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag")) != NULL) {
			if (homeFlag->parent == NULL && (homeFlag->flags & FL_DROPPED_ITEM) == 0) break;
		}
		if (!homeFlag) homeFlag = G_Find(NULL, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag");

		float distToBase = homeFlag ? Distance(ent->client->ps.origin, homeFlag->s.origin) : 999999.0f;
		if (distToBase < 1500.0f) {
			float run_time = (level.time - bot2_states[clientNum].test_start_time) / 1000.0f;
			int v = bot2_states[clientNum].test_variation, r = bot2_states[clientNum].test_run_idx;
			bot2_states[clientNum].test_results[v][r] = run_time;
			trap->Print("TEST RUN SUCCESS! Variation %d, Run %d: %.2f seconds. Resetting Force...\n", v, r, run_time);
			bot2_states[clientNum].test_retries = 0;

			if (++bot2_states[clientNum].test_run_idx >= 2) {
				bot2_states[clientNum].test_run_idx = 0;
				if (++bot2_states[clientNum].test_variation >= 10) {
					trap->Print("========== ROUTING TEST SCOREBOARD ==========\n");
					for (int i = 0; i < 10; i++) {
						float total = 0.0f;
						for (int j = 0; j < 2; j++) total += bot2_states[clientNum].test_results[i][j];
						trap->Print("Variation %d Avg: %.2f sec\n", i, total / 2.0f);
					}
					trap->Print("=============================================\n");
					bot2_states[clientNum].test_active = qfalse;
				}
			}
		} else {
			if (++bot2_states[clientNum].test_retries < 2) {
				trap->Print("TEST RUN FAILED! Bot lost flag %.0f units from base. Retrying (Retry %d/1)...\n", distToBase, bot2_states[clientNum].test_retries);
			} else {
				int v = bot2_states[clientNum].test_variation, r = bot2_states[clientNum].test_run_idx;
				bot2_states[clientNum].test_results[v][r] = 99.0f;
				bot2_states[clientNum].test_retries = 0;
				trap->Print("TEST RUN FAILED TWICE! Recording 99.0s penalty for Var %d, Run %d. Moving on.\n", v, r);

				if (++bot2_states[clientNum].test_run_idx >= 2) {
					bot2_states[clientNum].test_run_idx = 0;
					if (++bot2_states[clientNum].test_variation >= 10) {
						trap->Print("========== ROUTING TEST SCOREBOARD ==========\n");
						for (int i = 0; i < 10; i++) {
							float total = 0.0f;
							for (int j = 0; j < 2; j++) total += bot2_states[clientNum].test_results[i][j];
							trap->Print("Variation %d Avg: %.2f sec\n", i, total / 2.0f);
						}
						trap->Print("=============================================\n");
						bot2_states[clientNum].test_active = qfalse;
					}
				}
			}
		}
		bot2_states[clientNum].test_had_flag = qfalse;
		bot2_states[clientNum].test_waiting_for_teleport = qtrue;
		G_Kill(ent); return;
	}

	if (bot2_states[clientNum].tele_hasFlag && !hasFlag) {
		bot2_states[clientNum].tele_hasFlag = 0;
		if (bot2_states[clientNum].test_active) bot2_states[clientNum].test_waiting_for_teleport = qtrue;
		G_Kill(ent); return;
	}
	// --- ROUTING TEST HARNESS (Flag Pickup Detection) ---
	if (bot2_states[clientNum].test_active && !bot2_states[clientNum].test_had_flag
		&& hasFlag && !bot2_states[clientNum].tele_hasFlag) {
		bot2_states[clientNum].test_had_flag = qtrue;
		bot2_states[clientNum].test_start_time = level.time;
		trap->Print("TEST: Bot %d grabbed enemy flag! Timer started (Var %d, Run %d).\n",
			clientNum, bot2_states[clientNum].test_variation, bot2_states[clientNum].test_run_idx);
	}
	bot2_states[clientNum].tele_hasFlag = hasFlag;

	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {}
	ent->client->inactivityTime = level.time + 1000000;

	// --- FATALITIES & RECOVERY ---
	if (ent->health <= 0) {
		bot2_states[clientNum].spawnCooldown = level.time; memset(&ucmd, 0, sizeof(ucmd)); ucmd.serverTime = time; ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		if (!bot2_states[clientNum].tele_deadLogged) {
			trap->Print("[%s] STATE Transition: Bot Killed / Respawning.\n", ent->client->pers.netname);

			if (bot2_states[clientNum].tele_inAir) {
				vec3_t cPos; VectorCopy(ent->client->ps.origin, cPos);
				float dx = cPos[0] - bot2_states[clientNum].tele_startPos[0], dy = cPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy));
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0], pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));
				float pm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2], aZDrop = bot2_states[clientNum].tele_startPos[2] - cPos[2];

				int rawDir = bot2_states[clientNum].strafeDir;
				int sDir = (rawDir > 0) ? 1 : -1;
				qboolean isHardStrafe = (abs(rawDir) == 2);
				const char* keyStr = isHardStrafe ? (sDir == 1 ? "Just D" : "Just A") : (sDir == 1 ? "W+D" : "W+A");

				trap->Print("[%s] FATALITY %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime, bot2_states[clientNum].tele_takeoffSpd,
					aDist, pMoveDist, aZDrop, pm_ZDrop, cPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], cPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr);
			}

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
	
	// Test override: 1 = Offense, 2 = Chase, 3 = Base
	int forceRole = trap->Cvar_VariableIntegerValue("bot_forcerole");
	if (forceRole > 0 && forceRole <= 3) {
		bot2_states[clientNum].role = forceRole - 1;
	}

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

	// --- ROUTING TEST HARNESS (Teleport to Start Position) ---
	if (bot2_states[clientNum].test_active && bot2_states[clientNum].test_waiting_for_teleport) {
		// Find where the enemy flag lives
		const char* eFlagClass = (botTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag";
		gentity_t* eFlag = G_Find(NULL, FOFS(classname), eFlagClass);
		if (eFlag) {
			// Find nearest medpack to enemy flag as the run start position
			const char* medNames[] = { "item_medpac", "item_medpac_big" };
			gentity_t* medpack = GetNearestItem(eFlag->s.origin, medNames, 2, 3000.0f);
			vec3_t telePos;
			if (medpack) {
				VectorCopy(medpack->s.origin, telePos);
				telePos[2] += 24.0f;
				trap->Print("TEST: Teleporting Bot %d to medpack at (%.0f,%.0f,%.0f) — Var %d, Run %d.\n",
					clientNum, telePos[0], telePos[1], telePos[2],
					bot2_states[clientNum].test_variation, bot2_states[clientNum].test_run_idx);
			} else {
				// Fallback: land just in front of enemy flag
				VectorCopy(eFlag->s.origin, telePos);
				telePos[2] += 48.0f;
				trap->Print("TEST: No medpack found near enemy flag — teleporting Bot %d directly to flag.\n", clientNum);
			}
			// Perform the teleport
			VectorCopy(telePos, ent->client->ps.origin);
			VectorClear(ent->client->ps.velocity);
			ent->client->ps.pm_time = 0;
			trap->LinkEntity(ent);
			// Full health + force pool for a clean benchmark run
			ent->health = ent->client->ps.stats[STAT_HEALTH] = 100;
			ent->client->ps.stats[STAT_ARMOR] = 100;
			ent->client->ps.fd.forcePower = ent->client->ps.fd.forcePowerMax;
		}
		bot2_states[clientNum].test_waiting_for_teleport = qfalse;
		bot2_states[clientNum].test_had_flag = qfalse;
		bot2_states[clientNum].spawnCooldown = level.time;
		bot2_states[clientNum].stuck_timer = level.time;
		VectorCopy(ent->client->ps.origin, bot2_states[clientNum].stuck_pos);
		// Fall through — let normal think run from the new position
	}

	ucmd.weapon = WP_BRYAR_PISTOL; ent->client->ps.stats[STAT_WEAPONS] |= (1 << WP_BRYAR_PISTOL); if (ent->client->ps.ammo[2] < 10) ent->client->ps.ammo[2] = 100;
	if (bot2_states[clientNum].role != ROLE_BASE) {
		const int wpn_prio[] = { WP_ROCKET_LAUNCHER, WP_FLECHETTE, WP_REPEATER, WP_CONCUSSION, WP_BOWCASTER, WP_BLASTER, WP_DEMP2 };
		for (int i = 0; i < 7; i++) {
			if ((ent->client->ps.stats[STAT_WEAPONS] & (1 << wpn_prio[i])) && Bot2_GetAmmo(ent, wpn_prio[i]) > 0) { ucmd.weapon = wpn_prio[i]; break; }
		}
	}

	// Test override: Weapon Lock
	int forceWpn = trap->Cvar_VariableIntegerValue("bot_forceweapon");
	if (forceWpn > 0) {
		ucmd.weapon = forceWpn;
		ent->client->ps.stats[STAT_WEAPONS] |= (1 << forceWpn);
		for (int i = 0; i < 16; i++) ent->client->ps.ammo[i] = 999; // Unlimited ammo for testing
	}

	// --- MACRO TACTICS ENGINE ---
	macroState_t oldMacro = bot2_states[clientNum].macroState;
	gentity_t* redFlag = G_Find(NULL, FOFS(classname), "team_CTF_redflag"), * blueFlag = G_Find(NULL, FOFS(classname), "team_CTF_blueflag");
	myFlag = (botTeam == TEAM_RED) ? redFlag : blueFlag;
	gentity_t* enemyFlag = (botTeam == TEAM_RED) ? blueFlag : redFlag;

	// Calculate contextual distance metrics globally for state transitions
	float totalFlagDist = (myFlag && enemyFlag) ? Distance(myFlag->s.origin, enemyFlag->s.origin) : 5000.0f;
	float baseRadius = totalFlagDist * 0.40f;

	gentity_t* droppedMyFlag = NULL, * droppedEnemyFlag = NULL, * fSearch = NULL;
	while ((fSearch = G_Find(fSearch, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag")) != NULL) {
		if ((fSearch->flags & FL_DROPPED_ITEM) && fSearch->r.linked) { droppedMyFlag = fSearch; break; }
	}
	fSearch = NULL;
	while ((fSearch = G_Find(fSearch, FOFS(classname), (botTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag")) != NULL) {
		if ((fSearch->flags & FL_DROPPED_ITEM) && fSearch->r.linked) { droppedEnemyFlag = fSearch; break; }
	}

	qboolean enemyHasOurFlag = qfalse;
	for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { enemyHasOurFlag = qtrue; break; }

	qboolean weHaveEnemyFlag = qfalse;
	for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam == botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG]) { weHaveEnemyFlag = qtrue; break; }

	qboolean myFlagAtBase = (myFlag && myFlag->r.linked && (myFlag->flags & FL_DROPPED_ITEM) == 0 && myFlag->parent == NULL && !enemyHasOurFlag && !droppedMyFlag);
	qboolean enemyFlagAtBase = (enemyFlag && enemyFlag->r.linked && (enemyFlag->flags & FL_DROPPED_ITEM) == 0 && enemyFlag->parent == NULL && !weHaveEnemyFlag && !droppedEnemyFlag);

	qboolean hasHeavyWeapon = !!(ent->client->ps.stats[STAT_WEAPONS] & ((1 << WP_ROCKET_LAUNCHER) | (1 << WP_FLECHETTE) | (1 << WP_REPEATER) | (1 << WP_CONCUSSION)));
	if (trap->Cvar_VariableIntegerValue("bot_forceweapon") > 0) hasHeavyWeapon = qtrue; // Treat forced weapon as sufficient

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
	else if (bot2_states[clientNum].role == ROLE_CHASE) bot2_states[clientNum].macroState = !myFlagAtBase ? MACRO_CHASE_THIEF : (!hasHeavyWeapon ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND);
	else {
		gentity_t* alliedFC = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].client->sess.sessionTeam == botTeam && g_entities[i].client->ps.powerups[enemyFlagItem]) { alliedFC = &g_entities[i]; break; }

		// Base player escorts using the dynamic 40% threshold instead of a hardcoded 1500 units
		if (alliedFC && myFlag && Distance(alliedFC->client->ps.origin, myFlag->s.origin) < baseRadius) { bot2_states[clientNum].macroState = MACRO_ESCORT_FC; bot2_states[clientNum].targetEntNum = alliedFC->s.number; }
		else bot2_states[clientNum].macroState = (!(ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) || Bot2_GetAmmo(ent, WP_TRIP_MINE) < 4) ? MACRO_GET_WEAPON : MACRO_DEFEND_STAND;
	}

	if (bot2_states[clientNum].macroState != oldMacro) trap->Print("[%s] Mode changed to: %s\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);

	if (bot2_states[clientNum].macroState == MACRO_FETCH_FLAG || bot2_states[clientNum].macroState == MACRO_RETURN_FLAG) {
		ucmd.weapon = WP_BRYAR_PISTOL;
		ent->client->ps.stats[STAT_WEAPONS] |= (1 << WP_BRYAR_PISTOL);
	}

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

	vec3_t targetOrigin = { 0,0,0 }, aimDir, lead;
	qboolean lockAim = qfalse, wantsToShoot = qfalse, isShootingMine = qfalse;
	gentity_t* combatTarget = NULL;
	int currentMineCount = 0;

	if (myFlag) { gentity_t* m = NULL; while ((m = G_Find(m, FOFS(classname), "tripmine")) != NULL) if (Distance(m->s.origin, myFlag->s.origin) < 200.0f) currentMineCount++; }

	switch (bot2_states[clientNum].macroState) {
	case MACRO_FETCH_FLAG: if (enemyFlag) VectorCopy(enemyFlag->s.origin, targetOrigin); break;
	case MACRO_RETURN_FLAG: if (myFlag) VectorCopy(myFlag->s.origin, targetOrigin); break;
	case MACRO_DEFEND_STAND: if (myFlag) {
		qboolean plantingMines = (bot2_states[clientNum].role == ROLE_BASE && (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) && Bot2_GetAmmo(ent, WP_TRIP_MINE) > 0 && currentMineCount < 4);
		float pRad = (bot2_states[clientNum].role == ROLE_CHASE) ? 400.0f : ((bot2_states[clientNum].role == ROLE_BASE && !plantingMines) ? 250.0f : 0.0f);
		if (pRad > 0.0f) { float pa = (level.time / 1500.0f) + (clientNum * 0.5f); targetOrigin[0] = myFlag->s.origin[0] + cos(pa) * pRad; targetOrigin[1] = myFlag->s.origin[1] + sin(pa) * pRad; targetOrigin[2] = myFlag->s.origin[2]; }
		else VectorCopy(myFlag->s.origin, targetOrigin);
	} break;
	case MACRO_CHASE_THIEF: {
		gentity_t* thief = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++) if (g_entities[i].inuse && g_entities[i].client && g_entities[i].health > 0 && g_entities[i].client->sess.sessionTeam != botTeam && g_entities[i].client->ps.powerups[(botTeam == TEAM_RED) ? PW_REDFLAG : PW_BLUEFLAG]) { thief = &g_entities[i]; break; }

		// Tactical Self-Kill to refresh Force Pool / Speed for the chase
		if (thief && ent->client->ps.fd.forcePower < 20 && !(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED))) {
			if (bot2_states[clientNum].tele_inAir) {
				vec3_t cPos; VectorCopy(ent->client->ps.origin, cPos);
				float dx = cPos[0] - bot2_states[clientNum].tele_startPos[0], dy = cPos[1] - bot2_states[clientNum].tele_startPos[1];
				float aDist = sqrt((dx * dx) + (dy * dy));
				float pm_dx = bot2_states[clientNum].tele_pmovePredPos[0] - bot2_states[clientNum].tele_startPos[0], pm_dy = bot2_states[clientNum].tele_pmovePredPos[1] - bot2_states[clientNum].tele_startPos[1];
				float pMoveDist = sqrt((pm_dx * pm_dx) + (pm_dy * pm_dy));
				float pm_ZDrop = bot2_states[clientNum].tele_startPos[2] - bot2_states[clientNum].tele_pmovePredPos[2], aZDrop = bot2_states[clientNum].tele_startPos[2] - cPos[2];

				int rawDir = bot2_states[clientNum].strafeDir;
				int sDir = (rawDir > 0) ? 1 : -1;
				qboolean isHardStrafe = (abs(rawDir) == 2);
				const char* keyStr = isHardStrafe ? (sDir == 1 ? "Just D" : "Just A") : (sDir == 1 ? "W+D" : "W+A");

				trap->Print("[%s] FATALITY (TACTICAL) %d | Act T: %dms | Spd: %.0f | Act D: %.1f (Pred: %.1f) | Act Z: %.1f (Pred: %.1f) | XY Err: %+.1f, %+.1f | Keys: %s\n",
					ent->client->pers.netname, bot2_states[clientNum].tele_jumpSeq, level.time - bot2_states[clientNum].tele_jumpStartTime, bot2_states[clientNum].tele_takeoffSpd,
					aDist, pMoveDist, aZDrop, pm_ZDrop, cPos[0] - bot2_states[clientNum].tele_pmovePredPos[0], cPos[1] - bot2_states[clientNum].tele_pmovePredPos[1],
					keyStr);
			}
			trap->Print("[%s] Tactical self-kill to restore Force Pool for Chase.\n", ent->client->pers.netname);
			G_Kill(ent);
			return;
		}

		if (thief) {
			VectorCopy(thief->client->ps.origin, targetOrigin);
			combatTarget = thief; // Target acquired, aiming decoupled until trigger pull
		}
		else if (droppedMyFlag) {
			VectorCopy(droppedMyFlag->r.currentOrigin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Thief dead, fetching DROPPED flag.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
		else if (enemyFlag) {
			VectorCopy(enemyFlag->s.origin, targetOrigin);
			if (level.time % 2000 < 50) trap->Print("[%s | %s] Target lost, intercepting at enemy base.\n", ent->client->pers.netname, macroNames[bot2_states[clientNum].macroState]);
		}
	} break;
	case MACRO_ESCORT_FC: {
		int tEnt = bot2_states[clientNum].targetEntNum;
		if (tEnt >= 0 && tEnt < MAX_CLIENTS
			&& g_entities[tEnt].inuse && g_entities[tEnt].client
			&& g_entities[tEnt].client->pers.connected == CON_CONNECTED) {
			VectorCopy(g_entities[tEnt].client->ps.origin, targetOrigin);
		} else {
			// FC disconnected or state is stale — fall back to flag grab
			bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
			bot2_states[clientNum].targetEntNum = -1;
			if (enemyFlag) VectorCopy(enemyFlag->s.origin, targetOrigin);
		}
	} break;
	case MACRO_GET_WEAPON: {
		gentity_t* item = NULL;
		if (bot2_states[clientNum].role == ROLE_CHASE) {
			const char* searchList[8];
			int searchCount = Bot2_PopulateItemSearch(ent, searchList, 8, qtrue);
			if (searchCount > 0) {
				item = GetNearestItem(ent->client->ps.origin, searchList, searchCount, 99999.0f);
			}
		} else {
			if (!(ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE))) {
				const char* trips[] = { "weapon_trip_mine" };
				item = GetNearestItem(ent->client->ps.origin, trips, 1, 99999.0f);
			} else {
				const char* tripsAmmo[] = { "ammo_tripmine", "weapon_trip_mine" };
				item = GetNearestItem(ent->client->ps.origin, tripsAmmo, 2, 99999.0f);
			}
		}
		
		if (item) {
			VectorCopy(item->s.origin, targetOrigin);
		} else if (myFlag) {
			VectorCopy(myFlag->s.origin, targetOrigin); // Fallback so we don't path to 0,0,0
		}
	} break;
	case MACRO_CAMP_REGEN: {
		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f);
		if (enemy) {
			combatTarget = enemy; // Engage, but do NOT pursue (targetOrigin is not updated to enemy)
		}
		
		gentity_t* regenItem = NULL;
		
		// 1. Search for Health/Armor if injured
		if (ent->health < ent->client->ps.stats[STAT_MAX_HEALTH] || ent->client->ps.stats[STAT_ARMOR] < 100) {
			const char* regenW[] = { "item_medpac", "item_medpac_big", "item_shield_sm_instant", "item_shield_lrg_instant" };
			regenItem = GetNearestItem(ent->client->ps.origin, regenW, 4, 1000.0f);
		}
		
		// 2. Search for weapons if healthy or no health found
		if (!regenItem) {
			const char* allW[] = { "weapon_rocket_launcher", "weapon_flechette", "weapon_repeater", "weapon_bowcaster", "weapon_blaster", "weapon_disruptor", "weapon_demp2", "weapon_concussion_rifle" };
			regenItem = GetNearestItem(ent->client->ps.origin, allW, 8, 1000.0f);
		}

		if (regenItem) {
			VectorCopy(regenItem->s.origin, targetOrigin);
		}
		else if (enemyFlag) {
			// 3. Dynamic Loiter: sweep a rotating pattern to find a valid NavMesh node near the flag
			vec3_t bestLoiterSpot;
			float bestDist = 999999.0f;
			qboolean foundValidSpot = qfalse;

			float pa = (level.time / 2000.0f) + (clientNum * 1.5f); // Rotating baseline
			
			for (int i = 0; i < 4; i++) {
				float angle = pa + (i * 90.0f) * (M_PI / 180.0f);
				vec3_t testPos;

				testPos[0] = enemyFlag->s.origin[0] + cos(angle) * 350.0f;
				testPos[1] = enemyFlag->s.origin[1] + sin(angle) * 350.0f;
				testPos[2] = enemyFlag->s.origin[2] + 32.0f;

				vec3_t floorTraceEnd;
				VectorCopy(testPos, floorTraceEnd);
				floorTraceEnd[2] -= 512.0f;

				trace_t floorTr;
				trap->Trace(&floorTr, testPos, NULL, NULL, floorTraceEnd, ENTITYNUM_NONE, MASK_SOLID, qfalse, 0, 0);
				VectorCopy(floorTr.endpos, testPos);
				testPos[2] += 24.0f;

				if (NavMesh_IsPointOnMesh(testPos)) {
					float dist = Distance(ent->client->ps.origin, testPos);
					if (dist < bestDist) {
						bestDist = dist;
						VectorCopy(testPos, bestLoiterSpot);
						foundValidSpot = qtrue;
					}
				}
			}
			
			if (foundValidSpot) {
				VectorCopy(bestLoiterSpot, targetOrigin);
			} else {
				VectorCopy(enemyFlag->s.origin, targetOrigin); // Fallback
			}
		}
	} break;
	case MACRO_SURVIVAL: {
		gentity_t* enemy = GetNearestEnemy(ent->client->ps.origin, botTeam, 1200.0f); // TWEAK: Adjusted evasion scan
		if (myFlag && enemyFlag) {
			if (enemy) {
				vec3_t bestEvasionSpot;
				float bestScore = -999999.0f;
				qboolean foundValidSpot = qfalse;

				float checkRadii[] = { 300.0f, 600.0f, 900.0f };

				for (int r = 0; r < 3; r++) {
					for (int i = 0; i < 8; i++) {
						float angle = (i * 45.0f) * (M_PI / 180.0f);
						vec3_t testPos;

						testPos[0] = ent->client->ps.origin[0] + cos(angle) * checkRadii[r];
						testPos[1] = ent->client->ps.origin[1] + sin(angle) * checkRadii[r];
						testPos[2] = ent->client->ps.origin[2] + 32.0f;

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
							float distFromBase = Distance(testPos, myFlag->s.origin);
							float currentEnemyDist = Distance(ent->client->ps.origin, enemy->client->ps.origin);
							
							if (distFromEnemy > currentEnemyDist && distFromBase < baseRadius * 1.5f) {
								float score = distFromEnemy - (distFromBase * 0.5f);
								if (score > bestScore) {
									bestScore = score;
									VectorCopy(testPos, bestEvasionSpot);
									foundValidSpot = qtrue;
								}
							}
						}
					}
				}

				if (foundValidSpot) {
					VectorCopy(bestEvasionSpot, targetOrigin);
					if (level.time % 1000 < 50) trap->Print("[%s] FC Turtling - evading to valid node. Score: %.0f\n", ent->client->pers.netname, bestScore);
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
	}

	// --- TARGETING & WEAPON FIRE LOGIC ---
	if (bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		int mineEnt = bot2_states[clientNum].targetEntNum;
		gentity_t* mine = (mineEnt >= MAX_CLIENTS && mineEnt < MAX_GENTITIES) ? &g_entities[mineEnt] : NULL;
		if (mine && mine->inuse && mine->r.linked) {
			isShootingMine = qtrue; lockAim = wantsToShoot = qtrue;
			VectorSubtract(mine->s.origin, ent->client->ps.origin, aimDir);
			ucmd.buttons |= BUTTON_ATTACK;
		}
		else bot2_states[clientNum].macroState = MACRO_FETCH_FLAG;
	}
	else if (bot2_states[clientNum].macroState == MACRO_DEFEND_STAND && bot2_states[clientNum].role == ROLE_BASE && (ent->client->ps.stats[STAT_WEAPONS] & (1 << WP_TRIP_MINE)) && Bot2_GetAmmo(ent, WP_TRIP_MINE) > 0 && currentMineCount < 4 && !GetNearestEnemy(ent->client->ps.origin, botTeam, 1000.0f)) {
		// Plant Tripmine logic
		if (Distance(ent->client->ps.origin, myFlag->s.origin) < 150.0f) {
			ucmd.weapon = WP_TRIP_MINE; lockAim = wantsToShoot = qtrue; 
			
			// Rate limit firing to give the engine time to spawn the mine and increment currentMineCount
			if (level.time - bot2_states[clientNum].chargeTimer > 1000) {
				ucmd.buttons |= BUTTON_ALT_ATTACK;
				bot2_states[clientNum].chargeTimer = level.time;
			}
			
			// Aim in a cross pattern around the flag stand
			VectorCopy(myFlag->s.origin, aimDir); aimDir[2] -= 32.0f;
			float ao = currentMineCount * 90.0f * (M_PI / 180.0f); aimDir[0] += cos(ao) * 64.0f; aimDir[1] += sin(ao) * 64.0f;
			VectorSubtract(aimDir, ent->client->ps.origin, aimDir);
		}
	}
	else {
		// General combat scanning
		if (!combatTarget && !(bot2_states[clientNum].role == ROLE_OFFENSE && bot2_states[clientNum].macroState != MACRO_CAMP_REGEN)) {
			combatTarget = GetNearestEnemy(ent->client->ps.origin, botTeam, 1200.0f);
		}

		if (combatTarget) {
			vec3_t myEye = { ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2] + ent->client->ps.viewheight };
			trace_t tr; trap->Trace(&tr, myEye, NULL, NULL, combatTarget->client->ps.origin, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
			qboolean hasLOS = (tr.fraction == 1.0f || tr.entityNum == combatTarget->s.number);
			float dist = Distance(ent->client->ps.origin, combatTarget->client->ps.origin);
			qboolean weaponEffective = (dist <= 1000.0f);
			if (ucmd.weapon == WP_SABER && dist > 100.0f) weaponEffective = qfalse;

			// WEAPON SPECIFIC STATE MACHINES
			if (ucmd.weapon == WP_BRYAR_PISTOL) {
				if (weaponEffective) {
					// State 1 (Charging): Apply BUTTON_ALT_ATTACK. Do not alter angles.
					ucmd.buttons |= BUTTON_ALT_ATTACK;
					
					int chargeDuration = 0;
					if (ent->client->ps.weaponstate == WEAPON_CHARGING_ALT) {
						chargeDuration = level.time - ent->client->ps.weaponChargeTime;
					}

					// State 2 (The Flick Window): Take over aiming slightly before release (300ms window)
					if (chargeDuration >= 700 && hasLOS) {
						lockAim = qtrue;
						float tof = 0.0f;
						// Only log telemetry on the exact frame we intend to release
						Bot2_GetLeadOrigin(ent, combatTarget, lead, (chargeDuration >= 1000), &tof);
						VectorSubtract(lead, myEye, aimDir);
						
						if (chargeDuration >= 1000) {
							// Remove the BUTTON_ALT_ATTACK flag to release the shot
							ucmd.buttons &= ~BUTTON_ALT_ATTACK;
							// State 3 (The Hold): Keep aiming at the target until the shot hits
							bot2_states[clientNum].combatAimHoldTime = level.time + (int)(tof * 1000.0f);
						}
					}
				}
			}
			else if (hasLOS && weaponEffective) {
				// Always track intent if LOS is clear
				if (bot2_states[clientNum].combatIntentTime == 0) {
					bot2_states[clientNum].combatIntentTime = level.time;
				}

				// OTHER WEAPONS
				int intentDuration = level.time - bot2_states[clientNum].combatIntentTime;
				if (intentDuration >= 0) { // Start aiming immediately upon intent
					lockAim = qtrue; 
					float tof = 0.0f;
					qboolean synced = Bot2_GetLeadOrigin(ent, combatTarget, lead, (intentDuration >= 300), &tof);
					VectorSubtract(lead, myEye, aimDir);
					
					if (intentDuration >= 300 && synced) { // 300ms pre-fire aiming window
						bot2_states[clientNum].combatAimHoldTime = level.time + (int)(tof * 1000.0f);

						if (ucmd.weapon == WP_REPEATER) {
							if (Bot2_GetAmmo(ent, WP_REPEATER) >= 30) ucmd.buttons |= BUTTON_ALT_ATTACK; else ucmd.buttons |= BUTTON_ATTACK;
						}
						else if (ucmd.weapon == WP_ROCKET_LAUNCHER || ucmd.weapon == WP_CONCUSSION || ucmd.weapon == WP_DEMP2 || ucmd.weapon == WP_BOWCASTER || ucmd.weapon == WP_BLASTER) {
							ucmd.buttons |= BUTTON_ATTACK;
						}
						else if (ucmd.weapon == WP_FLECHETTE) {
							if (Bot2_GetAmmo(ent, WP_FLECHETTE) >= 15) ucmd.buttons |= BUTTON_ALT_ATTACK; else ucmd.buttons |= BUTTON_ATTACK;
						}
						else {
							ucmd.buttons |= BUTTON_ATTACK;
						}
					}
				}
			} else {
				bot2_states[clientNum].combatIntentTime = 0;
			}
			
			// POST-FIRE AIM HOLD
			// If we aren't currently triggering a shot, but recently fired, maintain the lock for smooth follow-through
			if (!lockAim && level.time < bot2_states[clientNum].combatAimHoldTime && hasLOS) {
				lockAim = qtrue;
				Bot2_GetLeadOrigin(ent, combatTarget, lead, qfalse, NULL);
				VectorSubtract(lead, myEye, aimDir);
			}
		} else {
			bot2_states[clientNum].combatIntentTime = 0;
		}
	}

	ent->client->ps.fd.forcePowersKnown |= (1 << FP_LEVITATION) | (1 << FP_PUSH) | (1 << FP_PULL) | (1 << FP_SPEED) | (1 << FP_ABSORB) | (1 << FP_PROTECT) | (1 << FP_TEAM_HEAL) | (1 << FP_SABER_OFFENSE) | (1 << FP_SABER_DEFENSE) | (1 << FP_TEAM_FORCE);
	ent->client->ps.fd.forcePowerLevel[FP_LEVITATION] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_PUSH] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_PULL] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_SPEED] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_ABSORB] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_PROTECT] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_TEAM_HEAL] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_SABER_OFFENSE] = 1;
	ent->client->ps.fd.forcePowerLevel[FP_SABER_DEFENSE] = 3;
	ent->client->ps.fd.forcePowerLevel[FP_TEAM_FORCE] = 3;

	qboolean speedActive = !!(ent->client->ps.fd.forcePowersActive & (1 << FP_SPEED));
	qboolean wantsSpeed = qtrue;

	if (bot2_states[clientNum].macroState == MACRO_CAMP_REGEN || bot2_states[clientNum].macroState == MACRO_HUNT_TRIPMINES) {
		wantsSpeed = qfalse;
	}
	else if (bot2_states[clientNum].macroState == MACRO_GET_WEAPON && VectorLength(targetOrigin) > 1.0f) {
		float dist = Distance(ent->client->ps.origin, targetOrigin);
		if (speedActive && dist < 300.0f) wantsSpeed = qfalse;
		else if (!speedActive && dist < 500.0f) wantsSpeed = qfalse;
	}
	else if (bot2_states[clientNum].macroState == MACRO_CHASE_THIEF && !combatTarget && VectorLength(targetOrigin) > 1.0f) {
		// Walking to pick up a dropped flag or intercepting at base (no active LOS combat target)
		float dist = Distance(ent->client->ps.origin, targetOrigin);
		if (speedActive && dist < 300.0f) wantsSpeed = qfalse;
		else if (!speedActive && dist < 500.0f) wantsSpeed = qfalse;
	}
	else if (bot2_states[clientNum].macroState == MACRO_DEFEND_STAND && VectorLength(targetOrigin) > 1.0f) {
		// Walk to avoid wildly overshooting the patrol points or flagstand when planting mines
		float dist = Distance(ent->client->ps.origin, targetOrigin);
		if (speedActive && dist < 200.0f) wantsSpeed = qfalse;
		else if (!speedActive && dist < 400.0f) wantsSpeed = qfalse;
	}
	else if (bot2_states[clientNum].macroState == MACRO_ESCORT_FC && VectorLength(targetOrigin) > 1.0f) {
		// Walk when close to the flag carrier to avoid pushing past them
		float dist = Distance(ent->client->ps.origin, targetOrigin);
		if (speedActive && dist < 300.0f) wantsSpeed = qfalse;
		else if (!speedActive && dist < 500.0f) wantsSpeed = qfalse;
	}

	// Absorb: activate when carrying the flag or inside the enemy base area while fetching.
	// baseRadius (40% of flag-to-flag dist) reuses the existing spatial threshold.
	qboolean inEnemyBase = enemyFlag && (Distance(ent->client->ps.origin, enemyFlag->s.origin) < baseRadius);
	qboolean wantsAbsorb = hasFlag || ((bot2_states[clientNum].macroState == MACRO_FETCH_FLAG) && inEnemyBase);
	qboolean absorbActive = !!(ent->client->ps.fd.forcePowersActive & (1 << FP_ABSORB));

	if (!ent->client->ps.fd.forceButtonNeedRelease) {

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
		else if (wantsAbsorb != absorbActive && (absorbActive || ent->client->ps.fd.forcePower >= 15)) {
			// Toggle absorb on or off to match desired state
			// If it's already active, it costs 0 to turn off. If it's off, it costs 15 to turn on.
			ucmd.forcesel = FP_ABSORB;
			ucmd.buttons |= BUTTON_FORCEPOWER;
		}
		else if (wantsSpeed != speedActive && (speedActive || ent->client->ps.fd.forcePower >= 50)) {
			// Toggle speed on or off. If active, costs 0 to turn off. If off, costs 50 to turn on.
			ucmd.forcesel = FP_SPEED; 
			ucmd.buttons |= BUTTON_FORCEPOWER;
		}
		else {
			// CRITICAL FIX: Explicitly zero forcesel if no new button press is sent. 
			// If forcesel is left as a previous value (e.g. FP_ABSORB) from lastucmd, and the engine
			// unexpectedly clears or processes forceButtonNeedRelease, the engine might re-evaluate
			// the lingering FP_ABSORB command as a toggle request and turn the power off.
			ucmd.forcesel = 0; 
		}
	}

	qboolean hasFallback = qfalse; vec3_t fallbackOrigin;
	if (bot2_states[clientNum].role == ROLE_CHASE && bot2_states[clientNum].macroState == MACRO_CHASE_THIEF && enemyFlag) {
		hasFallback = qtrue; VectorCopy(enemyFlag->s.origin, fallbackOrigin);
	}

	Bot2_ExecuteMovement(clientNum, &ucmd, targetOrigin, aimDir, lockAim, wantsSpeed, hasFallback, fallbackOrigin);

	// --- TELEMETRY: DECOUPLED HIT DETECTION ---
	if (ent->client->accuracy_hits > bot2_states[clientNum].tele_lastHits) {
		int hits = ent->client->accuracy_hits - bot2_states[clientNum].tele_lastHits;
		Bot2_PrintTelemetry(2, "%s -> [RESULT] TRUE HIT for %d tick(s)!\n", bot2_states[clientNum].tele_lastAimString, hits);
		bot2_states[clientNum].tele_lastHits = ent->client->accuracy_hits;
	}

	botstates[clientNum]->lastucmd = ucmd; trap->BotUserCommand(ent->s.number, &ucmd);
}
                                                                                                                                                                                                                                                                                                                               
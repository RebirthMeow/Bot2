// ==============================================================================
// bot2_svcmds.c - Bot2 server (rcon / dedicated console) commands.
//
// Four developer commands extracted from g_svcmds.c as part of the bot2
// footprint reduction.  The svcmd table in g_svcmds.c continues to reference
// these functions by name; only the bodies have moved.
// ==============================================================================

#include "g_local.h"
#include "ai_bot2.h"
#include "bot2_svcmds.h"

extern void G_Kill(gentity_t* ent);

void Svcmd_BotWallrunCheck_f( void ) {
	char arg[MAX_TOKEN_CHARS];
	int clientNum = 0;
	if (trap->Argc() >= 2) {
		trap->Argv(1, arg, sizeof(arg));
		clientNum = atoi(arg);
	}
	if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
		trap->Print("Usage: bot_wallrun_check <clientnum>\n");
		return;
	}
	gentity_t *ent = &g_entities[clientNum];
	trap->Print("[WR-CHECK] Checking wallrun surface for client %d (%s)...\n",
		clientNum, (ent->client ? ent->client->pers.netname : "?"));
	Bot2_WallrunCheck(ent);
}

void Svcmd_BotScanWallruns_f( void ) {
	char arg[MAX_TOKEN_CHARS];

	int   gridStep    = 96;
	float radius      = 0.0f;
	int   clientNum   = 0;

	if (trap->Argc() >= 2) { trap->Argv(1, arg, sizeof(arg)); gridStep  = atoi(arg); }
	if (trap->Argc() >= 3) { trap->Argv(2, arg, sizeof(arg)); radius    = (float)atof(arg); }
	if (trap->Argc() >= 4) { trap->Argv(3, arg, sizeof(arg)); clientNum = atoi(arg); }

	Bot2_ScanWallruns(gridStep, radius, clientNum);
}

void Svcmd_BotScanWallrunsHeadless_f( void ) {
	char arg[MAX_TOKEN_CHARS];

	int   gridStep    = 96;
	float radius      = 0.0f;
	int   clientNum   = 0;

	if (trap->Argc() >= 2) { trap->Argv(1, arg, sizeof(arg)); gridStep  = atoi(arg); }
	if (trap->Argc() >= 3) { trap->Argv(2, arg, sizeof(arg)); radius    = (float)atof(arg); }
	if (trap->Argc() >= 4) { trap->Argv(3, arg, sizeof(arg)); clientNum = atoi(arg); }

	Bot2_ScanWallrunsHeadless(gridStep, radius, clientNum);
}

void Svcmd_BotTestRouting_f( void ) {
	char arg[MAX_TOKEN_CHARS];
	int clientNum = 0;
	if (trap->Argc() >= 2) {
		trap->Argv(1, arg, sizeof(arg));
		clientNum = atoi(arg);
	}
	bot2_states[clientNum].test_active = qtrue;
	bot2_states[clientNum].test_variation = 0;
	bot2_states[clientNum].test_run_idx = 0;
	bot2_states[clientNum].test_retries = 0;
	bot2_states[clientNum].test_had_flag = qfalse;
	bot2_states[clientNum].test_waiting_for_teleport = qtrue;
	trap->Print("Started Routing Test on Bot %d. Resetting Bot for fresh run...\n", clientNum);
	G_Kill(&g_entities[clientNum]);
}

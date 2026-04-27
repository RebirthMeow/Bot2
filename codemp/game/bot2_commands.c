// ==============================================================================
// bot2_commands.c - Bot2 / navmesh client console commands.
//
// These are sv_cheats-gated diagnostics for inspecting the navmesh and the
// off-mesh connection graph.  All bodies were extracted from g_cmds.c as
// part of the bot2 footprint reduction; the command-table entries in
// g_cmds.c continue to reference these functions by name.
//
// Functions exposed via bot2_commands.h:
//   Cmd_NavInfo_f, Cmd_NavTest_f, Cmd_NavCheck_f,
//   Cmd_NavDraw_f, Cmd_NavDrawOffMesh_f
// ==============================================================================

#include "g_local.h"
#include "g_navmesh.h"
#include "bot2_commands.h"

extern void G_TestLine(vec3_t start, vec3_t end, int color, int time);

void Cmd_NavInfo_f( gentity_t *ent ) {
	gentity_t *world = &g_entities[ENTITYNUM_WORLD];
	trap->SendServerCommand(ent->s.number, va("print \"World BSP Bounds: MIN(%f %f %f) MAX(%f %f %f)\n\"",
		world->r.mins[0], world->r.mins[1], world->r.mins[2],
		world->r.maxs[0], world->r.maxs[1], world->r.maxs[2]));
	NavMesh_PrintDebugInfo();
}

void Cmd_NavTest_f( gentity_t *ent ) {
	vec3_t forward, end, nextWp;
	trace_t tr;

	AngleVectors(ent->client->ps.viewangles, forward, NULL, NULL);
	VectorMA(ent->client->ps.origin, 4096, forward, end);
	trap->Trace(&tr, ent->client->ps.origin, NULL, NULL, end, ent->s.number, MASK_SOLID, 0, 0, 0);

	trap->SendServerCommand(ent->s.number, va("print \"NavTest Input - Start(Quake): %f %f %f | End(Quake): %f %f %f\n\"",
		ent->client->ps.origin[0], ent->client->ps.origin[1], ent->client->ps.origin[2],
		tr.endpos[0], tr.endpos[1], tr.endpos[2]));

	if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)tr.endpos, (float*)nextWp)) {
		trap->SendServerCommand(ent->s.number, va("print \"NavTest: Path found! Next WP is %f %f %f\n\"", nextWp[0], nextWp[1], nextWp[2]));
		if (sv_cheats.integer) {
			G_TestLine(ent->client->ps.origin, nextWp, 0x0000ff, 5000);
		}
	} else {
		trap->SendServerCommand(ent->s.number, "print \"NavTest: Query failed. No path found.\n\"");
	}
}

void Cmd_NavCheck_f( gentity_t *ent ) {
	if (g_gametype.integer != GT_CTF) {
		trap->SendServerCommand(ent->s.number, "print \"NavCheck: Only supported in CTF mode.\n\"");
		return;
	}

	int myTeam = ent->client->sess.sessionTeam;
	if (myTeam != TEAM_RED && myTeam != TEAM_BLUE) {
		trap->SendServerCommand(ent->s.number, "print \"NavCheck: You must be on a team.\n\"");
		return;
	}

	char* targetClass = (myTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag";
	gentity_t* flagEnt = G_Find(NULL, FOFS(classname), targetClass);
	if (!flagEnt) {
		trap->SendServerCommand(ent->s.number, "print \"NavCheck: Could not find enemy flag stand.\n\"");
		return;
	}

	static float waypoints[512 * 3];
	int count = NavMesh_GetPath(ent->s.number, (const float*)ent->client->ps.origin, (const float*)flagEnt->s.origin, waypoints, 512);

	if (count > 0) {
		trap->SendServerCommand(ent->s.number, va("print \"NavCheck: Path found with %d waypoints.\n\"", count));
		if (sv_cheats.integer) {
			for (int i = 0; i < count; i++) {
				vec3_t wp;
				VectorCopy(&waypoints[i * 3], wp);
				// Draw a small vertical line at each waypoint
				vec3_t up = { wp[0], wp[1], wp[2] + 32 };
				G_TestLine(wp, up, 0x00ff00, 10000);

				// Draw path segment
				if (i > 0) {
					vec3_t prev;
					VectorCopy(&waypoints[(i - 1) * 3], prev);
					G_TestLine(prev, wp, 0xffff00, 10000);
				} else {
					G_TestLine(ent->client->ps.origin, wp, 0xffff00, 10000);
				}
			}
		}
	} else {
		trap->SendServerCommand(ent->s.number, "print \"NavCheck: No path found to enemy flag stand.\n\"");
	}
}

void Cmd_NavDraw_f( gentity_t *ent ) {
	if (!sv_cheats.integer) {
		trap->SendServerCommand(ent->s.number, "print \"NavDraw: sv_cheats must be 1.\n\"");
		return;
	}

	float radius = 1000.0f;
	if (trap->Argc() > 1) {
		char arg[MAX_TOKEN_CHARS];
		trap->Argv(1, arg, sizeof(arg));
		radius = atof(arg);
		if (radius <= 0) radius = 1000.0f;
		if (radius > 5000.0f) radius = 5000.0f; // cap for performance
	}

	trap->SendServerCommand(ent->s.number, va("print \"NavDraw: Rendering NavMesh within %.0f units (use navdrawoffmesh for connections)...\n\"", radius));
	NavMesh_DrawDebug(ent->client->ps.origin, radius);
}

void Cmd_NavDrawOffMesh_f( gentity_t *ent ) {
	if (!sv_cheats.integer) {
		trap->SendServerCommand(ent->s.number, "print \"NavDrawOffMesh: sv_cheats must be 1.\n\"");
		trap->SendServerCommand(ent->s.number, "print \"Usage: navdrawoffmesh [radius] [drop] [jump] [wallrun] [elevator] [jumppad]\n\"");
		return;
	}

	float radius = 1000.0f;
	int typeMask = 0; // 0 = draw all

	int argc = trap->Argc();
	for (int i = 1; i < argc; i++) {
		char arg[MAX_TOKEN_CHARS];
		trap->Argv(i, arg, sizeof(arg));

		// First numeric arg is radius
		if (arg[0] >= '0' && arg[0] <= '9') {
			radius = atof(arg);
			if (radius <= 0) radius = 1000.0f;
			if (radius > 5000.0f) radius = 5000.0f;
		} else if (Q_stricmp(arg, "drop") == 0) {
			typeMask |= (1 << OFFMESH_AREA_JUMP_DROP);
		} else if (Q_stricmp(arg, "jump") == 0) {
			typeMask |= (1 << OFFMESH_AREA_JUMP_BASIC);
		} else if (Q_stricmp(arg, "wallrun") == 0) {
			typeMask |= (1 << OFFMESH_AREA_WALLRUN);
		} else if (Q_stricmp(arg, "elevator") == 0) {
			typeMask |= (1 << 10);
		} else if (Q_stricmp(arg, "jumppad") == 0) {
			typeMask |= (1 << 11);
		}
	}

	const char* filter = (typeMask == 0) ? "all types" : va("mask 0x%X", typeMask);
	trap->SendServerCommand(ent->s.number, va("print \"NavDrawOffMesh: %.0f units, %s | red=drop yellow=jump green=wallrun blue=elevator orange=jumppad\n\"", radius, filter));
	NavMesh_DrawOffMeshDebug(ent->client->ps.origin, radius, typeMask);
}

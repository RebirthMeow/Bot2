#include "g_local.h"
#include "ai_bot2.h"
#include "ai_bot2_graph.h"
#include "g_navmesh.h"
#include <math.h>

actionNode_t g_actionNodes[MAX_ACTION_NODES];
int g_numActionNodes = 0;

static reverseNode_t g_reverseNodes[MAX_ACTION_NODES];
static short g_spatialGrid[GRID_DIM][GRID_DIM][MAX_NODES_PER_CELL];
static short g_spatialGridCount[GRID_DIM][GRID_DIM];

static void GetGridIndex(const vec3_t pos, int* ix, int* iy) {
	*ix = (int)((pos[0] + 32768.0f) / GRID_SIZE) % GRID_DIM;
	*iy = (int)((pos[1] + 32768.0f) / GRID_SIZE) % GRID_DIM;
}

void Bot2_BuildSpatialGrid(void) {
	memset(g_spatialGridCount, 0, sizeof(g_spatialGridCount));
	for (int i = 0; i < g_numActionNodes; i++) {
		int ix, iy; GetGridIndex(g_actionNodes[i].origin, &ix, &iy);
		if (g_spatialGridCount[ix][iy] < MAX_NODES_PER_CELL) g_spatialGrid[ix][iy][g_spatialGridCount[ix][iy]++] = i;
	}
}

void Bot2_BuildReverseAdjacency(void) {
	memset(g_reverseNodes, 0, sizeof(g_reverseNodes));
	for (int u = 0; u < g_numActionNodes; u++) {
		for (int e = 0; e < g_actionNodes[u].numEdges; e++) {
			int v = g_actionNodes[u].edges[e].targetNodeId;
			if (v < 0 || v >= g_numActionNodes) continue;
			// Skip phantom jump edges — high-speed bake overshoots that land at nodes
			// 1000-7000u away from the source. Including them in the reverse adjacency
			// causes the flow field Dijkstra to route through unreachable "shortcuts",
			// giving the bot flow costs it can never actually achieve at runtime.
			if (g_actionNodes[u].edges[e].upmove != 0) {
				float edgeDist = Distance(g_actionNodes[u].origin, g_actionNodes[v].origin);
				if (edgeDist > 1400.0f) continue;
			}
			if (g_reverseNodes[v].numIncoming < MAX_REVERSE_EDGES) {
				g_reverseNodes[v].incoming[g_reverseNodes[v].numIncoming].fromNodeId = u;
				g_reverseNodes[v].incoming[g_reverseNodes[v].numIncoming].edgeIdx = e;
				g_reverseNodes[v].numIncoming++;
			}
		}
	}
}

int Bot2_GetNearestNode(vec3_t origin) {
	int bestNode = -1; float bestDist = 99999999.0f; int cx, cy; GetGridIndex(origin, &cx, &cy);
	for (int x = cx - 1; x <= cx + 1; x++) {
		for (int y = cy - 1; y <= cy + 1; y++) {
			int gx = (x + GRID_DIM) % GRID_DIM, gy = (y + GRID_DIM) % GRID_DIM;
			for (int i = 0; i < g_spatialGridCount[gx][gy]; i++) {
				int nodeId = g_spatialGrid[gx][gy][i];
				float dx = origin[0] - g_actionNodes[nodeId].origin[0], dy = origin[1] - g_actionNodes[nodeId].origin[1], dz = (origin[2] - g_actionNodes[nodeId].origin[2]) * 4.0f;
				float dist = (dx * dx) + (dy * dy) + (dz * dz);
				if (dist < bestDist) { bestDist = dist; bestNode = nodeId; }
			}
		}
	}
	return bestNode;
}

void Bot2_SaveGraph(const char* mapname) {
	char path[MAX_QPATH]; Com_sprintf(path, sizeof(path), "maps/%s.act", mapname);
	fileHandle_t f; trap->FS_Open(path, &f, FS_WRITE); if (!f) return;
	int magic = BOT2_GRAPH_MAGIC, version = BOT2_GRAPH_VERSION;
	trap->FS_Write(&magic, sizeof(int), f); trap->FS_Write(&version, sizeof(int), f); trap->FS_Write(&g_numActionNodes, sizeof(int), f);
	for (int i = 0; i < g_numActionNodes; i++) {
		trap->FS_Write(&g_actionNodes[i].id, sizeof(int), f); trap->FS_Write(&g_actionNodes[i].origin, sizeof(vec3_t), f); trap->FS_Write(&g_actionNodes[i].numEdges, sizeof(int), f);
		if (g_actionNodes[i].numEdges > 0) trap->FS_Write(g_actionNodes[i].edges, sizeof(actionEdge_t) * g_actionNodes[i].numEdges, f);
	}
	trap->FS_Close(f);
}

void Bot2_LoadGraph(const char* mapname) {
	char path[MAX_QPATH]; Com_sprintf(path, sizeof(path), "maps/%s.act", mapname);
	fileHandle_t f; if (trap->FS_Open(path, &f, FS_READ) <= 0) { g_numActionNodes = 0; return; }
	int magic = 0, version = 0, totalEdges = 0; trap->FS_Read(&magic, sizeof(int), f); trap->FS_Read(&version, sizeof(int), f);
	if (magic != BOT2_GRAPH_MAGIC || version != BOT2_GRAPH_VERSION) { trap->FS_Close(f); g_numActionNodes = 0; return; }
	trap->FS_Read(&g_numActionNodes, sizeof(int), f);
	for (int i = 0; i < g_numActionNodes; i++) {
		trap->FS_Read(&g_actionNodes[i].id, sizeof(int), f); trap->FS_Read(&g_actionNodes[i].origin, sizeof(vec3_t), f); trap->FS_Read(&g_actionNodes[i].numEdges, sizeof(int), f);
		if (g_actionNodes[i].numEdges > 0) { trap->FS_Read(g_actionNodes[i].edges, sizeof(actionEdge_t) * g_actionNodes[i].numEdges, f); totalEdges += g_actionNodes[i].numEdges; }
	}
	trap->FS_Close(f); Bot2_BuildSpatialGrid(); Bot2_BuildReverseAdjacency();
}

void Bot2_CalculateFlowField(int clientNum, int targetNodeId) {
	int* flowField = bot2_states[clientNum].flowField; for (int i = 0; i < g_numActionNodes; i++) flowField[i] = 99999999;
	if (targetNodeId < 0 || targetNodeId >= g_numActionNodes) return;
	flowField[targetNodeId] = 0; static int worklist[MAX_ACTION_NODES * 2]; static qboolean inQueue[MAX_ACTION_NODES];
	int head = 0, tail = 0; memset(inQueue, 0, sizeof(inQueue)); worklist[tail++] = targetNodeId; inQueue[targetNodeId] = qtrue;
	while (head != tail) {
		int v = worklist[head++]; if (head >= MAX_ACTION_NODES * 2) head = 0; inQueue[v] = qfalse;
		for (int i = 0; i < g_reverseNodes[v].numIncoming; i++) {
			int u = g_reverseNodes[v].incoming[i].fromNodeId, e = g_reverseNodes[v].incoming[i].edgeIdx;
			int cost = flowField[v] + g_actionNodes[u].edges[e].costTimeMs;
			if (flowField[u] > cost) { flowField[u] = cost; if (!inQueue[u]) { worklist[tail++] = u; if (tail >= MAX_ACTION_NODES * 2) tail = 0; inQueue[u] = qtrue; } }
		}
	}
}

void Bot2_BakeMap(void) {
	g_numActionNodes = 0; memset(g_actionNodes, 0, sizeof(g_actionNodes));
	gentity_t* simEnt = G_Spawn(); simEnt->classname = "baker_bot";
	static gclient_t dummyClient; memset(&dummyClient, 0, sizeof(dummyClient)); simEnt->client = &dummyClient;
	playerState_t baselinePS; memset(&baselinePS, 0, sizeof(playerState_t));
	baselinePS.fd.forcePowersKnown |= (1 << FP_LEVITATION) | (1 << FP_SPEED);
	baselinePS.fd.forcePowersActive |= (1 << FP_SPEED);
	baselinePS.fd.forcePowerLevel[FP_LEVITATION] = baselinePS.fd.forcePowerLevel[FP_SPEED] = 3;
	baselinePS.fd.forcePower = 100; baselinePS.stats[STAT_HEALTH] = 100;
	baselinePS.basespeed = 250.0f;
	baselinePS.speed = 425.0f;

	// --- Geometry-based node placement ---
	// Sample the map directly with a world-aligned XY grid + downward box traces.
	// This finds every walkable floor surface (ramps, stairs, multi-level areas)
	// without depending on the navmesh, which can miss surfaces with non-standard
	// poly flags or steep-angle thresholds.
	//
	// Player bbox: mins=(-15,-15,-24), maxs=(15,15,32).
	// A downward box trace endpos is the player ORIGIN when the bbox bottom (-24)
	// touches the surface, so:
	//   floor surface Z  = endpos[2] - 24
	//   capsule top Z    = endpos[2] + 32
	//
	// After recording a floor at origin H we must pierce BELOW it for the next
	// iteration.  We step down 80 units from H:
	//   H - 80  =  (H - 24) - 56  (56 units below the floor surface)
	// which clears brush floors up to ~48 units thick.
	// Stepping upward (the old bug) would re-hit the same surface every iteration.
	{
		vec3_t gmins, gmaxs;
		VectorSet(gmins, -15, -15, -24);
		VectorSet(gmaxs,  15,  15,  32);
		const float GRID_STEP  =  64.0f;
		const float MAP_MIN_XY = -8192.0f;
		const float MAP_MAX_XY =  8192.0f;
		const float Z_TOP      =  8192.0f;
		const float Z_BOTTOM   = -4096.0f;
		const float PIERCE_DOWN =  80.0f;  // step below floor origin before next scan
		const float BBOX_TOP    =  32.0f;  // maxs[2] - headroom above origin
		const int   MAX_LEVELS  =  16;

		for (float gx = MAP_MIN_XY; gx <= MAP_MAX_XY && g_numActionNodes < MAX_ACTION_NODES; gx += GRID_STEP) {
			for (float gy = MAP_MIN_XY; gy <= MAP_MAX_XY && g_numActionNodes < MAX_ACTION_NODES; gy += GRID_STEP) {
				float scanZ = Z_TOP;
				for (int lvl = 0; lvl < MAX_LEVELS && scanZ > Z_BOTTOM; lvl++) {
					vec3_t start, end;
					VectorSet(start, gx, gy, scanZ);
					VectorSet(end,   gx, gy, Z_BOTTOM);
					trace_t tr;
					// Box trace so the full player footprint must be grounded -
					// avoids placing nodes on ledge edges where only the center lands.
					trap->Trace(&tr, start, gmins, gmaxs, end, ENTITYNUM_NONE, MASK_PLAYERSOLID, qfalse, 0, 0);
					if (tr.fraction >= 1.0f || tr.startsolid) break;

					vec3_t hitPos;
					VectorCopy(tr.endpos, hitPos);

					// Advance cursor BELOW this floor so next iteration finds lower levels.
					scanZ = hitPos[2] - PIERCE_DOWN;

					// Reject liquid surfaces
					if (trap->PointContents(hitPos, -1) & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER)) continue;

					// Reject trigger_hurt volumes (death pits, crushers)
					{
						vec3_t trigStart;
						VectorCopy(hitPos, trigStart); trigStart[2] += 1.0f;
						trace_t trT;
						trap->Trace(&trT, trigStart, gmins, gmaxs, hitPos, ENTITYNUM_NONE, CONTENTS_TRIGGER, qfalse, 0, 0);
						if (trT.fraction < 1.0f && trT.entityNum >= 0 && trT.entityNum < ENTITYNUM_WORLD) {
							gentity_t* hit = &g_entities[trT.entityNum];
							if (hit->inuse && hit->classname && Q_stricmp(hit->classname, "trigger_hurt") == 0) continue;
						}
					}

					// Headroom check: capsule top is BBOX_TOP above the origin.
					// Trace upward that amount; if blocked the player can't stand here.
					{
						vec3_t headStart, headEnd;
						VectorCopy(hitPos, headStart); headStart[2] += 1.0f;
						VectorCopy(hitPos, headEnd);   headEnd[2] += BBOX_TOP;
						trace_t trH;
						trap->Trace(&trH, headStart, gmins, gmaxs, headEnd, ENTITYNUM_NONE, MASK_PLAYERSOLID, qfalse, 0, 0);
						if (trH.fraction < 1.0f) continue;
					}

					g_actionNodes[g_numActionNodes].id = g_numActionNodes;
					VectorCopy(hitPos, g_actionNodes[g_numActionNodes].origin);
					g_numActionNodes++;
				}
			}
		}
	}
	// Angular sweep bake: O(N × A × S) instead of O(N²).
	//
	// Old approach: for each source node, iterate all N target nodes within a radius to
	// derive aim directions. With ~550 avg neighbors this is ~550 × 54 = 29,700 sim calls
	// per source node. Many neighbors cluster in the same angular direction, producing
	// massive redundancy (20 nearby nodes all ~"northeast" → 20 near-identical sim runs).
	//
	// New approach: sample A=36 evenly-spaced angles (10° steps) around the full circle.
	// Each angle is tried at all speeds/modes/strafe dirs; the landing node is found from
	// the simulation result, not pre-enumerated. This is 36 × 54 = 1,944 sim calls per
	// source node — a 15× reduction — and gives uniform directional coverage regardless
	// of node density. At 900u range, 10° spacing ≈ 157u chord, well below the 64u node
	// grid spacing projected at shorter ranges (~11° at 300u range).
	trap->Print("Geometry grid placed %d nodes. Beginning edge bake (angular sweep, O(N*A))...\n", g_numActionNodes);
	Bot2_BuildSpatialGrid();

	// Speed array. Force Speed 3 = 425 u/s base; strafe jumping can reach 600-1100 u/s.
	// Speeds ≤1100 u/s produce trajectories ≤1400u (Force Jump 3 flight ~1.24s), kept by
	// the distance cap. 1200+ u/s produce 1485u+ arcs which are discarded anyway, so
	// including them in the array wastes bake time.
	float speeds[] = { 250.0f, 425.0f, 500.0f, 600.0f, 700.0f, 800.0f, 900.0f, 1000.0f, 1100.0f };
	int numSpeeds = 9;
	int testDirs[] = { 0, 1, -1 };

	#define BAKE_NUM_ANGLES 36
	float bakeAngleStep = 360.0f / BAKE_NUM_ANGLES;

	for (int i = 0; i < g_numActionNodes; i++) {
		if (i % 100 == 0) trap->Print("Baking edges: %d / %d (%.1f%%)...\n", i, g_numActionNodes, 100.0f * i / g_numActionNodes);
		for (int a = 0; a < BAKE_NUM_ANGLES; a++) {
			float targetYaw = a * bakeAngleStep;
			for (int s = 0; s < numSpeeds; s++) {
				float speed = speeds[s];
				for (int jMode = 0; jMode < 2; jMode++) {
					qboolean isJump = (jMode == 1); if (!isJump && speed > 430.0f) continue;
					for (int d = 0; d < 3; d++) {
						vec3_t landPos; memcpy(&simEnt->client->ps, &baselinePS, sizeof(playerState_t));
						if (isJump) {
							// Runway check: need 128u of clear space behind the jump direction.
							// If there's a wall, the bot can't build approach speed.
							vec3_t runwayAngles, fwd, bStart, bEnd, bMins={-15,-15,-24}, bMaxs={15,15,32};
							VectorSet(runwayAngles, 0, targetYaw, 0);
							AngleVectors(runwayAngles, fwd, NULL, NULL);
							VectorMA(g_actionNodes[i].origin, -128.0f, fwd, bStart); bStart[2] += 32.0f;
							VectorCopy(g_actionNodes[i].origin, bEnd); bEnd[2] += 32.0f;
							trace_t trB; trap->Trace(&trB, bStart, bMins, bMaxs, bEnd, simEnt->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
							if (trB.fraction < 1.0f) continue;
						}
						VectorCopy(g_actionNodes[i].origin, simEnt->client->ps.origin); simEnt->client->ps.origin[2] += 0.25f;
						simEnt->client->ps.velocity[0] = cos(targetYaw * M_PI / 180.0f) * speed;
						simEnt->client->ps.velocity[1] = sin(targetYaw * M_PI / 180.0f) * speed;
						simEnt->client->ps.groundEntityNum = ENTITYNUM_WORLD;
						vec3_t dummyVel;
						// Use baselinePS.speed (Force Speed 3 = 425) as max_run_speed so the
						// bake computes the same strafe magic-angle as the runtime bot does.
						if (SimulatePmoveTrajectory(simEnt, simEnt->client->ps.origin, simEnt->client->ps.velocity, testDirs[d], baselinePS.speed, landPos, dummyVel, isJump, qtrue)) {
							int nearest = Bot2_GetNearestNode(landPos);
							if (nearest != -1 && nearest != i) {
								// Cap edge length: 1100 u/s × 1.3s ≈ 1400u max legitimate arc.
								float edgeDist = Distance(g_actionNodes[i].origin, g_actionNodes[nearest].origin);
								if (edgeDist > 1400.0f) continue;
								int cost = (int)((Distance(g_actionNodes[i].origin, landPos) / speed) * 1000.0f);
								if (abs(testDirs[d]) == 1) cost -= 500;
								else if (abs(testDirs[d]) == 2) cost -= 200;
								if (cost < 1) cost = 1;
								int existing = -1;
								for (int e = 0; e < g_actionNodes[i].numEdges; e++) {
									qboolean edgeIsJump = (g_actionNodes[i].edges[e].upmove != 0);
									if (g_actionNodes[i].edges[e].targetNodeId == nearest && edgeIsJump == isJump && fabs(g_actionNodes[i].edges[e].minSpeed - (speed - 20.0f)) < 50.0f) {
										existing = e; break;
									}
								}
								if (existing != -1) {
									if (cost < g_actionNodes[i].edges[existing].costTimeMs) {
										actionEdge_t* edge = &g_actionNodes[i].edges[existing];
										edge->minSpeed = speed - 20.0f; edge->entryYaw = targetYaw; edge->strafeDir = testDirs[d]; edge->upmove = isJump ? 127 : 0; edge->costTimeMs = cost;
									}
								} else if (g_actionNodes[i].numEdges < MAX_EDGES_PER_NODE) {
									actionEdge_t* edge = &g_actionNodes[i].edges[g_actionNodes[i].numEdges++];
									edge->targetNodeId = nearest; edge->minSpeed = speed - 20.0f; edge->entryYaw = targetYaw; edge->strafeDir = testDirs[d]; edge->upmove = isJump ? 127 : 0; edge->costTimeMs = cost;
								}
							}
						}
					}
				}
			}
		}
	}
	G_FreeEntity(simEnt); char mapname[MAX_QPATH]; trap->Cvar_VariableStringBuffer("mapname", mapname, sizeof(mapname));
	Bot2_SaveGraph(mapname); Bot2_BuildReverseAdjacency();
	trap->Print("Bake complete! Saved %d nodes.\n", g_numActionNodes);
}

void Bot2_PrintNodeInfo(int nodeId) {
	if (nodeId < 0 || nodeId >= g_numActionNodes) return;
	actionNode_t* node = &g_actionNodes[nodeId]; trap->Print("--- NODE %d ---\nEdges: %d\n", nodeId, node->numEdges);
	for (int i = 0; i < node->numEdges; i++) trap->Print("  -> %d | %s | Spd %.0f\n", node->edges[i].targetNodeId, node->edges[i].upmove ? "JUMP" : "WALK", node->edges[i].minSpeed);
}

void Bot2_PrintNodeFlowInfo(int nodeId, int clientNum) {
	if (nodeId < 0 || nodeId >= g_numActionNodes || clientNum < 0 || clientNum >= MAX_CLIENTS) return;
	actionNode_t* node = &g_actionNodes[nodeId];
	trap->Print("--- NODE %d at (%.0f %.0f %.0f) | %d edges ---\n", nodeId, node->origin[0], node->origin[1], node->origin[2], node->numEdges);
	for (int i = 0; i < node->numEdges; i++) {
		int tgt = node->edges[i].targetNodeId;
		int flowCost = (tgt >= 0 && tgt < g_numActionNodes) ? bot2_states[clientNum].flowField[tgt] : -1;
		trap->Print("  [%2d] -> %4d | %s | Spd %5.0f | Flow: %s\n", i, tgt, node->edges[i].upmove ? "JUMP" : "WALK", node->edges[i].minSpeed, (flowCost < 99999999) ? "OK" : "DEAD");
	}
}

void Bot2_DebugConnectivity(int clientNum) {
	if (clientNum < 0 || clientNum >= MAX_CLIENTS || !g_entities[clientNum].client) return;
	vec3_t botPos; VectorCopy(g_entities[clientNum].client->ps.origin, botPos);
	int myNode = Bot2_GetNearestNode(botPos);
	trap->Print("=== CONNECTIVITY DEBUG: myNode=%d ===\n", myNode);
	int borderCount = 0;
	for (int n = 0; n < g_numActionNodes; n++) {
		if (DistanceSquared(botPos, g_actionNodes[n].origin) > (1024.0f * 1024.0f)) continue;
		if (g_actionNodes[n].numEdges == 0) continue;
		int aliveEdges = 0, deadEdges = 0;
		for (int e = 0; e < g_actionNodes[n].numEdges; e++) {
			int tgt = g_actionNodes[n].edges[e].targetNodeId;
			int cost = (tgt >= 0 && tgt < g_numActionNodes) ? bot2_states[clientNum].flowField[tgt] : 99999999;
			vec3_t from, to; VectorCopy(g_actionNodes[n].origin, from); from[2] += 16.0f; VectorCopy(g_actionNodes[tgt].origin, to); to[2] += 16.0f;
			G_TestLine(from, to, (cost < 99999999) ? 3 : 1, 15000);
			if (cost < 99999999) aliveEdges++; else deadEdges++;
		}
		if (aliveEdges > 0 && deadEdges > 0) {
			trap->Print("  BORDER node %d at (%.0f %.0f %.0f): %d alive, %d dead edges\n", n, g_actionNodes[n].origin[0], g_actionNodes[n].origin[1], g_actionNodes[n].origin[2], aliveEdges, deadEdges);
			borderCount++; if (borderCount >= 20) break;
		}
	}
	if (borderCount == 0) trap->Print("  No border nodes found within 1024 units.\n");
	trap->Print("=== END CONNECTIVITY DEBUG ===\n");
}

void Bot2_TeleportToNode(int clientNum, int nodeId) {
	if (nodeId >= 0 && nodeId < g_numActionNodes) { VectorCopy(g_actionNodes[nodeId].origin, g_entities[clientNum].client->ps.origin); g_entities[clientNum].client->ps.origin[2] += 16.0f; }
}

#include "g_navmesh.h"

// Pull in the minimum engine types WITHOUT extern "C" wrapping to avoid template errors
#include "../qcommon/q_shared.h"
#include "g_public.h"

#ifndef MASK_PLAYERSOLID
#define	MASK_PLAYERSOLID		(CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY)
#endif

// Explicitly declare C-linkage symbols
extern "C" {
	extern gameImport_t *trap;
	extern char *va(const char *format, ...);
}

#include "detour/DetourNavMesh.h"
#include "detour/DetourNavMeshQuery.h"
#include "detour/DetourAlloc.h"
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

#ifdef INFINITE
#undef INFINITE
#endif
#include <windows.h>

static dtNavMesh* g_navMesh = NULL;
static dtNavMeshQuery* g_navQuery = NULL;

static const float QUAKE_TO_METERS = 0.0254f;
static const float METERS_TO_QUAKE = 1.0f / 0.0254f;

static const int NAVMESHSET_MAGIC = 'M'<<24 | 'S'<<16 | 'E'<<8 | 'T'; //'MSET';
static const int NAVMESHSET_VERSION = 1;

struct NavMeshSetHeader {
	int magic;
	int version;
	int numTiles;
	dtNavMeshParams params;
};

struct NavMeshTileHeader {
	dtTileRef tileRef;
	int dataSize;
};

static inline bool IsValidVector(const float* v) {
	if (!v) return false;
	return (v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && 
			v[0] < 1000000.0f && v[0] > -1000000.0f &&
			v[1] < 1000000.0f && v[1] > -1000000.0f &&
			v[2] < 1000000.0f && v[2] > -1000000.0f);
}

extern "C" void NavMesh_Log(const char* fmt, ...) {
	va_list argptr;
	char text[1024];
	FILE* f;
	va_start(argptr, fmt);
	vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);

	f = fopen("navmesh_crash.log", "a");
	if (f) {
		fprintf(f, "%s", text);
		fflush(f);
		fclose(f);
	}
	trap->Print(text);
}

// FIXED: No Y-axis inversion. Quake X=X, Y=Z, Z=Y.
static inline void QuakeToRecast(const float* q, float* r) {
	r[0] = q[0] * QUAKE_TO_METERS;
	r[1] = q[2] * QUAKE_TO_METERS;
	r[2] = q[1] * QUAKE_TO_METERS;
}

static inline void RecastToQuake(const float* r, float* q) {
	q[0] = r[0] * METERS_TO_QUAKE;
	q[1] = r[2] * METERS_TO_QUAKE;
	q[2] = r[1] * METERS_TO_QUAKE;
}

extern "C" void NavMesh_InitForMap(const char* mapname) {
	fileHandle_t f;
	int len;
	char* buffer;
	char path[MAX_QPATH];

	Com_sprintf(path, sizeof(path), "maps/%s.navmesh", mapname);
	
	if (g_navQuery) dtFreeNavMeshQuery(g_navQuery);
	g_navQuery = NULL;
	if (g_navMesh) dtFreeNavMesh(g_navMesh);
	g_navMesh = NULL;

	NavMesh_Log("\n--- NavMesh Init Start ---\n");

	len = trap->FS_Open(path, &f, FS_READ);
	if (len <= 0) {
		NavMesh_Log("INIT: File not found.\n");
		return;
	}

	trap->TrueMalloc((void**)&buffer, len);
	if (!buffer) return;

	trap->FS_Read(buffer, len, f);
	trap->FS_Close(f);

	unsigned char* ptr = (unsigned char*)buffer;
	int magic = *(int*)ptr;

	g_navMesh = dtAllocNavMesh();
	if (!g_navMesh) {
		trap->TrueFree((void**)&buffer);
		return;
	}

	if (magic == NAVMESHSET_MAGIC) {
		NavMeshSetHeader* header = (NavMeshSetHeader*)ptr;
		ptr += sizeof(NavMeshSetHeader);
		dtStatus status = g_navMesh->init(&header->params);
		if (dtStatusFailed(status)) {
			NavMesh_Free();
			trap->TrueFree((void**)&buffer);
			return;
		}
		for (int i = 0; i < header->numTiles; ++i) {
			NavMeshTileHeader* tileHeader = (NavMeshTileHeader*)ptr;
			ptr += sizeof(NavMeshTileHeader);
			unsigned char* data = (unsigned char*)dtAlloc(tileHeader->dataSize, DT_ALLOC_PERM);
			if (data) {
				memcpy(data, ptr, tileHeader->dataSize);
				g_navMesh->addTile(data, tileHeader->dataSize, DT_TILE_FREE_DATA, tileHeader->tileRef, 0);
			}
			ptr += tileHeader->dataSize;
		}
		NavMesh_Log("SUCCESS: Loaded Tiled Mesh.\n");
	} else {
		dtStatus status = g_navMesh->init((unsigned char*)buffer, len, 0); 
		if (dtStatusFailed(status)) {
			NavMesh_Free();
			trap->TrueFree((void**)&buffer);
			return;
		}
		NavMesh_Log("SUCCESS: Loaded Solo Mesh.\n");
	}

	for (int i = 0; i < g_navMesh->getMaxTiles(); ++i) {
		dtMeshTile* tile = const_cast<dtMeshTile*>(static_cast<const dtNavMesh*>(g_navMesh)->getTile(i));
		if (!tile || !tile->header) continue;
		for (int j = 0; j < tile->header->polyCount; ++j) {
			tile->polys[j].flags = 1;
		}
	}

	g_navQuery = dtAllocNavMeshQuery();
	if (g_navQuery) {
		g_navQuery->init(g_navMesh, 2048);
		NavMesh_Log("SUCCESS: NavQuery ready.\n");
	}

	trap->TrueFree((void**)&buffer);
}

extern "C" void NavMesh_Free(void) {
	if (g_navQuery) dtFreeNavMeshQuery(g_navQuery);
	g_navQuery = NULL;
	if (g_navMesh) dtFreeNavMesh(g_navMesh);
	g_navMesh = NULL;
}

static qboolean NavMesh_DoQuery(int passEntityNum, const float* startQuake, const float* endQuake, float* outWaypoint) {
	static const int MAX_PATH_NODES = 512;
	static dtPolyRef pathRefs[MAX_PATH_NODES];
	static float straightPath[MAX_PATH_NODES * 3];
	static unsigned char straightPathFlags[MAX_PATH_NODES];
	static dtPolyRef straightPathRefs[MAX_PATH_NODES];

	if (!IsValidVector(startQuake) || !IsValidVector(endQuake)) return qfalse;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);

	float startRecast[3], endRecast[3], extent[3] = { 2.0f, 4.0f, 2.0f }; // meters
	QuakeToRecast(startQuake, startRecast);
	QuakeToRecast(endQuake, endRecast);

	dtPolyRef startRef = 0, endRef = 0;
	float nearestStart[3], nearestEnd[3];

	g_navQuery->findNearestPoly(startRecast, extent, &filter, &startRef, nearestStart);
	g_navQuery->findNearestPoly(endRecast, extent, &filter, &endRef, nearestEnd);

	if (!startRef || !endRef) return qfalse;

	int pathCount = 0;
	dtStatus status = g_navQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathRefs, &pathCount, MAX_PATH_NODES);
	if (dtStatusFailed(status) || pathCount <= 0) return qfalse;

	int straightPathCount = 0;
	status = g_navQuery->findStraightPath(nearestStart, nearestEnd, pathRefs, pathCount, straightPath, straightPathFlags, straightPathRefs, &straightPathCount, MAX_PATH_NODES);
	if (dtStatusFailed(status) || straightPathCount < 2) return qfalse;

	vec3_t mins = {-13, -13, -20}, maxs = {13, 13, 30};

	for (int i = 1; i < straightPathCount; ++i) {
		float wpQuake[3];
		RecastToQuake(&straightPath[i * 3], wpQuake);
		trace_t tr;
		vec3_t traceStart = { startQuake[0], startQuake[1], startQuake[2] + 2.0f };
		trap->Trace(&tr, traceStart, mins, maxs, wpQuake, passEntityNum, MASK_PLAYERSOLID, 0, 0, 0);
		if (tr.fraction < 1.0f) {
			if (i == 1) { VectorCopy(wpQuake, outWaypoint); return qtrue; }
			else { RecastToQuake(&straightPath[(i-1) * 3], outWaypoint); return qtrue; }
		}
		float dx = wpQuake[0] - startQuake[0], dy = wpQuake[1] - startQuake[1];
		if (sqrtf(dx * dx + dy * dy) > 48.0f || i == straightPathCount - 1) {
			VectorCopy(wpQuake, outWaypoint);
			return qtrue;
		}
	}
	return qfalse;
}

extern "C" int NavMesh_GetNextWaypoint(int passEntityNum, const float* startPoint, const float* endPoint, float* outWaypoint) {
	if (!g_navMesh || !g_navQuery) return 0;

	__try {
		return (int)NavMesh_DoQuery(passEntityNum, startPoint, endPoint, outWaypoint);
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		NavMesh_Log("!!! HARDWARE CRASH IN NavMesh_DoQuery: 0x%08X !!!\n", GetExceptionCode());
		return 0;
	}
}

extern "C" void NavMesh_PrintDebugInfo(void) {
	if (!g_navMesh) return;
	float bmin[3] = { 99999.0f, 99999.0f, 99999.0f }, bmax[3] = {-99999.0f,-99999.0f,-99999.0f };
	int polyCount = 0, validTiles = 0;
	for (int i = 0; i < g_navMesh->getMaxTiles(); ++i) {
		const dtMeshTile* tile = static_cast<const dtNavMesh*>(g_navMesh)->getTile(i);
		if (!tile || !tile->header) continue;
		validTiles++; polyCount += tile->header->polyCount;
		for (int j=0; j<3; ++j) {
			if (tile->header->bmin[j] < bmin[j]) bmin[j] = tile->header->bmin[j];
			if (tile->header->bmax[j] > bmax[j]) bmax[j] = tile->header->bmax[j];
		}
	}

	NavMesh_Log("Tiles: %d | Polys: %d | Recast BMIN: %.2f %.2f %.2f | Recast BMAX: %.2f %.2f %.2f\n", 
		validTiles, polyCount, bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);

	float quakeMin[3], quakeMax[3];
	RecastToQuake(bmin, quakeMin);
	RecastToQuake(bmax, quakeMax);

	NavMesh_Log("Quake Nav Bounds -> MIN: %.2f %.2f %.2f | MAX: %.2f %.2f %.2f\n",
		quakeMin[0], quakeMin[1], quakeMin[2], quakeMax[0], quakeMax[1], quakeMax[2]);
}

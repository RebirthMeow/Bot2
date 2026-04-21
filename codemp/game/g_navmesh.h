#ifndef G_NAVMESH_H
#define G_NAVMESH_H

#ifdef __cplusplus
extern "C" {
#endif

// Use raw floats to avoid struct alignment issues between C and C++
typedef float navVec3_t[3];

void NavMesh_InitForMap(const char* mapname);
void NavMesh_Free(void);
int  NavMesh_GetNextWaypoint(int passEntityNum, const float* startPoint, const float* endPoint, float* outWaypoint);
int  NavMesh_GetPath(int passEntityNum, const float* startPoint, const float* endPoint, float* outWaypoints, int maxWaypoints);
float NavMesh_GetPathDistance(int passEntityNum, const float* startPoint, const float* endPoint);
int  NavMesh_IsPointOnMesh(const float* point);
void NavMesh_DrawDebug(const float* center, float radius);
void NavMesh_PrintDebugInfo(void);
void NavMesh_Log(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // G_NAVMESH_H

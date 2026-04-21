#ifndef __AI_BOT2_GRAPH_H__
#define __AI_BOT2_GRAPH_H__

#include "qcommon/q_shared.h"

#define MAX_EDGES_PER_NODE 128
#define MAX_ACTION_NODES 12288
#define MAX_REVERSE_EDGES 128 

#define BOT2_GRAPH_MAGIC   (('B'<<24)+('O'<<16)+('T'<<8)+'2')
#define BOT2_GRAPH_VERSION 20

// Spatial Hash constants
#define GRID_SIZE 512
#define MAX_NODES_PER_CELL 128
#define GRID_DIM 128 

typedef struct actionEdge_s {
	int targetNodeId;
	float minSpeed;
	float entryYaw;
	int strafeDir;
	signed char upmove;
	int costTimeMs;
} actionEdge_t;

typedef struct actionNode_s {
	int id;
	vec3_t origin;
	int numEdges;
	actionEdge_t edges[MAX_EDGES_PER_NODE];
} actionNode_t;

// Reverse edge for fast Dijkstra
typedef struct reverseEdge_s {
	int fromNodeId;
	int edgeIdx; 
} reverseEdge_t;

typedef struct reverseNode_s {
	int numIncoming;
	reverseEdge_t incoming[MAX_REVERSE_EDGES];
} reverseNode_t;

extern actionNode_t g_actionNodes[MAX_ACTION_NODES];
extern int g_numActionNodes;

void Bot2_BakeMap(void);
void Bot2_SaveGraph(const char* mapname);
void Bot2_LoadGraph(const char* mapname);
void Bot2_CalculateFlowField(int clientNum, int targetNodeId);
int Bot2_GetNearestNode(vec3_t origin);
void Bot2_TeleportToNode(int clientNum, int nodeId);
void Bot2_PrintNodeInfo(int nodeId);
// Prints every edge on a node alongside its current flow field cost for the given client.
void Bot2_PrintNodeFlowInfo(int nodeId, int clientNum);
// Draws edges around the bot colored green (reachable) or red (dead-end) and
// prints the border nodes where the connected and disconnected regions meet.
void Bot2_DebugConnectivity(int clientNum);

#endif // __AI_BOT2_GRAPH_H__

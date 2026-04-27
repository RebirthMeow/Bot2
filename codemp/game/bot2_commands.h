// ==============================================================================
// bot2_commands.h - Bot2 / navmesh client console commands.
//
// Five sv_cheats-gated client commands used to inspect the navmesh and the
// off-mesh connection graph at runtime.  They are referenced by name from
// the main g_cmds.c command table; this header just exposes their function
// pointers so the table can find them without each command's body living
// inline in the stock OpenJK file.
//
//   navinfo         - prints world bounds and navmesh debug stats.
//   navtest         - traces from the player's view, queries the next nav
//                     waypoint, draws it.
//   navcheck        - CTF-only: queries a full path to the enemy flag stand
//                     and draws the waypoint chain.
//   navdraw         - renders navmesh polys within a radius of the player.
//   navdrawoffmesh  - renders off-mesh connections (drops, jumps, wallruns,
//                     elevators, jumppads) within a radius, optionally
//                     filtered by type.
// ==============================================================================
#ifndef __BOT2_COMMANDS_H__
#define __BOT2_COMMANDS_H__

void Cmd_NavInfo_f( gentity_t *ent );
void Cmd_NavTest_f( gentity_t *ent );
void Cmd_NavCheck_f( gentity_t *ent );
void Cmd_NavDraw_f( gentity_t *ent );
void Cmd_NavDrawOffMesh_f( gentity_t *ent );

#endif

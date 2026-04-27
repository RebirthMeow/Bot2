// ==============================================================================
// bot2_svcmds.h - Bot2 server (rcon / dedicated console) commands.
//
// Four developer commands that drive bot2 internals from the server console.
// They are referenced by name from the main g_svcmds.c svcmd table; this
// header just exposes their function pointers so the bodies can live in
// bot2_svcmds.c instead of inline in stock OpenJK source.
//
//   bot_test_routing            - kicks off the 10-variation routing benchmark.
//   bot_wallrun_check           - wallrun diagnostic for the named client.
//   bot_scan_wallruns           - grid-sweep the map for wallrun connections.
//   bot_scan_wallruns_headless  - same but with extra navmesh pre-filtering.
// ==============================================================================
#ifndef __BOT2_SVCMDS_H__
#define __BOT2_SVCMDS_H__

void Svcmd_BotTestRouting_f( void );
void Svcmd_BotWallrunCheck_f( void );
void Svcmd_BotScanWallruns_f( void );
void Svcmd_BotScanWallrunsHeadless_f( void );

#endif

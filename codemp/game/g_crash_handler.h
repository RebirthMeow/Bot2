// ==============================================================================
// g_crash_handler.h - Optional Win32 minidump crash handler.
//
// Installs a Vectored Exception Handler at game DLL init that writes a
// minidump (jampgame_crash.dmp) and a one-line crash log (jampgame_crash.txt)
// when the game DLL faults.  Useful while debugging mods that touch hot paths
// like pmove, navmesh, or AI.  Compiles to no-ops on non-Windows platforms.
//
// Originally lived inline in g_main.c; extracted to keep g_main.c closer to
// stock OpenJK so this fork is easier to drop into other derivatives.
// ==============================================================================
#ifndef __G_CRASH_HANDLER_H__
#define __G_CRASH_HANDLER_H__

void G_CrashHandler_Install( void );
void G_CrashHandler_Shutdown( void );

#endif

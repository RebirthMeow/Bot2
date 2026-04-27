// ==============================================================================
// g_crash_handler.c - Optional Win32 minidump crash handler.
//
// On Windows, installs a Vectored Exception Handler that catches game DLL
// faults (access violation, illegal instruction, divide-by-zero, stack
// overflow, etc.) and writes:
//   - jampgame_crash.dmp - a normal-style minidump for post-mortem debugging
//   - jampgame_crash.txt - a one-line summary appended to the file
//
// The handler returns EXCEPTION_CONTINUE_SEARCH so the platform's crash flow
// still runs (no behaviour change for users — only side-effects on disk).
//
// On non-Windows platforms both entry points compile to empty stubs so the
// rest of the game code can call them unconditionally.
//
// Public surface:
//   G_CrashHandler_Install    - call once from G_InitGame.
//   G_CrashHandler_Shutdown   - call once from G_ShutdownGame to release the
//                               registered handler.
// ==============================================================================

#include "g_local.h"
#include "g_crash_handler.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

typedef BOOL (WINAPI *MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
    CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);

static PVOID s_vehHandle = NULL;

static LONG WINAPI GameCrashHandler( EXCEPTION_POINTERS *pExceptionInfo ) {
    HMODULE hDbgHelp;
    MINIDUMPWRITEDUMP pDump;
    HANDLE hFile;
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    FILE* f;
    static int crashed = 0;

    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if ( code != EXCEPTION_ACCESS_VIOLATION &&
         code != EXCEPTION_ILLEGAL_INSTRUCTION &&
         code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
         code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
         code != EXCEPTION_STACK_OVERFLOW ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if ( crashed ) return EXCEPTION_CONTINUE_SEARCH;
    crashed = 1;

    hDbgHelp = LoadLibraryA( "dbghelp.dll" );
    if ( hDbgHelp ) {
        pDump = (MINIDUMPWRITEDUMP)GetProcAddress( hDbgHelp, "MiniDumpWriteDump" );
        if ( pDump ) {
            hFile = CreateFileA( "jampgame_crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
            if ( hFile != INVALID_HANDLE_VALUE ) {
                mdei.ThreadId = GetCurrentThreadId();
                mdei.ExceptionPointers = pExceptionInfo;
                mdei.ClientPointers = FALSE;
                pDump( GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mdei, NULL, NULL );
                CloseHandle( hFile );
            }
        }
    }

    f = fopen( "jampgame_crash.txt", "a" );
    if ( f ) {
        fprintf( f, "!!! FATAL GAME DLL CRASH DETECTED !!!\n" );
        fprintf( f, "Exception Code: 0x%08X\n", code );
        fprintf( f, "Fault Address: 0x%p\n", pExceptionInfo->ExceptionRecord->ExceptionAddress );
        fflush( f );
        fclose( f );
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void G_CrashHandler_Install( void ) {
    if ( !s_vehHandle ) {
        s_vehHandle = AddVectoredExceptionHandler( 1, GameCrashHandler );
    }
}

void G_CrashHandler_Shutdown( void ) {
    if ( s_vehHandle ) {
        RemoveVectoredExceptionHandler( s_vehHandle );
        s_vehHandle = NULL;
    }
}

#else  // !_WIN32

void G_CrashHandler_Install( void )  { /* no-op outside Windows */ }
void G_CrashHandler_Shutdown( void ) { /* no-op outside Windows */ }

#endif // _WIN32

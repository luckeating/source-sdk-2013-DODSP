//===================== DOD-SP crash log =====================//
//
// The game writes no crash dumps on this setup, so DOD-SP writes its own. When any code in the
// process (engine, Mapbase, DOD-SP) hits a fatal-type CPU exception, this writes, in the mod folder:
//
//   dodsp_crash.txt   what happened, in which DLL and at what offset, the registers, and the
//                     return addresses found on the stack (each as DLL + offset)
//   dodsp_crash.mdmp  a Windows minidump of the same moment (for a debugger + symbols): the stack, and two levels of
//                     memory pointed to from it (up to 48 MB - enough to see a corrupted heap block and its neighbours)
//
// It catches the exception as it is raised (a "first-chance" handler), so it works even when
// something else would kill the process silently. It never handles the exception - the game goes
// on exactly as it would have. At most a few are logged per run; a line says whether the game
// survived it (some engine code raises and handles its own exceptions).
//
// Also logs a line when client.dll unloads while a map is still loaded - an abrupt exit that was
// not a CPU exception (Error(), a forced quit) then still leaves a trace.
//
//=============================================================//

#if defined( _WIN32 ) && !defined( _X360 )
#include "winlite.h"
#include <dbghelp.h>
#endif

#include "cbase.h"
#include "igamesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#if defined( _WIN32 ) && !defined( _X360 )

namespace
{
	char	s_szTxtPath[MAX_PATH] = "dodsp_crash.txt";
	char	s_szDumpPath[MAX_PATH] = "dodsp_crash.mdmp";
	volatile LONG s_nLogged = 0;
	enum { MAX_LOGGED = 8 };	// the dump file always holds the most recent one
	bool	s_bInLevel = false;
	char	s_szMap[64] = "";

	void Append( HANDLE h, const char *psz )
	{
		DWORD n;
		WriteFile( h, psz, (DWORD)Q_strlen( psz ), &n, NULL );
	}

	// "engine.dll+0x1234" for any address in a loaded module
	void DescribeAddress( const void *p, char *pszOut, int nOut )
	{
		HMODULE hMod = NULL;
		if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								 (LPCSTR)p, &hMod ) && hMod )
		{
			char szPath[MAX_PATH];
			GetModuleFileNameA( hMod, szPath, sizeof( szPath ) );
			const char *pszName = Q_strrchr( szPath, '\\' );
			Q_snprintf( pszOut, nOut, "%s+0x%x", pszName ? pszName + 1 : szPath, (unsigned int)( (const char *)p - (const char *)hMod ) );
		}
		else
		{
			Q_snprintf( pszOut, nOut, "0x%p (no module)", p );
		}
	}

	bool InExecutableModule( const void *p )
	{
		MEMORY_BASIC_INFORMATION mbi;
		if ( !VirtualQuery( p, &mbi, sizeof( mbi ) ) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE )
			return false;
		return ( mbi.Protect & ( PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY ) ) != 0;
	}

	bool Readable( const void *p, size_t n )
	{
		MEMORY_BASIC_INFORMATION mbi;
		if ( !VirtualQuery( p, &mbi, sizeof( mbi ) ) || mbi.State != MEM_COMMIT )
			return false;
		if ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) )
			return false;
		return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
	}

	const char *CodeName( DWORD code )
	{
		switch ( code )
		{
		case EXCEPTION_ACCESS_VIOLATION:		return "access violation";
		case EXCEPTION_ILLEGAL_INSTRUCTION:		return "illegal instruction";
		case EXCEPTION_PRIV_INSTRUCTION:		return "privileged instruction";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:		return "integer divide by zero";
		case EXCEPTION_STACK_OVERFLOW:			return "stack overflow";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:	return "array bounds exceeded";
		case EXCEPTION_IN_PAGE_ERROR:			return "in-page error";
		case 0xC0000374:						return "heap corruption";
		default:								return NULL;
		}
	}

	// Memory the dump carries beyond the engine's "indirectly referenced" (256 bytes around each pointer ON THE STACK):
	// two more levels of pointers out from the crashing thread's stack and registers. A heap corruption is seen through
	// a pointer INSIDE an object the crashing code was using (2026-11-21: a soldier's activity-hash bucket array, not on
	// the stack, so not in the dump) - level 1 holds that block and its neighbours, level 2 what THEY point to (the
	// culprit's objects). Static arrays: nothing is allocated inside the exception filter. Budget DUMP_BUDGET bytes.
	enum { MAX_DUMP_RANGES = 24576, DUMP_BUDGET = 48 * 1024 * 1024 };
	struct DumpRange_t { ULONG64 uBase; ULONG uSize; };
	DumpRange_t s_Ranges[MAX_DUMP_RANGES];
	int		s_nRanges = 0, s_iNextRange = 0;
	ULONG	s_uDumpBytes = 0;

	bool AddRange( ULONG_PTR uAt, ULONG uBefore, ULONG uAfter )
	{
		if ( uAt < 0x10000 || s_nRanges >= MAX_DUMP_RANGES || s_uDumpBytes >= (ULONG)DUMP_BUDGET )
			return false;
		ULONG_PTR uStart = ( uAt - MIN( (ULONG_PTR)uBefore, uAt ) ) & ~(ULONG_PTR)15;
		ULONG uSize = ( uBefore + uAfter + 15 ) & ~15u;
		if ( !Readable( (const void *)uStart, uSize ) )
		{
			uStart = uAt & ~(ULONG_PTR)15;		// the start of its page region may be all that's readable
			uSize = 64;
			if ( !Readable( (const void *)uStart, uSize ) )
				return false;
		}
		MEMORY_BASIC_INFORMATION mbi;
		if ( !VirtualQuery( (const void *)uStart, &mbi, sizeof( mbi ) ) || mbi.Type == MEM_IMAGE )
			return false;		// code and module data are in the dump's module list already; heap and stacks only
		s_Ranges[s_nRanges].uBase = uStart;
		s_Ranges[s_nRanges].uSize = uSize;
		s_nRanges++;
		s_uDumpBytes += uSize;
		return true;
	}

	// every dword in [p, p+n) that points at readable non-module memory adds a range around it
	void AddPointersIn( const unsigned char *p, ULONG n, ULONG uBefore, ULONG uAfter )
	{
		for ( ULONG i = 0; i + 4 <= n && s_nRanges < MAX_DUMP_RANGES; i += 4 )
		{
			ULONG_PTR v = *(const ULONG_PTR *)( p + i );
			if ( v >= 0x10000 && v < 0x7fff0000 )
				AddRange( v, uBefore, uAfter );
		}
	}

	void CollectDumpRanges( const CONTEXT *pCtx )
	{
		s_nRanges = s_iNextRange = 0;
		s_uDumpBytes = 0;
		ULONG_PTR uRegs[] = { pCtx->Eax, pCtx->Ebx, pCtx->Ecx, pCtx->Edx, pCtx->Esi, pCtx->Edi, pCtx->Ebp };
		for ( int i = 0; i < (int)ARRAYSIZE( uRegs ); i++ )
			AddRange( uRegs[i], 1024, 1024 );
		// level 1: around everything the stack points at (the top 64 KB of the crashing thread's stack)
		ULONG uStack = 64 * 1024;
		while ( uStack > 4096 && !Readable( (const void *)pCtx->Esp, uStack ) )
			uStack /= 2;
		if ( Readable( (const void *)pCtx->Esp, uStack ) )
			AddPointersIn( (const unsigned char *)pCtx->Esp, uStack, 512, 1024 );
		// level 2: around what those blocks point at
		int nLevel1 = s_nRanges;
		for ( int i = 0; i < nLevel1 && s_nRanges < MAX_DUMP_RANGES && s_uDumpBytes < (ULONG)DUMP_BUDGET; i++ )
			AddPointersIn( (const unsigned char *)(ULONG_PTR)s_Ranges[i].uBase, s_Ranges[i].uSize, 256, 512 );
	}

	BOOL CALLBACK DumpCallback( PVOID, const PMINIDUMP_CALLBACK_INPUT pInput, PMINIDUMP_CALLBACK_OUTPUT pOutput )
	{
		if ( !pInput || !pOutput )
			return FALSE;
		if ( pInput->CallbackType == MemoryCallback )
		{
			if ( s_iNextRange >= s_nRanges )
				return FALSE;		// no more
			pOutput->MemoryBase = s_Ranges[s_iNextRange].uBase;
			pOutput->MemorySize = s_Ranges[s_iNextRange].uSize;
			s_iNextRange++;
			return TRUE;
		}
		return TRUE;		// every other callback: the defaults
	}

	void WriteDump( EXCEPTION_POINTERS *pInfo )
	{
		typedef BOOL ( WINAPI *MiniDumpWriteDump_t )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
			PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION );
		HMODULE hDbgHelp = LoadLibraryA( "dbghelp.dll" );
		MiniDumpWriteDump_t pfn = hDbgHelp ? (MiniDumpWriteDump_t)GetProcAddress( hDbgHelp, "MiniDumpWriteDump" ) : NULL;
		if ( !pfn )
			return;
		HANDLE h = CreateFileA( s_szDumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if ( h == INVALID_HANDLE_VALUE )
			return;
		MINIDUMP_EXCEPTION_INFORMATION mei;
		mei.ThreadId = GetCurrentThreadId();
		mei.ExceptionPointers = pInfo;
		mei.ClientPointers = FALSE;
		CollectDumpRanges( pInfo->ContextRecord );
		MINIDUMP_CALLBACK_INFORMATION cb;
		cb.CallbackRoutine = DumpCallback;
		cb.CallbackParam = NULL;
		pfn( GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)( MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory ), &mei, NULL, &cb );
		CloseHandle( h );
	}

	void LogException( EXCEPTION_POINTERS *pInfo, const char *pszWhat )
	{
		HANDLE h = CreateFileA( s_szTxtPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if ( h == INVALID_HANDLE_VALUE )
			return;

		char sz[512], szWhere[192];
		SYSTEMTIME t;
		GetLocalTime( &t );
		const EXCEPTION_RECORD *pRec = pInfo->ExceptionRecord;
		const CONTEXT *pCtx = pInfo->ContextRecord;

		Q_snprintf( sz, sizeof( sz ), "\r\n==== DOD-SP crash log %04d-%02d-%02d %02d:%02d:%02d  map: %s  (%s)\r\n",
					t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, s_szMap[0] ? s_szMap : "none", pszWhat );
		Append( h, sz );

		DescribeAddress( pRec->ExceptionAddress, szWhere, sizeof( szWhere ) );
		const char *pszCode = CodeName( pRec->ExceptionCode );
		Q_snprintf( sz, sizeof( sz ), "%s (0x%08x) at %s, thread %u\r\n", pszCode ? pszCode : "exception",
					(unsigned int)pRec->ExceptionCode, szWhere, (unsigned int)GetCurrentThreadId() );
		Append( h, sz );
		if ( pRec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRec->NumberParameters >= 2 )
		{
			Q_snprintf( sz, sizeof( sz ), "  tried to %s address 0x%08x\r\n",
						pRec->ExceptionInformation[0] == 0 ? "read" : ( pRec->ExceptionInformation[0] == 8 ? "execute" : "write" ),
						(unsigned int)pRec->ExceptionInformation[1] );
			Append( h, sz );
		}

#if defined( _M_IX86 )
		Q_snprintf( sz, sizeof( sz ), "  eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x\r\n",
					(unsigned int)pCtx->Eax, (unsigned int)pCtx->Ebx, (unsigned int)pCtx->Ecx, (unsigned int)pCtx->Edx,
					(unsigned int)pCtx->Esi, (unsigned int)pCtx->Edi, (unsigned int)pCtx->Ebp, (unsigned int)pCtx->Esp );
		Append( h, sz );

		// Frame-pointer walk (DOD-SP and Mapbase keep frame pointers; the engine may not)
		Append( h, "  call chain (frame pointers):\r\n" );
		const DWORD *pFrame = (const DWORD *)pCtx->Ebp;
		for ( int i = 0; i < 24 && pFrame && Readable( pFrame, 8 ); i++ )
		{
			const void *pRet = (const void *)pFrame[1];
			if ( !InExecutableModule( pRet ) )
				break;
			DescribeAddress( pRet, szWhere, sizeof( szWhere ) );
			Q_snprintf( sz, sizeof( sz ), "    %s\r\n", szWhere );
			Append( h, sz );
			const DWORD *pNext = (const DWORD *)pFrame[0];
			if ( pNext <= pFrame )
				break;
			pFrame = pNext;
		}

		// Raw scan: every value on the top of the stack that points into code - works without frame pointers
		Append( h, "  code addresses on the stack (newest first):\r\n" );
		const DWORD *pStack = (const DWORD *)pCtx->Esp;
		int nFound = 0;
		for ( int i = 0; i < 2048 && nFound < 40; i++ )
		{
			if ( !Readable( pStack + i, 4 ) )
				break;
			const void *pVal = (const void *)pStack[i];
			if ( InExecutableModule( pVal ) )
			{
				DescribeAddress( pVal, szWhere, sizeof( szWhere ) );
				Q_snprintf( sz, sizeof( sz ), "    [esp+0x%04x] %s\r\n", i * 4, szWhere );
				Append( h, sz );
				nFound++;
			}
		}
#endif
		CloseHandle( h );
	}

	LONG CALLBACK FirstChanceHandler( EXCEPTION_POINTERS *pInfo )
	{
		if ( !CodeName( pInfo->ExceptionRecord->ExceptionCode ) )
			return EXCEPTION_CONTINUE_SEARCH;
		if ( InterlockedIncrement( &s_nLogged ) > MAX_LOGGED )
			return EXCEPTION_CONTINUE_SEARCH;
		LogException( pInfo, "raised - if the game kept running, it handled this itself" );
		if ( pInfo->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW )
			WriteDump( pInfo );
		return EXCEPTION_CONTINUE_SEARCH;
	}

	LPTOP_LEVEL_EXCEPTION_FILTER s_pfnPrevFilter = NULL;
	LONG WINAPI UnhandledFilter( EXCEPTION_POINTERS *pInfo )
	{
		HANDLE h = CreateFileA( s_szTxtPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if ( h != INVALID_HANDLE_VALUE )
		{
			char sz[256], szWhere[192];
			DescribeAddress( pInfo->ExceptionRecord->ExceptionAddress, szWhere, sizeof( szWhere ) );
			Q_snprintf( sz, sizeof( sz ), "==== UNHANDLED: exception 0x%08x at %s - this is what ended the game\r\n",
						(unsigned int)pInfo->ExceptionRecord->ExceptionCode, szWhere );
			Append( h, sz );
			CloseHandle( h );
		}
		return s_pfnPrevFilter ? s_pfnPrevFilter( pInfo ) : EXCEPTION_CONTINUE_SEARCH;
	}

	// Installed when client.dll loads, before anything else of ours runs
	struct CInstall
	{
		CInstall()
		{
			AddVectoredExceptionHandler( 1, FirstChanceHandler );
			s_pfnPrevFilter = SetUnhandledExceptionFilter( UnhandledFilter );
		}
		~CInstall()
		{
			// client.dll unloading with a map still loaded = the game was torn down abruptly
			if ( !s_bInLevel )
				return;
			HANDLE h = CreateFileA( s_szTxtPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
			if ( h != INVALID_HANDLE_VALUE )
			{
				char sz[192];
				SYSTEMTIME t;
				GetLocalTime( &t );
				Q_snprintf( sz, sizeof( sz ), "\r\n==== %02d:%02d:%02d client.dll unloaded while map %s was still loaded (exit without a CPU exception)\r\n",
							t.wHour, t.wMinute, t.wSecond, s_szMap );
				Append( h, sz );
				CloseHandle( h );
			}
		}
	};
	CInstall s_Install;

	class CDoDSPCrashLog : public CAutoGameSystem
	{
	public:
		CDoDSPCrashLog() : CAutoGameSystem( "CDoDSPCrashLog" ) {}
		virtual bool Init()
		{
			Q_snprintf( s_szTxtPath, sizeof( s_szTxtPath ), "%s\\dodsp_crash.txt", engine->GetGameDirectory() );
			Q_snprintf( s_szDumpPath, sizeof( s_szDumpPath ), "%s\\dodsp_crash.mdmp", engine->GetGameDirectory() );
			return true;
		}
		virtual void LevelInitPostEntity()
		{
			Q_FileBase( engine->GetLevelName(), s_szMap, sizeof( s_szMap ) );
			s_bInLevel = true;
		}
		virtual void LevelShutdownPostEntity()
		{
			s_bInLevel = false;
		}
	};
	CDoDSPCrashLog s_CrashLog;
}

CON_COMMAND( dodsp_crashlog_where, "DOD-SP: where the crash log is written" )
{
	Msg( "DOD-SP crash log: %s (dump: %s), %d logged this run\n", s_szTxtPath, s_szDumpPath, (int)s_nLogged );
}

#endif // _WIN32

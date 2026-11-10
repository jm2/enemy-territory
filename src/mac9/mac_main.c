#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "../client/client.h"
#include "mac_local.h"
#undef vsnprintf

// String helpers
int PStringToCString( char *s ) {
    int len = (unsigned char)s[0];
    int i;
    for (i=0; i<len; i++) s[i] = s[i+1];
    s[len] = 0;
    return len;
}

int CStringToPString( char *s ) {
    int len = strlen(s);
    if (len > 255) len = 255;
    int i;
    for (i=len; i>0; i--) s[i] = s[i-1];
    s[0] = (char)len;
    return len;
}

// Event Queue
#define MAX_MAC_EVENTS 256
static sysEvent_t eventQue[MAX_MAC_EVENTS];
static int eventHead = 0;
static int eventTail = 0;
static int eventOverflows = 0;	// events dropped since Sys_GetEvent last reported
static byte sys_keyDown[256];	// last queued state of each key (K_LAST_KEY < 256)

static qboolean Sys_IsKeyRelease( const sysEvent_t *ev ) {
    return ev->evType == SE_KEY && !ev->evValue2;
}

/*
==================
Sys_DropQueuedEvent

Makes room in a full queue (#18).  As on the other platforms the oldest event
goes and its Z_Malloc payload is freed, except that a key release is kept:
dropping one whose press was already delivered would leave that key down.  So
the oldest event that is not a release goes or, if every queued event is a
release, the oldest release of a key that is released again later.  Nothing
is printed here: Sys_GetEvent reports the count.
==================
*/
static void Sys_DropQueuedEvent( void ) {
    int drop, i, prev;

    for ( drop = eventTail ; drop != eventHead ; drop = (drop + 1) % MAX_MAC_EVENTS ) {
        if ( !Sys_IsKeyRelease( &eventQue[drop] ) ) {
            break;
        }
    }
    if ( drop == eventHead ) {
        for ( drop = eventTail ; drop != eventHead ; drop = (drop + 1) % MAX_MAC_EVENTS ) {
            for ( i = (drop + 1) % MAX_MAC_EVENTS ; i != eventHead ; i = (i + 1) % MAX_MAC_EVENTS ) {
                if ( eventQue[i].evValue == eventQue[drop].evValue ) {
                    break;
                }
            }
            if ( i != eventHead ) {
                break;
            }
        }
        if ( drop == eventHead ) {
            drop = eventTail;
        }
    }

    if ( eventQue[drop].evPtr ) {
        Z_Free( eventQue[drop].evPtr );
    }
    // close the gap: move the older events one slot toward the head
    for ( i = drop ; i != eventTail ; i = prev ) {
        prev = (i + MAX_MAC_EVENTS - 1) % MAX_MAC_EVENTS;
        eventQue[i] = eventQue[prev];
    }
    eventTail = (eventTail + 1) % MAX_MAC_EVENTS;
    eventOverflows++;
}

void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr ) {
    sysEvent_t *ev;
    int next = (eventHead + 1) % MAX_MAC_EVENTS;

    if (next == eventTail) {
        Sys_DropQueuedEvent();
    }

    if ( type == SE_KEY && value >= 0 && value < (int)sizeof( sys_keyDown ) ) {
        sys_keyDown[value] = ( value2 != 0 );
    }

    // time == 0 means "now" (contract from the other ports); InputSprocket
    // mouse/button events are queued with 0 and were getting timestamp 0.
    if ( !time ) {
        time = Sys_Milliseconds();
    }

    ev = &eventQue[eventHead];
    ev->evTime = time;
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;
    
    eventHead = next;
}

/*
==================
Sys_ReleaseKeys

Queues a release for every key whose last queued event was a press.  Called
on suspend (#291): the key-ups for keys held then go to the front process.
==================
*/
void Sys_ReleaseKeys( void ) {
    int key;

    for ( key = 0 ; key < (int)sizeof( sys_keyDown ) ; key++ ) {
        if ( sys_keyDown[key] ) {
            Sys_QueEvent( 0, SE_KEY, key, qfalse, 0, NULL );
        }
    }
}

// mac_event.c
void Sys_SendKeyEvents( void );
// mac_input.c
// mac_input.c
void Sys_Input( void );

// Retro68 Console Log Buffer
#define RETRO_LOG_SIZE (256 * 1024)
static char retroLogBuffer[RETRO_LOG_SIZE];
static int retroLogHead = 0;
static int retroLogTotal = 0;

// Sys_LogPrintf implementation
//
// Writes to the in-memory ring buffer and the boot log (mac_bootlog.c), and to
// the Retro68 console window once that is shown (Sys_ConsoleWanted,
// mac_consolehooks.cc). The ring reaches the disk through Sys_DumpRetroLogs,
// which is called from Sys_Quit, Sys_Error, and the Cmd-D panic-key handler.
// Each line of the boot log is a disk write, so keep this out of loops that
// run every frame.
// Append raw text to the in-memory ring and the boot log (no console output).
// Used by Sys_Print so that ALL engine console output reaches the crash-dump
// ring even when the on-screen console is hidden (viewlog 0).
void Sys_LogRecord( const char *text ) {
    int len = strlen( text );
    int i;

    Sys_BootLogWrite( text );

    for ( i = 0; i < len; i++ ) {
        retroLogBuffer[retroLogHead] = text[i];
        retroLogHead = (retroLogHead + 1) % RETRO_LOG_SIZE;
        if (retroLogTotal < RETRO_LOG_SIZE) retroLogTotal++;
    }
}

void Sys_LogPrintf( const char *fmt, ... ) {
    va_list argptr;
    char text[1024];

    va_start( argptr, fmt );
    vsnprintf( text, sizeof(text), fmt, argptr );
    va_end( argptr );

    printf("%s", text);
    Sys_LogRecord( text );
}

void Sys_DumpRetroLogs( const char *fileName ) {
    FILE *fp;
    int idx, count;

    fp = fopen( fileName, "wb" );
    if ( !fp ) {
        printf( "Sys_DumpRetroLogs: Failed to open %s\n", fileName );
        return;
    }

    // Write the ring in at most two contiguous spans (per-byte fwrite took
    // a File Manager trap per character).
    if (retroLogTotal < RETRO_LOG_SIZE) {
        idx = 0;
        count = retroLogTotal;
        fwrite( &retroLogBuffer[idx], 1, count, fp );
    } else {
        idx = retroLogHead;
        count = RETRO_LOG_SIZE;
        fwrite( &retroLogBuffer[idx], 1, RETRO_LOG_SIZE - idx, fp );
        fwrite( &retroLogBuffer[0], 1, idx, fp );
    }

    fclose( fp );
    printf( "Sys_DumpRetroLogs: Dumped %d bytes to %s\n", count, fileName );
}

// Debug Breadcrumb
void Debug_Breadcrumb( int color ) {
    Rect r;
    // Draw a small square in the top-left corner
    SetRect(&r, 0, 0, 10, 10);
    ForeColor(color);
    PaintRect(&r);
    ForeColor(blackColor);
}

sysEvent_t Sys_GetEvent( void ) {
    sysEvent_t ev;
    KeyMap keys;
    unsigned char *k;
    
    // Panic Key Dump: Command (55) + D (2)
    GetKeys(keys);
    k = (unsigned char *)keys;
    // Command is key 55. 55/8=6, 55%8=7. Bit check: (1<<(55%8)) = 0x80
    // D is key 2. 2/8=0, 2%8=2. Bit check: (1<<2) = 0x04
    if ( (k[6] & 0x80) && (k[0] & 0x04) ) {
        static int lastDump = 0;
        int now = Sys_Milliseconds();
        if (now - lastDump > 2000) {
            Com_Printf("PANIC KEY DETECTED - DUMPING FLIGHT RECORD\n");
            Com_DumpFlightRecord("flight_record.txt");
            Sys_DumpRetroLogs("retro68_console.txt");
            SysBeep(30); 
            lastDump = now;
        }
    }
    
    /*
    // Auto Dump loop disabled - using synchronous append logging instead
    {
        static int lastAutoDump = 0;
        int now = Sys_Milliseconds();
        if (now - lastAutoDump > 100) {
           Com_DumpFlightRecord("flight_record_auto.txt"); 
           Sys_DumpRetroLogs("retro68_console_auto.txt");
           lastAutoDump = now;
        }
    }
    */

    Sys_PumpEvents();

    if ( eventOverflows ) {
        Com_Printf( "Sys_QueEvent: overflow, dropped %i events\n", eventOverflows );
        eventOverflows = 0;
    }

    if (eventHead == eventTail) {
        memset( &ev, 0, sizeof(ev) );
        ev.evType = SE_NONE;
        ev.evTime = Sys_Milliseconds();
        // Keep the tick->msec conversion bases in sync. Sys_MsecForMacEvent
        // extrapolates event timestamps from these; they were never updated,
        // so event times and Sys_Milliseconds ran on two unrelated clocks.
        sys_ticBase = TickCount();
        sys_msecBase = ev.evTime;
        return ev;
    }
    
    ev = eventQue[eventTail];
    eventTail = (eventTail + 1) % MAX_MAC_EVENTS;
    return ev;
}


// ==========================================
// Main Entry Point and System Routines
// ==========================================

int		sys_ticBase;
int		sys_msecBase;
int		sys_lastEventTic;

void Sys_Init( void ) {
    Com_FlightRecord("Sys_Init: Flight Recorder Start.\n");
    Sys_InitConsole();

    // The system event mask excludes key-up events by default on classic
    // Mac OS; without this, DoKeyUp never fires and every key/+action
    // latches down permanently.
    SetEventMask( everyEvent );

    // Read every frame in Sys_SendKeyEvents once DSp fullscreen is active;
    // was declared but never registered (NULL deref in fullscreen).
    sys_waitNextEvent = Cvar_Get( "sys_waitNextEvent", "0", CVAR_ARCHIVE );

    // the Finder's Open Application, Open Documents and Quit (#19)
    Sys_InitAppleEvents();

    Sys_InitNetworking();
    Sys_InitInput();
}

void Sys_Quit( void ) {
    // CL_Shutdown has normally closed the sound channel already; a quit
    // during an error has not, and its interrupt-time callback must not
    // outlive the application
    SNDDMA_Shutdown();
    Sys_ShutdownInput();
    Sys_ShutdownNetworking();

    // Persist the session log instead of blocking in getchar(): under a
    // captured DSp display (or with the console hidden) the old prompt
    // could never be seen or answered, so every quit and fatal error
    // presented as a machine hang.
    Sys_DumpRetroLogs( "retro68_console.txt" );
    Sys_BootLogClose();

    exit( 0 );
}

void Sys_Error( const char *error, ... ) {
    va_list argptr;
    char    text[1024];
    Str255  title, message;
    int     length;

    va_start( argptr, error );
    vsnprintf( text, sizeof(text), error, argptr );
    va_end( argptr );

    Com_FlightRecord("Sys_Error: %s\n", text);
    Com_DumpFlightRecord("crashdump.txt");

    // The crash log first: stderr opens the console window if it is not
    // open yet, which allocates, and a fatal error often leaves the heap
    // nearly full.
    Sys_LogRecord( "Sys_Error: " );
    Sys_LogRecord( text );
    Sys_LogRecord( "\n" );
    Sys_DumpRetroLogs("retro68_console_crash.txt");
    Sys_BootLogFlush();
    // Com_Error's CL_Shutdown has normally released the display; a recursive
    // error has not, and nothing could be seen or answered on a captured one
    GLimp_ReleaseDisplay();
    fprintf( stderr, "Sys_Error: %s\n", text );
    // a recursive error skips CL_Shutdown: stop the channel looping the
    // last mixed buffer under the alert, and close it before exit
    SNDDMA_Shutdown();
    Sys_ShutdownInput();
    Sys_ShutdownNetworking();

    // As retail, a Stop alert (ALRT 128, mac_resources.r) keeps the message
    // on screen until it is dismissed.
    strcpy( (char *)title + 1, "Enemy Territory Error:" );
    title[0] = strlen( (char *)title + 1 );
    length = strlen( text );
    if ( length > 255 ) {
        length = 255;
    }
    message[0] = length;
    memcpy( message + 1, text, length );
    ParamText( title, message, message, message );
    StopAlert( 128, NULL );
    Sys_BootLogClose();
    exit( 1 );
}

// Time
//
// Use the full 64-bit Microseconds() value, based at first call. The old
// implementation used only micros.lo, which wraps every ~71.6 minutes and
// jumps backwards when it does; Q3 assumes a monotonic millisecond clock.
int Sys_Milliseconds( void ) {
    UnsignedWide micros;
    unsigned long long now;
    static unsigned long long base;

    Microseconds(&micros);
    now = ((unsigned long long)micros.hi << 32) | micros.lo;
    if (!base) {
        base = now;
    }
    return (int)((now - base) / 1000);
}

// The least predictable bits classic Mac OS has, for Netchan_Challenge:
// the microseconds since startup, absolute, unlike Sys_Milliseconds,
// and the ticks.
unsigned Sys_Entropy( void ) {
    UnsignedWide micros;

    Microseconds(&micros);
    return micros.lo ^ ((unsigned)micros.hi << 16) ^ ((unsigned)TickCount() << 24);
}

/*
==================
Sys_PumpEvents

Queues what has arrived: at most one Event Manager event, the InputSprocket
mouse, a dedicated server's console line, and one network packet.  Called by
Sys_GetEvent and, as on retail, by the renderer during long frames
(tr_backend.c), so that key releases, updates and suspend, resume and Apple
Events are not left waiting (#19).  It was empty, so those calls did
nothing.  Nothing it calls pumps again.
==================
*/
void Sys_PumpEvents( void ) {
    static byte sys_packetReceived[MAX_MSGLEN];
    msg_t       netmsg;
    netadr_t    adr;
    char        *s;

    // Pump Mac OS events (keyboard via WaitNextEvent)
    Sys_SendKeyEvents();
    // Pump InputSprocket events (mouse)
    Sys_Input();

    // A dedicated server's console commands, as id's Sys_PumpEvents and
    // unix_main.c read them
    s = Sys_ConsoleInput();
    if ( s ) {
        char    *b;
        int     len;

        len = strlen( s ) + 1;
        b = Z_Malloc( len );
        strcpy( b, s );
        Sys_QueEvent( 0, SE_CONSOLE, 0, 0, len, b );
    }

    // Check for network packets and queue them as SE_PACKET (same pattern
    // as unix_main.c). Without this, Sys_GetPacket had no caller at all and
    // the engine could never receive UDP traffic.
    MSG_Init( &netmsg, sys_packetReceived, sizeof( sys_packetReceived ) );
    if ( Sys_GetPacket( &adr, &netmsg ) ) {
        netadr_t  *buf;
        int       len;

        // copy out to a separate buffer for queuing; freed by Com_EventLoop
        len = sizeof( netadr_t ) + netmsg.cursize;
        buf = Z_Malloc( len );
        *buf = adr;
        memcpy( buf+1, netmsg.data, netmsg.cursize );
        Sys_QueEvent( 0, SE_PACKET, 0, 0, len, buf );
    }
}

// Yield to system to prevent timing race conditions
// This must provide timing equivalent to printf("...") + fflush(stdout)
// Key: We must actually WRITE to stdout to trigger I/O, not just flush empty buffer
void Sys_Yield( void ) {
    SystemTask();
    // Actually write to stdout like printf does - use space+backspace to be invisible
    printf(" \b");
    fflush(stdout);
    SystemTask();
}

// Return the HFS path of the directory containing the running application.
//
// Primary path: GetCurrentProcess + GetProcessInformation gives us an FSSpec
// for the running app, then we walk the dirID chain to the volume root and
// build "Volume:Folder1:Folder2" with no trailing colon (FS_BuildOSPath glues
// ":qpath" onto the result and a double colon is parent-directory).
//
// A volume root is the exception: it is "Volume:" (issue #267). A bare
// "Volume" has no colon, so HFS takes it as an item in the default
// directory, not as the volume; paths joined onto it still resolve, but
// catalog lookups of the base path itself (Sys_ListFiles, so the Mods menu
// and dir) fail. FS_BuildOSPath and Sys_JoinHFSPath add no second colon
// after it.
//
// Fallback: some Process Manager configurations (debuggers, certain
// emulator paths, very early call sites before the app is fully registered)
// hand back noErr from GetProcessInformation but never populate the FSSpec.
// We detect that with a sanity check on (vRefNum, parID) and fall back to
// GetVol, which is always correct for "the volume the app is running from"
// even if it loses the per-folder structure. That path is a volume root,
// "Volume:".
//
// FS_BuildOSPath normalizes forward slashes, so caller paths like
// "/baseq3/q3key/" concatenate correctly with the result either way.
char *Sys_GetCwd( void ) {
    static char     cached[512];
    ProcessSerialNumber psn;
    ProcessInfoRec  pinfo;
    FSSpec          appSpec;
    CInfoPBRec      cipb;
    Str63           name;
    Str255          volName;
    char            segment[64];
    char            scratch[512];
    OSErr           err;
    long            dirID;
    short           vRefNum;
    int             segLen;

    if ( cached[0] != 0 ) {
        return cached;
    }

    err = GetCurrentProcess( &psn );
    if ( err != noErr ) {
        Sys_Error( "Sys_GetCwd: GetCurrentProcess failed: %d", err );
    }

    // Zero before the call. The Toolbox documents most fields as outputs but
    // some Process Manager versions trip if input fields are uninitialized.
    memset( &pinfo,   0, sizeof( pinfo ) );
    memset( &appSpec, 0, sizeof( appSpec ) );
    pinfo.processInfoLength = sizeof( pinfo );
    pinfo.processName       = NULL;
    pinfo.processAppSpec    = &appSpec;

    err = GetProcessInformation( &psn, &pinfo );
    if ( err != noErr ) {
        Sys_Error( "Sys_GetCwd: GetProcessInformation failed: %d", err );
    }

    // Sanity check the FSSpec. A real spec has vRefNum != 0 (volumes are
    // numbered with negative shorts on Mac OS Classic) and parID > 0
    // (fsRtParID = 1, fsRtDirID = 2; legitimate parIDs grow from there).
    // If either is bogus, GetProcessInformation didn't actually populate
    // the spec; fall back to GetVol.
    if ( appSpec.vRefNum == 0 || appSpec.parID <= 0 ) {
        Com_FlightRecord( "Sys_GetCwd: Process Manager returned empty FSSpec "
                          "(vRefNum=%d parID=%ld); falling back to GetVol\n",
                          appSpec.vRefNum, appSpec.parID );
        err = GetVol( volName, &vRefNum );
        if ( err != noErr ) {
            Sys_Error( "Sys_GetCwd: GetVol fallback failed: %d", err );
        }
        segLen = volName[0];
        if ( segLen >= (int)sizeof( cached ) - 1 ) {
            segLen = sizeof( cached ) - 2;
        }
        memcpy( cached, &volName[1], segLen );
        cached[segLen++] = ':';
        cached[segLen] = 0;
        Com_FlightRecord( "Sys_GetCwd (GetVol fallback): '%s'\n", cached );
        return cached;
    }

    // Walk from the application's parent directory up to the volume root,
    // prepending each directory name. PBGetCatInfoSync with ioFDirIndex = -1
    // and ioDrDirID set asks for the directory's own catalog entry, which
    // gives us its name and its parent's dirID.
    cached[0] = 0;
    vRefNum = appSpec.vRefNum;
    dirID   = appSpec.parID;

    while ( dirID != fsRtParID ) {
        memset( &cipb, 0, sizeof( cipb ) );
        cipb.dirInfo.ioNamePtr   = name;
        cipb.dirInfo.ioVRefNum   = vRefNum;
        cipb.dirInfo.ioDrDirID   = dirID;
        cipb.dirInfo.ioFDirIndex = -1;

        err = PBGetCatInfoSync( &cipb );
        if ( err != noErr ) {
            Sys_Error( "Sys_GetCwd: PBGetCatInfoSync(vRefNum=%d dirID=%ld) "
                       "failed: %d", vRefNum, dirID, err );
        }

        segLen = name[0];
        if ( segLen > (int)sizeof( segment ) - 1 ) {
            segLen = sizeof( segment ) - 1;
        }
        memcpy( segment, &name[1], segLen );
        segment[segLen] = 0;

        if ( cached[0] == 0 ) {
            Q_strncpyz( cached, segment, sizeof( cached ) );
        } else {
            Com_sprintf( scratch, sizeof( scratch ), "%s:%s", segment, cached );
            Q_strncpyz( cached, scratch, sizeof( cached ) );
        }

        dirID = cipb.dirInfo.ioDrParID;
    }

    if ( cached[0] == 0 ) {
        Sys_Error( "Sys_GetCwd: produced empty path (vRefNum=%d, parID=%ld)",
                   appSpec.vRefNum, appSpec.parID );
    }

    // the application is at the root of its volume
    if ( !strchr( cached, ':' ) ) {
        Q_strcat( cached, sizeof( cached ), ":" );
    }

    Com_FlightRecord( "Sys_GetCwd (FSSpec walk): '%s'\n", cached );
    return cached;
}

/*
=================
Sys_JoinHFSPath

dest = directory:leaf. A volume root from Sys_GetCwd ("Vol:") already ends
in the separator, and a second colon would name its parent (issue #267).
=================
*/
static void Sys_JoinHFSPath( char *dest, int size, const char *directory,
                             const char *leaf ) {
    int length = strlen( directory );

    snprintf( dest, size, "%s%s%s", directory,
              length > 0 && directory[length - 1] == ':' ? "" : ":", leaf );
}

char *Sys_DefaultCDPath( void ) {
    return "";
}

char *Sys_DefaultBasePath( void ) {
    return Sys_GetCwd();
}

// Stubs for missing symbols
// Streamed-file shims: same synchronous fallback the unix port uses (no
// background reader thread). The old stubs returned 0 from StreamedRead,
// which broke every RoQ cinematic and streamed-music read.
void Sys_BeginStreamedFile( int handle, int readAhead ) {}
void Sys_EndStreamedFile( int handle ) {}
int Sys_StreamedRead( void *buffer, int size, int count, int handle ) {
    return FS_Read( buffer, size * count, handle );
}
qboolean Sys_LowPhysicalMemory( void ) { return qfalse; }

cvar_t *sys_waitNextEvent;

#define MAX_FOUND_FILES 0x1000

// Helper: Convert C path to FSSpec
static OSErr PathToFSSpec(const char *path, FSSpec *spec) {
    Str255 ppath;
    OSErr err;
    int len = strlen(path);
    if (len > 255) len = 255;
    ppath[0] = len;
    memcpy(&ppath[1], path, len);
    err = FSMakeFSSpec(0, 0, ppath, spec);
    if (err != noErr && err != fnfErr) { // fnfErr is okay for checking existence
        //printf("DEBUG: PathToFSSpec failed for '%s', err=%d\n", path, err);
    } else {
        //printf("DEBUG: PathToFSSpec success for '%s', vRefNum=%d, parID=%ld, err=%d\n", path, spec->vRefNum, spec->parID, err);
    }
    return err;
}

/*
=================
Sys_GetDirectoryID

Resolve an HFS path to the catalog directory ID needed for indexed
PBGetCatInfoSync calls. FSSpec.parID is the parent ID, not the requested
directory's ID, so it must not be used directly for enumeration.
=================
*/
static qboolean Sys_GetDirectoryID( const char *directory, short *vRefNum,
                                    long *dirID ) {
    FSSpec spec;
    CInfoPBRec pb;
    char volume[MAX_OSPATH];
    OSErr err;

    if ( !directory || !directory[0] ) {
        return HGetVol( NULL, vRefNum, dirID ) == noErr;
    }

    // A colon-free base path, such as a hand-set fs_basepath "Vol", names a
    // volume, as it does in FS_BuildOSPath's "Vol:baseq3"; HFS alone would
    // look for "Vol" in the default directory (issue #267).
    if ( !strchr( directory, ':' ) ) {
        Com_sprintf( volume, sizeof( volume ), "%s:", directory );
        directory = volume;
    }

    err = PathToFSSpec( directory, &spec );
    if ( err != noErr ) {
        return qfalse;
    }

    memset( &pb, 0, sizeof( pb ) );
    pb.hFileInfo.ioNamePtr = spec.name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    pb.hFileInfo.ioFDirIndex = 0;

    err = PBGetCatInfoSync( &pb );
    if ( err != noErr || !(pb.hFileInfo.ioFlAttrib & ioDirMask) ) {
        return qfalse;
    }

    *vRefNum = spec.vRefNum;
    *dirID = pb.dirInfo.ioDrDirID;
    return qtrue;
}

/*
=================
Sys_ListFilteredDirectory

Classic File Manager equivalent of the recursive Unix filtered-file walk.
Catalog names are converted to C strings, while returned relative paths use
Q3's portable '/' separator for Com_FilterPath.
=================
*/
static void Sys_ListFilteredDirectory( short vRefNum, long dirID,
                                       const char *subdirs, char *filter,
                                       char **list, int *numfiles ) {
    int index;

    for ( index = 1; *numfiles < MAX_FOUND_FILES - 1; index++ ) {
        CInfoPBRec pb;
        Str255 name;
        OSErr err;
        qboolean isDir;
        long childDirID;
        char filename[MAX_OSPATH];
        char newsubdirs[MAX_OSPATH];

        memset( &pb, 0, sizeof( pb ) );
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vRefNum;
        pb.hFileInfo.ioDirID = dirID;
        pb.hFileInfo.ioFDirIndex = index;

        err = PBGetCatInfoSync( &pb );
        if ( err != noErr ) {
            break;
        }

        isDir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
        childDirID = isDir ? pb.dirInfo.ioDrDirID : 0;
        PStringToCString( (char *)name );

        if ( subdirs[0] ) {
            Com_sprintf( filename, sizeof( filename ), "%s/%s",
                         subdirs, (char *)name );
        } else {
            Q_strncpyz( filename, (char *)name, sizeof( filename ) );
        }

        if ( isDir ) {
            Q_strncpyz( newsubdirs, filename, sizeof( newsubdirs ) );
            Sys_ListFilteredDirectory( vRefNum, childDirID, newsubdirs,
                                       filter, list, numfiles );
        }

        if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
            break;
        }
        if ( !Com_FilterPath( filter, filename, qfalse ) ) {
            continue;
        }

        list[*numfiles] = CopyString( filename );
        (*numfiles)++;
    }
}

char **Sys_ListFiles( const char *directory, const char *extension, char *filter,
                      int *numfiles, qboolean wantsubs ) {
    char *list[MAX_FOUND_FILES];
    char **listCopy;
    short vRefNum;
    long dirID;
    qboolean dironly = wantsubs;
    int extensionLength;
    int nfiles = 0;
    int index;
    int i;

    *numfiles = 0;

    if ( !Sys_GetDirectoryID( directory, &vRefNum, &dirID ) ) {
        return NULL;
    }

    if ( filter ) {
        Sys_ListFilteredDirectory( vRefNum, dirID, "", filter, list, &nfiles );
    } else {
        if ( !extension ) {
            extension = "";
        }
        if ( extension[0] == '/' && extension[1] == 0 ) {
            extension = "";
            dironly = qtrue;
        }
        extensionLength = strlen( extension );

        for ( index = 1; nfiles < MAX_FOUND_FILES - 1; index++ ) {
            CInfoPBRec pb;
            Str255 name;
            OSErr err;
            qboolean isDir;
            int nameLength;

            memset( &pb, 0, sizeof( pb ) );
            pb.hFileInfo.ioNamePtr = name;
            pb.hFileInfo.ioVRefNum = vRefNum;
            pb.hFileInfo.ioDirID = dirID;
            pb.hFileInfo.ioFDirIndex = index;

            err = PBGetCatInfoSync( &pb );
            if ( err != noErr ) {
                break;
            }

            isDir = (pb.hFileInfo.ioFlAttrib & ioDirMask) != 0;
            if ( (dironly && !isDir) || (!dironly && isDir) ) {
                continue;
            }

            nameLength = PStringToCString( (char *)name );
            if ( extensionLength &&
                 (nameLength < extensionLength ||
                  Q_stricmp( (char *)name + nameLength - extensionLength,
                             extension )) ) {
                continue;
            }

            list[nfiles++] = CopyString( (char *)name );
        }
    }

    *numfiles = nfiles;
    if ( !nfiles ) {
        return NULL;
    }

    listCopy = Z_Malloc( (nfiles + 1) * sizeof( *listCopy ) );
    for ( i = 0; i < nfiles; i++ ) {
        listCopy[i] = list[i];
    }
    listCopy[i] = NULL;
    return listCopy;
}

// OLD IMPLEMENTATION BELOW - COMMENTED OUT
#if 0
char **Sys_ListFiles_OLD( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs ) {
    FSSpec dirSpec;
    CInfoPBRec pb;
    Str255 name;
    OSErr err;
    short vRefNum;
    long dirID;
    int nfiles = 0;
    char *list[MAX_FOUND_FILES];
    char **listCopy;
    int i;
    int extLen;
    qboolean dironly = wantsubs;
    
    // //printf("DEBUG: Sys_ListFiles dir='%s' ext='%s'\n", directory, extension);
    
    *numfiles = 0;
    
    // Convert path to FSSpec
    err = PathToFSSpec(directory, &dirSpec);
    if (err != noErr && err != fnfErr) {
        //printf("DEBUG: Sys_ListFiles: PathToFSSpec failed for '%s'\n", directory);
        return NULL;
    }
    
    // FIX: FSMakeFSSpec returning -1.
    // Try 0 (Default Volume) first.
    if (dirSpec.vRefNum == -1) {
        //printf("DEBUG: Sys_ListFiles: FSMakeFSSpec returned vRefNum=-1. Forcing to 0.\n");
        dirSpec.vRefNum = 0;
    }
    
    // Get directory ID
    memset(&pb, 0, sizeof(pb));
    pb.dirInfo.ioNamePtr = dirSpec.name;
    pb.dirInfo.ioVRefNum = dirSpec.vRefNum;
    pb.dirInfo.ioDrDirID = dirSpec.parID;
    pb.dirInfo.ioFDirIndex = 0;
    
    err = PBGetCatInfoSync(&pb);
    if (err != noErr) {
        //printf("DEBUG: Sys_ListFiles: PBGetCatInfoSync (DirInfo) failed for '%s' (vRef=%d, parID=%ld), err=%d\n", directory, dirSpec.vRefNum, dirSpec.parID, err);
        
        // Fallback: PROBE the current directory.
        short probeVRefNum = 0;
        long probeDirID = 0;
        Str255 currentVolName;
        
        // 1. Get the current Volume Name and WDRefNum
        if (GetVol(currentVolName, &probeVRefNum) == noErr) {
             //printf("DEBUG: Sys_ListFiles: GetVol -> vRef=%d, Name Len=%d\n", probeVRefNum, currentVolName[0]);
             
             // 2. Get the Directory ID (and repeat vRef)
             short hVRef;
             if (HGetVol(NULL, &hVRef, &probeDirID) != noErr) {
                 //printf("DEBUG: Sys_ListFiles: HGetVol Failed! Cannot proceed.\n");
                 return NULL;
             }
             //printf("DEBUG: Sys_ListFiles: HGetVol -> dirID=%ld\n", probeDirID);
             
             // 3. Find the REAL VRefNum for current volume using PBHGetVInfoSync
             HParamBlockRec volPB;
             Str255 volNameBuffer;
             short realVRefNum = 0;
             int volIndex = 1;
             
             //printf("DEBUG: Sys_ListFiles: List of Mounted Volumes (PBHGetVInfoSync):\n");
             while (1) {
                 memset(&volPB, 0, sizeof(volPB));
                 volPB.volumeParam.ioNamePtr = volNameBuffer;
                 volPB.volumeParam.ioVolIndex = volIndex;
                 volPB.volumeParam.ioVRefNum = 0; // Essential for indexing mode
                 
                 err = PBHGetVInfoSync(&volPB);
                 if (err != noErr) {
                     if (volIndex == 1) //printf("DEBUG: Sys_ListFiles: PBHGetVInfoSync failed on FIRST index! err=%d\n", err);
                     break; // End of volumes
                 }
                 
                 // Convert Pascal to C for print
                 char vName[256];
                 int vLen = volNameBuffer[0];
                 memcpy(vName, &volNameBuffer[1], vLen);
                 vName[vLen] = 0;
                 
                 //printf("DEBUG: Sys_ListFiles: Vol %d: '%s' (vRef=%d)\n", volIndex, vName, volPB.volumeParam.ioVRefNum);
                 
                 if (volPB.volumeParam.ioNamePtr[0] == currentVolName[0] &&
                     memcmp(&volPB.volumeParam.ioNamePtr[1], &currentVolName[1], currentVolName[0]) == 0) {
                     realVRefNum = volPB.volumeParam.ioVRefNum;
                 }
                 volIndex++;
             }
             
             // LOGIC:
             // 1. If we found a Real VRefNum via name matching, use it.
             // 2. If 'probeVRefNum' is a WDRefNum (-32xxx) and we found NO Real VRefNum:
             //    ---> USE THE WDRefNUm, but FORCE DirID = 0.
             
             if (realVRefNum != 0) {
                 //printf("DEBUG: Sys_ListFiles: Using Real VRefNum %d and HGetVol DirID %ld\n", realVRefNum, probeDirID);
                 probeVRefNum = realVRefNum;
             } else if (probeVRefNum < -32000) {
                 //printf("DEBUG: Sys_ListFiles: WDRefNum %d detected. Volume List failed or unneeded. Forcing DirID to 0 to use WD Context.\n", probeVRefNum);
                 // Do NOT blindly force to -1. Trust the WDRefNum from GetVol.
                 // probeVRefNum remains -32xxx
                 probeDirID = 0; 
             } else {
                  //printf("DEBUG: Sys_ListFiles: No better VRefNum found. Using original.\n");
             }
             


             // 4. Perform Probe with Clean VRef and DirID
             
             //printf("DEBUG: Sys_ListFiles: PROBE START using vRef=%d, dirID=%ld\n", probeVRefNum, probeDirID);

             int probeIndex = 1;
             int safety = 0;
             int safetyProbeAttempted = 0;
             Str255 probeName;
             
restart_probe:
             while (safety < 50) { 
                 memset(&pb, 0, sizeof(pb));
                 pb.dirInfo.ioNamePtr = probeName;
                 pb.dirInfo.ioVRefNum = probeVRefNum;
                 pb.dirInfo.ioDrDirID = probeDirID;
                 pb.dirInfo.ioFDirIndex = probeIndex;
                 
                 err = PBGetCatInfoSync(&pb);
                 if (err != noErr) {
                     if (err == fnfErr) //printf("DEBUG: Sys_ListFiles: PROBE END (End of Dir).\n");
                     else //printf("DEBUG: Sys_ListFiles: PROBE ERROR index=%d, err=%d\n", probeIndex, err);
                     
                     // SAFETY PROBE: If the first item failed with error...
                     if (probeIndex == 1 && err != fnfErr && !safetyProbeAttempted) {
                         // Fallback Levels:
                         // 1. Try Root Directory of the SAME Volume (DirID = 2)
                         // 2. Try Default Context (vRef=0, dirID=0)
                         
                         if (probeDirID != 0 && probeDirID != 2) {
                             //printf("DEBUG: Sys_ListFiles: Probe failed. Retrying with ROOT Directory (DirID=2) of vRef=%d.\n", probeVRefNum);
                             probeDirID = 2; // Root ID
                             probeIndex = 1;
                             safety = 0;
                             goto restart_probe; // This counts as part of the primary attempt, sort of...
                             // Actually, this might loop if ROOT fails. 
                             // Let's rely on safetyProbeAttempted to eventually kill it, but we need meaningful states.
                         }
                         
                         //printf("DEBUG: Sys_ListFiles: Standard Probe failed immediately. Attempting SAFETY PROBE (vRef=0, dirID=0).\n");
                         probeVRefNum = 0;
                         probeDirID = 0;
                         probeIndex = 1; 
                         safety = 0;     
                         safetyProbeAttempted = 1; 
                         goto restart_probe; 
                     }
                     break;
                 }
                 
                 char cName[256];
                 int pLen = probeName[0];
                 memcpy(cName, &probeName[1], pLen);
                 cName[pLen] = 0;
                 
                 //printf("DEBUG: Sys_ListFiles: PROBE Item %d: '%s' (dirID=%ld) [Attrib: %x]\n", probeIndex, cName, pb.dirInfo.ioDrDirID, pb.dirInfo.ioFlAttrib);
                 
                 const char *cleanTarget = directory;
                 if (cleanTarget[0] == ':') cleanTarget++;
                 
                 if (strcasecmp(cName, cleanTarget) == 0) {
                     //printf("DEBUG: Sys_ListFiles: PROBE FOUND TARGET! Found '%s'.\n", cName);
                     
                     if (pb.dirInfo.ioFlAttrib & 0x10) { 
                         // Success! We found the directory manually.
                         goto list_files_valid;
                     } else {
                         //printf("DEBUG: Sys_ListFiles: Target found but it is NOT a directory!\n");
                     }
                 }
                 probeIndex++;
                 safety++;
             }
        } else {
             //printf("DEBUG: Sys_ListFiles: PROBE FAILED. GetVol failed.\n");
        }
        return NULL;
    }

list_files_valid:
    //printf("DEBUG: Sys_ListFiles: Proceeding with valid DirID=%ld\n", pb.dirInfo.ioDrDirID);
    
    if (!(pb.dirInfo.ioFlAttrib & 0x10)) {
        // Not a directory
        return NULL;
    }
    
    vRefNum = dirSpec.vRefNum;
    dirID = pb.dirInfo.ioDrDirID;
    
    if (!extension)
        extension = "";
    
    if (extension[0] == '/' && extension[1] == 0) {
        extension = "";
        dironly = qtrue;
    }
    
    extLen = strlen(extension);
    
    // Iterate through directory
    for (i = 1; ; i++) {
        memset(&pb, 0, sizeof(pb));
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vRefNum;
        pb.hFileInfo.ioDirID = dirID;
        pb.hFileInfo.ioFDirIndex = i;
        
        err = PBGetCatInfoSync(&pb);
        if (err != noErr) {
            break;  // No more files
        }
        
        // Convert Pascal string to C string
        char cname[256];
        int nameLen = name[0];
        memcpy(cname, &name[1], nameLen);
        cname[nameLen] = '\0';
        
        // Check if directory
        qboolean isDir = (pb.hFileInfo.ioFlAttrib & 0x10) != 0;
        
        if ((dironly && !isDir) || (!dironly && isDir)) {
            continue;
        }
        
        // Check extension
        if (*extension) {
            if (nameLen < extLen ||
                Q_stricmp(cname + nameLen - extLen, extension) != 0) {
                continue;
            }
        }
        
        if (nfiles >= MAX_FOUND_FILES - 1) {
            break;
        }
        
        list[nfiles] = CopyString(cname);
        nfiles++;
    }
    
    list[nfiles] = NULL;
    *numfiles = nfiles;
    
    if (!nfiles) {
        return NULL;
    }
    
    // Copy list to Z_Malloc'd memory
    listCopy = Z_Malloc((nfiles + 1) * sizeof(*listCopy));
    for (i = 0; i < nfiles; i++) {
        listCopy[i] = list[i];
    }
    listCopy[i] = NULL;
    
    return listCopy;
}
#endif // Old Sys_ListFiles implementation

void Sys_FreeFileList( char **list ) {
    int i;
    
    if (!list) {
        return;
    }
    
    for (i = 0; list[i]; i++) {
        Z_Free(list[i]);
    }
    
    Z_Free(list);
}

// Statically linked VM entry points
// Round velocity vectors for network determinism (trap_SnapVector from
// bg_pmove). A no-op here causes client prediction to disagree with the
// server. Matches the generic unix implementation (rint per component).
void Sys_SnapVector( float *v ) {
    v[0] = rint( v[0] );
    v[1] = rint( v[1] );
    v[2] = rint( v[2] );
}
void Sys_BeginProfiling( void ) {}
qboolean Sys_CheckCD( void ) { return qfalse; }

// Create a directory from an HFS path ("Disk:Folder:NewDir", no trailing
// colon). Called by FS_CreatePath with successively longer path prefixes;
// the first prefix is the bare volume name ("Disk", no colon), which must
// not be created — FSMakeFSSpec would treat it as a leaf name relative to
// the current directory and we would create a stray folder named after
// the volume. Was a no-op, which silently broke q3config.cfg / q3key
// persistence whenever a directory was missing.
void Sys_Mkdir( const char *path ) {
    FSSpec  spec;
    Str255  ppath;
    long    createdDirID;
    OSErr   err;
    int     len;

    if ( !strchr( path, ':' ) ) {
        return;     // bare volume name from FS_CreatePath; nothing to create
    }

    len = strlen( path );
    if ( len > 255 ) {
        len = 255;
    }
    ppath[0] = len;
    memcpy( &ppath[1], path, len );

    err = FSMakeFSSpec( 0, 0, ppath, &spec );
    if ( err == noErr ) {
        return;     // already exists
    }
    if ( err != fnfErr ) {
        Com_FlightRecord( "Sys_Mkdir: FSMakeFSSpec('%s') failed: %d\n", path, err );
        return;
    }

    err = FSpDirCreate( &spec, smSystemScript, &createdDirID );
    if ( err != noErr && err != dupFNErr ) {
        Com_FlightRecord( "Sys_Mkdir: FSpDirCreate('%s') failed: %d\n", path, err );
    }
}
char *Sys_DefaultInstallPath( void ) { return Sys_GetCwd(); }
char *Sys_DefaultHomePath( void ) { return Sys_GetCwd(); }
void Sys_StreamSeek( int handle, int offset, int origin ) {
    FS_Seek( handle, offset, origin );
}

// VM Stubs

// Startup parameters (issues #261 and #24).
//
// Retro68's startup always calls main( 1, argv ), so, as in id's original
// Classic Mac port (ReadCommandLineParms), the command line comes from:
//   - a line typed into the console window when Shift is held at launch
//     (id used Metrowerks' ccommand dialog there), or otherwise
//   - the text file MAC_PARMS_FILE in the application's folder.
// Com_ParseCommandLine splits console lines at line breaks as well as at
// '+', so each line of the file can hold one command, such as
// "+set s_initsound 1" or "safe". Input that does not fit the command line
// stops the launch instead of being cut short or overflowing it.
#define MAC_PARMS_FILE			"METParms.txt"
#define MAC_COMMAND_LINE_SIZE	MAX_STRING_CHARS

/*
==================
Sys_AppendStartupText

Appends textLength bytes of startup text to the NUL-terminated commandLine
(*commandLength bytes in commandSize bytes of storage), after separator when
both are non-empty. CR, LF and CRLF line breaks become '\n', other control
characters become spaces, and Mac Roman bytes 0x80-0xFF are kept. Returns 0,
leaving the command line unchanged, when the result would not fit.
==================
*/
static int Sys_AppendStartupText( char *commandLine, int commandSize, int *commandLength,
		const char *text, int textLength, char separator ) {
	int		length;
	int		i;
	int		c;

	length = *commandLength;
	if ( textLength <= 0 ) {
		return 1;
	}
	if ( length > 0 ) {
		if ( length >= commandSize - 1 ) {
			return 0;
		}
		commandLine[length++] = separator;
	}
	for ( i = 0 ; i < textLength ; i++ ) {
		c = (unsigned char)text[i];
		if ( c == '\r' ) {
			if ( i + 1 < textLength && text[i + 1] == '\n' ) {
				i++;
			}
			c = '\n';
		} else if ( c != '\n' && ( c < ' ' || c == 127 ) ) {
			c = ' ';
		}
		if ( length >= commandSize - 1 ) {
			commandLine[*commandLength] = '\0';
			return 0;
		}
		commandLine[length++] = c;
	}
	commandLine[length] = '\0';
	*commandLength = length;
	return 1;
}

/*
==================
Sys_ReadStartupFile

Appends the startup parameters file at path, if there is one, to the
command line, through a textSize-byte scratch buffer that must hold at
least twice the command line's storage (a CRLF file shrinks to half its
size). Returns 1 when the file was read, 0 when there is no such file,
-1 when it does not fit, and -2 on a read error.
==================
*/
static int Sys_ReadStartupFile( const char *path, char *text, int textSize,
		char *commandLine, int commandSize, int *commandLength ) {
	FILE	*f;
	int		textLength;
	int		readError;

	Sys_LogPrintf( "main: parameters: opening %s\n", path );
	f = fopen( path, "rb" );
	if ( !f ) {
		Sys_LogPrintf( "main: parameters: no file\n" );
		return 0;
	}
	Sys_LogPrintf( "main: parameters: reading\n" );
	textLength = (int)fread( text, 1, textSize, f );
	readError = ferror( f );
	Sys_LogPrintf( "main: parameters: read %d bytes, closing\n", textLength );
	fclose( f );
	if ( readError ) {
		return -2;
	}
	if ( textLength >= textSize ) {
		return -1;	// textSize bytes or more can never fit
	}
	if ( !Sys_AppendStartupText( commandLine, commandSize, commandLength,
			text, textLength, '\n' ) ) {
		return -1;
	}
	return 1;
}

/*
==================
Sys_StartupError

Shows a startup error (bad parameters, or a static module that cannot be
saved by VM_InitStaticModules) in the console window and waits for
Return, so the reason the game did not start can be read.
==================
*/
static int Sys_StartupError( const char *error ) {
	fprintf( stderr, "Enemy Territory: %s\nPress Return to quit.\n", error );
	getchar();
	return 1;
}

/*
==================
Sys_InitToolbox

Initializes the Toolbox once, before anything draws, logs or reads the
keyboard, as id's InitMacStuff did (issue #263). Retro68's startup code
initializes none of it, and its console used to do so as a side effect of
the first printf. That console now opens only on demand
(mac_consolehooks.cc), so main must not depend on it. The EventAvail calls
are what that console did to bring the application to the front
(Technote TB 35).
==================
*/
static void Sys_InitToolbox( void ) {
	EventRecord	event;
	int			i;

	MaxApplZone();
	MoreMasters();

	InitGraf( &qd.thePort );
	InitFonts();
	FlushEvents( everyEvent, 0 );
	InitWindows();
	InitMenus();
	TEInit();
	InitDialogs( NULL );
	InitCursor();

	for ( i = 0 ; i < 5 ; i++ ) {
		EventAvail( everyEvent, &event );
	}
}

#ifndef Q3_BUILD_REV
#define Q3_BUILD_REV "unknown"
#endif

// what a log needs to say about the machine it came from
// the sections, from the linker: PC - _text is the address in the XCOFF
extern char _text[], _data[];

/*
==================
Sys_GestaltValue
==================
*/
static long Sys_GestaltValue( OSType selector ) {
	long	value;

	if ( Gestalt( selector, &value ) != noErr ) {
		return -1;
	}
	return value;
}

/*
==================
Sys_LogSystemFacts

The first lines of a log: what built it and what it ran on, so a report can
be read without knowing the machine.
==================
*/
static void Sys_LogSystemFacts( void ) {
	ProcessSerialNumber	psn;
	ProcessInfoRec		info;
	int					stackProbe;

	Sys_LogPrintf( "build: %s, PEF code section at %p, data at %p\n", Q3_BUILD_REV, (void *)_text, (void *)_data );
	Sys_LogPrintf( "system: 0x%lx, CPU type 0x%lx at %ld Hz, %ld bytes of RAM, %ld logical, VM attributes 0x%lx, Open Transport 0x%lx\n",
		Sys_GestaltValue( gestaltSystemVersion ), Sys_GestaltValue( gestaltNativeCPUtype ),
		Sys_GestaltValue( gestaltProcClkSpeed ), Sys_GestaltValue( gestaltPhysicalRAMSize ),
		Sys_GestaltValue( gestaltLogicalRAMSize ), Sys_GestaltValue( gestaltVMAttr ),
		Sys_GestaltValue( 'otvr' ) );

	memset( &info, 0, sizeof( info ) );
	info.processInfoLength = sizeof( info );
	if ( GetCurrentProcess( &psn ) == noErr && GetProcessInformation( &psn, &info ) == noErr ) {
		Sys_LogPrintf( "partition: %ld bytes, %ld free, largest block %ld, heap limit %p, stack near %p\n",
			(long)info.processSize, (long)info.processFreeMem, (long)MaxBlock(), (void *)GetApplLimit(), (void *)&stackProbe );
	}
}

/*
==================
Sys_StartBootLog

Starts the boot log and gives it what the crash ring has so far.
==================
*/
static void Sys_StartBootLog( void ) {
	if ( !Sys_BootLogOpen( Sys_GetCwd() ) ) {
		Sys_LogPrintf( "main: no boot log this run, so etboot.log is an older run's\n" );
		return;
	}
	retroLogBuffer[retroLogHead] = 0;	// the ring has not wrapped this early
	Sys_BootLogWrite( retroLogBuffer );
	Sys_LogSystemFacts();
}

/*
==================
Sys_Heartbeat

A line every few seconds, so a log that ends says when the game stopped
running and not only what it last printed, and gives a rough frame rate.
==================
*/
#define HEARTBEAT_MSEC	5000
static void Sys_Heartbeat( void ) {
	static int	lastTime, frames;
	int			now;

	frames++;
	now = Sys_Milliseconds();
	if ( now - lastTime < HEARTBEAT_MSEC ) {
		return;
	}
	Sys_LogPrintf( "heartbeat: state %i, %i frames in %i msec, %ld bytes free, largest block %ld\n",
		cls.state, frames, now - lastTime, (long)FreeMem(), (long)MaxBlock() );
	lastTime = now;
	frames = 0;
}

/*
==================
Sys_MaskStartupSecrets

Overwrites, in the NUL-terminated line, what follows "password" in any word
naming a password cvar (rconpassword, g_password, sv_privatePassword, ...)
up to the end of its console line with '*': "set" takes every remaining word
as the value. Masking starts right after "password", not at the end of the
word, because Cmd_TokenizeString also ends a word at a "/*" comment and, with
signed chars, at Mac Roman bytes such as Option-Space (0xCA).
Console lines end where Com_ParseCommandLine splits them, at a line break or
a '+' outside quotes, until MAX_CONSOLE_LINES: it puts the rest of the
command line in the last line, which is masked to its end (that can also
mask commands after a line break there). Quotes and spaces are kept. The startup
command line goes to etboot.log and the crash report, which people attach
to bug reports.
==================
*/
static void Sys_MaskStartupSecrets( char *line ) {
	char		*p;
	char		*word;
	qboolean	inQuote;
	qboolean	masking;
	int			lines;

	inQuote = qfalse;
	masking = qfalse;
	lines = 1;
	p = line;
	while ( *p ) {
		if ( lines < MAX_CONSOLE_LINES
			&& ( *p == '\n' || *p == '\r' || ( *p == '+' && !inQuote ) ) ) {
			lines++;
			masking = qfalse;
			p++;
			continue;
		}
		if ( *p == '"' ) {
			inQuote = !inQuote;
			p++;
			continue;
		}
		if ( (unsigned char)*p <= ' ' ) {
			p++;
			continue;
		}
		if ( masking ) {
			*p++ = '*';
			continue;
		}
		word = p;
		while ( (unsigned char)*p > ' ' && *p != '"'
			&& !( *p == '+' && !inQuote && lines < MAX_CONSOLE_LINES ) ) {
			p++;
		}
		for ( ; word + 8 <= p ; word++ ) {
			if ( !Q_stricmpn( word, "password", 8 ) ) {
				masking = qtrue;
				p = word + 8;
				break;
			}
		}
	}
}

/*
==================
Sys_ClipboardText

The clipboard text the console and UI fields paste: its first line, as on
win32 (which stops at CR, LF or backspace), at most MAX_STRING_CHARS - 1
bytes, in a zone copy the caller frees.  NULL when that line is empty.
==================
*/
static char *Sys_ClipboardText( const char *text, long length ) {
	char	*copy;
	long	i;

	if ( length > MAX_STRING_CHARS - 1 ) {
		length = MAX_STRING_CHARS - 1;
	}
	for ( i = 0 ; i < length ; i++ ) {
		if ( text[i] == '\r' || text[i] == '\n' || text[i] == '\b' || !text[i] ) {
			break;
		}
	}
	if ( !i ) {
		return NULL;
	}
	copy = Z_Malloc( i + 1 );
	memcpy( copy, text, i );
	copy[i] = 0;
	return copy;
}

/*
==================
Sys_GetClipboardData

Paste (#266): the Scrap Manager's TEXT.  It was a stub returning NULL, so
Shift-Insert and Ctrl-V (Cmd-V, from DoKeyDown) pasted nothing.
==================
*/
char *Sys_GetClipboardData( void ) {
	Handle	text;
	SInt32	offset;
	long	length;
	char	*copy;

	text = NewHandle( 0 );
	if ( !text ) {
		return NULL;
	}
	copy = NULL;
	length = GetScrap( text, kScrapFlavorTypeText, &offset );
	if ( length > 0 ) {
		// GetScrap sized the handle to the text.  Nothing before the copy
		// can move it (Z_Malloc never calls the Memory Manager), so it is
		// not locked: NewHandle, GetScrap and DisposeHandle are the only
		// new imports (InterfaceLib 7.1 and later).
		copy = Sys_ClipboardText( *text, length );
	}
	DisposeHandle( text );
	return copy;
}

int main( int argc, char **argv ) {
    int i;
    static char commandLine[MAC_COMMAND_LINE_SIZE];
    int commandLength;
    const char *error;
    KeyMap keys;

    // Before any output: the console window and its error messages need it.
    Sys_InitToolbox();

    // Save each module's initialized data before any module code runs.
    error = Sys_InitStaticModules();
    if ( error ) {
        return Sys_StartupError( error );
    }

    Sys_LogPrintf("main: START\n");

    // Before the parameters, so a launch that stops reading them says where.
    // A first launch on a real G4 froze here and left nothing. The parameters
    // can turn both off again below.
    Sys_StartBootLog();
    Sys_InstallCrashHandler();

    commandLine[0] = 0;
    commandLength = 0;
    for (i = 1; i < argc; i++) {
        if ( !Sys_AppendStartupText( commandLine, sizeof( commandLine ), &commandLength,
                                     argv[i], strlen( argv[i] ), ' ' ) ) {
            return Sys_StartupError( va( "command line exceeds %d bytes",
                                         (int)sizeof( commandLine ) - 1 ) );
        }
    }

    // Shift is key code 0x38: bit 0 of byte 7 of the big-endian KeyMap.
    GetKeys( keys );
    if ( ((unsigned char *)keys)[7] & 0x01 ) {
        static char line[MAC_COMMAND_LINE_SIZE + 1];
        int lineLength;

        Sys_ShowConsole( 1, qfalse );	// until Com_Init applies viewlog
        printf( "Enemy Territory startup parameters, e.g. +set s_initsound 1 (Return for none):\n" );
        fflush( stdout );
        if ( fgets( line, sizeof( line ), stdin ) ) {
            lineLength = strlen( line );
            if ( lineLength > 0 && line[lineLength - 1] == '\n' ) {
                lineLength--;
            } else if ( lineLength == (int)sizeof( line ) - 1 ) {
                int c;

                // Drop the rest of the line, so Sys_StartupError waits for
                // a new Return instead of reading the next typed byte.
                do {
                    c = getchar();
                } while ( c != '\n' && c != EOF );
                return Sys_StartupError( va( "startup parameters exceed %d bytes",
                                             (int)sizeof( commandLine ) - 1 ) );
            }
            if ( !Sys_AppendStartupText( commandLine, sizeof( commandLine ), &commandLength,
                                         line, lineLength, ' ' ) ) {
                return Sys_StartupError( va( "startup parameters exceed %d bytes",
                                             (int)sizeof( commandLine ) - 1 ) );
            }
        }
        Sys_LogPrintf( "main: startup parameters typed at launch\n" );
    } else {
        static char text[MAC_COMMAND_LINE_SIZE * 2];
        char path[MAX_OSPATH * 2 + sizeof( MAC_PARMS_FILE )];
        int result;

        // Next to the application, where the engine looks for baseq3
        // (Sys_GetCwd returns at most 2 * MAX_OSPATH - 1 bytes).
        Sys_JoinHFSPath( path, sizeof( path ), Sys_GetCwd(), MAC_PARMS_FILE );
        result = Sys_ReadStartupFile( path, text, sizeof( text ),
                                      commandLine, sizeof( commandLine ), &commandLength );
        if ( result == -2 ) {
            return Sys_StartupError( va( "could not read %s", path ) );
        }
        if ( result == -1 ) {
            return Sys_StartupError( va( "%s exceeds %d bytes", path,
                                         (int)sizeof( commandLine ) - 1 ) );
        }
        if ( result ) {
            Sys_LogPrintf( "main: startup parameters read from %s\n", path );
        }
    }
    {
        static char logged[MAC_COMMAND_LINE_SIZE];

        Q_strncpyz( logged, commandLine, sizeof( logged ) );
        Sys_MaskStartupSecrets( logged );
        Sys_LogPrintf( "main: command line: %s\n", logged );
    }
    if ( strstr( commandLine, "sys_bootlog 0" ) ) {
        Sys_BootLogClose();
    }
    if ( strstr( commandLine, "sys_crashhandler 0" ) ) {
        Sys_RemoveCrashHandler();
    }

    // Note: Sys_Init() is called by Com_Init() after the cvar/zone systems
    // exist. Calling it here too (as this port once did) dereferenced the
    // NULL com_dedicated cvar in Sys_InitInput and double-initialized Open
    // Transport, leaking an endpoint and shifting the UDP port to 27961.


    Sys_LogPrintf("main: Com_Init\n");
    Com_Init( commandLine );
    Sys_LogPrintf("main: Com_Init done\n");
    Debug_Breadcrumb(341); // greenColor

    Sys_LogPrintf("main: entering main loop\n");
    Com_FlightRecord("main: Entering Com_Frame loop.\n");
    while ( 1 ) {
        Com_Frame();
        Sys_Heartbeat();
    }

    return 0;
}

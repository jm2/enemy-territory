/* Issues #277, #265 and #20: the classic Mac Open Transport network code.
 *
 * #277: Sys_StringToAdr handed any host name to OTInitDNSAddress, which copies
 * it into the 256-byte DNSAddress.fName on the stack with no bound, and
 * NET_StringToAdr passes names of up to 1023 characters.  Names of 256
 * characters or more must now fail with a message and never reach OT; names
 * of up to 255 must resolve as before.
 *
 * #265: Sys_GetPacket ignored OTRcvUData's T_MORE flag, so a datagram larger
 * than its MAX_MSGLEN buffer came out as a truncated packet followed by a
 * second "packet" holding the rest, with no source address.  Such a datagram
 * must be read to its end and dropped whole with the "Oversize packet from"
 * message the Unix and Win32 backends print, and, as there, so must one that
 * exactly fills the buffer.  If a read fails partway through, the rest of the
 * datagram, which has no source address, must be dropped the same way on the
 * next call.  Other packets must come through byte for byte.
 * Com_EventLoop's own oversize-packet branch must free its event, as every
 * other branch does.
 *
 * #20: Sys_InitNetworking ignored OTSetNonBlocking and the resolver's OTBind
 * failing, left Open Transport and endpoints open after a failed stage, and
 * always bound PORT_SERVER + i on every interface.  As NET_OpenIP does on the
 * other platforms, it must honour net_noudp, net_ip ("localhost" or "" for
 * every interface) and net_port, try ten ports from net_port and set net_port
 * to the one bound; every failed stage must be reported and must close what
 * was opened, leaving the UDP paths inert and shutdown with nothing to close.
 *
 * #21: NET_Sleep was empty, so a dedicated server spun at full speed between
 * frames.  As unix_net.c's select does, it must now sleep (WaitNextEvent, a
 * sleep of more than zero ticks: never a spin) until msec pass or a datagram
 * arrives, which the game endpoint's notifier reports with WakeUpProcess; a
 * datagram already queued or a Mac event (console typing) must end it too.
 * A client, or a server with no endpoint, must not sleep at all.
 *
 * #33: Sys_StringToAdr resolved every string with a synchronous
 * OTResolveAddress that could freeze the Mac for ten seconds.  A dotted quad
 * must now be converted without OT (as inet_addr does elsewhere), even with
 * networking off, and anything else must go to the resolver as before.  A
 * name must be looked up asynchronously while Sys_WaitEvent keeps the Mac
 * running: never a spin, completion within a wait of OT's report, failure
 * reported, at most RESOLVE_TIMEOUT plus a second when OT never answers,
 * and Esc giving up at once.  A lookup given up must leave a fresh resolver
 * for the next one.  The start-up must install both notifiers and make the
 * resolver asynchronous, each failure handled like the other stages.
 *
 * The runner extracts the real mac_net.c globals, Sys_InitNetworking through
 * Sys_ShutdownNetworking with the notifiers and helpers before them,
 * Sys_StringToAdr with its helper, Sys_GetPacket and NET_Sleep (mac_net.c),
 * and Com_EventLoop (common.c) verbatim; tests/mac_ot_fake.h stands in for
 * Open Transport, with a virtual clock, and the functions they call are
 * replaced here.  Each case runs in its own process: on master the long
 * names overflow dnsAddr, which AddressSanitizer stops at. */
#include "../../src/game/q_shared.h"
#include "et_compat.h"
#include "../../src/qcommon/qcommon.h"
#include "mac_ot_fake.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

static const UInt8 serverIP[4] = { 192, 246, 40, 70 };
static const UInt8 serverPort[2] = { 0x6d, 0x38 };	/* 27960 */
static const UInt8 clientIP[4] = { 10, 0, 0, 2 };
static const UInt8 clientPort[2] = { 0x6d, 0x39 };	/* 27961 */

static const char *currentCase = "setup";
static char printed[16384];
static int handleOTErrors;

static void Check( int ok, const char *what ) {
	if ( !ok ) {
		fprintf( stderr, "Mac net regression failed (%s): %s\nprinted: %s\n", currentCase, what, printed );
		exit( 1 );
	}
}

static void Unexpected( const char *name ) {
	fprintf( stderr, "Mac net regression reached %s (%s)\n", name, currentCase );
	exit( 1 );
}

/* How many times text was printed since the case began. */
static int Printed( const char *text ) {
	const char *p;
	int count = 0;

	for ( p = strstr( printed, text ) ; p ; p = strstr( p + 1, text ) ) {
		count++;
	}
	return count;
}

void QDECL Com_Printf( const char *fmt, ... ) {
	size_t length = strlen( printed );
	va_list ap;

	va_start( ap, fmt );
	vsnprintf( printed + length, sizeof( printed ) - length, fmt, ap );
	va_end( ap );
}

void QDECL Com_Error( int level, const char *error, ... ) {
	(void)level;
	Unexpected( error );
}

const char *NET_AdrToString( netadr_t a ) {
	static char s[64];

	snprintf( s, sizeof( s ), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3],
		( ((byte *)&a.port)[0] << 8 ) | ((byte *)&a.port)[1] );
	return s;
}

static int rcvUDErrs;
void RcvUDErr( EndpointRef ep ) {
	(void)ep;
	rcvUDErrs++;
	fakeOTLookEvent = 0;
}

void HandleOTError( EndpointRef ep, int err, const char *func ) {
	(void)ep; (void)err; (void)func;
	handleOTErrors++;
}

/* Sys_InitNetworking's */

static int localAddressCalls, broadcastCalls;

static cvar_t cvars[8];
static char cvarStrings[8][MAX_CVAR_VALUE_STRING];
static int cvarCount;

static cvar_t *FindCvar( const char *name ) {
	int i;

	for ( i = 0 ; i < cvarCount ; i++ ) {
		if ( !strcmp( cvars[i].name, name ) ) {
			return &cvars[i];
		}
	}
	return NULL;
}

/* Set a cvar as +set on the command line does, before Sys_Init. */
static cvar_t *SetCvar( const char *name, const char *value ) {
	cvar_t *var = FindCvar( name );

	if ( !var ) {
		Check( cvarCount < 8, "the cvar table has room" );
		var = &cvars[cvarCount];
		var->name = (char *)name;
		var->string = cvarStrings[cvarCount++];
	}
	Q_strncpyz( var->string, value, MAX_CVAR_VALUE_STRING );
	var->integer = atoi( value );
	var->value = (float)atof( value );
	return var;
}

cvar_t *Cvar_Get( const char *name, const char *value, int flags ) {
	cvar_t *var = FindCvar( name );

	if ( !var ) {
		var = SetCvar( name, value );
	}
	var->flags |= flags;
	return var;
}

void Cvar_SetValue( const char *name, float value ) {
	char s[32];

	snprintf( s, sizeof( s ), "%i", (int)value );
	SetCvar( name, s );
}

char * QDECL va( char *format, ... ) {
	static char s[MAX_STRING_CHARS];
	va_list ap;

	va_start( ap, format );
	vsnprintf( s, sizeof( s ), format, ap );
	va_end( ap );
	return s;
}

int Q_stricmp( const char *s1, const char *s2 ) {
	return strcasecmp( s1, s2 );
}

void Q_strncpyz( char *dest, const char *src, int destsize ) {
	strncpy( dest, src, destsize - 1 );
	dest[destsize - 1] = 0;
}

void NET_GetLocalAddress( void ) {
	localAddressCalls++;
}

static OTResult SetFourByteOption( EndpointRef ep, OTXTILevel level, OTXTIName name, UInt32 value ) {
	Check( ep == FAKE_OT_ENDPOINT && level == INET_IP && name == IP_BROADCAST && value == T_YES,
		"broadcasts are enabled on the game endpoint" );
	broadcastCalls++;
	return noErr;
}

/* NET_Sleep's and Sys_StringToAdr's */

static cvar_t dedicated;
cvar_t *com_dedicated = &dedicated;

int Sys_Milliseconds( void ) {
	return fakeOTNow;
}

/* The fake WaitNextEvent: it sleeps until sleepTicks pass, or until a
 * notifier wakes the process, delivering what OT has due on the way.  An
 * Event Manager event (a key) can be scripted for one call, and an Esc that
 * sets *cancel. */
static int waitCalls, waitSpins, waitEventCall, waitCancelCall;

qboolean Sys_WaitEvent( long sleepTicks, qboolean *cancel ) {
	int end, due, wakeups;

	waitCalls++;
	Check( waitCalls < 100000, "the wait ends" );
	if ( sleepTicks <= 0 ) {
		waitSpins++;	/* returns at once: a loop around it spins */
		Check( 0, "every wait sleeps: a wait of no ticks spins" );
	}
	if ( waitCalls == waitEventCall ) {
		return qtrue;
	}
	if ( waitCalls == waitCancelCall ) {
		Check( cancel != NULL, "the wait can be cancelled" );
		*cancel = qtrue;
		return qtrue;
	}
	end = fakeOTNow + sleepTicks * 1000 / 60;
	wakeups = fakeOTWakeups;
	while ( ( due = FakeOT_NextDue() ) >= 0 && due <= end ) {
		FakeOT_Advance( due );
		if ( fakeOTWakeups != wakeups ) {
			return qfalse;	/* WakeUpProcess: a null event, early */
		}
	}
	FakeOT_Advance( end );
	return qfalse;
}

/* mac_net.c */
#include "mac_net_globals.c"
#include "mac_net_init_extracted.c"
#include "mac_net_resolve_extracted.c"
#include "mac_net_extracted.c"
#include "mac_net_sleep_extracted.c"

/* Before each case: nothing open, no cvars, a fresh fake. */
static void ResetInit( void ) {
	FakeOT_ResetLifetime();
	FakeOT_ResetEndpoint();
	endpoint = resolverEndpoint = kOTInvalidEndpointRef;
	gOTInited = qfalse;
	endpointTSDU = 0;
	cvarCount = 0;
	localAddressCalls = broadcastCalls = handleOTErrors = 0;
	fakeOTResolveCalls = fakeOTInitDNSCalls = 0;
	printed[0] = 0;
	waitCalls = waitSpins = waitEventCall = waitCancelCall = 0;
	dedicated.integer = 0;
}

static void InitNetworking( void ) {
	Sys_InitNetworking();
	Check( fakeOTMisuse == 0, "OT is only used as documented" );
}

/* ---------- #277: Sys_StringToAdr ---------- */

/* A name of the given length must resolve to the fake's address, reaching OT
 * intact, or (when it does not fit DNSAddress.fName) fail before OT sees it. */
static void CheckHostName( const char *name, qboolean fits ) {
	netadr_t a;

	ResetInit();
	InitNetworking();
	memset( &a, 0, sizeof( a ) );
	printed[0] = 0;
	fakeOTInitDNSCalls = fakeOTResolveCalls = 0;
	memset( fakeOTResolvedName, 0, sizeof( fakeOTResolvedName ) );
	memcpy( &fakeOTResolvedHost, serverIP, 4 );

	if ( fits ) {
		Check( Sys_StringToAdr( name, &a ), "the name resolves" );
		Check( fakeOTInitDNSCalls == 1 && fakeOTResolveCalls == 1, "OT resolves the name once" );
		Check( !strcmp( fakeOTResolvedName, name ), "OT is asked for the whole name" );
		Check( a.type == NA_IP && !memcmp( a.ip, serverIP, 4 ), "the address is OT's" );
		Check( !printed[0], "nothing is printed" );
	} else {
		Check( !Sys_StringToAdr( name, &a ), "the name is refused" );
		Check( fakeOTInitDNSCalls == 0 && fakeOTResolveCalls == 0, "OT never sees the name" );
		Check( !strncmp( printed, "Sys_StringToAdr: ", 17 ) && Printed( "\n" ) == 1,
			"one Sys_StringToAdr message is printed" );
	}
	Check( handleOTErrors == 0, "no OT error is reported" );
}

static void HostNameOfLength( int length, qboolean fits ) {
	static char name[MAX_STRING_CHARS];
	int i;

	Check( length < (int)sizeof( name ), "the name fits NET_StringToAdr's copy" );
	for ( i = 0 ; i < length ; i++ ) {
		name[i] = ( i % 64 == 63 ) ? '.' : 'a' + i % 26;	/* 63-character labels */
	}
	name[length] = 0;
	CheckHostName( name, fits );
}

/* ---------- #265: Sys_GetPacket ---------- */

static UInt8 datagramData[4][MAX_MSGLEN * 4];

static const UInt8 *Datagram( int slot, int length ) {
	int i;

	Check( length <= (int)sizeof( datagramData[slot] ), "the datagram fits the fixture" );
	for ( i = 0 ; i < length ; i++ ) {
		datagramData[slot][i] = (UInt8)( i * 31 + slot * 7 + ( i >> 8 ) );
	}
	return datagramData[slot];
}

static void ResetEndpoint( void ) {
	FakeOT_ResetEndpoint();
	fakeOTLookEvent = 0;
	fakeOTNow += 2000;		/* past any Sys_ReportDroppedPacket limit */
	endpoint = FAKE_OT_ENDPOINT;
	printed[0] = 0;
	handleOTErrors = 0;
}

/* Receive into a MAX_MSGLEN buffer as mac_main.c's Sys_GetEvent does. */
static qboolean GetPacket( netadr_t *from, msg_t *msg ) {
	static byte buffer[MAX_MSGLEN];

	memset( from, 0, sizeof( *from ) );
	memset( msg, 0, sizeof( *msg ) );
	msg->data = buffer;
	msg->maxsize = sizeof( buffer );
	return Sys_GetPacket( from, msg );
}

/* The next packet must be this datagram, whole and from this address. */
static void ExpectPacket( const UInt8 *data, int length, const UInt8 ip[4], const UInt8 port[2] ) {
	netadr_t from;
	msg_t msg;

	Check( GetPacket( &from, &msg ), "the datagram is received" );
	Check( msg.cursize == length && !memcmp( msg.data, data, length ), "the packet is the datagram byte for byte" );
	Check( from.type == NA_IP && !memcmp( from.ip, ip, 4 ) && !memcmp( &from.port, port, 2 ),
		"the packet has the datagram's source address" );
}

/* The next datagram must be read to its end and dropped with one message. */
static void ExpectOversize( int queued ) {
	netadr_t from;
	msg_t msg;

	Check( !GetPacket( &from, &msg ), "an oversize datagram is dropped" );
	Check( fakeOTNext == queued + 1 && fakeOTOffset == 0, "the whole datagram is read from the endpoint" );
	Check( Printed( "Oversize packet from 10.0.0.2:27961\n" ) == 1 && Printed( "\n" ) == 1,
		"one Oversize packet message names the sender" );
}

static void ExpectNoPacket( void ) {
	netadr_t from;
	msg_t msg;

	Check( !GetPacket( &from, &msg ), "an empty endpoint gives no packet" );
}

static void NormalPackets( void ) {
	static const int lengths[] = { 1, 1400, MAX_MSGLEN - 1 };
	int i;

	ResetEndpoint();
	for ( i = 0 ; i < 3 ; i++ ) {
		FakeOT_QueueDatagram( Datagram( i, lengths[i] ), lengths[i],
			i & 1 ? clientIP : serverIP, i & 1 ? clientPort : serverPort );
	}
	for ( i = 0 ; i < 3 ; i++ ) {
		ExpectPacket( datagramData[i], lengths[i], i & 1 ? clientIP : serverIP, i & 1 ? clientPort : serverPort );
	}
	ExpectNoPacket();
	Check( fakeOTRcvCalls == 4, "each packet takes one OTRcvUData call" );
	Check( !printed[0] && handleOTErrors == 0, "nothing is printed" );
}

/* An oversize datagram is dropped and the packet after it arrives intact. */
static void OversizeThenNormal( int length ) {
	const UInt8 *next = Datagram( 1, 1400 );

	ResetEndpoint();
	FakeOT_QueueDatagram( Datagram( 0, length ), length, clientIP, clientPort );
	FakeOT_QueueDatagram( next, 1400, serverIP, serverPort );
	ExpectOversize( 0 );
	ExpectPacket( next, 1400, serverIP, serverPort );
	ExpectNoPacket();
	Check( handleOTErrors == 0, "no OT error is reported" );
}

/* The first continuation read fails, leaving the rest of the datagram (here
 * a connectionless getstatus) queued.  The next call must drop that tail, not
 * deliver it as a packet from 0.0.0.0, and the packet after it must arrive
 * intact. */
static void DrainError( void ) {
	static const char tail[] = "\xff\xff\xff\xffgetstatus";
	const UInt8 *next = Datagram( 1, 1400 );
	netadr_t from;
	msg_t msg;

	ResetEndpoint();
	Datagram( 0, MAX_MSGLEN + 600 );
	memcpy( datagramData[0] + MAX_MSGLEN, tail, sizeof( tail ) - 1 );
	FakeOT_QueueDatagram( datagramData[0], MAX_MSGLEN + 600, clientIP, clientPort );
	FakeOT_QueueDatagram( next, 1400, serverIP, serverPort );
	fakeOTFailCall = 2;
	fakeOTFailError = kOTLookErr;

	Check( !GetPacket( &from, &msg ), "an oversize datagram is dropped when its drain fails" );
	Check( handleOTErrors == 1 && fakeOTNext == 0 && fakeOTOffset == MAX_MSGLEN,
		"the failed read is reported and leaves the tail queued" );
	Check( Printed( "Oversize packet from 10.0.0.2:27961\n" ) == 1, "the drop names the sender" );

	Check( !GetPacket( &from, &msg ), "the tail is not delivered as a packet" );
	Check( fakeOTNext == 1 && fakeOTOffset == 0, "the tail is read from the endpoint" );
	Check( Printed( "\n" ) == 1, "and a burst prints once" );

	ExpectPacket( next, 1400, serverIP, serverPort );
	ExpectNoPacket();
	Check( handleOTErrors == 1, "only the injected error is reported" );
}

/*
 * Run on Mac OS 9, pieces with no source address arrived in bursts: each one
 * was a report, and each report a disk write.
 */
static void DroppedPacketReports( void ) {
	static UInt8 noAddress[4] = { 0, 0, 0, 0 }, noPort[2] = { 0, 0 };
	const UInt8 *next = Datagram( 1, 1400 );
	netadr_t from;
	msg_t msg;
	int i;

	/* a burst: one line that says what was returned, then a count */
	ResetEndpoint();
	for ( i = 0 ; i < 5 ; i++ ) {
		FakeOT_QueueDatagram( Datagram( 0, 100 ), 100, noAddress, noPort );
		fakeOTDatagrams[i].noAddress = 1;
	}
	FakeOT_QueueDatagram( next, 1400, serverIP, serverPort );
	for ( i = 0 ; i < 5 ; i++ ) {
		Check( !GetPacket( &from, &msg ), "a piece with no source address is not a packet" );
	}
	Check( Printed( "Dropped a packet with no source address: 100 bytes, flags 0x0, OTLook 0\n" ) == 1
		&& Printed( "\n" ) == 1, "a burst prints one report, saying what Open Transport returned" );
	ExpectPacket( next, 1400, serverIP, serverPort );

	printed[0] = 0;
	fakeOTNow += 1000;
	FakeOT_QueueDatagram( Datagram( 0, 100 ), 100, noAddress, noPort );
	fakeOTDatagrams[fakeOTQueued - 1].noAddress = 1;
	Check( !GetPacket( &from, &msg ), "a later piece is dropped too" );
	Check( Printed( "4 more packets dropped since the last report\n" ) == 1, "and counts what went unprinted" );

	/* a pending error accounts for the piece, and is cleared */
	ResetEndpoint();
	rcvUDErrs = 0;
	fakeOTLookEvent = T_UDERR;
	FakeOT_QueueDatagram( Datagram( 0, 100 ), 100, noAddress, noPort );
	fakeOTDatagrams[0].noAddress = 1;
	Check( !GetPacket( &from, &msg ), "a piece with an error waiting is dropped" );
	Check( rcvUDErrs == 1 && !fakeOTLookEvent, "the error is cleared" );
	Check( !printed[0], "silently" );

	/* a real oversize datagram with an error waiting still names its sender */
	ResetEndpoint();
	rcvUDErrs = 0;
	fakeOTLookEvent = T_UDERR;
	FakeOT_QueueDatagram( Datagram( 0, MAX_MSGLEN + 600 ), MAX_MSGLEN + 600, clientIP, clientPort );
	Check( !GetPacket( &from, &msg ), "an oversize datagram is dropped" );
	Check( rcvUDErrs == 1 && Printed( "Oversize packet from 10.0.0.2:27961\n" ) == 1, "and still reported" );
}

/* ---------- Com_EventLoop's oversize branch ---------- */

static cvar_t dropsim, svRunning;
cvar_t *com_dropsim = &dropsim;
cvar_t *com_sv_running = &svRunning;

static sysEvent_t events[4];
static int eventCount, eventNext, outstanding, delivered;

static byte PayloadByte( int i, int payload ) {
	return (byte)( i * 13 + payload );
}

/* The queue lets go of each block as it hands the event out, so a block
 * Com_EventLoop fails to free is a leak LeakSanitizer reports. */
sysEvent_t Com_GetEvent( void ) {
	sysEvent_t ev;

	if ( eventNext < eventCount ) {
		ev = events[eventNext];
		events[eventNext++].evPtr = NULL;
		return ev;
	}
	memset( &ev, 0, sizeof( ev ) );
	ev.evType = SE_NONE;
	return ev;
}

/* Queue a packet event as mac_main.c does: a netadr_t, then the payload. */
static void QueuePacketEvent( int payload ) {
	sysEvent_t *ev = &events[eventCount++];
	netadr_t *block = malloc( sizeof( netadr_t ) + payload );
	byte *data = (byte *)( block + 1 );
	int i;

	Check( block != NULL, "the event block is allocated" );
	memset( block, 0, sizeof( *block ) );
	block->type = NA_IP;
	memcpy( block->ip, clientIP, 4 );
	memcpy( &block->port, clientPort, 2 );
	for ( i = 0 ; i < payload ; i++ ) {
		data[i] = PayloadByte( i, payload );
	}
	memset( ev, 0, sizeof( *ev ) );
	ev->evType = SE_PACKET;
	ev->evPtrLength = sizeof( netadr_t ) + payload;
	ev->evPtr = block;
	outstanding++;
}

/* AddressSanitizer reports a block freed twice or never allocated. */
void Z_Free( void *ptr ) {
	free( ptr );
	outstanding--;
}

/* The client must get the event Com_GetEvent last handed out, intact. */
void CL_PacketEvent( netadr_t from, msg_t *msg ) {
	int length = events[eventNext - 1].evPtrLength - (int)sizeof( netadr_t );
	int i;

	Check( from.type == NA_IP && !memcmp( from.ip, clientIP, 4 ), "the packet has its sender" );
	Check( msg->cursize == length, "the packet has its length" );
	for ( i = 0 ; i < length ; i++ ) {
		Check( msg->data[i] == PayloadByte( i, length ), "the packet is delivered byte for byte" );
	}
	delivered++;
}

void Com_RunAndTimeServerPacket( netadr_t *evFrom, msg_t *buf ) { Unexpected( __func__ ); }
qboolean NET_GetLoopPacket( netsrc_t sock, netadr_t *net_from, msg_t *net_message ) { return qfalse; }
qboolean consoleButtonWasPressed;	/* ET: Com_EventLoop reads it */
void CL_KeyEvent( int key, qboolean down, unsigned time ) { Unexpected( __func__ ); }
void CL_CharEvent( int key ) { Unexpected( __func__ ); }
void CL_MouseEvent( int dx, int dy, int time ) { Unexpected( __func__ ); }
void CL_JoystickEvent( int axis, int value, int time ) { Unexpected( __func__ ); }
void Cbuf_AddText( const char *text ) { Unexpected( __func__ ); }
float Q_random( int *seed ) { Unexpected( __func__ ); return 0; }
void MSG_Init( msg_t *buf, byte *data, int length ) {
	memset( buf, 0, sizeof( *buf ) );
	buf->data = data;
	buf->maxsize = length;
}
void Com_Memcpy( void *dest, const void *src, const size_t count ) { memcpy( dest, src, count ); }

#include "com_event_loop_extracted.c"

/* One packet past the netchan buffer, one that fills it and a small one: the
 * first is dropped with its message, the others are delivered, and every
 * event block is freed once. */
static void EventLoopOversize( void ) {
	QueuePacketEvent( MAX_MSGLEN_BUF + 1 );
	QueuePacketEvent( MAX_MSGLEN_BUF );
	QueuePacketEvent( 1400 );
	printed[0] = 0;

	Com_EventLoop();
	Check( eventNext == 3 && delivered == 2, "the two packets that fit are delivered" );
	Check( Printed( "Com_EventLoop: oversize packet\n" ) == 1, "the oversize packet is reported once" );
	Check( outstanding == 0, "every event block is freed" );
}

/* Start-up must have bound the game endpoint, non-blocking, to host:port and
 * opened and bound a resolver, and shutting down must release it all. */
static void ExpectStarted( InetHost host, int port ) {
	netadr_t from;
	msg_t msg;

	Check( fakeOTInited == 1 && gOTInited, "Open Transport is open" );
	Check( endpoint == FAKE_OT_ENDPOINT && resolverEndpoint == FAKE_OT_RESOLVER, "both endpoints are open" );
	Check( FakeOT_OpenEndpoints() == 2, "two endpoints are open" );
	Check( fakeOTEndpoints[0].nonBlocking, "the game endpoint is non-blocking" );
	Check( fakeOTEndpoints[0].bound && fakeOTEndpoints[0].port == port && fakeOTEndpoints[0].host == host,
		"the game endpoint is bound to net_ip:net_port" );
	Check( fakeOTEndpoints[1].bound, "the resolver endpoint is bound" );
	Check( fakeOTEndpoints[0].notifier && fakeOTEndpoints[1].notifier, "both endpoints have notifiers" );
	Check( !fakeOTEndpoints[0].async && fakeOTEndpoints[1].async,
		"the resolver is asynchronous, the game endpoint synchronous" );
	Check( FindCvar( "net_port" ) && FindCvar( "net_port" )->integer == port, "net_port names the port bound" );
	Check( endpointTSDU == FAKE_OT_TSDU, "the endpoint's datagram limit is kept" );
	Check( localAddressCalls == 1 && broadcastCalls == 1, "local addresses and broadcasts are set up" );
	Check( !Printed( "WARNING" ), "no warning is printed" );

	fakeOTRcvCalls = 0;
	Check( !GetPacket( &from, &msg ) && fakeOTRcvCalls == 1, "the game endpoint is read" );

	Sys_ShutdownNetworking();
	Check( FakeOT_OpenEndpoints() == 0 && fakeOTEndpoints[0].closed == 1 && fakeOTEndpoints[1].closed == 1,
		"shutting down closes each endpoint once" );
	Check( fakeOTInited == 0 && fakeOTCloseCalls == 1 && !gOTInited, "shutting down closes Open Transport once" );
	Check( endpoint == kOTInvalidEndpointRef && resolverEndpoint == kOTInvalidEndpointRef, "no endpoint is left" );
	Check( fakeOTMisuse == 0, "OT is only used as documented" );
}

/* Start-up must have left networking off: everything it opened closed again,
 * so the UDP paths do nothing (loopback goes through net_chan.c, not here),
 * and a later shutdown must not close anything twice. */
static void ExpectDisabled( const char *warning ) {
	netadr_t from, a;
	msg_t msg;

	Check( FakeOT_OpenEndpoints() == 0, "every endpoint opened is closed" );
	Check( fakeOTInited == 0 && !gOTInited, "Open Transport is closed again" );
	Check( fakeOTCloseCalls == ( fakeOTInitCalls && !fakeOTFailInit ), "Open Transport is closed once if it opened" );
	Check( endpoint == kOTInvalidEndpointRef && resolverEndpoint == kOTInvalidEndpointRef, "no endpoint is left" );

	fakeOTRcvCalls = fakeOTResolveCalls = 0;
	Check( !GetPacket( &from, &msg ) && fakeOTRcvCalls == 0, "no packet is read" );
	Check( !Sys_StringToAdr( "idnewt", &a ) && fakeOTResolveCalls == 0, "no name is resolved" );

	Sys_ShutdownNetworking();
	Check( fakeOTMisuse == 0 && fakeOTCloseCalls == ( fakeOTInitCalls && !fakeOTFailInit ),
		"shutting down afterwards closes nothing twice" );
	Check( Printed( warning ) == 1, "the failure is reported" );
}

static void InitDefault( void ) {
	ResetInit();
	InitNetworking();
	ExpectStarted( kOTAnyInetAddress, PORT_SERVER );
	Check( Printed( "Opening IP socket: localhost:27960\n" ) == 1, "the port opened is printed" );
}

/* net_ip and net_port choose the address, as on the other platforms. */
static void InitCvars( void ) {
	static const UInt8 ip[4] = { 10, 0, 0, 7 };
	InetHost host;

	ResetInit();
	SetCvar( "net_port", "28000" );
	SetCvar( "net_ip", "10.0.0.7" );
	memcpy( &host, ip, 4 );
	fakeOTResolvedHost = host;
	InitNetworking();
	Check( fakeOTResolveCalls == 0, "a dotted net_ip is not looked up" );
	ExpectStarted( host, 28000 );

	/* a host name is looked up */
	ResetInit();
	SetCvar( "net_ip", "server.example" );
	fakeOTResolvedHost = host;
	InitNetworking();
	Check( fakeOTResolveCalls == 1 && !strcmp( fakeOTResolvedName, "server.example" ), "net_ip is resolved" );
	ExpectStarted( host, PORT_SERVER );

	/* "localhost" and "" mean every interface, as in NET_IPSocket */
	ResetInit();
	SetCvar( "net_ip", "LocalHost" );
	SetCvar( "net_port", "27961" );
	InitNetworking();
	Check( fakeOTResolveCalls == 0, "localhost is not resolved" );
	ExpectStarted( kOTAnyInetAddress, 27961 );

	ResetInit();
	SetCvar( "net_ip", "" );
	InitNetworking();
	Check( fakeOTResolveCalls == 0, "an empty net_ip is not resolved" );
	ExpectStarted( kOTAnyInetAddress, PORT_SERVER );
}

/* A port in use moves on to the next, and net_port follows. */
static void InitPortBusy( void ) {
	ResetInit();
	SetCvar( "net_port", "28000" );
	FakeOT_BusyPort( 28000 );
	FakeOT_BusyPort( 28001 );
	InitNetworking();
	Check( Printed( "Opening IP socket: " ) == 3, "three ports are tried" );
	Check( Printed( "WARNING: OTBind: error -3172\n" ) == 2, "each busy port is reported" );
	Check( !Printed( "networking disabled" ), "networking stays on" );
	printed[0] = 0;
	ExpectStarted( kOTAnyInetAddress, 28002 );
}

/* An OTBind that hands back another port than asked for counts as busy. */
static void InitPortReassigned( void ) {
	ResetInit();
	FakeOT_BusyPort( PORT_SERVER );
	fakeOTBindReassigns = 1;
	InitNetworking();
	printed[0] = 0;
	ExpectStarted( kOTAnyInetAddress, PORT_SERVER + 1 );
}

/* Ten busy ports: nothing is left open. */
static void InitPortExhausted( void ) {
	int i;

	ResetInit();
	for ( i = 0 ; i < 10 ; i++ ) {
		FakeOT_BusyPort( PORT_SERVER + i );
	}
	FakeOT_BusyPort( PORT_SERVER + 10 );	/* never tried */
	InitNetworking();
	Check( Printed( "Opening IP socket: " ) == 10, "ten ports are tried" );
	Check( Printed( "Couldn't allocate IP port\n" ) == 1, "running out of ports is reported" );
	Check( FindCvar( "net_port" )->integer == PORT_SERVER, "net_port is left alone" );
	ExpectDisabled( "WARNING: OTBind() failed" );
}

static void InitNoUDP( void ) {
	ResetInit();
	SetCvar( "net_noudp", "1" );
	InitNetworking();
	Check( fakeOTInitCalls == 0, "Open Transport is never opened" );
	ExpectDisabled( "UDP networking disabled by net_noudp\n" );
}

static int InitFailureCase( const char *name ) {
	ResetInit();
	if ( !strcmp( name, "init-fail-ot" ) ) {
		fakeOTFailInit = 1;
		InitNetworking();
		ExpectDisabled( "WARNING: InitOpenTransport() failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-config" ) ) {
		fakeOTFailConfig = 1;
		InitNetworking();
		ExpectDisabled( "WARNING: OTOpenEndpoint() failed (error -3282)" );
	} else if ( !strcmp( name, "init-fail-open" ) ) {
		fakeOTFailOpen = 1;
		InitNetworking();
		ExpectDisabled( "WARNING: OTOpenEndpoint() failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-nonblocking" ) ) {
		fakeOTFailNonBlocking = 1;
		InitNetworking();
		ExpectDisabled( "WARNING: OTSetNonBlocking() failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-resolver-open" ) ) {
		fakeOTFailOpen = 2;
		InitNetworking();
		ExpectDisabled( "WARNING: OTOpenEndpoint() for resolver failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-resolver-bind" ) ) {
		fakeOTFailBind = FAKE_OT_RESOLVER;
		InitNetworking();
		ExpectDisabled( "WARNING: OTBind() for resolver failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-notifier" ) ) {
		fakeOTFailNotifier = FAKE_OT_ENDPOINT;
		InitNetworking();
		ExpectDisabled( "WARNING: OTInstallNotifier() failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-resolver-notifier" ) ) {
		fakeOTFailNotifier = FAKE_OT_RESOLVER;
		InitNetworking();
		ExpectDisabled( "WARNING: OTInstallNotifier() for resolver failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-resolver-async" ) ) {
		fakeOTFailAsync = 1;
		InitNetworking();
		ExpectDisabled( "WARNING: OTSetAsynchronous() for resolver failed (error -3211)" );
	} else if ( !strcmp( name, "init-fail-net-ip" ) ) {
		SetCvar( "net_ip", "nosuchhost.invalid" );
		fakeOTResolveError = kOTBadAddressErr;
		InitNetworking();
		Check( Printed( "WARNING: couldn't resolve net_ip nosuchhost.invalid\n" ) == 1, "the bad net_ip is named" );
		ExpectDisabled( "WARNING: net_ip failed" );
	} else {
		return 0;
	}
	return 1;
}

/* ---------- #33: dotted quads and asynchronous lookups ---------- */

/* s must convert to ip with no lookup and no wait. */
static void ExpectDotted( const char *s, const UInt8 ip[4] ) {
	netadr_t a;

	memset( &a, 0, sizeof( a ) );
	fakeOTInitDNSCalls = fakeOTResolveCalls = waitCalls = 0;
	Check( Sys_StringToAdr( s, &a ), "a dotted quad converts" );
	Check( fakeOTInitDNSCalls == 0 && fakeOTResolveCalls == 0, "a dotted quad is not looked up" );
	Check( a.type == NA_IP && !memcmp( a.ip, ip, 4 ), "to its address" );
	Check( waitCalls == 0, "a dotted quad needs no wait" );
}

/* s is not a dotted quad: it must go to the resolver, as before. */
static void ExpectNotDotted( const char *s ) {
	netadr_t a;

	memset( &a, 0, sizeof( a ) );
	fakeOTResolveCalls = 0;
	memcpy( &fakeOTResolvedHost, serverIP, 4 );
	Check( Sys_StringToAdr( s, &a ) && fakeOTResolveCalls == 1 && !strcmp( fakeOTResolvedName, s )
		&& !memcmp( a.ip, serverIP, 4 ), "anything else is left to the resolver" );
}

static void HostDotted( void ) {
	static const UInt8 zero[4] = { 0, 0, 0, 0 }, ones[4] = { 255, 255, 255, 255 };
	static const UInt8 ten[4] = { 10, 0, 0, 7 };

	ResetInit();
	InitNetworking();
	printed[0] = 0;
	ExpectDotted( "192.246.40.70", serverIP );
	ExpectDotted( "0.0.0.0", zero );
	ExpectDotted( "255.255.255.255", ones );
	ExpectDotted( "10.0.0.7", ten );
	ExpectNotDotted( "1.2.3" );
	ExpectNotDotted( "1.2.3.4.5" );
	ExpectNotDotted( "256.1.1.1" );
	ExpectNotDotted( "1.2.3.1000" );
	ExpectNotDotted( "01.2.3.4" );		/* octal to inet_addr */
	ExpectNotDotted( "1.2.3.4x" );
	ExpectNotDotted( "1..2.3" );
	ExpectNotDotted( "3com.com" );
	Check( !printed[0] && handleOTErrors == 0, "nothing is printed" );
}

/* With networking off a dotted quad still converts, as inet_addr does on the
 * other platforms; a name fails without reaching OT. */
static void HostDisabled( void ) {
	netadr_t a;

	ResetInit();
	SetCvar( "net_noudp", "1" );
	InitNetworking();
	ExpectDotted( "192.246.40.70", serverIP );
	fakeOTResolveCalls = 0;
	Check( !Sys_StringToAdr( "idnewt", &a ) && fakeOTResolveCalls == 0, "a name is not looked up" );
}

static void StartResolver( void ) {
	ResetInit();
	InitNetworking();
	printed[0] = 0;
	memcpy( &fakeOTResolvedHost, serverIP, 4 );
}

/* Look name up: one lookup, during which the Mac keeps running (Sys_WaitEvent
 * is called) and never spins.  *elapsed is the virtual time it took. */
static qboolean Lookup( const char *name, netadr_t *a, int *elapsed ) {
	int start = fakeOTNow;
	qboolean ok;

	memset( a, 0, sizeof( *a ) );
	waitCalls = waitSpins = 0;
	fakeOTResolveCalls = 0;
	ok = Sys_StringToAdr( name, a );
	*elapsed = fakeOTNow - start;
	Check( fakeOTResolveCalls == 1, "the name is looked up once" );
	Check( waitCalls > 0, "the Mac keeps running while the name is looked up" );
	Check( waitSpins == 0, "every wait sleeps: the lookup does not spin" );
	return ok;
}

/* The resolver must have been replaced once by a fresh one, ready to use. */
static void ExpectFreshResolver( void ) {
	Check( fakeOTEndpoints[1].opened == 2 && fakeOTEndpoints[1].closed == 1,
		"the resolver is replaced once" );
	Check( resolverEndpoint == FAKE_OT_RESOLVER && fakeOTEndpoints[1].bound
		&& fakeOTEndpoints[1].notifier && fakeOTEndpoints[1].async, "the new resolver is ready" );
	Check( fakeOTMisuse == 0, "OT is only used as documented" );
}

/* The next lookup must work. */
static void ExpectNextLookup( void ) {
	netadr_t a;
	int elapsed;

	fakeOTResolveDelay = 50;
	memcpy( &fakeOTResolvedHost, clientIP, 4 );
	Check( Lookup( "idnewt", &a, &elapsed ) && !memcmp( a.ip, clientIP, 4 ), "the next lookup resolves" );
	Check( elapsed == 50, "the next lookup ends as OT reports it" );
}

static void ResolveAsync( void ) {
	netadr_t a;
	int elapsed;

	StartResolver();
	fakeOTResolveDelay = 250;
	Check( Lookup( "idnewt", &a, &elapsed ), "the name resolves" );
	Check( a.type == NA_IP && !memcmp( a.ip, serverIP, 4 ), "to OT's address" );
	Check( elapsed == 250, "the notifier wakes the wait as OT reports the address" );
	Check( waitCalls <= 3, "in a few waits" );
	Check( handleOTErrors == 0 && !printed[0], "nothing is printed" );
	ExpectNextLookup();
	Check( fakeOTEndpoints[1].opened == 1 && fakeOTMisuse == 0, "on the same resolver" );
}

/* NXDOMAIN: OT reports a failure, which fails the lookup. */
static void ResolveError( void ) {
	netadr_t a;
	int elapsed;

	StartResolver();
	fakeOTResolveDelay = 200;
	fakeOTResolveError = kOTBadNameErr;
	Check( !Lookup( "nosuchhost.invalid", &a, &elapsed ), "an unknown name fails" );
	Check( elapsed == 200 && handleOTErrors == 1, "as soon as OT reports it, with its error" );
	fakeOTResolveError = noErr;
	ExpectNextLookup();
}

/* No DNS server answers: OT's own timeout ends the lookup. */
static void ResolveOTTimeout( void ) {
	netadr_t a;
	int elapsed;

	StartResolver();
	fakeOTResolveDelay = 60000;
	Check( !Lookup( "idnewt", &a, &elapsed ), "a lookup OT times out fails" );
	Check( elapsed == 10000 && handleOTErrors == 1, "after OT's ten seconds, with its error" );
	Check( waitCalls <= 101, "in waits of a tenth of a second or more" );
	fakeOTResolveError = noErr;
	ExpectNextLookup();
}

/* OT never answers: the lookup still ends, a second after OT should have. */
static void ResolveTimeout( void ) {
	netadr_t a;
	int elapsed;

	StartResolver();
	fakeOTResolveDelay = -1;
	Check( !Lookup( "idnewt", &a, &elapsed ), "a lookup OT never answers fails" );
	Check( elapsed >= 11000 && elapsed <= 11100, "eleven seconds on" );
	Check( waitCalls <= 111, "in waits of a tenth of a second or more" );
	Check( Printed( "Sys_StringToAdr: lookup of idnewt timed out\n" ) == 1, "the timeout is reported" );
	ExpectFreshResolver();
	ExpectNextLookup();
}

/* Esc gives up at once. */
static void ResolveCancel( void ) {
	netadr_t a;
	int elapsed;

	StartResolver();
	fakeOTResolveDelay = -1;
	waitCancelCall = 3;
	Check( !Lookup( "idnewt", &a, &elapsed ), "a cancelled lookup fails" );
	Check( waitCalls == 3 && elapsed == 200, "as soon as Esc is pressed" );
	Check( Printed( "Sys_StringToAdr: lookup of idnewt cancelled\n" ) == 1, "the cancel is reported" );
	ExpectFreshResolver();
	waitCancelCall = 0;
	ExpectNextLookup();
}

/* ---------- #21: NET_Sleep ---------- */

static void StartServer( void ) {
	ResetInit();
	InitNetworking();
	printed[0] = 0;
	dedicated.integer = 1;
}

/* Sleep msec, returning the virtual time it took. */
static int Sleep( int msec ) {
	int start = fakeOTNow;

	waitCalls = waitSpins = fakeOTCountCalls = 0;
	NET_Sleep( msec );
	return fakeOTNow - start;
}

/* No datagram: the whole time is slept, rounded up to a tick, not spun. */
static void SleepTimeout( void ) {
	static const int msecs[] = { 50, 37, 100, 1, 16, 17 };
	int i, elapsed;

	StartServer();
	for ( i = 0 ; i < (int)( sizeof( msecs ) / sizeof( msecs[0] ) ) ; i++ ) {
		elapsed = Sleep( msecs[i] );
		Check( elapsed >= msecs[i] && elapsed < msecs[i] + 17, "NET_Sleep sleeps msec, to the tick" );
		Check( waitCalls >= 1 && waitCalls <= 2 && waitSpins == 0, "in one or two waits, never spinning" );
		Check( fakeOTCountCalls == 1, "the endpoint is checked once" );
	}
	Check( Sleep( 0 ) == 0 && waitCalls == 0, "no time, no sleep" );
	Check( fakeOTMisuse == 0, "OT is only used as documented" );
}

/* A datagram ends the sleep as it arrives, and is then read. */
static void SleepPacket( void ) {
	const UInt8 *data = Datagram( 0, 100 );
	netadr_t from;
	msg_t msg;
	int elapsed;

	StartServer();
	FakeOT_ArriveAt( 20, data, 100 );
	elapsed = Sleep( 50 );
	Check( elapsed == 20, "a datagram wakes the sleep as it arrives" );
	Check( waitCalls == 1 && waitSpins == 0, "in one wait" );
	Check( GetPacket( &from, &msg ) && msg.cursize == 100 && !memcmp( msg.data, data, 100 ), "the datagram is read" );
	Check( fakeOTMisuse == 0, "OT is only used as documented" );
}

/* A datagram that came before the sleep: no T_DATA will, so no sleep. */
static void SleepQueued( void ) {
	StartServer();
	FakeOT_QueueDatagram( Datagram( 0, 100 ), 100, clientIP, clientPort );
	Check( Sleep( 50 ) == 0 && waitCalls == 0, "a datagram waiting already means no sleep" );
}

/* A Mac event (a key typed at the console) ends the sleep, as stdin does. */
static void SleepEvent( void ) {
	StartServer();
	waitEventCall = 1;
	Check( Sleep( 50 ) == 0 && waitCalls == 1, "an event ends the sleep" );
}

/* A client runs full speed, as on Unix; so does a server with no network. */
static void SleepClient( void ) {
	StartServer();
	dedicated.integer = 0;
	Check( Sleep( 50 ) == 0 && waitCalls == 0 && fakeOTCountCalls == 0, "a client does not sleep" );

	ResetInit();
	SetCvar( "net_noudp", "1" );
	InitNetworking();
	dedicated.integer = 1;
	Check( Sleep( 50 ) == 0 && waitCalls == 0 && fakeOTCountCalls == 0, "nor a server without UDP" );
}

int main( int argc, char **argv ) {
	if ( argc != 2 ) {
		fprintf( stderr, "usage: %s case\n", argv[0] );
		return 2;
	}
	currentCase = argv[1];

	if ( !strcmp( currentCase, "host-normal" ) ) {
		CheckHostName( "idnewt", qtrue );
		CheckHostName( "1.2.3", qtrue );		/* not a dotted quad: left to OT */
	} else if ( !strcmp( currentCase, "host-255" ) ) {
		HostNameOfLength( 255, qtrue );
	} else if ( !strcmp( currentCase, "host-256" ) ) {
		HostNameOfLength( 256, qfalse );
	} else if ( !strcmp( currentCase, "host-1023" ) ) {
		HostNameOfLength( MAX_STRING_CHARS - 1, qfalse );
	} else if ( !strcmp( currentCase, "packet-normal" ) ) {
		NormalPackets();
	} else if ( !strcmp( currentCase, "packet-split" ) ) {
		OversizeThenNormal( MAX_MSGLEN + 1 );		/* two pieces */
		OversizeThenNormal( MAX_MSGLEN * 2 );		/* two full pieces */
		OversizeThenNormal( MAX_MSGLEN * 3 + 5 );	/* four pieces */
	} else if ( !strcmp( currentCase, "packet-full" ) ) {
		OversizeThenNormal( MAX_MSGLEN );
	} else if ( !strcmp( currentCase, "packet-drain-error" ) ) {
		DrainError();
	} else if ( !strcmp( currentCase, "packet-dropped-reports" ) ) {
		DroppedPacketReports();
	} else if ( !strcmp( currentCase, "event-oversize" ) ) {
		EventLoopOversize();
	} else if ( !strcmp( currentCase, "init-default" ) ) {
		InitDefault();
	} else if ( !strcmp( currentCase, "init-cvars" ) ) {
		InitCvars();
	} else if ( !strcmp( currentCase, "init-port-busy" ) ) {
		InitPortBusy();
	} else if ( !strcmp( currentCase, "init-port-reassigned" ) ) {
		InitPortReassigned();
	} else if ( !strcmp( currentCase, "init-port-exhausted" ) ) {
		InitPortExhausted();
	} else if ( !strcmp( currentCase, "init-noudp" ) ) {
		InitNoUDP();
	} else if ( !strcmp( currentCase, "host-dotted" ) ) {
		HostDotted();
	} else if ( !strcmp( currentCase, "host-disabled" ) ) {
		HostDisabled();
	} else if ( !strcmp( currentCase, "resolve-async" ) ) {
		ResolveAsync();
	} else if ( !strcmp( currentCase, "resolve-error" ) ) {
		ResolveError();
	} else if ( !strcmp( currentCase, "resolve-ot-timeout" ) ) {
		ResolveOTTimeout();
	} else if ( !strcmp( currentCase, "resolve-timeout" ) ) {
		ResolveTimeout();
	} else if ( !strcmp( currentCase, "resolve-cancel" ) ) {
		ResolveCancel();
	} else if ( !strcmp( currentCase, "sleep-timeout" ) ) {
		SleepTimeout();
	} else if ( !strcmp( currentCase, "sleep-packet" ) ) {
		SleepPacket();
	} else if ( !strcmp( currentCase, "sleep-queued" ) ) {
		SleepQueued();
	} else if ( !strcmp( currentCase, "sleep-event" ) ) {
		SleepEvent();
	} else if ( !strcmp( currentCase, "sleep-client" ) ) {
		SleepClient();
	} else if ( InitFailureCase( currentCase ) ) {
	} else {
		fprintf( stderr, "unknown case %s\n", currentCase );
		return 2;
	}
	return 0;
}

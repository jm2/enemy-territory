#include "../client/client.h"
#include "mac_local.h"
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <OpenTptInternet.h>

#ifndef AF_INET
#define AF_INET 2 
// Fallback if not defined, though OpenTptInternet usually defines it.
#endif

// InetAddress is defined in OpenTransportProviders.h

static qboolean	gOTInited;
static EndpointRef endpoint = kOTInvalidEndpointRef;
static EndpointRef resolverEndpoint = kOTInvalidEndpointRef;
static OTDataSize	endpointTSDU;	// largest datagram OTSndUData takes, <= 0 if unbounded

// set by the notifiers, which also wake the application from WaitNextEvent
static ProcessSerialNumber	netProcess;
static OTNotifyUPP			endpointNotifyUPP, resolverNotifyUPP;
static volatile qboolean	netDataArrived;		// T_DATA on the game endpoint
static volatile qboolean	resolveDone;		// T_RESOLVEADDRCOMPLETE
static volatile OTResult	resolveResult;

#define	RESOLVE_TIMEOUT		10000	// msec, as id's synchronous lookup
#define	RESOLVE_SLEEP		6		// ticks WaitNextEvent sleeps per check

#define	MAX_IPS		16
static	int		numIP;
static	InetInterfaceInfo	sys_inetInfo[MAX_IPS];

static	TUDErr	uderr;

void RcvUDErr( EndpointRef ep ) {
	memset( &uderr, 0, sizeof( uderr ) );
	uderr.addr.maxlen = 0;
	uderr.opt.maxlen = 0;
	OTRcvUDErr( ep, &uderr );
}

// Takes the endpoint the error happened on: previously this always ran
// OTLook on the main game endpoint, so look events latched on the resolver
// endpoint were never cleared and DNS could wedge permanently after its
// first error.
void HandleOTError( EndpointRef ep, int err, const char *func ) {
	int		r;
	static int lastErr;

	if ( err != lastErr ) {
		Com_Printf( "%s: error %i\n", func, err );
	}

	// if we don't call OTLook, things wedge
	r = OTLook( ep );
	if ( err != lastErr ) {
		Com_DPrintf( "%s: OTLook %i\n", func, r );
	}

	switch( r ) {
	case T_UDERR:
		RcvUDErr( ep );
		if ( err != lastErr ) {
			Com_DPrintf( "%s: OTRcvUDErr %i\n", func, (int)uderr.error );
		}
		break;
	default:
//		Com_Printf( "%s: Unknown OTLook error %i\n", func, r );
		break;
	}
	lastErr = err;	// don't spew tons of messages
}

/*
=================
GetFourByteOption
=================
*/
static OTResult GetFourByteOption(EndpointRef ep,
                           OTXTILevel level,
                           OTXTIName  name,
                           UInt32   *value)
{
   OTResult err;
   TOption  option;
   TOptMgmt request;
   TOptMgmt result;
   
   /* Set up the option buffer */
   option.len  = kOTFourByteOptionSize;
   option.level= level;
   option.name = name;
   option.status = 0;
   option.value[0] = 0;// Ignored because we're getting the value.

   /* Set up the request parameter for OTOptionManagement to point
    to the option buffer we just filled out */

   request.opt.buf= (UInt8 *) &option;
   request.opt.len= sizeof(option);
   request.flags= T_CURRENT;

   /* Set up the reply parameter for OTOptionManagement. */
   result.opt.buf  = (UInt8 *) &option;
   result.opt.maxlen = sizeof(option);
   
   err = OTOptionManagement(ep, &request, &result);

   if (err == noErr) {
      switch (option.status) 
      {
         case T_SUCCESS:
         case T_READONLY:
            *value = option.value[0];
            break;
         default:
            err = option.status;
            break;
      }
   }
            
   return (err);
}


/*
=================
SetFourByteOption
=================
*/
static OTResult SetFourByteOption(EndpointRef ep,
                           OTXTILevel level,
                           OTXTIName  name,
                           UInt32   value)
{
   OTResult err;
   TOption  option;
   TOptMgmt request;
   TOptMgmt result;
   
   /* Set up the option buffer to specify the option and value to
         set. */
   option.len  = kOTFourByteOptionSize;
   option.level= level;
   option.name = name;
   option.status = 0;
   option.value[0] = value;

   /* Set up request parameter for OTOptionManagement */
   request.opt.buf= (UInt8 *) &option;
   request.opt.len= sizeof(option);
   request.flags  = T_NEGOTIATE;

   /* Set up reply parameter for OTOptionManagement. */
   result.opt.buf  = (UInt8 *) &option;
   result.opt.maxlen  = sizeof(option);

   
   err = OTOptionManagement(ep, &request, &result);

   if (err == noErr) {
      if (option.status != T_SUCCESS) 
         err = option.status;
   }
            
   return (err);
}


/*
=====================
NET_GetLocalAddress
=====================
*/
void NET_GetLocalAddress( void ) {
	OSStatus		err;

	for ( numIP = 0 ; numIP < MAX_IPS ; numIP++ ) {
		err = OTInetGetInterfaceInfo( &sys_inetInfo[ numIP ], numIP );
		if ( err ) {
			break;
		}
		Com_Printf( "LocalAddress: %i.%i.%i.%i\n",
			((byte *)&sys_inetInfo[numIP].fAddress)[0],		
			((byte *)&sys_inetInfo[numIP].fAddress)[1],		
			((byte *)&sys_inetInfo[numIP].fAddress)[2],		
			((byte *)&sys_inetInfo[numIP].fAddress)[3] );		

		Com_Printf( "Netmask: %i.%i.%i.%i\n",
			((byte *)&sys_inetInfo[numIP].fNetmask)[0],		
			((byte *)&sys_inetInfo[numIP].fNetmask)[1],		
			((byte *)&sys_inetInfo[numIP].fNetmask)[2],		
			((byte *)&sys_inetInfo[numIP].fNetmask)[3] );		
	}
}


/*
=================
NET_EndpointNotify

Notifiers run at deferred task time, so they only set a flag and wake the
application (WakeUpProcess is interrupt-safe).  OT calls the game endpoint's
for T_DATA even though the endpoint is synchronous: NET_Sleep waits for it.
=================
*/
static pascal void NET_EndpointNotify( void *contextPtr, OTEventCode code,
									   OTResult result, void *cookie ) {
	if ( code == T_DATA ) {
		netDataArrived = qtrue;
		WakeUpProcess( &netProcess );
	}
}

/*
=================
NET_ResolverNotify

The asynchronous resolver endpoint's: Sys_StringToAdr waits for its lookup.
=================
*/
static pascal void NET_ResolverNotify( void *contextPtr, OTEventCode code,
									   OTResult result, void *cookie ) {
	if ( code == T_RESOLVEADDRCOMPLETE ) {
		resolveResult = result;
		resolveDone = qtrue;
		WakeUpProcess( &netProcess );
	}
}


/*
==================
NET_CloseResolver

Closing the endpoint also abandons a lookup in progress: its notifier is
not called again.
==================
*/
static void NET_CloseResolver( void ) {
	if ( resolverEndpoint != kOTInvalidEndpointRef ) {
		OTUnbind( resolverEndpoint );
		OTCloseProvider( resolverEndpoint );
		resolverEndpoint = kOTInvalidEndpointRef;
	}
}

/*
==================
NET_CloseOpenTransport

Releases what Sys_InitNetworking opened, in reverse order, so a failed
start leaves UDP off (loopback still works) rather than half-initialised.
==================
*/
static void NET_CloseOpenTransport( void ) {
	NET_CloseResolver();
	if ( endpoint != kOTInvalidEndpointRef ) {
		OTUnbind( endpoint );
		OTCloseProvider( endpoint );
		endpoint = kOTInvalidEndpointRef;
	}
	endpointTSDU = 0;
	if ( gOTInited ) {
		CloseOpenTransport();
		gOTInited = false;
	}
}

/*
==================
NET_InitFailed
==================
*/
static void NET_InitFailed( const char *stage, OSStatus err ) {
	Com_Printf( "WARNING: %s failed (error %i), networking disabled\n", stage, (int)err );
	NET_CloseOpenTransport();
	Com_Printf( "------------------------------\n" );
}

/*
==================
NET_OpenUDPEndpoint
==================
*/
static EndpointRef NET_OpenUDPEndpoint( TEndpointInfo *info, OSStatus *err ) {
	OTConfigurationRef config;
	EndpointRef	ep;

	config = OTCreateConfiguration( kUDPName );
	if ( !config || config == kOTInvalidConfigurationPtr ) {
		*err = kOTBadConfigurationErr;
		return kOTInvalidEndpointRef;
	}
	// OTOpenEndpoint disposes of config whether or not it succeeds
	ep = OTOpenEndpoint( config, 0, info, err );
	if ( *err != noErr ) {
		return kOTInvalidEndpointRef;
	}
	return ep;
}

/*
==================
NET_BindEndpoint

Binds ep to host:port and checks OT gave that port, not another one.
==================
*/
static OSStatus NET_BindEndpoint( EndpointRef ep, InetHost host, int port ) {
	TBind			bind, bindOut;
	InetAddress		in, out;
	OSStatus		err;

	memset( &in, 0, sizeof( in ) );
	in.fAddressType = AF_INET;
	in.fPort = port;
	in.fHost = host;

	memset( &out, 0, sizeof( out ) );

	bind.addr.maxlen = sizeof( in );
	bind.addr.len = sizeof( in );
	bind.addr.buf = (unsigned char *)&in;
	bind.qlen = 0;
	
	bindOut.addr.maxlen = sizeof( out );
	bindOut.addr.len = sizeof( out );
	bindOut.addr.buf = (unsigned char *)&out;
	bindOut.qlen = 0;
	
	err = OTBind( ep, &bind, &bindOut );
	if ( err == noErr && port && out.fPort != in.fPort ) {
		OTUnbind( ep );
		err = kOTAddressBusyErr;
	}
	return err;
}

/*
==================
NET_OpenResolver

Opens the endpoint that resolves names, bound to any port and asynchronous,
so that Sys_StringToAdr can keep the Mac running while it waits.  Returns
the stage that failed, with its error in *err, or NULL.
==================
*/
static const char *NET_OpenResolver( OSStatus *err ) {
	resolverEndpoint = NET_OpenUDPEndpoint( NULL, err );
	if ( *err != noErr ) {
		return "OTOpenEndpoint() for resolver";
	}
	*err = NET_BindEndpoint( resolverEndpoint, kOTAnyInetAddress, 0 );
	if ( *err != noErr ) {
		return "OTBind() for resolver";
	}
	*err = OTInstallNotifier( resolverEndpoint, resolverNotifyUPP, NULL );
	if ( *err != noErr ) {
		return "OTInstallNotifier() for resolver";
	}
	*err = OTSetAsynchronous( resolverEndpoint );
	if ( *err != noErr ) {
		return "OTSetAsynchronous() for resolver";
	}
	return NULL;
}

/*
==================
NET_ResetResolver

A lookup that is given up leaves its request with the resolver: replace the
endpoint, so the next lookup starts clean.
==================
*/
static void NET_ResetResolver( void ) {
	const char	*stage;
	OSStatus	err;

	NET_CloseResolver();
	stage = NET_OpenResolver( &err );
	if ( stage ) {
		Com_Printf( "WARNING: %s failed (error %i), host names will not resolve\n", stage, (int)err );
		NET_CloseResolver();
	}
}

/*
==================
Sys_InitNetworking

Opens a UDP endpoint on net_ip:net_port, trying the next nine ports when one
is taken, as NET_OpenIP does on the other platforms, plus a second endpoint
for resolving names.  Any failure closes everything again.


struct InetAddress
{
		OTAddressType	fAddressType;	// always AF_INET
		InetPort		fPort;			// Port number 
		InetHost		fHost;			// Host address in net byte order
		UInt8			fUnused[8];		// Traditional unused bytes
};
typedef struct InetAddress InetAddress;

==================
*/
void Sys_InitNetworking( void ) {
	OSStatus		err;
	const char		*stage;
	TEndpointInfo	info;
	cvar_t			*noudp;
	cvar_t			*ip;
	netadr_t		adr;
	InetHost		host;
	int				port;
	int				i;

	Com_Printf( "----- Sys_InitNetworking -----\n" );

	noudp = Cvar_Get( "net_noudp", "0", CVAR_LATCH | CVAR_ARCHIVE );
	ip = Cvar_Get( "net_ip", "localhost", CVAR_LATCH );
	port = Cvar_Get( "net_port", va( "%i", PORT_SERVER ), CVAR_LATCH )->integer;

	if ( noudp->integer ) {
		Com_Printf( "UDP networking disabled by net_noudp\n" );
		Com_Printf( "------------------------------\n" );
		return;
	}

	// init OpenTransport	
	Com_Printf( "... InitOpenTransport()\n" );
	err = InitOpenTransport();
	if ( err != noErr ) {
		NET_InitFailed( "InitOpenTransport()", err );
		return;
	}
	
  	gOTInited = true;

	// the notifiers wake this process
	GetCurrentProcess( &netProcess );
	if ( !endpointNotifyUPP ) {
		endpointNotifyUPP = NewOTNotifyUPP( NET_EndpointNotify );
		resolverNotifyUPP = NewOTNotifyUPP( NET_ResolverNotify );
	}

	// get an endpoint
	Com_Printf( "... OTOpenEndpoint()\n" );
	endpoint = NET_OpenUDPEndpoint( &info, &err );
	if ( err != noErr ) {
		NET_InitFailed( "OTOpenEndpoint()", err );
		return;
	}

	// remember the largest datagram the endpoint accepts for Sys_SendPacket;
	// T_INFINITE or T_INVALID (<= 0) leave the check to OTSndUData itself
	endpointTSDU = info.tsdu;

	// set non-blocking: a blocking OTRcvUData would stall the main loop
	err = OTSetNonBlocking( endpoint );
	if ( err != noErr ) {
		NET_InitFailed( "OTSetNonBlocking()", err );
		return;
	}

	// T_DATA wakes NET_Sleep
	err = OTInstallNotifier( endpoint, endpointNotifyUPP, NULL );
	if ( err != noErr ) {
		NET_InitFailed( "OTInstallNotifier()", err );
		return;
	}

	// get an endpoint just for resolving addresses, because
	// I was having crashing problems doing it on the same endpoint
	stage = NET_OpenResolver( &err );
	if ( stage ) {
		NET_InitFailed( stage, err );
		return;
	}

	// bind to net_ip, or to every interface for "localhost" or ""
	if ( !ip->string[0] || !Q_stricmp( ip->string, "localhost" ) ) {
		host = kOTAnyInetAddress;
	} else if ( Sys_StringToAdr( ip->string, &adr ) ) {
		host = *(InetHost *)adr.ip;
	} else {
		Com_Printf( "WARNING: couldn't resolve net_ip %s\n", ip->string );
		NET_InitFailed( "net_ip", kOTBadAddressErr );
		return;
	}

	// automatically scan for a valid port, so multiple
	// dedicated servers can be started without requiring
	// a different net_port for each one
	Com_Printf( "... OTBind()\n" );
	for ( i = 0 ; i < 10 ; i++ ) {
		Com_Printf( "Opening IP socket: %s:%i\n", ip->string, port + i );
		err = NET_BindEndpoint( endpoint, host, port + i == PORT_ANY ? 0 : port + i );
		if ( err == noErr ) {
			break;
		}
		Com_Printf( "WARNING: OTBind: error %i\n", (int)err );
	}
	if ( err != noErr ) {
		Com_Printf( "WARNING: Couldn't allocate IP port\n" );
		NET_InitFailed( "OTBind()", err );
		return;
	}
	Cvar_SetValue( "net_port", port + i );

	// get the local address for LAN client detection
	NET_GetLocalAddress();

	// set to allow broadcasts
	err = SetFourByteOption( endpoint, INET_IP, IP_BROADCAST, T_YES );

	if ( err != noErr ) {
		Com_Printf( "IP_BROADCAST failed\n" );
	}
		
	Com_Printf( "------------------------------\n" );
}


/*
==================
Sys_ShutdownNetworking
==================
*/
void Sys_ShutdownNetworking( void ) {
	Com_Printf( "Sys_ShutdownNetworking();\n" );

	NET_CloseOpenTransport();
}

/*
=============
NET_StringToHost

A dotted quad, four decimal numbers 0-255, needs no lookup: convert it here,
as inet_addr does in id's Sys_StringToSockaddr.  (OTInetStringToHost is
CarbonLib only.)  Anything else, leading zeros included, which inet_addr
reads as octal, is left to the resolver as before.
=============
*/
static qboolean NET_StringToHost( const char *s, byte *ip ) {
	byte	b[4];
	int		i, n, digits;

	for ( i = 0 ; i < 4 ; i++ ) {
		n = digits = 0;
		while ( *s >= '0' && *s <= '9' ) {
			if ( digits && !n ) {
				return qfalse;	// a leading zero
			}
			n = n * 10 + *s++ - '0';
			if ( ++digits > 3 || n > 255 ) {
				return qfalse;
			}
		}
		if ( !digits || *s++ != ( i < 3 ? '.' : 0 ) ) {
			return qfalse;
		}
		b[i] = n;
	}
	memcpy( ip, b, 4 );
	return qtrue;
}

/*
=============
Sys_StringToAdr


Does NOT parse port numbers


idnewt
192.246.40.70

A host name is looked up asynchronously.  While OT works, Sys_WaitEvent
keeps the Mac running: the system and other processes get the time, windows
update and File > Quit works.  Esc or Command-period gives up, as does
RESOLVE_TIMEOUT passing, which OT's own timeout normally reports first.
=============
*/
qboolean	Sys_StringToAdr( const char *s, netadr_t *a ) {
	// static: OT fills them in after OTResolveAddress returns
	static TBind		in, out;
	static InetAddress	inAddr;
	static DNSAddress	dnsAddr;
	static qboolean		resolving;
	OSStatus	err;
	qboolean	cancel;
	int			start;

	if ( NET_StringToHost( s, a->ip ) ) {
		a->type = NA_IP;
		return qtrue;
	}

	// a lookup can't start while another waits (from Sys_WaitEvent)
	if ( !resolverEndpoint || resolving ) {
		return qfalse;
	}

	// OTInitDNSAddress copies the whole name into dnsAddr.fName, which holds
	// kMaxHostNameLen (255) characters; NET_StringToAdr passes up to 1023
	if ( strlen( s ) >= sizeof( dnsAddr.fName ) ) {
		Com_Printf( "Sys_StringToAdr: host name longer than %i characters\n",
			(int)sizeof( dnsAddr.fName ) - 1 );
		return qfalse;
	}

	memset( &in, 0, sizeof( in ) );
	in.addr.buf = (UInt8 *) &dnsAddr;
	in.addr.len = OTInitDNSAddress(&dnsAddr, (char *)s );
	in.qlen = 0;
	
	memset( &out, 0, sizeof( out ) );
	out.addr.buf = (byte *)&inAddr;
	out.addr.maxlen = sizeof( inAddr );
	out.qlen = 0;

	resolveDone = qfalse;
	err = OTResolveAddress( resolverEndpoint, &in, &out, RESOLVE_TIMEOUT );
	if ( err == noErr ) {
		resolving = qtrue;
		cancel = qfalse;
		start = Sys_Milliseconds();
		while ( !resolveDone && !cancel && Sys_Milliseconds() - start < RESOLVE_TIMEOUT + 1000 ) {
			Sys_WaitEvent( RESOLVE_SLEEP, &cancel );
		}
		resolving = qfalse;
		if ( !resolveDone ) {
			Com_Printf( "Sys_StringToAdr: lookup of %s %s\n", s, cancel ? "cancelled" : "timed out" );
			NET_ResetResolver();
			return qfalse;
		}
		err = resolveResult;
	}
	if ( err ) {
		HandleOTError( resolverEndpoint, err, "Sys_StringToAdr" );
		return qfalse;
	}
	
	a->type = NA_IP;
	*(int *)a->ip = inAddr.fHost;

	return qtrue;
}

/*
==================
Sys_SendPacket
==================
*/
void Sys_SendPacket( int length, const void *data, netadr_t to ) {
	TUnitData	d;
	InetAddress	inAddr;
	OSStatus	err;
	int			now;
	static int	lastDropTime;
	static qboolean	dropped;

	if ( !endpoint ) {
		return;
	}

	// Out-of-band replies such as statusResponse can legitimately exceed
	// the netchan's 1400 byte fragment size; send them whole like the other
	// platforms do.  Only a datagram the endpoint cannot carry is dropped,
	// never Com_Error'd: that let one spoofed getstatus stop the server.
	if ( endpointTSDU > 0 && length > endpointTSDU ) {
		now = Sys_Milliseconds();
		if ( !dropped || now - lastDropTime >= 1000 ) {
			Com_Printf( "Sys_SendPacket: dropped %i byte datagram to %s (limit %i)\n",
				length, NET_AdrToString( to ), (int)endpointTSDU );
			lastDropTime = now;
			dropped = qtrue;
		}
		return;
	}

	inAddr.fAddressType = AF_INET;
	inAddr.fPort = to.port;	
	if ( to.type == NA_BROADCAST ) {
		inAddr.fHost = -1;
	} else {
		inAddr.fHost = *(int *)&to.ip;
	}
	
	memset( &d, 0, sizeof( d ) );
	
	d.addr.len = sizeof( inAddr );
	d.addr.maxlen = sizeof( inAddr );
	d.addr.buf = (unsigned char *)&inAddr;

	d.opt.len = 0;
	d.opt.maxlen = 0;
	d.opt.buf = NULL;
		
	d.udata.len = length;
	d.udata.maxlen = length;
	d.udata.buf = (unsigned char *)data;
	
	err = OTSndUData( endpoint, &d );
	if ( err ) {
		HandleOTError( endpoint, err, "Sys_SendPacket" );
	}
}

/*
==================
Sys_ReportDroppedPacket

Run on Mac OS 9, a client saw bursts of pieces with no source address after
long stalls, a report for each. The cause is not known, so a report says what
Open Transport returned. An error waiting on the endpoint explains the piece,
and clears it. Otherwise this prints once a second at most: a print can be a
disk write, and these come in bursts, in the frames that can least afford it.
==================
*/
#define	DROPPED_PACKET_REPORT_MSEC	1000

static void Sys_ReportDroppedPacket( const netadr_t *from, OTByteCount addrLen, OTByteCount length, OTFlags flags ) {
	static int	lastReport;
	static int	suppressed;
	OTResult	look;
	int			now;

	look = OTLook( endpoint );
	if ( look == T_UDERR ) {
		RcvUDErr( endpoint );
		if ( addrLen == 0 ) {
			return;
		}
	}

	now = Sys_Milliseconds();
	if ( lastReport && now - lastReport < DROPPED_PACKET_REPORT_MSEC ) {
		suppressed++;
		return;
	}
	lastReport = now ? now : 1;

	if ( addrLen ) {
		Com_Printf( "Oversize packet from %s\n", NET_AdrToString( *from ) );
	} else {
		Com_Printf( "Dropped a packet with no source address: %i bytes, flags 0x%x, OTLook %i\n",
			(int)length, (unsigned int)flags, (int)look );
	}
	if ( suppressed ) {
		Com_Printf( "%i more packets dropped since the last report\n", suppressed );
		suppressed = 0;
	}
}


/*
==================
Sys_GetPacket

Never called by the game logic, just the system event queing
==================
*/
qboolean	Sys_GetPacket ( netadr_t *net_from, msg_t *net_message ) {
	TUnitData	d;
	InetAddress	inAddr;
	OSStatus	err;
	OTFlags		flags;
	
	if ( !endpoint ) {
		return qfalse;
	}

	inAddr.fAddressType = AF_INET;
	inAddr.fPort = 0;
	inAddr.fHost = 0;

	memset( &d, 0, sizeof( d ) );

	d.addr.len = sizeof( inAddr );
	d.addr.maxlen = sizeof( inAddr );
	d.addr.buf = (unsigned char *)&inAddr;

	d.opt.len = 0;
	d.opt.maxlen = 0;
	d.opt.buf = 0;
		
	d.udata.len = net_message->maxsize;
	d.udata.maxlen = net_message->maxsize;
	d.udata.buf = net_message->data;
	
	err = OTRcvUData( endpoint, &d, &flags );
	if ( err ) {
		if ( err == kOTNoDataErr ) {
			return false;
		}
		HandleOTError( endpoint, err, "Sys_GetPacket" );
		return qfalse;
	}

	net_from->type = NA_IP;
	net_from->port = inAddr.fPort;
	*(int *)net_from->ip = inAddr.fHost;

	// A datagram larger than the buffer comes back in pieces with T_MORE set.
	// Read the rest, or the next call would return it as a packet of its own
	// with no source address, and drop the whole datagram as unix_net.c and
	// win_net.c do; like them, also drop one that exactly fills the buffer.
	// A piece with no source address is not a packet: drop it the same way.
	if ( ( flags & T_MORE ) || (int)d.udata.len >= net_message->maxsize
		|| d.addr.len == 0 ) {
		OTFlags		firstFlags = flags;
		OTByteCount	firstAddrLen = d.addr.len, firstLen = d.udata.len;

		while ( flags & T_MORE ) {
			err = OTRcvUData( endpoint, &d, &flags );
			if ( err ) {
				HandleOTError( endpoint, err, "Sys_GetPacket" );
				break;
			}
		}
		Sys_ReportDroppedPacket( net_from, firstAddrLen, firstLen, firstFlags );
		return qfalse;
	}

	net_message->cursize = d.udata.len;
	
	return qtrue;
}


/*
==================
Sys_IsLANAddress

LAN clients will have their rate var ignored
==================
*/
qboolean	Sys_IsLANAddress (netadr_t adr) {
	int		i;
	int		ip;
	
	if ( adr.type == NA_LOOPBACK ) {
		return qtrue;
	}

	if ( adr.type != NA_IP ) {
		return qfalse;
	}
	
	for ( ip = 0 ; ip < numIP ; ip++ ) {
		for ( i = 0 ; i < 4 ; i++ ) {
			if ( ( adr.ip[i] & ((byte *)&sys_inetInfo[ip].fNetmask)[i] )
			!= ( ((byte *)&sys_inetInfo[ip].fAddress)[i] & ((byte *)&sys_inetInfo[ip].fNetmask)[i] ) ) {
				break;
			}
		}
		if ( i == 4 ) {
			return qtrue;	// matches this subnet
		}
	}
	
	return qfalse;
}


/*
====================
NET_Sleep

sleeps msec or until net socket is ready, as unix_net.c does with select:
WaitNextEvent gives the time to other processes until the endpoint's
notifier wakes it for a datagram.  A Mac event, such as a key typed at the
console, ends the sleep too, as stdin does select.
====================
*/
void NET_Sleep( int msec ) {
	OTByteCount	bytes;
	int			start, remaining;

	if ( !endpoint || !com_dedicated->integer ) {
		return; // we're not a server, just run full speed
	}

	netDataArrived = qfalse;
	if ( OTCountDataBytes( endpoint, &bytes ) == noErr ) {
		return;	// a datagram is already waiting, so no T_DATA will come
	}

	start = Sys_Milliseconds();
	for ( ;; ) {
		remaining = msec - ( Sys_Milliseconds() - start );
		if ( remaining <= 0 || netDataArrived ) {
			break;
		}
		// in ticks, rounded up so the last moments are slept, not spun
		if ( Sys_WaitEvent( ( remaining * 60 + 999 ) / 1000, NULL ) ) {
			break;
		}
	}
}

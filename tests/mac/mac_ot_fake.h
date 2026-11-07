/* A fake Open Transport for host regressions of code/mac/mac_net.c, whose
 * functions the runner extracts verbatim (the real file needs the whole Mac
 * Toolbox).  The types and constants are those of Universal Interfaces 3.4
 * (Retro68 OpenTransport.h, OpenTransportProviders.h, MacErrors.h), cut to
 * what the extracted code uses, with the target's field sizes.
 *
 * The fake endpoint holds a queue of datagrams and reads them as XTI's
 * t_rcvudata does: a datagram larger than the caller's buffer comes back in
 * pieces with T_MORE set, and only the first piece carries the source
 * address.  fakeOTFailCall makes that OTRcvUData call (counting from 1)
 * fail with fakeOTFailError and read nothing.  The fake resolver returns
 * fakeOTResolvedHost.  OTInitDNSAddress copies the name with no bound, as
 * the target's does.  Every call is counted.
 *
 * For Sys_InitNetworking the fake also opens, binds and closes endpoints:
 * the first OTOpenEndpoint gives FAKE_OT_ENDPOINT and the second
 * FAKE_OT_RESOLVER.  Ports in fakeOTBusyPorts are taken: binding one fails
 * with kOTAddressBusyErr, or with fakeOTBindReassigns succeeds on another
 * port as XTI's t_bind may.  The fakeOTFail* settings make one stage fail
 * with FAKE_OT_INJECTED_ERR, and fakeOTResolveError makes resolving fail.
 *
 * For NET_Sleep and the asynchronous lookups (#21, #33) the fake keeps a
 * virtual clock, fakeOTNow, in msec.  OTInstallNotifier and OTSetAsynchronous
 * are recorded.  On a synchronous endpoint OTResolveAddress blocks, moving the
 * clock on by the lookup's time.  On an asynchronous one it returns at once
 * and the lookup completes fakeOTResolveDelay msec later (never if negative),
 * reported to the notifier when FakeOT_Advance passes that time, as is a
 * datagram FakeOT_ArriveAt schedules (T_DATA).  WakeUpProcess is counted, so
 * the fake WaitNextEvent can end its sleep early. */
#ifndef MAC_OT_FAKE_H
#define MAC_OT_FAKE_H

#include <stdint.h>
#include <string.h>

typedef uint8_t		UInt8;
typedef uint16_t	UInt16;
typedef uint32_t	UInt32;
typedef int32_t		OSStatus;
typedef UInt32		ByteCount;
typedef ByteCount	OTByteCount;
typedef UInt32		OTFlags;
typedef UInt32		OTQLen;
typedef UInt32		OTTimeout;
typedef UInt16		OTAddressType;
typedef UInt16		InetPort;
typedef UInt32		InetHost;
typedef void		*EndpointRef;
typedef char		InetDomainName[256];
typedef int32_t		OTResult;
typedef int32_t		OTDataSize;
typedef UInt32		OTServiceType;
typedef UInt32		OTOpenFlags;
typedef UInt32		OTXTILevel;
typedef UInt32		OTXTIName;
typedef void		*ProviderRef;
typedef struct OTConfiguration	*OTConfigurationRef;
typedef UInt32		OTEventCode;
typedef void		( *OTNotifyUPP )( void *contextPtr, OTEventCode code, OTResult result, void *cookie );
typedef struct {
	unsigned long	highLongOfPSN;
	unsigned long	lowLongOfPSN;
} ProcessSerialNumber;

#define pascal					/* Mac calling convention: nothing on the host */
#define NewOTNotifyUPP( proc )	( proc )

#define kOTInvalidEndpointRef		((EndpointRef)0L)
#define kOTInvalidConfigurationPtr	((OTConfigurationRef)-1L)
#define kUDPName					"udp"

enum { false = 0, true = 1 };	/* MacTypes.h */
#define nil NULL
enum { T_MORE = 0x0001 };
enum { noErr = 0 };
enum { kOTLookErr = -3158, kOTNoDataErr = -3162 };
enum { T_UDERR = 0x0040 };
enum { kOTBadAddressErr = -3150, kOTOutStateErr = -3155, kOTStateChangeErr = -3168,
	kOTBadNameErr = -3170, kOTAddressBusyErr = -3172, kOTOutOfMemoryErr = -3211,
	kETIMEDOUTErr = -3259, kOTBadConfigurationErr = -3282 };
enum { T_DATA = 0x0004, T_RESOLVEADDRCOMPLETE = 0x20000009 };
enum { INET_IP = 0x00, IP_BROADCAST = 0x20, T_YES = 1 };
enum { kOTAnyInetAddress = 0 };
enum { AF_DNS = 42 };
#define AF_INET 2

typedef struct {
	ByteCount	maxlen;
	ByteCount	len;
	UInt8		*buf;
} TNetbuf;

typedef struct {
	TNetbuf		addr;
	OTQLen		qlen;
} TBind;

typedef struct {
	TNetbuf		addr;
	TNetbuf		opt;
	TNetbuf		udata;
} TUnitData;

typedef struct {
	OTDataSize		addr;
	OTDataSize		options;
	OTDataSize		tsdu;
	OTDataSize		etsdu;
	OTDataSize		connect;
	OTDataSize		discon;
	OTServiceType	servtype;
	UInt32			flags;
} TEndpointInfo;

typedef struct {
	OTAddressType	fAddressType;	/* always AF_INET */
	InetPort		fPort;			/* network byte order */
	InetHost		fHost;			/* network byte order */
	UInt8			fUnused[8];
} InetAddress;

typedef struct {
	OTAddressType	fAddressType;	/* always AF_DNS */
	InetDomainName	fName;
} DNSAddress;

typedef struct {
	TNetbuf		addr;
	TNetbuf		opt;
	int32_t		error;
} TUDErr;

typedef struct {
	InetHost		fAddress;
	InetHost		fNetmask;
	InetHost		fBroadcastAddr;
	InetHost		fDefaultGatewayAddr;
	InetHost		fDNSAddr;
	UInt16			fVersion;
	UInt16			fHWAddrLen;
	UInt8			*fHWAddr;
	UInt32			fIfMTU;
	UInt8			*fReservedPtrs[2];
	InetDomainName	fDomainName;
	UInt32			fIPSecondaryCount;
	UInt8			fReserved[252];
} InetInterfaceInfo;

#define FAKE_OT_ENDPOINT	((EndpointRef)&fakeOTDatagrams)
#define FAKE_OT_RESOLVER	((EndpointRef)&fakeOTResolvedHost)
#define FAKE_OT_MAX_QUEUE	8

typedef struct {
	const UInt8	*data;
	int			length;
	InetAddress	from;
	int			noAddress;
} fakeOTDatagram_t;

static fakeOTDatagram_t	fakeOTDatagrams[FAKE_OT_MAX_QUEUE];
static int		fakeOTQueued;		/* datagrams queued */
static int		fakeOTNext;			/* the datagram the next read returns from */
static int		fakeOTOffset;		/* bytes of it already read */
static int		fakeOTRcvCalls;
static int		fakeOTFailCall;		/* 0: no call fails */
static OSStatus	fakeOTFailError;
static OSStatus	fakeOTResolveError;	/* noErr: resolving succeeds */

static DNSAddress	*fakeOTDNSAddress;	/* the last OTInitDNSAddress call's */
static OTByteCount	fakeOTDNSLength;
static int		fakeOTInitDNSCalls;
static InetHost		fakeOTResolvedHost;
static char		fakeOTResolvedName[sizeof( InetDomainName )];
static int		fakeOTResolveCalls;

/* Empty the endpoint and clear its counts and injected failure. */
static void FakeOT_ResetEndpoint( void ) {
	fakeOTQueued = fakeOTNext = fakeOTOffset = fakeOTRcvCalls = fakeOTFailCall = 0;
}

/* Queue a datagram from ip:port (bytes in network order) on the endpoint. */
static void FakeOT_QueueDatagram( const UInt8 *data, int length, const UInt8 ip[4], const UInt8 port[2] ) {
	fakeOTDatagram_t *d = &fakeOTDatagrams[fakeOTQueued++];

	d->data = data;
	d->length = length;
	d->noAddress = 0;
	memset( &d->from, 0, sizeof( d->from ) );
	d->from.fAddressType = AF_INET;
	memcpy( &d->from.fHost, ip, 4 );
	memcpy( &d->from.fPort, port, 2 );
}

/* fakeOTLookEvent is what OTLook reports, until RcvUDErr takes a T_UDERR. */
static OTResult fakeOTLookEvent;
OTResult OTLook( EndpointRef ref ) {
	(void)ref;
	return fakeOTLookEvent;
}

OSStatus OTRcvUData( EndpointRef ref, TUnitData *udata, OTFlags *flags ) {
	fakeOTDatagram_t	*d;
	int					n;

	fakeOTRcvCalls++;
	if ( fakeOTRcvCalls == fakeOTFailCall ) {
		return fakeOTFailError;
	}
	if ( ref != FAKE_OT_ENDPOINT || fakeOTNext == fakeOTQueued ) {
		return kOTNoDataErr;
	}
	d = &fakeOTDatagrams[fakeOTNext];

	udata->addr.len = 0;	/* continuation pieces carry no address */
	if ( fakeOTOffset == 0 && !d->noAddress && udata->addr.maxlen >= sizeof( d->from ) ) {
		memcpy( udata->addr.buf, &d->from, sizeof( d->from ) );
		udata->addr.len = sizeof( d->from );
	}
	udata->opt.len = 0;

	n = d->length - fakeOTOffset;
	if ( n > (int)udata->udata.maxlen ) {
		n = udata->udata.maxlen;
	}
	memcpy( udata->udata.buf, d->data + fakeOTOffset, n );
	udata->udata.len = n;
	fakeOTOffset += n;

	*flags = 0;
	if ( fakeOTOffset < d->length ) {
		*flags = T_MORE;
	} else {
		fakeOTNext++;
		fakeOTOffset = 0;
	}
	return 0;
}

OTByteCount OTInitDNSAddress( DNSAddress *addr, char *str ) {
	int		i;

	fakeOTInitDNSCalls++;
	addr->fAddressType = AF_DNS;
	for ( i = 0 ; ( addr->fName[i] = str[i] ) != 0 ; i++ ) {
	}
	fakeOTDNSAddress = addr;
	fakeOTDNSLength = sizeof( addr->fAddressType ) + i + 1;
	return fakeOTDNSLength;
}

/* ---------- the virtual clock, notifiers and asynchronous lookups ---------- */

static int		fakeOTNow;				/* msec */
static int		fakeOTWakeups;			/* WakeUpProcess calls */
static int		fakeOTProcessKnown;		/* GetCurrentProcess was called */
static int		fakeOTCountCalls;		/* OTCountDataBytes calls */
static int		fakeOTResolveDelay;		/* msec a lookup takes; < 0: never completes */
static int		fakeOTArriveAt = -1;	/* when a scheduled datagram arrives */
static const UInt8	*fakeOTArriveData;
static int		fakeOTArriveLength;

/* the lookup in progress on the asynchronous resolver */
static int		fakeOTResolvePending;
static int		fakeOTResolveDue;		/* fakeOTNow it completes at, < 0: never */
static TBind	*fakeOTResolveRet;

static void FakeOT_Notify( EndpointRef ref, OTEventCode code, OTResult result );
static void FakeOT_QueueDatagram( const UInt8 *data, int length, const UInt8 ip[4], const UInt8 port[2] );

void GetCurrentProcess( ProcessSerialNumber *psn ) {
	psn->highLongOfPSN = 0;
	psn->lowLongOfPSN = 5;	/* kCurrentProcess is 2: a real PSN is not */
	fakeOTProcessKnown = 1;
}

void WakeUpProcess( const ProcessSerialNumber *psn ) {
	if ( !fakeOTProcessKnown || psn->lowLongOfPSN != 5 ) {
		fakeOTWakeups = -1000;	/* woke no process this one knows */
		return;
	}
	fakeOTWakeups++;
}

/* Resolve the name OTInitDNSAddress built into retAddr, or fail. */
static OSStatus FakeOT_Resolve( TBind *retAddr ) {
	InetAddress	result;

	if ( fakeOTResolveError ) {
		return fakeOTResolveError;
	}
	memcpy( fakeOTResolvedName, fakeOTDNSAddress->fName, sizeof( fakeOTResolvedName ) );
	memset( &result, 0, sizeof( result ) );
	result.fAddressType = AF_INET;
	result.fHost = fakeOTResolvedHost;
	memcpy( retAddr->addr.buf, &result, sizeof( result ) );
	retAddr->addr.len = sizeof( result );
	return 0;
}

/* Move the clock on to when, delivering what falls due on the way. */
static void FakeOT_Advance( int when ) {
	static const UInt8 ip[4] = { 10, 0, 0, 9 }, port[2] = { 0x6d, 0x40 };

	for ( ;; ) {
		if ( fakeOTArriveAt >= 0 && fakeOTArriveAt <= when ) {
			fakeOTNow = fakeOTArriveAt;
			fakeOTArriveAt = -1;
			FakeOT_QueueDatagram( fakeOTArriveData, fakeOTArriveLength, ip, port );
			FakeOT_Notify( FAKE_OT_ENDPOINT, T_DATA, 0 );
		} else if ( fakeOTResolvePending && fakeOTResolveDue >= 0 && fakeOTResolveDue <= when ) {
			fakeOTNow = fakeOTResolveDue;
			fakeOTResolvePending = 0;
			FakeOT_Notify( FAKE_OT_RESOLVER, T_RESOLVEADDRCOMPLETE, FakeOT_Resolve( fakeOTResolveRet ) );
		} else {
			break;
		}
	}
	if ( fakeOTNow < when ) {
		fakeOTNow = when;
	}
}

/* The next time something falls due, or -1. */
static int FakeOT_NextDue( void ) {
	int due = -1;

	if ( fakeOTArriveAt >= 0 ) {
		due = fakeOTArriveAt;
	}
	if ( fakeOTResolvePending && fakeOTResolveDue >= 0 && ( due < 0 || fakeOTResolveDue < due ) ) {
		due = fakeOTResolveDue;
	}
	return due;
}

/* A datagram arrives on the game endpoint after delay msec. */
static void FakeOT_ArriveAt( int delay, const UInt8 *data, int length ) {
	fakeOTArriveAt = fakeOTNow + delay;
	fakeOTArriveData = data;
	fakeOTArriveLength = length;
}

static int FakeOT_IsAsync( EndpointRef ref );

OSStatus OTResolveAddress( EndpointRef ref, TBind *reqAddr, TBind *retAddr, OTTimeout timeOut ) {
	fakeOTResolveCalls++;
	if ( ref != FAKE_OT_RESOLVER || reqAddr->addr.buf != (UInt8 *)fakeOTDNSAddress
		|| reqAddr->addr.len != fakeOTDNSLength || retAddr->addr.maxlen < sizeof( InetAddress ) ) {
		return kOTNoDataErr;	/* not the request OTInitDNSAddress built */
	}
	if ( !FakeOT_IsAsync( ref ) ) {
		/* synchronous: the whole lookup happens in here, the Mac frozen */
		if ( fakeOTResolveDelay < 0 || fakeOTResolveDelay > (int)timeOut ) {
			FakeOT_Advance( fakeOTNow + timeOut );
			return kETIMEDOUTErr;
		}
		FakeOT_Advance( fakeOTNow + fakeOTResolveDelay );
		return FakeOT_Resolve( retAddr );
	}
	if ( fakeOTResolvePending ) {
		return kOTStateChangeErr;	/* one lookup at a time */
	}
	fakeOTResolvePending = 1;
	fakeOTResolveRet = retAddr;
	fakeOTResolveDue = -1;
	if ( fakeOTResolveDelay >= 0 ) {
		/* OT's own timeout reports a lookup that takes too long */
		fakeOTResolveDue = fakeOTNow + ( fakeOTResolveDelay > (int)timeOut ? (int)timeOut : fakeOTResolveDelay );
		if ( fakeOTResolveDelay > (int)timeOut ) {
			fakeOTResolveError = kETIMEDOUTErr;
		}
	}
	return 0;
}

/* ---------- endpoint lifetime, for Sys_InitNetworking ---------- */

#define FAKE_OT_INJECTED_ERR	kOTOutOfMemoryErr
#define FAKE_OT_TSDU			65507
#define FAKE_OT_MAX_BUSY		16

typedef struct {
	EndpointRef	ref;
	int			opened, closed;		/* times */
	int			nonBlocking;
	int			async;
	OTNotifyUPP	notifier;
	void		*context;
	int			bound;
	InetPort	port;				/* as bound */
	InetHost	host;
} fakeOTEndpoint_t;

static fakeOTEndpoint_t	fakeOTEndpoints[2];	/* FAKE_OT_ENDPOINT, FAKE_OT_RESOLVER */
static struct OTConfiguration { int unused; } fakeOTConfiguration;
static int		fakeOTInited;			/* InitOpenTransport calls not yet closed */
static int		fakeOTInitCalls, fakeOTCloseCalls, fakeOTConfigCalls, fakeOTOpenCalls, fakeOTBindCalls;
static int		fakeOTMisuse;			/* calls on endpoints that are not open, ... */
static InetPort	fakeOTBusyPorts[FAKE_OT_MAX_BUSY];
static int		fakeOTBusyCount;
static int		fakeOTBindReassigns;

static int		fakeOTFailInit;			/* InitOpenTransport fails */
static int		fakeOTFailConfig;		/* this OTCreateConfiguration call (from 1) returns NULL */
static int		fakeOTFailOpen;			/* this OTOpenEndpoint call (from 1) fails */
static int		fakeOTFailNonBlocking;	/* OTSetNonBlocking fails */
static EndpointRef	fakeOTFailBind;		/* OTBind on this endpoint fails */
static EndpointRef	fakeOTFailNotifier;	/* OTInstallNotifier on this endpoint fails */
static int		fakeOTFailAsync;		/* OTSetAsynchronous fails */

static void FakeOT_ResetLifetime( void ) {
	memset( fakeOTEndpoints, 0, sizeof( fakeOTEndpoints ) );
	fakeOTEndpoints[0].ref = FAKE_OT_ENDPOINT;
	fakeOTEndpoints[1].ref = FAKE_OT_RESOLVER;
	fakeOTInited = fakeOTInitCalls = fakeOTCloseCalls = fakeOTConfigCalls = 0;
	fakeOTOpenCalls = fakeOTBindCalls = fakeOTMisuse = 0;
	fakeOTBusyCount = fakeOTBindReassigns = 0;
	fakeOTFailInit = fakeOTFailConfig = fakeOTFailOpen = fakeOTFailNonBlocking = 0;
	fakeOTFailBind = fakeOTFailNotifier = kOTInvalidEndpointRef;
	fakeOTFailAsync = 0;
	fakeOTResolveError = noErr;
	fakeOTNow = 1000;
	fakeOTWakeups = fakeOTProcessKnown = fakeOTCountCalls = 0;
	fakeOTResolveDelay = 30;
	fakeOTArriveAt = -1;
	fakeOTResolvePending = 0;
}

static void FakeOT_BusyPort( int port ) {
	fakeOTBusyPorts[fakeOTBusyCount++] = (InetPort)port;
}

static int FakeOT_PortBusy( InetPort port ) {
	int i;

	for ( i = 0 ; i < fakeOTBusyCount ; i++ ) {
		if ( fakeOTBusyPorts[i] == port ) {
			return 1;
		}
	}
	return 0;
}

/* The open endpoint ref names, or NULL (counted as misuse). */
static fakeOTEndpoint_t *FakeOT_Endpoint( void *ref ) {
	int i;

	for ( i = 0 ; i < 2 ; i++ ) {
		if ( fakeOTEndpoints[i].ref == ref && fakeOTEndpoints[i].opened > fakeOTEndpoints[i].closed ) {
			return &fakeOTEndpoints[i];
		}
	}
	fakeOTMisuse++;
	return NULL;
}

/* How many endpoints are open. */
static int FakeOT_OpenEndpoints( void ) {
	return ( fakeOTEndpoints[0].opened - fakeOTEndpoints[0].closed )
		+ ( fakeOTEndpoints[1].opened - fakeOTEndpoints[1].closed );
}

OSStatus InitOpenTransport( void ) {
	fakeOTInitCalls++;
	if ( fakeOTFailInit ) {
		return FAKE_OT_INJECTED_ERR;
	}
	fakeOTInited++;
	return noErr;
}

void CloseOpenTransport( void ) {
	fakeOTCloseCalls++;
	if ( fakeOTInited <= 0 || FakeOT_OpenEndpoints() ) {
		fakeOTMisuse++;		/* not open, or endpoints left behind */
	}
	fakeOTInited--;
}

OTConfigurationRef OTCreateConfiguration( const char *path ) {
	fakeOTConfigCalls++;
	if ( !fakeOTInited || strcmp( path, "udp" ) ) {
		fakeOTMisuse++;
	}
	if ( fakeOTConfigCalls == fakeOTFailConfig ) {
		return NULL;
	}
	return &fakeOTConfiguration;
}

EndpointRef OTOpenEndpoint( OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info, OSStatus *err ) {
	fakeOTEndpoint_t *ep;

	(void)oflag;
	fakeOTOpenCalls++;
	/* the first is the game endpoint, the rest resolvers, one at a time */
	ep = &fakeOTEndpoints[fakeOTOpenCalls == 1 ? 0 : 1];
	if ( !fakeOTInited || ep->opened > ep->closed ) {
		fakeOTMisuse++;
		*err = kOTOutStateErr;
		return kOTInvalidEndpointRef;
	}
	if ( config == NULL || config == kOTInvalidConfigurationPtr ) {
		*err = kOTBadConfigurationErr;
		return kOTInvalidEndpointRef;
	}
	if ( fakeOTOpenCalls == fakeOTFailOpen ) {
		*err = FAKE_OT_INJECTED_ERR;
		return kOTInvalidEndpointRef;
	}
	ep->opened++;
	ep->nonBlocking = ep->async = ep->bound = 0;
	ep->notifier = NULL;
	if ( info ) {
		memset( info, 0, sizeof( *info ) );
		info->addr = sizeof( InetAddress );
		info->tsdu = FAKE_OT_TSDU;
	}
	*err = noErr;
	return ep->ref;
}

OSStatus OTSetNonBlocking( ProviderRef ref ) {
	fakeOTEndpoint_t *ep = FakeOT_Endpoint( ref );

	if ( !ep ) {
		return kOTOutStateErr;
	}
	if ( fakeOTFailNonBlocking ) {
		return FAKE_OT_INJECTED_ERR;
	}
	ep->nonBlocking = 1;
	return noErr;
}

OSStatus OTSetAsynchronous( ProviderRef ref ) {
	fakeOTEndpoint_t *ep = FakeOT_Endpoint( ref );

	if ( !ep ) {
		return kOTOutStateErr;
	}
	if ( !ep->bound || !ep->notifier ) {
		fakeOTMisuse++;		/* bound synchronously first, completions need a notifier */
	}
	if ( fakeOTFailAsync ) {
		return FAKE_OT_INJECTED_ERR;
	}
	ep->async = 1;
	return noErr;
}

OSStatus OTInstallNotifier( ProviderRef ref, OTNotifyUPP proc, void *contextPtr ) {
	fakeOTEndpoint_t *ep = FakeOT_Endpoint( ref );

	if ( !ep || !proc || ep->notifier ) {
		fakeOTMisuse++;
		return kOTOutStateErr;
	}
	if ( !fakeOTProcessKnown ) {
		fakeOTMisuse++;		/* a notifier that cannot wake the process yet */
	}
	if ( ref == fakeOTFailNotifier ) {
		return FAKE_OT_INJECTED_ERR;
	}
	ep->notifier = proc;
	ep->context = contextPtr;
	return noErr;
}

static int FakeOT_IsAsync( EndpointRef ref ) {
	return ref == FAKE_OT_RESOLVER && fakeOTEndpoints[1].opened > fakeOTEndpoints[1].closed
		&& fakeOTEndpoints[1].async;
}

/* Call ref's notifier, as OT does at deferred task time, if it has one. */
static void FakeOT_Notify( EndpointRef ref, OTEventCode code, OTResult result ) {
	fakeOTEndpoint_t *ep = &fakeOTEndpoints[ref == FAKE_OT_RESOLVER];

	if ( ep->opened > ep->closed && ep->notifier ) {
		ep->notifier( ep->context, code, result, NULL );
	}
}

OTResult OTCountDataBytes( EndpointRef ref, OTByteCount *countPtr ) {
	fakeOTCountCalls++;
	if ( !FakeOT_Endpoint( ref ) ) {
		return kOTOutStateErr;
	}
	if ( ref != FAKE_OT_ENDPOINT || fakeOTNext == fakeOTQueued ) {
		return kOTNoDataErr;
	}
	*countPtr = fakeOTDatagrams[fakeOTNext].length - fakeOTOffset;
	return noErr;
}

OSStatus OTBind( EndpointRef ref, TBind *reqAddr, TBind *retAddr ) {
	fakeOTEndpoint_t	*ep = FakeOT_Endpoint( ref );
	InetAddress			in, out;

	fakeOTBindCalls++;
	if ( !ep || ep->bound || !reqAddr || reqAddr->addr.len != sizeof( in ) ) {
		fakeOTMisuse++;
		return kOTOutStateErr;
	}
	if ( ref == fakeOTFailBind ) {
		return FAKE_OT_INJECTED_ERR;
	}
	memcpy( &in, reqAddr->addr.buf, sizeof( in ) );
	out = in;
	if ( in.fAddressType != AF_INET ) {
		return kOTBadAddressErr;
	}
	if ( in.fPort == 0 ) {
		out.fPort = 49152 + ( ep - fakeOTEndpoints );	/* any free port */
	} else if ( FakeOT_PortBusy( in.fPort ) ) {
		if ( !fakeOTBindReassigns ) {
			return kOTAddressBusyErr;
		}
		out.fPort = 50000;
	}
	ep->bound = 1;
	ep->port = out.fPort;
	ep->host = out.fHost;
	if ( retAddr && retAddr->addr.maxlen >= sizeof( out ) ) {
		memcpy( retAddr->addr.buf, &out, sizeof( out ) );
		retAddr->addr.len = sizeof( out );
	}
	return noErr;
}

OSStatus OTUnbind( EndpointRef ref ) {
	fakeOTEndpoint_t *ep = FakeOT_Endpoint( ref );

	if ( !ep || !ep->bound ) {
		return kOTOutStateErr;	/* unbinding an unbound endpoint just fails */
	}
	ep->bound = 0;
	return noErr;
}

OSStatus OTCloseProvider( ProviderRef ref ) {
	fakeOTEndpoint_t *ep = FakeOT_Endpoint( ref );

	if ( !ep ) {
		return kOTOutStateErr;
	}
	ep->closed++;
	ep->bound = 0;
	if ( ref == FAKE_OT_RESOLVER ) {
		fakeOTResolvePending = 0;	/* closing abandons the lookup: no notification */
	}
	return noErr;
}

#endif

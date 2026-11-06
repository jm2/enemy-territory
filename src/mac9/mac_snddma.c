
// mac_snddma.c
// all other sound mixing is portable

#include "../client/snd_local.h"
#include <Sound.h>

#define	MAX_MIXED_SAMPLES	0x8000
#define	SUBMISSION_CHUNK	0x100

static	short			s_mixedSamples[MAX_MIXED_SAMPLES];
static	volatile int	s_chunkCount;		// number of chunks submitted
static	SndChannel		*s_sndChan;
static	ExtSoundHeader	s_sndHeader;
static	SndCallBackUPP	s_callbackUPP;
// S_Callback runs at interrupt time, where it cannot print: a failed
// SndDoCommand there stops the chain of chunks and leaves its error here
// for SNDDMA_GetDMAPos to report
static	volatile OSErr	s_callbackErr;
static	qboolean		s_callbackErrReported;

/*
===============
S_QueueChunk

Queues the next submission chunk and a callBackCmd behind it.  S_Callback
runs this at interrupt time, so it may only touch static data and make
Sound Manager calls with noWait set.
===============
*/
static OSErr S_QueueChunk( SndChannel *sc ) {
	SndCommand		mySndCmd;
	SndCommand		mySndCmd2;
	int				offset;
	OSErr			err;
	
	offset = ( s_chunkCount * SUBMISSION_CHUNK ) & (MAX_MIXED_SAMPLES-1);
	
	// queue up another sound buffer
	memset( &s_sndHeader, 0, sizeof( s_sndHeader ) );
	s_sndHeader.samplePtr = (void *)(s_mixedSamples + offset);
	s_sndHeader.numChannels = 2;
	// exactly dma.speed: rate22khz is the 22254.54 Hz Mac hardware rate,
	// which played everything 0.9% sharp and ran the DMA position ahead of
	// the mixer; the Sound Manager converts 22050 Hz to the output rate
	s_sndHeader.sampleRate = rate22050hz;
	s_sndHeader.loopStart = 0;
	s_sndHeader.loopEnd = 0;
	s_sndHeader.encode = extSH;
	s_sndHeader.baseFrequency = 1;
	s_sndHeader.numFrames = SUBMISSION_CHUNK / 2;
	s_sndHeader.markerChunk = NULL;
	s_sndHeader.instrumentChunks = NULL;
	s_sndHeader.AESRecording = NULL;
	s_sndHeader.sampleSize = 16;
	
	mySndCmd.cmd = bufferCmd;
	mySndCmd.param1 = 0;
	mySndCmd.param2 = (int)&s_sndHeader;
	err = SndDoCommand( sc, &mySndCmd, true );
	if ( err ) {
		return err;
	}
	
	// and another callback
	mySndCmd2.cmd = callBackCmd;
	mySndCmd2.param1 = 0;
	mySndCmd2.param2 = 0;
	err = SndDoCommand( sc, &mySndCmd2, true );
	if ( err ) {
		return err;
	}

	s_chunkCount++;		// this is the next buffer we will submit
	return 0;
}

/*
===============
S_Callback
===============
*/
void S_Callback( SndChannel *sc, SndCommand *cmd ) {
	OSErr	err;

	// a channel SNDDMA_Shutdown is disposing of must not be fed again
	if ( sc != s_sndChan ) {
		return;
	}
	err = S_QueueChunk( sc );
	if ( err ) {
		s_callbackErr = err;
	}
}

/*
===============
S_MakeTestPattern
===============
*/
void S_MakeTestPattern( void ) {
	int		i;
	float	v;
	int		sample;
	
	for ( i = 0 ; i < dma.samples / 2 ; i ++ ) {
		v = sin( M_PI * 2 * i / 64 );
		sample = v * 0x4000;
		((short *)dma.buffer)[i*2] = sample;	
		((short *)dma.buffer)[i*2+1] = sample;	
	}
}

/*
===============
SNDDMA_Init
===============
*/
qboolean SNDDMA_Init(void) {
	OSErr	err;
	
	// never leak a channel that is still open
	SNDDMA_Shutdown();

	s_chunkCount = 0;
	s_callbackErr = 0;
	s_callbackErrReported = qfalse;

	// create a sound channel
	s_sndChan = NULL;
	s_callbackUPP = NewSndCallBackUPP( S_Callback );
	if ( !s_callbackUPP ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: NewSndCallBackUPP failed, sound disabled\n" );
		return qfalse;
	}
	err = SndNewChannel( &s_sndChan, sampledSynth, initStereo, s_callbackUPP );
	if ( err ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: SndNewChannel failed (%d), sound disabled\n", err );
		s_sndChan = NULL;
		SNDDMA_Shutdown();
		return qfalse;
	}
	
	dma.channels = 2;
	dma.samples = MAX_MIXED_SAMPLES;
	dma.submission_chunk = SUBMISSION_CHUNK;
	dma.samplebits = 16;
	dma.speed = 22050;		// must match the buffer headers' rate22050hz
	dma.buffer = (byte *)s_mixedSamples;
	
	// que up the first submission-chunk sized buffer
	err = S_QueueChunk( s_sndChan );
	if ( err ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: SndDoCommand failed (%d), sound disabled\n", err );
		SNDDMA_Shutdown();
		return qfalse;
	}
	
	return qtrue;
}

/*
===============
SNDDMA_GetDMAPos
===============
*/
int	SNDDMA_GetDMAPos(void) {
	if ( s_callbackErr && !s_callbackErrReported ) {
		s_callbackErrReported = qtrue;
		Com_Printf( S_COLOR_YELLOW "WARNING: SndDoCommand failed (%d), sound stopped; snd_restart to retry\n",
			s_callbackErr );
	}
	return (s_chunkCount * SUBMISSION_CHUNK) & (dma.samples - 1);
}

/*
===============
SNDDMA_Shutdown

Safe to call any number of times, and from Sys_Quit and Sys_Error.
===============
*/
void SNDDMA_Shutdown(void) {
	SndChannel	*chan;
	OSErr		err;

	chan = s_sndChan;
	s_sndChan = NULL;		// S_Callback stops requeuing
	if ( chan ) {
		// quietNow flushes the queue, so no callBackCmd is left to run
		// S_Callback once the channel is gone; without it the dispose
		// would wait for a queue that S_Callback keeps refilling
		err = SndDisposeChannel( chan, true );
		if ( err ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: SndDisposeChannel failed (%d)\n", err );
			// the channel may still call through the UPP: leak it
			s_callbackUPP = NULL;
		}
	}
	if ( s_callbackUPP ) {
		DisposeSndCallBackUPP( s_callbackUPP );
		s_callbackUPP = NULL;
	}
}

/*
===============
SNDDMA_BeginPainting
===============
*/
void SNDDMA_BeginPainting(void) {
}

/*
===============
SNDDMA_Submit
===============
*/
void SNDDMA_Submit(void) {
}

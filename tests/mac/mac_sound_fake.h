/* Issue #264: just enough of Sound Manager 3.x's Sound.h for
 * tests/mac_snddma_regression.c to compile the real code/mac/mac_snddma.c on
 * the host.  run_mac_snddma_tests.sh installs this file as Sound.h.  The
 * constants and field order are copied from Universal Interfaces 3.4
 * (Sound.h, "Technology: Sound Manager 3.6"); only field order and types are
 * mirrored, not the mac68k packing or SndChannel's private fields.  The Sound
 * Manager itself is the test's. */
#ifndef MAC_SOUND_FAKE_H
#define MAC_SOUND_FAKE_H

#ifndef true
#define true	1
#define false	0
#endif

typedef char				*Ptr;
typedef unsigned char		UInt8;
typedef unsigned char		Boolean;
typedef short				OSErr;
typedef unsigned long		UnsignedFixed;
typedef struct { short exp; short man[4]; } extended80;

enum {
	rate48khz		= (long)0xBB800000,	/* 48000.00000 in fixed-point */
	rate44khz		= (long)0xAC440000,	/* 44100.00000 in fixed-point */
	rate32khz		= 0x7D000000,		/* 32000.00000 in fixed-point */
	rate22050hz		= 0x56220000,		/* 22050.00000 in fixed-point */
	rate22khz		= 0x56EE8BA3,		/* 22254.54545 in fixed-point */
	rate16khz		= 0x3E800000,		/* 16000.00000 in fixed-point */
	rate11khz		= 0x2B7745D1,		/* 11127.27273 in fixed-point */
	rate11025hz		= 0x2B110000,		/* 11025.00000 in fixed-point */
	rate8khz		= 0x1F400000		/*  8000.00000 in fixed-point */
};

enum {
	sampledSynth	= 5,
	extSH			= 0xFF,
	callBackCmd		= 13,
	bufferCmd		= 81,
	initStereo		= 0x00C0
};

typedef struct SndCommand {
	unsigned short	cmd;
	short			param1;
	long			param2;
} SndCommand;

typedef struct SndChannel	SndChannel;
typedef SndChannel			*SndChannelPtr;
typedef void (*SndCallBackProcPtr)( SndChannelPtr chan, SndCommand *cmd );
/* On PowerPC a UPP is a routine descriptor that NewRoutineDescriptor
 * allocates and can fail to; the test defines the struct and allocates one
 * per NewSndCallBackUPP, so a call through a disposed UPP is a
 * heap-use-after-free under ASan. */
typedef struct FakeSndCallBackUPP	*SndCallBackUPP;

struct SndChannel {
	SndChannelPtr	nextChan;
	SndCallBackUPP	callBack;
};

typedef struct ExtSoundHeader {
	Ptr				samplePtr;
	unsigned long	numChannels;
	UnsignedFixed	sampleRate;
	unsigned long	loopStart;
	unsigned long	loopEnd;
	UInt8			encode;
	UInt8			baseFrequency;
	unsigned long	numFrames;
	extended80		AIFFSampleRate;
	Ptr				markerChunk;
	Ptr				instrumentChunks;
	Ptr				AESRecording;
	unsigned short	sampleSize;
	unsigned short	futureUse1;
	unsigned long	futureUse2;
	unsigned long	futureUse3;
	unsigned long	futureUse4;
	UInt8			sampleArea[1];
} ExtSoundHeader;

SndCallBackUPP NewSndCallBackUPP( SndCallBackProcPtr userRoutine );
void DisposeSndCallBackUPP( SndCallBackUPP userUPP );
OSErr SndNewChannel( SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine );
OSErr SndDoCommand( SndChannelPtr chan, const SndCommand *cmd, Boolean noWait );
OSErr SndDisposeChannel( SndChannelPtr chan, Boolean quietNow );

#endif

#include "Types.r"
#include "CodeFragments.r"
#include "et_icons.r"

resource 'cfrg' (0) {
	{	/* array memberArray: 1 elements */
		/* [1] */
		kPowerPC,
		kFullLib,
		kNoVersionNum, kNoVersionNum,
		/* appStackSize: 2 MB. 0 means the system default (~64 KB), which the
		   botlib's recursion and legacy's large stack frames overflow. */
		2 * 1024 * 1024, 0,
		kIsApp, kOnDiskFlat, kZeroOffset, kWholeFork,
		"ET"
	}
};

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreAppDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	localAndRemoteHLEvents,
	isStationeryAware,
	useTextEditServices,
	reserved,
	reserved,
	reserved,
	
	/* Preferred and minimum partition. At the minimum the fixed demand is about
	   291 MB: the image (5.8 MB of text and data, 146 MB of module bss, which
	   CFM loads into the partition when virtual memory is off), the 80 MB
	   hunk, the 16 MB zone, 0.5 MB of small zone, the 2 MB stack, the sound
	   pool (24.7 MB at com_soundMegs 8) and 16 MB for AGL/OpenGL, the Sound
	   Manager and fragmentation. */
	340000 * 1024,
	300000 * 1024
};

/* Sys_Error's Stop alert (mac_main.c), as retail's ALRT 128: ParamText
   puts "Quake 3 Error:" in ^0 and the message in ^1. */
resource 'ALRT' (128, purgeable) {
	{0, 0, 180, 420},
	128,
	beepStages,
	alertPositionMainScreen
};

resource 'DITL' (128, purgeable) {
	{
		{145, 330, 165, 400},
		Button { enabled, "Quit" };
		{13, 78, 31, 400},
		StaticText { disabled, "^0" };
		{35, 78, 135, 400},
		StaticText { disabled, "^1" }
	}
};

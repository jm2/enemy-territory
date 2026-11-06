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
	   263 MiB: the image (6.4 MiB of text, 0.6 of data and 112 of bss, of which
	   92 is the modules'; CFM loads it into the partition when virtual memory is
	   off), the 72 MiB hunk, the 10.5 MiB of zones, the 2 MiB stack, and the
	   60 MiB MAC_HUNK_RESERVE_KB holds back for the sound pool, AGL/OpenGL and
	   the image buffers.

	   Bigger is not better. Apple's OpenGL keeps its copy of every texture in
	   temporary memory, outside this partition, and fails without a report when
	   that runs out. At a 417 MiB partition on a 512 MB Mac, temporary memory fell
	   to under 200 KB during a map load and the machine froze with 78 MB free in
	   the application heap. So this should stay as small as the game can use. */
	300000 * 1024,
	270000 * 1024
};

/* Sys_Error's Stop alert (mac_main.c), as retail's ALRT 128: ParamText
   puts "Enemy Territory Error:" in ^0 and the message in ^1. */
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

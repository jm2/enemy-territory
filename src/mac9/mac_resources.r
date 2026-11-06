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
	   333 MiB: the image (6.4 MiB of text, 0.6 of data and 166 of bss, of which
	   146 is the modules'; CFM loads it into the partition when virtual memory is
	   off), the 80 MiB hunk, the 16.5 MiB of zones, the 2 MiB stack, and the
	   60 MiB MAC_HUNK_RESERVE_KB holds back for the sound pool, AGL/OpenGL and
	   the image buffers. A 2048x2048 texture in the mod's pk3 needs 17 MiB in one
	   block, and was the first thing to run out at 355 MiB with 16 MiB of zones
	   and no such reserve. */
	410000 * 1024,
	342000 * 1024
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

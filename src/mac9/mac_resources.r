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
	   258 MiB: the image (113.5 MiB, of which module bss is 88; CFM loads it into
	   the partition when virtual memory is off), the 72 MiB hunk, the 10.5 MiB
	   of zones, the 2 MiB stack, and the 60 MiB MAC_HUNK_RESERVE_KB holds back for
	   the sound pool, the renderer's 24 MiB block, a listen server's clients,
	   AGL/OpenGL and the image buffers. Above the minimum is what r_picmip 0
	   needs: the 2048x2048 texture in the mod's pk3 takes 32 MiB there, and
	   the preferred size leaves room for it.

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

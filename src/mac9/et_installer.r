#include "Types.r"
#include "CodeFragments.r"
#include "et_installer_pef.r"

/* The length, not kWholeFork: the data fork holds the whole game after the PEF. */
resource 'cfrg' (0) {
	{
		kPowerPC,
		kFullLib,
		kNoVersionNum, kNoVersionNum,
		0, 0,
		kIsApp, kOnDiskFlat, kZeroOffset, ET_INSTALLER_PEF_BYTES,
		"ET Installer"
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
	notHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	4 * 1024 * 1024,
	3 * 1024 * 1024
};

resource 'ALRT' (128, purgeable) {
	{0, 0, 180, 420},
	128,
	beepStages,
	alertPositionMainScreen
};

resource 'DITL' (128, purgeable) {
	{
		{145, 330, 165, 400},
		Button { enabled, "OK" };
		{13, 20, 31, 400},
		StaticText { disabled, "^0" };
		{35, 20, 135, 400},
		StaticText { disabled, "^1" }
	}
};

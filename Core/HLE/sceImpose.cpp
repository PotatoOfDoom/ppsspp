// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceImpose.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MIPS/MIPS.h"
#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/System.h"

const int PSP_UMD_POPUP_DISABLE = 0;
const int PSP_UMD_POPUP_ENABLE = 1;

#define	PSP_IMPOSE_BATTICON_NONE    0x80000000
#define	PSP_IMPOSE_BATTICON_VISIBLE 0x00000000
#define	PSP_IMPOSE_BATTICON_BLINK   0x00000001

static u32 language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
static u32 buttonValue = PSP_SYSTEMPARAM_BUTTON_CIRCLE;
static u32 umdPopup = PSP_UMD_POPUP_DISABLE;
static u32 backlightOffTime;

// The impose params, as used by sceImposeGetParam/SetParam. Names and IDs from PSPSDK's
// pspimpose_driver.h. These are the system settings the XMB's own settings screens read and write;
// PPSSPP has no separate state for most of them, so they simply round-trip. LANGUAGE and
// BACKLIGHT_OFF_INTERVAL are aliases for state we already keep, and are handled separately.
static const struct ImposeParam {
	int id;
	const char *name;
	int defaultValue;
} g_imposeParams[] = {
	{0x001, "MAIN_VOLUME",           30},
	{0x002, "BACKLIGHT_BRIGHTNESS",   2},
	{0x004, "EQUALIZER_MODE",         0},
	{0x008, "MUTE",                   0},
	{0x010, "AVLS",                   0},
	{0x020, "TIME_FORMAT",            0},
	{0x040, "DATE_FORMAT",            0},
	{0x400, "SOUND_REDUCTION",        0},
};
static const int PSP_IMPOSE_PARAM_LANGUAGE = 0x080;
static const int PSP_IMPOSE_PARAM_BACKLIGHT_OFF_INTERVAL = 0x200;

static int imposeParamValues[ARRAY_SIZE(g_imposeParams)];

static int ImposeParamIndex(int param) {
	for (size_t i = 0; i < ARRAY_SIZE(g_imposeParams); i++) {
		if (g_imposeParams[i].id == param) {
			return (int)i;
		}
	}
	return -1;
}

const char *ImposeParamName(int param) {
	switch (param) {
	case PSP_IMPOSE_PARAM_LANGUAGE: return "LANGUAGE";
	case PSP_IMPOSE_PARAM_BACKLIGHT_OFF_INTERVAL: return "BACKLIGHT_OFF_INTERVAL";
	default:
	{
		const int index = ImposeParamIndex(param);
		return index >= 0 ? g_imposeParams[index].name : "(unknown)";
	}
	}
}

bool ImposeGetParam(int param, int *value) {
	switch (param) {
	case PSP_IMPOSE_PARAM_LANGUAGE:
		*value = language;
		return true;
	case PSP_IMPOSE_PARAM_BACKLIGHT_OFF_INTERVAL:
		*value = backlightOffTime;
		return true;
	default:
	{
		const int index = ImposeParamIndex(param);
		if (index < 0) {
			return false;
		}
		*value = imposeParamValues[index];
		return true;
	}
	}
}

bool ImposeSetParam(int param, int value) {
	switch (param) {
	case PSP_IMPOSE_PARAM_LANGUAGE:
		// Same as sceImposeSetLanguageMode: we don't let the guest change our language setting.
		return language == (u32)value;
	case PSP_IMPOSE_PARAM_BACKLIGHT_OFF_INTERVAL:
		backlightOffTime = value;
		return true;
	default:
	{
		const int index = ImposeParamIndex(param);
		if (index < 0) {
			return false;
		}
		imposeParamValues[index] = value;
		return true;
	}
	}
}

void __ImposeInit() {
	for (size_t i = 0; i < ARRAY_SIZE(g_imposeParams); i++) {
		imposeParamValues[i] = g_imposeParams[i].defaultValue;
	}
	language = GetPSPLanguage();
	if (PSP_CoreParameter().compat.flags().EnglishOrJapaneseOnly) {
		if (language != PSP_SYSTEMPARAM_LANGUAGE_ENGLISH && language != PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
			language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
		}
	}
	buttonValue = PSP_CoreParameter().compat.flags().ForceCircleButtonConfirm ? PSP_SYSTEMPARAM_BUTTON_CIRCLE : g_Config.iButtonPreference;
	umdPopup = PSP_UMD_POPUP_DISABLE;
	backlightOffTime = 0;
}

void __ImposeDoState(PointerWrap &p) {
	auto s = p.Section("sceImpose", 1, 2);
	if (!s)
		return;

	Do(p, language);
	Do(p, buttonValue);
	Do(p, umdPopup);
	Do(p, backlightOffTime);
	if (s >= 2) {
		DoArray(p, imposeParamValues, (int)ARRAY_SIZE(imposeParamValues));
	}
	// Older states simply keep the defaults __ImposeInit set, which is fine - these are settings,
	// not state a game depends on.
}

static u32 sceImposeGetBatteryIconStatus(u32 chargingPtr, u32 iconStatusPtr)
{
	if (Memory::IsValidAddress(chargingPtr))
		Memory::WriteUnchecked_U32(PSP_IMPOSE_BATTICON_NONE, chargingPtr);
	if (Memory::IsValidAddress(iconStatusPtr))
		Memory::WriteUnchecked_U32(3, iconStatusPtr);
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceImposeSetLanguageMode(u32 languageVal, u32 buttonVal) {
	language = languageVal;
	buttonValue = buttonVal;
	if (language != GetPSPLanguage()) {
		return hleLogWarning(Log::sceUtility, 0, "ignoring requested language");
	}
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceImposeGetLanguageMode(u32 languagePtr, u32 btnPtr) {
	if (Memory::IsValidAddress(languagePtr))
		Memory::WriteUnchecked_U32(language, languagePtr);
	if (Memory::IsValidAddress(btnPtr))
		Memory::WriteUnchecked_U32(buttonValue, btnPtr);
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceImposeSetUMDPopup(int mode) {
	umdPopup = mode;
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceImposeGetUMDPopup() {
	return hleLogDebug(Log::sceUtility, umdPopup);
}

static u32 sceImposeSetBacklightOffTime(int time) {
	backlightOffTime = time;
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceImposeGetBacklightOffTime() {
	return hleLogDebug(Log::sceUtility, backlightOffTime);
}

//OSD stuff? home button?
const HLEFunction sceImpose[] = {
	{0X36AA6E91, &WrapU_UU<sceImposeSetLanguageMode>,      "sceImposeSetLanguageMode",      'i', "ii"},
	{0X381BD9E7, nullptr,                                  "sceImposeHomeButton",           '?', ""  },
	{0X0F341BE4, nullptr,                                  "sceImposeGetHomePopup",         '?', ""  },
	{0X5595A71A, nullptr,                                  "sceImposeSetHomePopup",         '?', ""  },
	{0X24FD7BCF, &WrapU_UU<sceImposeGetLanguageMode>,      "sceImposeGetLanguageMode",      'x', "xx"},
	{0X8C943191, &WrapU_UU<sceImposeGetBatteryIconStatus>, "sceImposeGetBatteryIconStatus", 'x', "xx"},
	{0X72189C48, &WrapU_I<sceImposeSetUMDPopup>,           "sceImposeSetUMDPopup",          'x', "i" },
	{0XE0887BC8, &WrapU_V<sceImposeGetUMDPopup>,           "sceImposeGetUMDPopup",          'x', ""  },
	{0X8F6E3518, &WrapU_V<sceImposeGetBacklightOffTime>,   "sceImposeGetBacklightOffTime",  'x', ""  },
	{0X967F6D4A, &WrapU_I<sceImposeSetBacklightOffTime>,   "sceImposeSetBacklightOffTime",  'x', "i" },
	{0XFCD44963, nullptr,                                  "sceImpose_FCD44963",            '?', ""  },
	{0XA9884B00, nullptr,                                  "sceImpose_A9884B00",            '?', ""  },
	{0XBB3F5DEC, nullptr,                                  "sceImpose_BB3F5DEC",            '?', ""  },
	{0X9BA61B49, nullptr,                                  "sceImpose_9BA61B49",            '?', ""  },
	{0XFF1A2F07, nullptr,                                  "sceImpose_FF1A2F07",            '?', ""  },
};

void Register_sceImpose() {
	RegisterHLEModule("sceImpose", ARRAY_SIZE(sceImpose), sceImpose);
}

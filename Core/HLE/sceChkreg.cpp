// Copyright (c) 2026- PPSSPP Project.

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

// chkreg.prx - the region check and console-identity service. The XMB reads this early on, and
// reaches it through sceVshBridge (see vshChkregGetPsCode / vshChkregCheckRegion there).
//
// Function names and NIDs are from PSPLibDoc (https://github.com/pspdev/psplibdoc), for firmware
// 6.61; all four hash to their NID, so they're solid. The library is called sceChkreg_driver -
// there is no plain sceChkreg library. Semantics and the constants below follow uofw's
// include/chkreg.h (https://github.com/uofw/uofw, MIT).
//
// PPSSPP has no per-console identity to report, so the PS code is derived from the emulated model
// and the system language. That's a made-up console, but a self-consistent one.

#include "Common/Serialize/Serializer.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceChkreg.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MemMap.h"
#include "Core/ConfigValues.h"

// A subset of SceConsoleId, see uofw's include/openpsid_kernel.h.
struct ScePsCode {
	u16_le companyCode;
	u16_le productCode;
	u16_le productSubCode;
	u16_le factoryCode;
};

// SCE_PSP_PRODUCT_CODE_* - the retail region codes.
enum {
	PSP_PRODUCT_CODE_CEX_JAPAN = 0x03,
	PSP_PRODUCT_CODE_CEX_NORTH_AMERICA = 0x04,
	PSP_PRODUCT_CODE_CEX_EUROPE = 0x05,
	PSP_PRODUCT_CODE_CEX_KOREA = 0x06,
	PSP_PRODUCT_CODE_CEX_RUSSIA = 0x0C,
	PSP_PRODUCT_CODE_CEX_CHINA = 0x0D,
	PSP_PRODUCT_CODE_CEX_TAIWAN = 0x0B,
};

// SCE_PSP_PRODUCT_SUB_CODE_* - the motherboard revision, which is what identifies the model.
enum {
	PSP_PRODUCT_SUB_CODE_TA_082_TA_086 = 0x02,  // PSP-100x, "01g"
	PSP_PRODUCT_SUB_CODE_TA_085_TA_088 = 0x03,  // PSP-200x, "02g"
};

// SCE_CHKREG_PSP_MODEL_*_SERIES.
enum {
	PSP_MODEL_1000_SERIES = 1,
	PSP_MODEL_2000_SERIES = 2,
};

// Only the media types the XMB can actually start.
enum {
	PSP_CHKREG_UMD_MEDIA_TYPE_GAME = 0x00,
	PSP_CHKREG_UMD_MEDIA_TYPE_VIDEO = 0x20,
	PSP_CHKREG_UMD_MEDIA_TYPE_AUDIO = 0x40,
};

// Which region a PSP would have been sold in, guessed from the system language. PPSSPP doesn't
// model a region of its own, and this is the only user-visible setting that correlates with one.
static u16 ProductCodeFromLanguage() {
	switch (GetPSPLanguage()) {
	case PSP_SYSTEMPARAM_LANGUAGE_JAPANESE: return PSP_PRODUCT_CODE_CEX_JAPAN;
	case PSP_SYSTEMPARAM_LANGUAGE_ENGLISH: return PSP_PRODUCT_CODE_CEX_NORTH_AMERICA;
	case PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN: return PSP_PRODUCT_CODE_CEX_RUSSIA;
	case PSP_SYSTEMPARAM_LANGUAGE_KOREAN: return PSP_PRODUCT_CODE_CEX_KOREA;
	case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL: return PSP_PRODUCT_CODE_CEX_TAIWAN;
	case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED: return PSP_PRODUCT_CODE_CEX_CHINA;
	default:
		// French, Spanish, German, Italian, Dutch, Portuguese.
		return PSP_PRODUCT_CODE_CEX_EUROPE;
	}
}

int sceChkregGetPsCode(u32 psCodePtr) {
	auto psCode = PSPPointer<ScePsCode>::Create(psCodePtr);
	if (!psCode.IsValid()) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_POINTER, "bad ScePsCode pointer");
	}

	psCode->companyCode = 1;  // Always 1, this is Sony.
	psCode->productCode = ProductCodeFromLanguage();
	psCode->productSubCode = Memory::g_PSPModel == PSP_MODEL_FAT
		? PSP_PRODUCT_SUB_CODE_TA_082_TA_086 : PSP_PRODUCT_SUB_CODE_TA_085_TA_088;
	psCode->factoryCode = 0;
	psCode.NotifyWrite("ChkregGetPsCode");

	return hleLogInfo(Log::HLE, 0);
}

int sceChkregCheckRegion(u32 umdMediaType, u32 regionId) {
	// PPSSPP doesn't region-lock anything, so everything is playable. Returns SCE_TRUE/SCE_FALSE
	// rather than an error code.
	switch (umdMediaType) {
	case PSP_CHKREG_UMD_MEDIA_TYPE_GAME:
	case PSP_CHKREG_UMD_MEDIA_TYPE_VIDEO:
	case PSP_CHKREG_UMD_MEDIA_TYPE_AUDIO:
		return hleLogInfo(Log::HLE, 1);
	default:
		return hleLogWarning(Log::HLE, 0, "unknown UMD media type");
	}
}

static int sceChkregGetPsFlags(u32 psFlagsPtr, int index) {
	// The PS flags carry things like the QA flag, which unlocks "Debug settings" in the XMB.
	// Retail hardware refuses to hand them out, and that's what we emulate - claiming to be a
	// dev unit would put menus in the XMB that don't belong there.
	return hleLogDebug(Log::HLE, SCE_KERNEL_ERROR_INVALID_VALUE, "not a dev unit");
}

static int sceChkregGetPspModel() {
	return hleLogInfo(Log::HLE, Memory::g_PSPModel == PSP_MODEL_FAT
		? PSP_MODEL_1000_SERIES : PSP_MODEL_2000_SERIES);
}

const HLEFunction sceChkreg_driver[] = {
	{0X59F8491D, &WrapI_U<sceChkregGetPsCode>,    "sceChkregGetPsCode",   'i', "x",  HLE_KERNEL_SYSCALL},
	{0X54495B19, &WrapI_UU<sceChkregCheckRegion>, "sceChkregCheckRegion", 'i', "xx", HLE_KERNEL_SYSCALL},
	{0X6894A027, &WrapI_UI<sceChkregGetPsFlags>,  "sceChkregGetPsFlags",  'i', "xi", HLE_KERNEL_SYSCALL},
	{0X7939C851, &WrapI_V<sceChkregGetPspModel>,  "sceChkregGetPspModel", 'i', "",   HLE_KERNEL_SYSCALL},
};

void Register_sceChkreg() {
	RegisterHLEModule("sceChkreg_driver", ARRAY_SIZE(sceChkreg_driver), sceChkreg_driver);
}

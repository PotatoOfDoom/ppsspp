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

// vshbridge.prx, the kernel bridge the PSP's system software (the VSH, better known as the XMB)
// and the modules it loads call into. Most of its exports are thin wrappers letting VSH-mode
// user code reach a kernel driver.
//
// Names and NIDs come from PSPLibDoc (https://github.com/pspdev/psplibdoc, GPL-2.0), for
// firmware 6.61.
//
// Two things to know before adding to this table:
//
//  * SCE obfuscated the kernel NIDs from firmware 3.70 on, so unlike user-mode libraries these are
//    NOT SHA-1(name) and they differ between firmware versions. (Measured over PSPLibDoc's named
//    *_driver exports: 98% hash to their NID on 3.60, 37% on 3.70, 47% on 6.61.) Only 21 of the
//    names below hash to their NID; the rest were recovered by comparing modules across firmwares.
//    A dump of a firmware older than 6.60 will not resolve against this table.
//  * Where one name appears under several NIDs, those are the per-hardware-model builds of
//    vshbridge.prx (01g, 02g, ...), which we give the same implementation.
//
// The library exports 189 functions on 6.61; the 87 whose names are known are listed here, plus the
// 43 name-less ones a real vshmain.prx/paf.prx were seen importing, as sceVshBridge_<NID>.
//
// Calls that map onto something PPSSPP implements are wired through to it with hleCall, so there is
// exactly one implementation of each: ctrl reads and sampling mode, sceIoDevctl/sceIoIoctl,
// chkreg (sceChkreg.cpp), ID storage lookups (sceIdStorage.cpp) and the impose params
// (sceImpose.cpp). The rest report UNIMPL, so the log tells you what the VSH actually asked for
// instead of trapping anonymously. See docs/XMB.md.

#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceChkreg.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceIdStorage.h"
#include "Core/HLE/sceImpose.h"
#include "Core/HLE/sceIo.h"
#include "Core/HLE/sceVshBridge.h"

static int VshBridgeUnimpl() {
	return hleLogError(Log::HLE, 0, "UNIMPL");
}

// Same thing for the exports we only know a NID for. Templated on the NID so each one gets its own
// "reported" flag: without a table entry these trap as unresolved imports and return
// SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED, which firmware code happily uses as a pointer, but some
// of them (0x21c243fe) are called every frame and would drown the log if they shouted every time.
template <u32 nid>
static int VshBridgeUnknown() {
	static bool reported = false;
	if (!reported) {
		reported = true;
		return hleLogError(Log::HLE, 0, "UNIMPL - export known only by NID");
	}
	return hleLogDebug(Log::HLE, 0, "UNIMPL");
}

#define VSHBRIDGE_UNKNOWN(nid) {0X##nid, &WrapI_V<VshBridgeUnknown<0X##nid>>, "sceVshBridge_" #nid, 'i', "", HLE_KERNEL_SYSCALL}

static int vshCtrlReadBufferPositive(u32 ctrlDataPtr, u32 nBufs) {
	return hleCall(sceCtrl, int, sceCtrlReadBufferPositive, ctrlDataPtr, nBufs);
}

static int vshCtrlGetSamplingMode(u32 modePtr) {
	return hleCall(sceCtrl, int, sceCtrlGetSamplingMode, modePtr);
}

static u32 vshCtrlSetSamplingMode(u32 mode) {
	return hleCall(sceCtrl, u32, sceCtrlSetSamplingMode, mode);
}

// The module name hleCall takes is looked up in the HLE tables, so it has to be the name the
// function is actually registered under - there is no module called "sceIo". These are the
// kernel-side entry points, which is what a bridge call is.
static u32 vshIoDevctl(const char *name, int cmd, u32 argAddr, int argLen, u32 outPtr, int outLen) {
	return hleCall(IoFileMgrForKernel, u32, sceIoDevctl, name, cmd, argAddr, argLen, outPtr, outLen);
}

static u32 vshIoIoctl(u32 id, u32 cmd, u32 indataPtr, u32 inlen, u32 outdataPtr, u32 outlen) {
	return hleCall(IoFileMgrForKernel, u32, sceIoIoctl, id, cmd, indataPtr, inlen, outdataPtr, outlen);
}

static int vshChkregGetPsCode(u32 psCodePtr) {
	return hleCall(sceChkreg_driver, int, sceChkregGetPsCode, psCodePtr);
}

static int vshChkregCheckRegion(u32 umdMediaType, u32 regionId) {
	return hleCall(sceChkreg_driver, int, sceChkregCheckRegion, umdMediaType, regionId);
}

static int vshIdStorageLookup(u32 leafId, u32 offset, u32 bufPtr, u32 len) {
	return hleCall(sceIdStorage_driver, int, sceIdStorageLookup, leafId, offset, bufPtr, len);
}

// The XMB reaches the impose params through here rather than through sceImpose_driver, which is
// just as well - that library's NIDs were obfuscated from firmware 3.70 on and its names were never
// recovered, so it can't be implemented. See sceImpose.h.
static int vshImposeGetParam(int param) {
	int value = 0;
	if (!ImposeGetParam(param, &value)) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_VALUE, "unknown impose param %03x", param);
	}
	return hleLogDebug(Log::HLE, value, "%s", ImposeParamName(param));
}

static int vshImposeSetParam(int param, int value) {
	if (!ImposeSetParam(param, value)) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_VALUE, "impose param %03x (%s) rejected", param, ImposeParamName(param));
	}
	return hleLogDebug(Log::HLE, 0, "%s = %d", ImposeParamName(param), value);
}

const HLEFunction sceVshBridge[] = {
	{0X5F35E8FE, &WrapI_V<VshBridgeUnimpl>,            "vshAudioSRCChReserve",                   'i', "",       HLE_KERNEL_SYSCALL},
	{0XC886B91B, &WrapI_V<VshBridgeUnimpl>,            "vshAudioSRCOutputBlocking",              'i', "",       HLE_KERNEL_SYSCALL},
	{0X5C2983C2, &WrapI_UU<vshChkregCheckRegion>,      "vshChkregCheckRegion",                   'i', "xx",     HLE_KERNEL_SYSCALL},
	{0X74DBE57E, &WrapI_UU<vshChkregCheckRegion>,      "vshChkregCheckRegion",                   'i', "xx",     HLE_KERNEL_SYSCALL},
	{0X01730088, &WrapI_U<vshChkregGetPsCode>,         "vshChkregGetPsCode",                     'i', "x",      HLE_KERNEL_SYSCALL},
	{0X61001D64, &WrapI_U<vshChkregGetPsCode>,         "vshChkregGetPsCode",                     'i', "x",      HLE_KERNEL_SYSCALL},
	{0XEBC3A334, &WrapI_U<vshCtrlGetSamplingMode>,     "vshCtrlGetSamplingMode",                 'i', "x",      HLE_KERNEL_SYSCALL},
	{0X0D7A4FE4, &WrapI_UU<vshCtrlReadBufferPositive>, "vshCtrlReadBufferPositive",              'i', "xx",     HLE_KERNEL_SYSCALL},
	{0XC6395C03, &WrapI_UU<vshCtrlReadBufferPositive>, "vshCtrlReadBufferPositive",              'i', "xx",     HLE_KERNEL_SYSCALL},
	{0X4DB43867, &WrapI_UUUU<vshIdStorageLookup>,      "vshIdStorageLookup",                     'i', "xxxx",   HLE_KERNEL_SYSCALL},
	{0X228B9BC0, &WrapI_V<VshBridgeUnimpl>,            "vshImposeChanges",                       'i', "",       HLE_KERNEL_SYSCALL},
	{0X5894C339, &WrapI_V<VshBridgeUnimpl>,            "vshImposeChanges",                       'i', "",       HLE_KERNEL_SYSCALL},
	{0X360752BF, &WrapI_I<vshImposeGetParam>,          "vshImposeGetParam",                      'i', "i",      HLE_KERNEL_SYSCALL},
	{0X639C3CB3, &WrapI_I<vshImposeGetParam>,          "vshImposeGetParam",                      'i', "i",      HLE_KERNEL_SYSCALL},
	{0XF71BB4D5, &WrapI_I<vshImposeGetParam>,          "vshImposeGetParam",                      'i', "i",      HLE_KERNEL_SYSCALL},
	{0X6234B2F2, &WrapI_V<VshBridgeUnimpl>,            "vshImposeGetStatus",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XCA719C34, &WrapI_V<VshBridgeUnimpl>,            "vshImposeGetStatus",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XD818FD24, &WrapI_V<VshBridgeUnimpl>,            "vshImposeGetStatus",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0X4A596D2D, &WrapI_II<vshImposeSetParam>,         "vshImposeSetParam",                      'i', "ii",     HLE_KERNEL_SYSCALL},
	{0X88C35487, &WrapI_II<vshImposeSetParam>,         "vshImposeSetParam",                      'i', "ii",     HLE_KERNEL_SYSCALL},
	{0X4E4E4DA3, &WrapI_V<VshBridgeUnimpl>,            "vshImposeSetStatus",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XD7D7E7B6, &WrapI_V<VshBridgeUnimpl>,            "vshImposeSetStatus",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0X2380DC08, &WrapU_CIUIUI<vshIoDevctl>,           "vshIoDevctl",                            'i', "sxpipi", HLE_KERNEL_SYSCALL},
	{0X75C939B9, &WrapU_UUUUUU<vshIoIoctl>,            "vshIoIoctl",                             'i', "ixpipi", HLE_KERNEL_SYSCALL},
	{0XEF8229E9, &WrapI_V<VshBridgeUnimpl>,            "vshKernelDipswClear",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0XCF9DA76A, &WrapI_V<VshBridgeUnimpl>,            "vshKernelDipswSet",                      'i', "",       HLE_KERNEL_SYSCALL},
	{0X6032E5EE, &WrapI_V<VshBridgeUnimpl>,            "vshKernelExitVSHVSH",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0X7423151D, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecBufferVSHUsbWlan",      'i', "",       HLE_KERNEL_SYSCALL},
	{0X59E6C2E1, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecBufferVSHUsbWlanDebug", 'i', "",       HLE_KERNEL_SYSCALL},
	{0XC5B25CA7, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHDiscDebug",          'i', "",       HLE_KERNEL_SYSCALL},
	{0X0DCD4377, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHDiscUpdater",        'i', "",       HLE_KERNEL_SYSCALL},
	{0X81682A40, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHDisk",               'i', "",       HLE_KERNEL_SYSCALL},
	{0X9D3856CB, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHMs1",                'i', "",       HLE_KERNEL_SYSCALL},
	{0XD6862A9E, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHMs2",                'i', "",       HLE_KERNEL_SYSCALL},
	{0X21D4D038, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadExecVSHMs3",                'i', "",       HLE_KERNEL_SYSCALL},
	{0XC9626587, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleBufferVSH",           'i', "",       HLE_KERNEL_SYSCALL},
	{0X24BC5B26, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleVSH",                 'i', "",       HLE_KERNEL_SYSCALL},
	{0XA5628F0D, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleVSH",                 'i', "",       HLE_KERNEL_SYSCALL},
	{0XCCD27632, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleVSH",                 'i', "",       HLE_KERNEL_SYSCALL},
	{0X1881A5AD, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleVSHByID",             'i', "",       HLE_KERNEL_SYSCALL},
	{0X41C54ADF, &WrapI_V<VshBridgeUnimpl>,            "vshKernelLoadModuleVSHByID",             'i', "",       HLE_KERNEL_SYSCALL},
	{0X5E5AF7A2, &WrapI_V<VshBridgeUnimpl>,            "vshKernelSetParamSfo",                   'i', "",       HLE_KERNEL_SYSCALL},
	{0X74DA9D25, &WrapI_V<VshBridgeUnimpl>,            "vshLflashFatfmtStartFatfmt",             'i', "",       HLE_KERNEL_SYSCALL},
	{0X6CAEB765, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioAuth",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0X53BFD101, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioCheckICV",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XE174218C, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioCheckICVn",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0X7EA32357, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioDeauth",                       'i', "",       HLE_KERNEL_SYSCALL},
	{0X09EBF066, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioDecryptFringe",                'i', "",       HLE_KERNEL_SYSCALL},
	{0XD4163117, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioDecryptTrack",                 'i', "",       HLE_KERNEL_SYSCALL},
	{0X5947F162, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioEnd",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0X7584D38A, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioEnd",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0X99299855, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioEnd",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0XE5DA5E95, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioEnd",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0XF0A465C2, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioGetICVEKBVersion",             'i', "",       HLE_KERNEL_SYSCALL},
	{0XCBD84D3F, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioGetICVInfo",                   'i', "",       HLE_KERNEL_SYSCALL},
	{0XA29B5A33, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioGetInitialEKB",                'i', "",       HLE_KERNEL_SYSCALL},
	{0X914F3DC8, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInit",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0XC04F9353, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInit",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0XC40F345F, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInit",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0XCE32CBEF, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInit",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0X34D51F83, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInitFringe",                   'i', "",       HLE_KERNEL_SYSCALL},
	{0X29644970, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioInitTrack",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0XF1E03935, &WrapI_V<VshBridgeUnimpl>,            "vshMSAudioReadMACList",                  'i', "",       HLE_KERNEL_SYSCALL},
	{0X30E78D9C, &WrapI_V<VshBridgeUnimpl>,            "vshMeBootStart",                         'i', "",       HLE_KERNEL_SYSCALL},
	{0X2C2DB18C, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoCheckICV",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0X24A3355D, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoClearMACList",                 'i', "",       HLE_KERNEL_SYSCALL},
	{0X2C88E671, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoEndMovie",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XDFA5A40A, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoFormatICV",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0X4B687C1C, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoGetDefaultEKB",                'i', "",       HLE_KERNEL_SYSCALL},
	{0X38962187, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoGetLicenseInfo",               'i', "",       HLE_KERNEL_SYSCALL},
	{0X37EFB3B5, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoInitMovie",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0X3785D08B, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoInitTrack",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0XFD47C29F, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoNetBindLicense",               'i', "",       HLE_KERNEL_SYSCALL},
	{0XB90254A9, &WrapI_V<VshBridgeUnimpl>,            "vshMgVideoUpdateICV",                    'i', "",       HLE_KERNEL_SYSCALL},
	{0X7B3B17DC, &WrapI_V<VshBridgeUnimpl>,            "vshPowerSetWakeupCondition",             'i', "",       HLE_KERNEL_SYSCALL},
	{0XB374EF4C, &WrapI_V<VshBridgeUnimpl>,            "vshPowerSetWakeupCondition",             'i', "",       HLE_KERNEL_SYSCALL},
	{0XC949966C, &WrapI_V<VshBridgeUnimpl>,            "vshPowerSetWakeupCondition",             'i', "",       HLE_KERNEL_SYSCALL},
	{0XC51A6C26, &WrapI_V<VshBridgeUnimpl>,            "vshReceivePowerCallback",                'i', "",       HLE_KERNEL_SYSCALL},
	{0XDB7C3D5A, &WrapI_V<VshBridgeUnimpl>,            "vshRegisterPowerCallback",               'i', "",       HLE_KERNEL_SYSCALL},
	{0XFCFCAF0C, &WrapI_V<VshBridgeUnimpl>,            "vshRtcGetAlarmTick",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0X13299AA5, &WrapI_V<VshBridgeUnimpl>,            "vshRtcSetAlarmTick",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0XD49C57E3, &WrapI_V<VshBridgeUnimpl>,            "vshRtcSetConf",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0X27CD418C, &WrapI_V<VshBridgeUnimpl>,            "vshRtcSetCurrentTick",                   'i', "",       HLE_KERNEL_SYSCALL},
	{0X0543156C, &WrapI_V<VshBridgeUnimpl>,            "vshUmdManTerm",                          'i', "",       HLE_KERNEL_SYSCALL},
	{0X837C457A, &WrapI_V<VshBridgeUnimpl>,            "vshUnregisterPowerCallback",             'i', "",       HLE_KERNEL_SYSCALL},
	{0XAF135135, &WrapI_V<VshBridgeUnimpl>,            "vshVaudioChReserve",                     'i', "",       HLE_KERNEL_SYSCALL},
	{0X81706DA7, &WrapI_V<VshBridgeUnimpl>,            "vshVaudioOutputBlocking",                'i', "",       HLE_KERNEL_SYSCALL},

	// Exports with no recovered name, listed by NID. These are the ones a 6.61 vshmain.prx and
	// paf.prx actually import - not the full set of unnamed exports, just the ones observed being
	// asked for. Without an entry each of these traps as an unresolved import and hands the caller
	// SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED, which is worse than admitting we don't know.
	VSHBRIDGE_UNKNOWN(0296CA2B),
	VSHBRIDGE_UNKNOWN(0C0D5913),
	VSHBRIDGE_UNKNOWN(0D684A0B),
	VSHBRIDGE_UNKNOWN(12B07B05),
	VSHBRIDGE_UNKNOWN(157E2EAF),
	VSHBRIDGE_UNKNOWN(1D5C579F),
	VSHBRIDGE_UNKNOWN(21C243FE),
	VSHBRIDGE_UNKNOWN(27BDA326),
	VSHBRIDGE_UNKNOWN(29CDFFBA),
	VSHBRIDGE_UNKNOWN(2EBD2323),
	VSHBRIDGE_UNKNOWN(3512F4BC),
	VSHBRIDGE_UNKNOWN(3A46C639),
	VSHBRIDGE_UNKNOWN(3C90E435),
	VSHBRIDGE_UNKNOWN(3D30FEB6),
	VSHBRIDGE_UNKNOWN(582B5281),
	VSHBRIDGE_UNKNOWN(59197BE8),
	VSHBRIDGE_UNKNOWN(5B7F3339),
	VSHBRIDGE_UNKNOWN(5E0F5543),
	VSHBRIDGE_UNKNOWN(63047647),
	VSHBRIDGE_UNKNOWN(63E69956),
	VSHBRIDGE_UNKNOWN(65692F57),
	VSHBRIDGE_UNKNOWN(734D0F4F),
	VSHBRIDGE_UNKNOWN(787A8BCD),
	VSHBRIDGE_UNKNOWN(791FCD43),
	VSHBRIDGE_UNKNOWN(79B916E1),
	VSHBRIDGE_UNKNOWN(7A90D816),
	VSHBRIDGE_UNKNOWN(7B14CE2B),
	VSHBRIDGE_UNKNOWN(7D1C13B5),
	VSHBRIDGE_UNKNOWN(7E117907),
	VSHBRIDGE_UNKNOWN(9056DE3A),
	VSHBRIDGE_UNKNOWN(9347D693),
	VSHBRIDGE_UNKNOWN(9427C909),
	VSHBRIDGE_UNKNOWN(9940D95C),
	VSHBRIDGE_UNKNOWN(AAB9A9EF),
	VSHBRIDGE_UNKNOWN(ABB84565),
	VSHBRIDGE_UNKNOWN(B8B07CAF),
	VSHBRIDGE_UNKNOWN(CC864F6E),
	VSHBRIDGE_UNKNOWN(CD1A2C46),
	VSHBRIDGE_UNKNOWN(D39DE400),
	VSHBRIDGE_UNKNOWN(D3A07961),
	VSHBRIDGE_UNKNOWN(D47041CA),
	VSHBRIDGE_UNKNOWN(E533E98C),
	VSHBRIDGE_UNKNOWN(F702FC07),
};

void Register_sceVshBridge() {
	RegisterHLEModule("sceVshBridge", ARRAY_SIZE(sceVshBridge), sceVshBridge);
}

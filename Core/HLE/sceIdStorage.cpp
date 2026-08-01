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

// idstorage.prx - the small key/value store in the PSP's NAND holding per-console identity: the
// ConsoleId ("PSID") and OpenPSID certificates, and the UMD region codes. The XMB reads it early,
// via sceVshBridge (vshIdStorageLookup).
//
// Function names and NIDs are from PSPLibDoc (https://github.com/pspdev/psplibdoc), for firmware
// 6.61; all eighteen hash to their NID. The library is called sceIdStorage_driver - there is no
// plain sceIdStorage library. Leaf IDs, the leaf size and the layout notes come from uofw's
// include/idstorage.h (https://github.com/uofw/uofw, MIT).
//
// What's emulated: the shape of the store, honestly. It reports itself as formatted, read-only and
// clean, and refuses every write. What it deliberately does NOT do is invent leaf contents. The
// interesting leaves (0x100-0x102, and their backups at 0x120-0x122) hold ECDSA-signed
// certificates over a real console's ConsoleId; a fabricated one could not be signed correctly, so
// handing back zeros or random bytes would only turn a clean "not found" into a corrupt read.
// Leaf reads therefore fail with the leaf ID in the log, which is the diagnostic that's actually
// useful here. See docs/XMB.md.

#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceIdStorage.h"

// SCE_ID_STORAGE_LEAF_SIZE.
static const int ID_STORAGE_LEAF_SIZE = 512;

// uofw's SCE_ERROR_NOT_FOUND. PPSSPP has no name for the generic SCE error family, and this is
// what the real driver returns for a leaf that isn't present.
static const u32 ID_STORAGE_ERROR_NOT_FOUND = 0x80000025;
// uofw's SCE_ERROR_NOT_SUPPORTED, returned for writes to a read-only store.
static const u32 ID_STORAGE_ERROR_NOT_SUPPORTED = 0x80000004;

static int sceIdStorageInit() {
	return hleLogDebug(Log::HLE, 0);
}

static int sceIdStorageEnd() {
	return hleLogDebug(Log::HLE, 0);
}

static int sceIdStorageGetLeafSize() {
	return hleLogDebug(Log::HLE, ID_STORAGE_LEAF_SIZE);
}

static int sceIdStorageIsFormatted() {
	// A retail console's ID storage is always formatted; claiming otherwise would invite callers
	// to try to format it.
	return hleLogDebug(Log::HLE, 1);
}

static int sceIdStorageIsReadOnly() {
	return hleLogDebug(Log::HLE, 1);
}

static int sceIdStorageIsDirty() {
	return hleLogDebug(Log::HLE, 0);
}

static int sceIdStorageGetFreeLeaves() {
	return hleLogDebug(Log::HLE, 0);
}

static int sceIdStorageFlush() {
	return hleLogDebug(Log::HLE, 0);
}

int sceIdStorageReadLeaf(u32 leafId, u32 bufPtr) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_FOUND, "no leaf %04x - PPSSPP has no ID storage contents", leafId);
}

int sceIdStorageLookup(u32 leafId, u32 offset, u32 bufPtr, u32 len) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_FOUND, "no leaf %04x (offset %d, len %d)", leafId, offset, len);
}

static int sceIdStorageWriteLeaf(u32 leafId, u32 bufPtr) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageUpdate(u32 leafId, u32 offset, u32 bufPtr, u32 len) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageCreateLeaf(u32 leafId) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageCreateAtomicLeaves(u32 leafIdsPtr, int count) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageDeleteLeaf(u32 leafId) {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageFormat() {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageUnformat() {
	return hleLogWarning(Log::HLE, ID_STORAGE_ERROR_NOT_SUPPORTED, "read-only");
}

static int sceIdStorageEnumId(u32 callbackPtr, u32 optPtr) {
	// Would have to call back into MIPS code once per leaf. Nothing to enumerate anyway.
	return hleLogError(Log::HLE, 0, "UNIMPL");
}

const HLEFunction sceIdStorage_driver[] = {
	{0XAB129D20, &WrapI_V<sceIdStorageInit>,                "sceIdStorageInit",               'i', "",     HLE_KERNEL_SYSCALL},
	{0X2CE0BE69, &WrapI_V<sceIdStorageEnd>,                 "sceIdStorageEnd",                'i', "",     HLE_KERNEL_SYSCALL},
	{0XEB830733, &WrapI_V<sceIdStorageGetLeafSize>,         "sceIdStorageGetLeafSize",        'i', "",     HLE_KERNEL_SYSCALL},
	{0XFEFA40C2, &WrapI_V<sceIdStorageIsFormatted>,         "sceIdStorageIsFormatted",        'i', "",     HLE_KERNEL_SYSCALL},
	{0X2D633688, &WrapI_V<sceIdStorageIsReadOnly>,          "sceIdStorageIsReadOnly",         'i', "",     HLE_KERNEL_SYSCALL},
	{0XB9069BAD, &WrapI_V<sceIdStorageIsDirty>,             "sceIdStorageIsDirty",            'i', "",     HLE_KERNEL_SYSCALL},
	{0X37833CB8, &WrapI_V<sceIdStorageGetFreeLeaves>,       "sceIdStorageGetFreeLeaves",      'i', "",     HLE_KERNEL_SYSCALL},
	{0X3AD32523, &WrapI_V<sceIdStorageFlush>,               "sceIdStorageFlush",              'i', "",     HLE_KERNEL_SYSCALL},
	{0XEB00C509, &WrapI_UU<sceIdStorageReadLeaf>,           "sceIdStorageReadLeaf",           'i', "xx",   HLE_KERNEL_SYSCALL},
	{0X6FE062D1, &WrapI_UUUU<sceIdStorageLookup>,           "sceIdStorageLookup",             'i', "xxxx", HLE_KERNEL_SYSCALL},
	{0X1FA4D135, &WrapI_UU<sceIdStorageWriteLeaf>,          "sceIdStorageWriteLeaf",          'i', "xx",   HLE_KERNEL_SYSCALL},
	{0X683AAC10, &WrapI_UUUU<sceIdStorageUpdate>,           "sceIdStorageUpdate",             'i', "xxxx", HLE_KERNEL_SYSCALL},
	{0X08A471A6, &WrapI_U<sceIdStorageCreateLeaf>,          "sceIdStorageCreateLeaf",         'i', "x",    HLE_KERNEL_SYSCALL},
	{0X99ACCB71, &WrapI_UI<sceIdStorageCreateAtomicLeaves>, "sceIdStorageCreateAtomicLeaves", 'i', "xi",   HLE_KERNEL_SYSCALL},
	{0X2C97AB36, &WrapI_U<sceIdStorageDeleteLeaf>,          "sceIdStorageDeleteLeaf",         'i', "x",    HLE_KERNEL_SYSCALL},
	{0X958089DB, &WrapI_V<sceIdStorageFormat>,              "sceIdStorageFormat",             'i', "",     HLE_KERNEL_SYSCALL},
	{0XF4BCB3EE, &WrapI_V<sceIdStorageUnformat>,            "sceIdStorageUnformat",           'i', "",     HLE_KERNEL_SYSCALL},
	{0X31E08AFB, &WrapI_UU<sceIdStorageEnumId>,             "sceIdStorageEnumId",             'i', "xx",   HLE_KERNEL_SYSCALL},
};

void Register_sceIdStorage() {
	RegisterHLEModule("sceIdStorage_driver", ARRAY_SIZE(sceIdStorage_driver), sceIdStorage_driver);
}

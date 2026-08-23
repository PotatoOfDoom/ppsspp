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

// resmgr.prx - the VSH's "resource manager". The XMB imports exactly one function from it, and only
// to get at one file: flash0:/vsh/etc/index_<model>g.dat, which holds the firmware version and build
// info in encrypted form. Without it the VSH gives up on the file and the job thread that wanted it
// exits with an error. See docs/XMB.md.
//
// No name for the NID is known - it isn't in PSPLibDoc - so it keeps the sceResmgr_<NID> form. The
// argument list was read off the call site in vshmain.prx, which does:
//
//     sceIoRead(fd, buf, 0x1f0); sceIoClose(fd);
//     if (sceResmgr_9DC14891(buf, size, &outSize) >= 0) { ...parse buf... } else give up;
//
// so it decrypts the blob in place and reports how long the plaintext is.
//
// **We do not decrypt anything, and don't need to.** Sony ships the plaintext of exactly this data
// right next to it in the same directory, as flash0:/vsh/etc/version.txt - the PSP dev wiki has said
// so for years ("version.txt is simply the decrypted (plaintext) version of the same data"), and the
// dump agrees: every index_XXg.dat on a 6.61 dump carries a little-endian u32 at offset 0xB0 that is
// the exact byte length of the version.txt beside it (159 on 6.61). So read version.txt, check that
// its length is the one the encrypted blob declares, and hand that over.
//
// The alternative was a stub returning success, and that would have been actively harmful: the tail
// of the file is real ciphertext (the last 240 of its 496 bytes measure 7.13 bits of entropy per
// byte), so the VSH would have parsed encrypted bytes as a version string. Everything below is
// checked before anything is written, and anything that doesn't line up returns an error - which is
// what the caller already handles, since that is what it gets today.

#include <cstring>
#include <vector>

#include "Core/Debugger/MemBlockInfo.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceResmgr.h"
#include "Core/MemMap.h"
#include "Core/System.h"

// The magic every index_XXg.dat starts with. The seven per-model files differ from each other from
// here on - each is encrypted for its own model - but they all share this header and all declare the
// same plaintext length.
static const char INDEX_DAT_MAGIC[8] = { 'P', 'S', 'P', 's', 'y', 's', 'G', 'P' };

// Little-endian u32: the length of the decrypted contents.
static const u32 INDEX_DAT_PLAINTEXT_SIZE_OFFSET = 0xB0;
static const u32 INDEX_DAT_MIN_SIZE = INDEX_DAT_PLAINTEXT_SIZE_OFFSET + 4;

static const char *const VERSION_TXT_PATH = "flash0:/vsh/etc/version.txt";

// Decrypts flash0:/vsh/etc/index_<model>g.dat in place, writing the plaintext length to outSizeAddr.
static int sceResmgr_9DC14891(u32 bufAddr, int size, u32 outSizeAddr) {
	if (size < (int)INDEX_DAT_MIN_SIZE || !Memory::IsValidRange(bufAddr, size)) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_SIZE, "bad buffer %08x size %d", bufAddr, size);
	}

	const u8 *buf = Memory::GetPointerRange(bufAddr, size);
	if (!buf || memcmp(buf, INDEX_DAT_MAGIC, sizeof(INDEX_DAT_MAGIC)) != 0) {
		// Some other resource we've never seen. Better to say no than to invent a decryption.
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_FORMAT, "not an index.dat");
	}

	const u32 plaintextSize = Memory::Read_U32(bufAddr + INDEX_DAT_PLAINTEXT_SIZE_OFFSET);

	std::vector<u8> plaintext;
	if (pspFileSystem.ReadEntireFile(VERSION_TXT_PATH, plaintext) < 0) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND,
			"can't decrypt index.dat without %s next to it", VERSION_TXT_PATH);
	}

	// If these disagree, version.txt doesn't belong to this index.dat - a mixed-up dump, or a
	// firmware whose format isn't the one this was worked out against. Don't guess.
	if (plaintext.size() != plaintextSize) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_FORMAT,
			"%s is %d bytes, index.dat declares %d", VERSION_TXT_PATH, (int)plaintext.size(), plaintextSize);
	}
	if (plaintext.size() > (size_t)size) {
		return hleLogError(Log::HLE, SCE_KERNEL_ERROR_INVALID_SIZE,
			"plaintext (%d) doesn't fit the caller's buffer (%d)", (int)plaintext.size(), size);
	}

	Memory::MemcpyUnchecked(bufAddr, plaintext.data(), (u32)plaintext.size());
	NotifyMemInfo(MemBlockFlags::WRITE, bufAddr, (u32)plaintext.size(), "ResmgrDecryptIndex");
	if (Memory::IsValidRange(outSizeAddr, 4)) {
		Memory::Write_U32((u32)plaintext.size(), outSizeAddr);
	}

	return hleLogInfo(Log::HLE, 0, "served %d bytes from %s", (int)plaintext.size(), VERSION_TXT_PATH);
}

const HLEFunction sceResmgr[] = {
	{0X9DC14891, &WrapI_UIU<sceResmgr_9DC14891>, "sceResmgr_9DC14891", 'i', "xix"},
};

void Register_sceResmgr() {
	RegisterHLEModule("sceResmgr", ARRAY_SIZE(sceResmgr), sceResmgr);
}

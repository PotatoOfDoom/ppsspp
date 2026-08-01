# Running the XMB (the PSP's VSH)

The XMB (XrossMediaBar) is the PSP's system software. Its main module is
`flash0:/vsh/module/vshmain.prx`, and internally it's usually called the VSH ("Visual Shell").
PPSSPP has the beginnings of a boot path for it. **It does not get to a drawn frame yet** — see
[What's still missing](#whats-still-missing).

Nothing in this repo contains PSP firmware, and PPSSPP can't provide it. Running the VSH requires
files dumped from a PSP you own.

## How to try it

1. Dump `flash0` from a PSP. The directory layout PPSSPP expects is the flash volume's own layout:

   ```
   <flash0 directory>/
     vsh/module/vshmain.prx     <- the boot module
     vsh/module/*.prx           <- paf.prx, common_gui.prx, sysconf_plugin.prx, ...
     vsh/resource/*.rco         <- the XMB's own graphics and layout resources
     kd/*.prx                   <- kernel modules
     font/*.pgf
     data/cert/
   ```

2. Point PPSSPP at it. The `flash0` directory defaults to `assets/flash0` next to the executable
   (it normally only holds the bundled fonts), and can be changed in the config as
   `flash0Directory`. Alternatively, just boot any `vshmain.prx` directly — the directory three
   levels above it is mounted as `flash0:`, so a dump can live anywhere.

3. Boot it, either way:

   ```
   PPSSPPSDL --vsh
   PPSSPPHeadless --vsh --log=vsh.log
   ```

   or open the `vshmain.prx` from the dump like any other file. Files named `vshmain.prx` are
   identified as `IdentifiedFileType::PSP_VSH` and get the VSH boot path rather than the homebrew
   ELF path.

Use `--log=` and read the log — that's where all the interesting information is right now.

## What the VSH boot path does differently

Implemented in `Load_PSP_VSH` and `MountVSHFlash` (`Core/PSPLoaders.cpp`), instead of
`Load_PSP_ELF_PBP`. Note that the mounting happens from `__IoInit`, not from the loader — loadexec
re-initializes the kernel, and with it the mount table, after the loader has run.

- **Mounts the dump as `flash0:`.** The default `flash0:` is the read-only asset filesystem with
  just the fonts in it (and on non-Windows/Apple builds it can't even list directories), so it's
  replaced with a real `DirectoryFileSystem` over the dump. This matters because the VSH loads all
  its siblings by absolute `flash0:` path, not relative to itself.
- **Mounts `flash1:`, `flash2:` and `flash3:`** as writable directories under
  `<system>/flash/`, created on demand. `flash1:` is where the real VSH keeps the registry and the
  settings it writes back. These mounts are only added when booting the VSH, so games see exactly
  the mount list they always have.
- **Sets the working directory** to `flash0:/vsh/module`.
- **Pins a stable disc ID** (`PSPVSH000`) and title, since there's no `PARAM.SFO`. Without this a
  fake ID gets generated from the filename, which would make savestates and per-game config drift.

Also relevant, outside that function:

- `Identify_File` (`Core/Loaders.cpp`) now accepts `~PSP` and `~SCE` magic, not just plain ELF.
  Real flash0 modules are encrypted and/or signed, and were previously rejected as unknown files
  before the module loader — which knows how to decrypt them — ever saw them.
- VSH-mode modules (module attribute `0x0800`, `PSP_MODULE_VSH_MODE`) count as privileged for
  thread creation, so they may use the non-user thread attributes. See
  `KernelModuleIsPrivileged` in `Core/HLE/sceKernelModule.cpp`.

## What's still missing

Roughly in the order you hit them.

### 1. KL4E decompression

Most `kd/` and `vsh/` modules are compressed with one of Sony's in-house LZ variants — `KL4E`, or
the older `KL3E`/`2RLZ`/`1RLZ` — rather than gzip. PPSSPP only implements gzip, so loading such a
module fails with:

```
Module 'vshmain' uses KL4E compression, which PPSSPP can't decompress
```

`DetectPrxCompression` in `Core/HLE/sceKernelModule.cpp` names the format so this is obvious in the
log. Implementing the decompressor is the single biggest blocker.

Note on sourcing an implementation: KL4E was reverse engineered from firmware 6.60 (originally
`UtilsForKernel_6C6887EE` in `sysmem.prx`), and the well-known implementations —
[pspdecrypt](https://github.com/John-K/pspdecrypt)'s `kl4e.c` and
[JPCSP](https://github.com/jpcsp/jpcsp)'s port of it — are **GPLv3**, which can't be copied into
PPSSPP (GPLv2-or-later). It needs to be written from the algorithm rather than copied. There is
already an LZRC range decoder in `Core/FileSystems/tlzrc.cpp` that shares machinery with it.

### 2. The kernel/driver HLE surface

PPSSPP's HLE surface is essentially the user-mode API that games use. Practically none of what the
VSH imports exists: `sceVshBridge`, `sceImpose_driver`, `sceSysreg_driver`, `sceSyscon_driver`,
`sceIdStorage_driver`, `sceNand_driver`, `sceMScm_driver`, `sceChkreg`, `sceCertLoader`,
`sceMesgLed`, `sceClockgen_driver`, `sceUmdMan_driver`, `sceMeCore`, `sceLibUpdateDL`,
`sceVshCommonGui`/`sceVshCommonUtil`, and the kernel-side `sceUtility`/`sceRegistry`.

Unresolved imports are not fatal — `ImportFuncSymbol` writes a stub that returns
`SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED` — so `vshmain` loads and runs, but every call into the
kernel fails. Unresolved *variable* imports are worse: the relocation is skipped entirely, leaving
whatever was baked into the `lui`/`addiu` pair.

Adding these follows the normal recipe in `AGENTS.md`. Note that NIDs are the first four bytes of
the SHA-1 of the exported function name, read little-endian, which is a useful way to check a
name/NID pair before adding it.

`scePaf` (the VSH's whole widget/resource framework) does **not** need HLE — `paf.prx` is a real
module in the dump and can run as-is, once it can be decompressed and loaded.

### 3. Privilege model

VSH-mode modules run in user space on real hardware, so loading them into the user partition — which
is what PPSSPP does — is right. What's missing is the privilege *rights* that go with it:

- Privilege is a property of the HLE function being called (the `HLE_KERNEL_SYSCALL` flag, via
  `hleIsKernelMode()`), not of the calling module. So a VSH module asking
  `sceKernelAllocPartitionMemory` for the kernel partition gets
  `SCE_KERNEL_ERROR_ILLEGAL_PARTITION` regardless of what it is.
- Nothing gates `*ForKernel` or vsh-only exports, in either direction.
- Modules that need to land at a fixed address in a specific partition can't say so:
  `sceKernelLoadModule` rejects `PSP_SMEM_Addr` and discards the lmoption partition IDs.
- Memory size is picked by the generic non-FAT-model rule, not from what the real VSH would see.
- The kernel partition is only 4 MB and doubles as PPSSPP's own HLE scratch heap (the PPGe font
  atlas, per-thread return hacks, Atrac contexts, one `SceModule` per loaded module), which real
  firmware would be using for resident kernel code.

Genuine kernel-mode modules are further out of reach: `MIPSState` has no COP0 at all, and
`mfc0`/`mtc0`/`eret`/`tlbw*` are decoded but unimplemented.

### 4. The registry

`Core/HLE/sceReg.cpp` is a static in-memory dump of a real PSP's registry with no write-back and no
`flash1:` backing. The VSH reads it heavily and expects to write settings to it.

## Debugging tips

- The interpreter (`--interpreter`) makes breakpoints far more reliable than the JITs.
- `PPSSPPHeadless --debugger=PORT` breaks before anything runs, so you can step from the first
  instruction. See [WebSocketDebugger.md](WebSocketDebugger.md), and `Tools/wsdbg/`.
- Watch for `Unknown module`/`Unknown syscall: unresolved import` lines in the log — that's the
  list of libraries to implement next.

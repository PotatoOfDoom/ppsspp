# Running the XMB (the PSP's VSH)

The XMB (XrossMediaBar) is the PSP's system software. Its main module is
`flash0:/vsh/module/vshmain.prx`, and internally it's usually called the VSH ("Visual Shell").
PPSSPP has the beginnings of a boot path for it. **It does not get to a drawn frame yet** — see
[What's still missing](#whats-still-missing).

Nothing in this repo contains PSP firmware, and PPSSPP neither ships nor downloads it. Running the
VSH requires files you supply yourself, from a PSP you own or from an official Sony update file.

## How to try it

1. Get a `flash0` tree, either by dumping it from a PSP or by extracting `DATA.PSAR` from an official
   Sony update `EBOOT.PBP` (which carries the complete flash0 contents) with a tool like
   [pspdecrypt](https://github.com/John-K/pspdecrypt). PPSSPP neither ships nor fetches firmware.

   **Decrypt and decompress the modules on the PC side while you're at it.** PPSSPP takes a plain
   ELF/PRX straight to `ElfReader` and never enters the `~PSP` decrypt/decompress path at all
   (`__KernelLoadELFFromPtr` gates that whole block on the `~PSP` magic), so a pre-decrypted tree
   sidesteps the KL4E problem below entirely. This is the recommended route.

   Note that per-console data — IdStorage, the PSID, the MAC address — is in neither a PSAR nor a
   plain flash0 dump. PPSSPP has to fake those regardless.

2. The directory layout PPSSPP expects is the flash volume's own layout:

   ```
   <flash0 directory>/
     vsh/module/vshmain.prx     <- the boot module
     vsh/module/*.prx           <- paf.prx, common_gui.prx, sysconf_plugin.prx, ...
     vsh/resource/*.rco         <- the XMB's own graphics and layout resources
     kd/*.prx                   <- kernel modules
     font/*.pgf
     data/cert/
   ```

3. Point PPSSPP at it. The `flash0` directory defaults to `assets/flash0` next to the executable
   (it normally only holds the bundled fonts), and can be changed in the config as
   `flash0Directory`. Alternatively, just boot any `vshmain.prx` directly — the directory three
   levels above it is mounted as `flash0:`, so a dump can live anywhere.

4. Boot it, either way:

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

### 1. KL4E decompression — only if you feed PPSSPP raw modules

Most `kd/` and `vsh/` modules as stored in flash are compressed with one of Sony's in-house LZ
variants — `KL4E`, or the older `KL3E`/`2RLZ`/`1RLZ` — rather than gzip. PPSSPP only implements gzip,
so loading such a module as-is fails with:

```
Module 'vshmain' uses KL4E compression, which PPSSPP can't decompress
```

`DetectPrxCompression` in `Core/HLE/sceKernelModule.cpp` names the format so this is obvious in the
log rather than showing up as a generic failure.

**This is avoidable, not a hard blocker** — decrypt and decompress the tree on the PC side and
PPSSPP loads the resulting plain ELFs directly, as described under "How to try it". Implementing the
decompressor is only needed to load an untouched flash0 dump.

If someone does implement it: KL4E was reverse engineered from firmware 6.60 (originally
`UtilsForKernel_6C6887EE` in `sysmem.prx`), and the well-known implementations —
[pspdecrypt](https://github.com/John-K/pspdecrypt)'s `kl4e.c` and
[JPCSP](https://github.com/jpcsp/jpcsp)'s port of it — are **GPLv3**, which can't be copied into
PPSSPP (GPLv2-or-later). It has to be written from the algorithm rather than copied. There is
already an LZRC range decoder in `Core/FileSystems/tlzrc.cpp` that shares machinery with it.

### 2. The kernel/driver HLE surface — the actual frontier

PPSSPP's HLE surface is essentially the user-mode API that games use. Three of the VSH's kernel
libraries now exist:

| Library | File | State |
| --- | --- | --- |
| `sceVshBridge` | `Core/HLE/sceVshBridge.cpp` | 87 of 189 exports named; the ones that map onto something PPSSPP has are wired through |
| `sceChkreg_driver` | `Core/HLE/sceChkreg.cpp` | complete — PS code, region check, PSP model, PS flags |
| `sceIdStorage_driver` | `Core/HLE/sceIdStorage.cpp` | complete API, but no leaf contents (see below) |

Still missing: `sceSysreg_driver`, `sceSyscon_driver`, `sceNand_driver`, `sceMScm_driver`,
`sceCertLoader`, `sceMesgLed`, `sceClockgen_driver`, `sceUmdMan_driver`, `sceMeCore`,
`sceLibUpdateDL`, `sceVshCommonGui`/`sceVshCommonUtil`, and the kernel-side `sceUtility`.

Two things learned while adding those, which shape what else is possible:

**Kernel NIDs are obfuscated and firmware-specific.** SCE scrambled them in later firmwares, so for
kernel libraries the NID is *not* SHA-1(name) and it differs per firmware version. The tables target
**6.61** and will not resolve against an older dump. Worse, for some libraries the names were never
recovered at all — `sceImpose_driver` has 31 exports on 6.61 and *zero* known names (they're known
up to 5.00, before obfuscation), so that library simply cannot be implemented for a modern dump.
Check [PSPLibDoc](https://github.com/pspdev/psplibdoc) before planning work on one.

**The VSH mostly doesn't call these libraries directly** — it goes through `sceVshBridge`, whose
names *are* largely known. So `vshImposeGetParam`/`vshImposeSetParam` are implemented against
impose-param state in `Core/HLE/sceImpose.cpp` even though `sceImpose_driver` can't be. When a
kernel library is a dead end, check whether the bridge route is open.

**ID storage has no contents.** The API answers truthfully (512-byte leaves, formatted, read-only)
but every leaf read fails with the leaf ID in the log. The interesting leaves (0x100–0x102) hold
ECDSA-signed certificates over a real console's ConsoleId, which cannot be fabricated — and per-
console data is in neither a firmware dump nor a PSAR. If the XMB turns out to need a specific
non-signed field, the UMD region codes at leaf 0x102 offset 0xB0 are the place to look.

Unresolved imports are not fatal — `ImportFuncSymbol` writes a stub that returns
`SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED` — so `vshmain` loads and runs, but every call into the
kernel fails. Unresolved *variable* imports are worse: the relocation is skipped entirely, leaving
whatever was baked into the `lui`/`addiu` pair.

Adding these follows the normal recipe in `AGENTS.md`. For NIDs and names, use
[PSPLibDoc](https://github.com/pspdev/psplibdoc) (GPL-2.0) — it has per-firmware exports for every
module, and marks which names hash to their NID. For user-mode libraries the NID is the first four
bytes of the SHA-1 of the export name read little-endian, which is a cheap way to check a name/NID
pair; for kernel libraries on later firmwares it is not, because SCE obfuscated them.

`scePaf` (the VSH's whole widget/resource framework) does **not** need HLE — `paf.prx` is a real
module in the dump and runs as-is. Nothing in the HLE blacklist (`g_moduleMeta` in
`Core/HLE/HLE.cpp`) matches `vshmain`, `paf` or the `*_plugin` modules, so they are loaded for real
rather than faked.

With a pre-decrypted tree this, not decompression, is what stands between here and an XMB frame.

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

# Running the XMB (the PSP's VSH)

The XMB (XrossMediaBar) is the PSP's system software. Its main module is
`flash0:/vsh/module/vshmain.prx`, and internally it's usually called the VSH ("Visual Shell").
PPSSPP has the beginnings of a boot path for it. Against a real 6.61 dump it starts `vshmain`,
`paf`, `common_gui` and `common_util`, brings up the `SCE_VSH_GRAPHICS` thread, reads its settings
out of the registry, loads the system fonts, opens the XMB's own resource files
(`opening_plugin.rco`, `system_plugin.rco` and its `_bg`/`_fg` companions), loads and starts
`opening_plugin.prx`, `impose_plugin.prx` and `mpeg_vsh.prx`, and submits GE display lists
continuously without faulting. Nothing is presented, though — `sceDisplaySetFramebuf` is never
called. **It does not reach a usable XMB** — see [What's still missing](#whats-still-missing).

Nothing in this repo contains PSP firmware, and PPSSPP neither ships nor downloads it. Running the
VSH requires files you supply yourself, from a PSP you own or from an official Sony update file.

## How to try it

1. Get a `flash0` tree, either by dumping it from a PSP or by extracting `DATA.PSAR` from an official
   Sony update `EBOOT.PBP`, which carries the complete flash0 contents. PPSSPP neither ships nor
   fetches firmware.

   `Tools/extract_flash0.py` does the update-file route and validates the result:

   ```
   python3 Tools/extract_flash0.py EBOOT.PBP -o <flash0 directory>
   python3 Tools/extract_flash0.py --check <flash0 directory>   # validate a tree you already have
   ```

   It delegates the actual extraction to [pspdecrypt](https://github.com/John-K/pspdecrypt), which
   has the PSAR keys and the KL4E decompressor; what it adds is knowing which files PPSSPP needs,
   where they go, and whether they came out in a state it can load.

   **Decrypt and decompress the modules on the PC side** (which is what the script does by default).
   PPSSPP takes a plain ELF/PRX straight to `ElfReader` and never enters the `~PSP`
   decrypt/decompress path at all (`__KernelLoadELFFromPtr` gates that whole block on the `~PSP`
   magic), so a pre-decrypted tree sidesteps the KL4E problem below entirely.

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

3. Boot it by opening the `vshmain.prx` from your tree like any other file:

   ```
   PPSSPPSDL       <tree>/vsh/module/vshmain.prx
   PPSSPPHeadless  <tree>/vsh/module/vshmain.prx -l --log=vsh.log
   ```

   Files named `vshmain.prx` are identified as `IdentifiedFileType::PSP_VSH` and get the VSH boot
   path rather than the homebrew ELF path. **A tree can live anywhere** — the directory three levels
   above `vshmain.prx` is mounted as `flash0:`, and a `flash1` next to it as `flash1:`.

   The `--vsh` flag exists too, but it is only useful if your dump happens to be in the built-in
   `flash0` directory. That is `assets/flash0` next to the executable on desktop (where the bundled
   fonts live) and platform-specific elsewhere; `g_Config.flash0Directory` is set at startup per
   platform and is **not** a persisted config setting, so it can't be pointed somewhere else without
   a code change.

Use `--log=` and read the log — that's where all the interesting information is right now.

## What the VSH boot path does differently

Implemented in `Load_PSP_VSH` and `MountVSHFlash` (`Core/PSPLoaders.cpp`), instead of
`Load_PSP_ELF_PBP`. Note that the mounting happens from `__IoInit`, not from the loader — loadexec
re-initializes the kernel, and with it the mount table, after the loader has run.

- **Mounts the dump as `flash0:`.** The default `flash0:` is the read-only asset filesystem with
  just the fonts in it (and on non-Windows/Apple builds it can't even list directories), so it's
  replaced with a real `DirectoryFileSystem` over the dump. This matters because the VSH loads all
  its siblings by absolute `flash0:` path, not relative to itself.
- **Mounts `flash1:`, `flash2:` and `flash3:`.** `flash1:` is where the real VSH keeps the registry
  and the settings it writes back. Each is taken from a sibling of the flash0 root if one exists —
  which is where `Tools/extract_flash0.py` puts the `flash1` it extracts — otherwise from a writable
  directory under `<system>/flash/`, created on demand. These mounts are only added when booting the
  VSH, so games see exactly the mount list they always have.
- **Sets the working directory** to `flash0:/vsh/module`.
- **Pins a stable disc ID** (`PSPVSH000`) and title, since there's no `PARAM.SFO`. Without this a
  fake ID gets generated from the filename, which would make savestates and per-game config drift.
- **Loads the shared libraries `vshmain` links against** — `paf.prx`, `common_gui.prx` and
  `common_util.prx` — out of `flash0:/vsh/module/`. Unlike a game's `EBOOT.BIN`, `vshmain` is not
  self-contained: on a real PSP those are already resident when it starts. `LoadVSHSharedModules`
  loads all of them first (so cross-references between them resolve at load time rather than
  depending on which `module_start` thread runs first), then starts them and makes the loadexec
  thread wait for them, reusing the same mechanism plugins use. The deferred-linking path in
  `sceKernelModule.cpp` patches `vshmain`'s still-missing import stubs as each one registers its
  exports. Only the shared libraries are preloaded — the `*_plugin.prx` modules are loaded on demand
  by the VSH itself.

Also relevant, outside that function:

- `Identify_File` (`Core/Loaders.cpp`) now accepts `~PSP` and `~SCE` magic, not just plain ELF.
  Real flash0 modules are encrypted and/or signed, and were previously rejected as unknown files
  before the module loader — which knows how to decrypt them — ever saw them.
- VSH-mode modules (module attribute `0x0800`, `PSP_MODULE_VSH_MODE`) count as privileged for
  thread creation, so they may use the non-user thread attributes. See
  `KernelModuleIsPrivileged` in `Core/HLE/sceKernelModule.cpp`. This matters in two places:
  `sceKernelCreateThread`, and `__KernelStartModule`, which passes a module's *module* attribute
  as the thread attribute when the module has no `module_start` — and `0x0800` is not a legal
  user thread attribute.
- After the shared modules are loaded, `KernelLogUnresolvedImports` lists every import nothing
  provides, so a missing module shows up as one clear line at boot instead of as a crash somewhere
  in the middle of `vshmain`. It reports functions and *variables* separately — see below for why
  the variables matter more than they look.

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
PPSSPP loads the resulting plain ELFs directly, as described under "How to try it".
`Tools/extract_flash0.py --check` tells you whether any module in a tree is still compressed.
Implementing the decompressor is only needed to load an untouched flash0 dump.

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
| `sceVshBridge` | `Core/HLE/sceVshBridge.cpp` | 87 of 189 exports named, plus the 43 name-less ones a real 6.61 `vshmain`/`paf` import, as `sceVshBridge_<NID>`; the ones that map onto something PPSSPP has are wired through |
| `sceChkreg_driver` | `Core/HLE/sceChkreg.cpp` | complete — PS code, region check, PSP model, PS flags |
| `sceIdStorage_driver` | `Core/HLE/sceIdStorage.cpp` | complete API, but no leaf contents (see below) |

Still missing entirely: `sceSysreg_driver`, `sceSyscon_driver`, `sceNand_driver`, `sceMScm_driver`,
`sceCertLoader`, `sceMesgLed`, `sceClockgen_driver`, `sceUmdMan_driver`, `sceMeCore`,
`sceLibUpdateDL`, and the kernel-side `sceUtility`. A boot against real 6.61 additionally wanted
`sceBSMan`, `sceMlnBridge`, `sceResmgr`, `sceUtility_netparam_internal`, `sceNpCommerce2Store` and
`sceNpCommerce2RegCam` — one to three functions each, imported but not called yet.
`sceVshCommonGui` and `sceVshCommonUtil` are *not* on any of these lists — like `scePaf`, they come
from real modules in the dump and don't need HLE.

Existing modules can be short a NID or two as well, which looks the same from the outside but is a
much smaller job. `KernelLogUnresolvedImports` separates the two cases at boot: "no module provides
library X" versus "HLE library X is missing N NID(s)".

Two things learned while adding those, which shape what else is possible:

**Kernel NIDs are obfuscated and firmware-specific.** For kernel libraries the NID is *not*
SHA-1(name) and it differs per firmware version, so the tables target **6.61** and will not resolve
against an older dump. The switch is sharp and easy to measure against PSPLibDoc: counting named
`*_driver` exports whose NID equals the first four bytes of SHA-1(name), firmware 3.60 is at
1476/1499 (98%) and 3.70 drops to 553/1462 (37%) — everything from 3.70 on is partly scrambled, and
6.61 sits at 663/1390 (47%). Worse, for some libraries the names were never recovered at all —
`sceImpose_driver` has 31 exports on 6.61 and *zero* known names — so that library simply cannot be
implemented for a modern dump. Check [PSPLibDoc](https://github.com/pspdev/psplibdoc) before
planning work on one.

**The VSH mostly doesn't call these libraries directly** — it goes through `sceVshBridge`, whose
names *are* largely known. So `vshImposeGetParam`/`vshImposeSetParam` are implemented against
impose-param state in `Core/HLE/sceImpose.cpp` even though `sceImpose_driver` can't be. When a
kernel library is a dead end, check whether the bridge route is open.

**ID storage has no contents.** The API answers truthfully (512-byte leaves, formatted, read-only)
but every leaf read fails with the leaf ID in the log. The interesting leaves (0x100–0x102) hold
ECDSA-signed certificates over a real console's ConsoleId, which cannot be fabricated — and per-
console data is in neither a firmware dump nor a PSAR. If the XMB turns out to need a specific
non-signed field, the UMD region codes at leaf 0x102 offset 0xB0 are the place to look.

Unresolved imports are not fatal *at load time* — `ImportFuncSymbol` writes a stub that returns
`SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED` — so `vshmain` loads and runs, but every call into the
kernel fails. They tend to be fatal shortly afterwards: firmware code rarely checks these return
values, so `0x8002013a` gets used as a pointer and the crash surfaces far from its cause. Unresolved
*variable* imports are worse still: the relocation is skipped entirely, leaving whatever was baked
into the `lui`/`addiu` pair.

That is why a table entry that admits it does nothing beats no entry at all, and it applies to
`nullptr` entries in existing modules too — those return the same error and never write their output
parameters, so the caller reads whatever was on its stack.

An unresolved **variable** import is quieter and worse. `ImportVarSymbol` skips the relocation
entirely, so the `lui`/`addiu` pair keeps whatever immediate the PRX was built with: no trap, no
error return, and a bad pointer that is *identical on every run*. That last property makes it look
like anything but a linking problem — a deterministic garbage address is easy to mistake for
uninitialised-but-stable stack. There is no stub to inspect the way there is for a function, so
`KernelLogUnresolvedImports` has to ask whether any loaded module exports the variable; it does, and
reports those separately. The only other trace is one `INFO` line per reference at load time,
`Variable (<library>,<nid>) unresolved, storing for later resolving`.

Every function the XMB imports **by name** now has an implementation. The ones that were `nullptr`
until it asked for them: `sceRtcGetAlarmTick`, `sceRtcIsAlarmed`, `sceRtcRegisterCallback` and
`sceRtcUnregisterCallback` (there is no alarm hardware, so they report none set),
`scePowerIsRequest`, `scePowerCancelRequest` and `scePowerRequestSuspend` (suspend isn't emulated),
`sceHttpsEnableOption` (the counterpart of the already-present `sceHttpsDisableOption`), and
`sceNpCommerce2Init`/`Term`. Worth redoing after any change to the dump — the list came from
cross-referencing the `Importing <name>` lines in a boot log against the `HLEFunction` tables,
which is a few minutes of scripting and finds them all at once.

**Plugins load through `vshKernelLoadModuleVSH`.** The VSH does not call `ModuleMgr` itself; it goes
through this bridge export, which takes exactly `sceKernelLoadModule`'s arguments and is now
forwarded to it. The argument list is not in PSPLibDoc — it was read off the call site, where
`paf.prx` asks for `flash0:/vsh/module/opening_plugin.prx` with flags 0 and an `SceKernelLMOption`
that sets only position and access, no partition IDs. It is exported under three NIDs on 6.61
(`0x24BC5B26`, `0xA5628F0D`, `0xCCD27632`); `vshKernelLoadModuleVSHByID` has two more and is still a
stub, because nothing has called it yet. With this in place a boot loads and starts
`opening_plugin.prx`, `impose_plugin.prx` and `flash0:/kd/mpeg_vsh.prx`.

Two things the plugins then poll every frame, thousands of times per boot, are the obvious next
candidates: `scePowerIsSuspendRequired`, which is still a `nullptr` entry in `scePower.cpp` (so it
logs `Unimplemented HLE function` and never writes anything), and `vshImposeChanges`, a name-only
`sceVshBridge` entry. Note also `sceKernelLoadModule: unsupported options` in the log — the
`SceKernelLMOption` the VSH passes is accepted but its position/access fields are discarded, which
ties into the privilege-model gap below.

Of the six libraries the XMB links against, **`sceResmgr` is now actually called** — one
`0x9dc14891` from `vsh_module`, right after a resource file is closed. That is the trigger this
document asked for, but it needs care rather than a quick stub: `sceResmgr` looks like the resource
decryptor, so "return success without doing anything" hands the caller undecrypted data, which is
worse than an honest failure. Work out what the call site does with the result first. The other five
still have no call site at all and are deliberately left alone, for the same reason — stubbing a
function whose purpose is unknown means guessing its contract.

Adding these follows the normal recipe in `AGENTS.md`. For NIDs and names, use
[PSPLibDoc](https://github.com/pspdev/psplibdoc) (GPL-2.0) — it has per-firmware exports for every
module, and marks which names hash to their NID. For user-mode libraries the NID is the first four
bytes of the SHA-1 of the export name read little-endian, which is a cheap way to check a name/NID
pair; for kernel libraries from firmware 3.70 on it is not, because SCE obfuscated them.

`scePaf` (the VSH's whole widget/resource framework) does **not** need HLE — `paf.prx` is a real
module in the dump and runs as-is, and PPSSPP now loads it (see the boot path above). Nothing in
the HLE blacklist (`g_moduleMeta` in `Core/HLE/HLE.cpp`) matches `vshmain`, `paf` or the `*_plugin`
modules, so they are loaded for real rather than faked.

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

`Core/HLE/sceReg.cpp` is a static const dump of a real PSP's registry. Writes used to be stubs that
returned success while changing nothing, so the VSH's first-boot setup could never complete — it
would write the owner name or language and read back the dumped value.

`sceRegSetKeyValue` now works, through an in-memory overlay that shadows the static tree on reads.
Two limits, both deliberate:

- **Session-local.** The overlay is cleared on init and never reaches the host, so a title that pokes
  at the system settings can't affect the next one, or the user's PPSSPP config. It *is* serialized
  into savestates, since whoever wrote a value expects to read it back.
- **Existing keys only.** Key handles are plain indices into the static array, so adding a key would
  move the handles of everything after it. `sceRegCreateKey` therefore still refuses. The XMB writes
  keys that exist in the dump, so this hasn't been a problem.

There's still no `flash1:` backing — the registry and the mounted `flash1:` volume are unrelated.

## Finding out what's actually needed

`Tools/dump_prx_imports.py` walks a module's `.lib.stub` import tables and cross-references every
(library, NID) pair against PPSSPP's own HLE tables, so it can say what a dump wants that PPSSPP
doesn't have:

```
python3 Tools/dump_prx_imports.py <flash0 dir>/vsh/module/vshmain.prx
python3 Tools/dump_prx_imports.py <flash0 dir> --summary       # ranked work list for a whole tree
```

Its output is library names, NIDs and counts, so it's a way to work out what to implement next
without moving firmware around. Modules have to be decrypted first.

## Debugging tips

- **Reproduce crashes on the interpreter** (`-i`). Breakpoints are far more reliable there than on the
  JITs, and it's the only backend where a bad memory access can tell you *which* instruction faulted
  and which register held the address:

  ```
  Read Word: Invalid access at deadc007 ... op: lw a0, 0x18(t3) (address = t3(deadbfef) + 24)
  ```

  Under a JIT the pc reported at a fault is the start of the compiled block, not the faulting
  instruction, so that line is left out rather than pointing at the wrong opcode.
- **`--memread=ignore` (and `--memwrite=ignore`) walks straight past a bad access** instead of
  stopping emulation, so one wild pointer doesn't hide everything behind it. Good for finding out
  how much further the boot would get; the results after the first bad access are fiction, so don't
  read anything into them beyond "what does it try next".
- `PPSSPPHeadless --debugger=PORT` breaks before anything runs, so you can step from the first
  instruction. See [WebSocketDebugger.md](WebSocketDebugger.md), and `Tools/wsdbg/`.
- **A deterministic bad pointer is not always a missing import.** The first real blocker here turned
  out to be a loader bug, not an HLE gap: `LoadRelocations2` never recorded `last_type`, so the
  "reuse the previous entry's lo16" form of a `R_MIPS_HI16` relocation always fell back to an addend
  of 0. That drops the +1 carry when the low half is negative, and the `lui` ends up pointing
  0x10000 below the symbol. In `vshmain` this hit exactly one of the two `lui`s that build the alarm
  table address, so a loop bound was read out of an unrelated float table as 0x3F666666 (`0.9f`) and
  the loop walked off the end of a 1-element array. It looks like anything but a relocation problem:
  no error, no stub, identical on every run. If a pointer is wrong but *stable*, compare the
  disassembly of the loaded code against the module on disk before hunting for a missing function —
  `memory.disasm` over the WebSocket debugger shows the relocated instruction, which is the one that
  matters.
- Watch for `no module provides library` lines at boot — that's the definitive list of what's
  missing, printed once, before anything can go wrong because of it. At runtime, an actual call
  through such a stub logs `Unresolved import <library>/<nid> called from '<module>'`; that lookup
  finds the stub through `ra` (a stub is `jr ra; syscall`, so the `jal` that called it sits at
  `ra - 8`) rather than through `pc`, which means different things on each CPU backend.

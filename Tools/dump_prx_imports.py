#!/usr/bin/env python3
# Copyright (c) 2026- PPSSPP Project.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2.0 or later versions.
#
# See the file LICENSE.TXT at the root of the repository for details.

"""Lists what a PSP module imports, and which of it PPSSPP implements.

Point it at a PRX/ELF or at a whole flash0 tree and it walks the .lib.stub
import tables, then cross-references every (library, NID) pair against PPSSPP's
own HLE tables in Core/HLE/*.cpp. In directory mode it ranks the missing
libraries by how many modules want them, which is the work list for making
something like the XMB run - see docs/XMB.md.

Three states, and the difference matters:
  * the library isn't in PPSSPP at all
  * the library is there but the NID isn't in its table
  * the NID is in the table with a nullptr implementation
The last one is the easiest to miss and the nastiest to debug: a nullptr entry
returns SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED and never writes the function's
output parameters, so the caller reads whatever was on its stack and dies
somewhere else entirely.

Nothing here needs the firmware itself to be shared: the output is library
names, NIDs and counts.

Modules must be decrypted first. A ~PSP or ~SCE file is reported as such and
skipped; see Tools/extract_flash0.py.

Usage:
    dump_prx_imports.py vshmain.prx
    dump_prx_imports.py <flash0 dir> --summary
    dump_prx_imports.py <flash0 dir> --summary --missing-only
"""

import argparse
import os
import re
import struct
import sys

ELF_MAGIC = b"\x7fELF"
ET_EXEC, ET_PSP_PRX = 2, 0xFFA0
PT_LOAD = 1
# See Core/HLE/sceKernelModule.cpp: struct PspLibStubEntry. 'size' is in 32-bit words.
STUB_MIN_WORDS = 5
# See Core/ELF/PrxDecrypter.h / PSPElfTypes.h.
MODULE_ATTR_VSH, MODULE_ATTR_KERNEL = 0x0800, 0x1000


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


class Elf:
    """Just enough ELF to resolve virtual addresses back to file offsets."""

    def __init__(self, data):
        self.data = data
        if data[:4] != ELF_MAGIC:
            raise ValueError(f"not an ELF (magic {data[:4]!r})")
        self.e_type, = struct.unpack_from("<H", data, 16)
        self.e_phoff, self.e_shoff = struct.unpack_from("<II", data, 28)
        (self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from("<HHHHH", data, 42)

        self.sections = []
        if self.e_shnum and self.e_shoff:
            raw = [struct.unpack_from("<10I", data, self.e_shoff + i * self.e_shentsize)
                   for i in range(self.e_shnum)]
            strtab = raw[self.e_shstrndx][4] if self.e_shstrndx < len(raw) else 0
            for s in raw:
                name = ""
                if strtab:
                    end = data.find(b"\0", strtab + s[0])
                    name = data[strtab + s[0]:end].decode("utf-8", "replace")
                # name, type, addr, offset, size
                self.sections.append((name, s[1], s[3], s[4], s[5]))

        self.segments = [struct.unpack_from("<8I", data, self.e_phoff + i * self.e_phentsize)
                         for i in range(self.e_phnum)]

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def to_offset(self, vaddr, length=1):
        """Maps a virtual address to a file offset, or None if it isn't mapped."""
        for _name, stype, addr, off, size in self.sections:
            if stype != 8 and size and addr <= vaddr and vaddr + length <= addr + size:  # skip .bss
                return off + (vaddr - addr)
        for p_type, p_offset, p_vaddr, _pa, p_filesz, _memsz, _fl, _al in self.segments:
            if p_type == PT_LOAD and p_filesz and p_vaddr <= vaddr and vaddr + length <= p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        return None

    def cstring(self, vaddr, limit=128):
        off = self.to_offset(vaddr)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + limit)
        return self.data[off:end].decode("utf-8", "replace") if end != -1 else None


def module_info(elf):
    """Returns (name, attrs, libstub, libstubend) from .rodata.sceModuleInfo."""
    sec = elf.section(".rodata.sceModuleInfo")
    if sec:
        off = sec[3]
    elif elf.segments:
        # Same fallback the loader uses when the section is absent.
        p_offset, p_vaddr, p_paddr = elf.segments[0][1], elf.segments[0][2], elf.segments[0][3]
        off = elf.to_offset(p_vaddr + (p_paddr & 0x7FFFFFFF) - p_offset)
        if off is None:
            raise ValueError("could not locate the module info")
    else:
        raise ValueError("no sections and no segments")
    attrs, _ver = struct.unpack_from("<HH", elf.data, off)
    name = elf.data[off + 4:off + 32].split(b"\0")[0].decode("utf-8", "replace")
    _gp, _ent, _entend, libstub, libstubend = struct.unpack_from("<5I", elf.data, off + 32)
    return name, attrs, libstub, libstubend


def read_imports(elf):
    """Returns [(library, [nid, ...]), ...] in the order the module lists them."""
    _name, _attrs, libstub, libstubend = module_info(elf)
    if not libstub or libstubend <= libstub:
        return []
    pos = elf.to_offset(libstub, libstubend - libstub)
    if pos is None:
        raise ValueError(f"import table at {libstub:#x}..{libstubend:#x} isn't in the file")

    out, end = [], pos + (libstubend - libstub)
    while pos + STUB_MIN_WORDS * 4 <= end:
        name_ptr, _ver, _flags, size, _numvars, numfuncs = struct.unpack_from("<IHHBBH", elf.data, pos)
        nid_ptr, = struct.unpack_from("<I", elf.data, pos + 12)
        if size == 0:
            break
        lib = elf.cstring(name_ptr) or f"(bad name ptr {name_ptr:#x})"
        nids = []
        nid_off = elf.to_offset(nid_ptr, numfuncs * 4) if numfuncs and nid_ptr else None
        if nid_off is not None:
            nids = list(struct.unpack_from(f"<{numfuncs}I", elf.data, nid_off))
        out.append((lib, nids))
        pos += size * 4
    return out


def find_repo_root(start_dirs):
    """Walks up from each starting point looking for a PPSSPP checkout."""
    for start in start_dirs:
        cur = os.path.abspath(start)
        while True:
            if os.path.isfile(os.path.join(cur, "Core", "HLE", "HLETables.cpp")):
                return cur
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return None


def load_ppsspp_tables(repo_root):
    """Parses Core/HLE/*.cpp into {library name: {nid: func name}}."""
    hle_dir = os.path.join(repo_root, "Core", "HLE")
    if not os.path.isdir(hle_dir):
        return None
    # The middle field is the implementation - a &WrapX_YYY<func>, or nullptr for a name-only entry.
    entry_re = re.compile(r'\{\s*(0[xX][0-9a-fA-F]{8})\s*,\s*([^,]*?)\s*,\s*"([A-Za-z_0-9]+)"')
    table_re = re.compile(r'HLEFunction\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)^\};', re.S | re.M)
    reg_re = re.compile(r'RegisterHLEModule\(\s*"([^"]+)"\s*,[^,]*,\s*(\w+)\s*\)')
    # HLETables.cpp also has a static moduleList[] of {"name", ARRAY_SIZE(t), t} (or just {"name"}
    # for a name-only placeholder), which never goes through RegisterHLEModule.
    modlist_re = re.compile(r'HLEModule\s+moduleList\s*\[\s*\]\s*=\s*\{(.*?)^\};', re.S | re.M)
    modlist_entry_re = re.compile(r'\{\s*"([^"]+)"\s*(?:,[^,]*,\s*(\w+)\s*)?\}')

    tables, libs = {}, {}
    for fn in sorted(os.listdir(hle_dir)):
        if not fn.endswith(".cpp"):
            continue
        try:
            src = open(os.path.join(hle_dir, fn), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for var, body in table_re.findall(src):
            tables[var] = {int(n, 16): (name, impl != "nullptr") for n, impl, name in entry_re.findall(body)}
        for lib, var in reg_re.findall(src):
            libs[lib] = var
        for body in modlist_re.findall(src):
            for lib, var in modlist_entry_re.findall(body):
                libs[lib] = var  # empty var for the name-only placeholders
    # Name-only registrations (no table, or a table variable we never saw) resolve to an empty set.
    return {lib: tables.get(var, {}) for lib, var in libs.items()}


def analyze(path, tables):
    """Returns (module name, attrs, [(lib, [(nid, funcname|None, implemented)], lib_known)]), or a
    string on failure. funcname is None when the NID isn't in the library's table at all;
    implemented is False for a nullptr table entry."""
    try:
        data = open(path, "rb").read(64 * 1024 * 1024)
    except OSError as e:
        return f"unreadable: {e}"
    if data[:4] in (b"~PSP", b"~SCE"):
        return "still encrypted - decrypt it first (see Tools/extract_flash0.py)"
    if data[:4] != ELF_MAGIC:
        return None  # not a module at all; silently skipped
    try:
        elf = Elf(data)
        name, attrs, _a, _b = module_info(elf)
        imports = read_imports(elf)
    except (ValueError, struct.error) as e:
        return f"malformed: {e}"

    result = []
    for lib, nids in imports:
        known = tables.get(lib) if tables is not None else None
        entries = []
        for n in nids:
            funcname, implemented = (known or {}).get(n, (None, False))
            entries.append((n, funcname, implemented))
        result.append((lib, entries, known is not None))
    return name, attrs, result


def attr_str(attrs):
    if attrs & MODULE_ATTR_KERNEL:
        return "kernel"
    if attrs & MODULE_ATTR_VSH:
        return "VSH"
    return "user"


def print_module(path, info, missing_only, checked=True):
    """checked is False under --no-cross-reference, where we know nothing about what PPSSPP has and
    must not imply that everything is missing."""
    name, attrs, libs = info
    total = sum(len(nids) for _l, nids, _k in libs)
    lines = []
    for lib, nids, lib_known in sorted(libs):
        unknown = [n for n, fn, _i in nids if fn is None]
        stubs = [n for n, fn, impl in nids if fn is not None and not impl]
        if not checked:
            state = "not checked"
        elif not lib_known:
            state = "library NOT in PPSSPP"
        elif unknown or stubs:
            bits = []
            if unknown:
                bits.append(f"{len(unknown)} unknown NID(s)")
            if stubs:
                bits.append(f"{len(stubs)} nullptr")
            state = f"{len(nids)} imports, " + ", ".join(bits)
        else:
            state = "all implemented"
        if missing_only and (not checked or (lib_known and not unknown and not stubs)):
            continue
        lines.append(f"    {lib:<28} {len(nids):>4} imports   [{state}]")
        if not checked:
            continue
        for n, fn, impl in nids:
            if fn is None:
                lines.append(f"        {n:#010x}  <NID not in PPSSPP's table>")
            elif not impl:
                lines.append(f"        {n:#010x}  {fn}: nullptr entry, returns LIBRARY_NOT_YET_LINKED")

    # Under --missing-only a module with nothing to report is just noise, so drop it entirely
    # rather than printing a bare header.
    if missing_only and not lines:
        return
    print(f"\n{path}")
    print(f"  module '{name}', attr {attr_str(attrs)}, {len(libs)} libraries, {total} imports")
    for line in lines:
        print(line)


def main():
    ap = argparse.ArgumentParser(
        description="List what PSP modules import, and what PPSSPP is missing.",
        epilog="See docs/XMB.md. Modules must be decrypted first.")
    ap.add_argument("target", help="a .prx/.elf module, or a directory to walk")
    ap.add_argument("--summary", action="store_true", help="aggregate over a directory and rank what's missing")
    ap.add_argument("--missing-only", action="store_true",
                    help="only show libraries with unknown NIDs or nullptr entries")
    ap.add_argument("--repo", help="PPSSPP source root, for cross-referencing "
                                   "(default: found by walking up from this script and the cwd)")
    ap.add_argument("--no-cross-reference", action="store_true",
                    help="just list imports, without saying what PPSSPP is missing")
    args = ap.parse_args()

    tables = None
    if not args.no_cross_reference:
        repo = args.repo or find_repo_root([os.path.dirname(os.path.abspath(__file__)), os.getcwd()])
        tables = load_ppsspp_tables(repo) if repo else None
        if tables is None:
            fail("could not find a PPSSPP checkout to cross-reference against"
                 + (f" (looked under {args.repo})" if args.repo else "")
                 + ".\n       Without it, every library would be reported as missing whether it is or"
                   " not.\n       Pass --repo /path/to/ppsspp, or --no-cross-reference to just list"
                   " the imports.")
        print(f"cross-referencing against {len(tables)} libraries registered in {repo}")

    if os.path.isfile(args.target):
        info = analyze(args.target, tables)
        if isinstance(info, str):
            fail_msg = info
            print(f"{args.target}: {fail_msg}", file=sys.stderr)
            sys.exit(1)
        if info is None:
            print(f"{args.target}: not a PSP module", file=sys.stderr)
            sys.exit(1)
        print_module(args.target, info, args.missing_only, checked=tables is not None)
        return

    if not os.path.isdir(args.target):
        print(f"error: {args.target}: no such file or directory", file=sys.stderr)
        sys.exit(1)

    # Directory mode.
    modules, skipped = [], []
    for root, _dirs, files in os.walk(args.target):
        for fn in sorted(files):
            path = os.path.join(root, fn)
            info = analyze(path, tables)
            if info is None:
                continue
            if isinstance(info, str):
                skipped.append((os.path.relpath(path, args.target), info))
                continue
            modules.append((os.path.relpath(path, args.target), info))

    if not args.summary:
        for path, info in modules:
            print_module(path, info, args.missing_only, checked=tables is not None)

    # missing library -> (modules that want it, total imports, unresolved NIDs, nullptr NIDs)
    missing = {}
    for _path, (_n, _a, libs) in modules:
        for lib, nids, lib_known in libs:
            unresolved = {n for n, fn, _i in nids if fn is None}
            stubs = {n for n, fn, impl in nids if fn is not None and not impl}
            if lib_known and not unresolved and not stubs:
                continue
            m = missing.setdefault(lib, {"modules": 0, "imports": 0, "nids": set(), "stubs": set(),
                                         "present": lib_known})
            m["modules"] += 1
            m["imports"] += len(nids)
            m["nids"] |= unresolved
            m["stubs"] |= stubs

    label = "incomplete" if tables is not None else "imported (not checked against PPSSPP)"
    print(f"\n{'=' * 78}\n{len(modules)} module(s) scanned, {len(missing)} library/libraries {label}")
    if any(os.sep + "kd" + os.sep in p or p.startswith("kd" + os.sep) for p, _i in modules):
        print("\nNote: this scan includes kd/ - the kernel modules PPSSPP replaces with its own HLE and\n"
              "never loads, so their imports don't need implementing. For the work list that actually\n"
              "matters, scan just the modules the VSH loads:  <tree>/vsh/module")
    if skipped:
        print(f"{len(skipped)} file(s) skipped:")
        for rel, why in skipped[:10]:
            print(f"  {rel}: {why}")
        if len(skipped) > 10:
            print(f"  ... and {len(skipped) - 10} more")
    if missing:
        print(f"\n{'library':<30}{'modules':>8}{'imports':>9}{'missing':>9}{'nullptr':>9}  state")
        for lib, m in sorted(missing.items(), key=lambda kv: (-kv[1]["modules"], -kv[1]["imports"])):
            if tables is None:
                state = "not checked"
            elif not m["present"]:
                state = "NOT IMPLEMENTED"
            elif m["nids"]:
                state = "partial"
            else:
                state = "nullptr entries only"
            print(f"{lib:<30}{m['modules']:>8}{m['imports']:>9}{len(m['nids']):>9}"
                  f"{len(m['stubs']):>9}  {state}")
    else:
        print("\nEverything these modules import is implemented.")


if __name__ == "__main__":
    main()

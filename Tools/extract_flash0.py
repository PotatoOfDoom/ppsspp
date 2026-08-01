#!/usr/bin/env python3
# Copyright (c) 2026- PPSSPP Project.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2.0 or later versions.
#
# See the file LICENSE.TXT at the root of the repository for details.

"""Prepares a flash0 tree that PPSSPP can boot the XMB (the VSH) from.

Takes an official Sony PSP firmware update - an EBOOT.PBP, or the DATA.PSAR out
of one - and produces the directory layout that `PPSSPPSDL --vsh` expects, then
tells you whether the result will actually boot. See docs/XMB.md.

This is a driver, not a self-contained extractor. The PSAR inside an update is
encrypted, and its contents are compressed with Sony's in-house KL4E/KL3E/2RLZ,
so the extraction itself is delegated to pspdecrypt:

    https://github.com/John-K/pspdecrypt

pspdecrypt is GPLv3 and PPSSPP is GPLv2-or-later, so its code can't be brought
in here - and PPSSPP has no KL4E decompressor of its own (docs/XMB.md explains
why that matters and what it would take). What this script adds is the part
pspdecrypt has no reason to know: which files PPSSPP needs, where they go, and
whether they came out in a state it can load.

No firmware is downloaded or included. Supply your own update file.

Usage:
    extract_flash0.py EBOOT.PBP                      # install into the default flash0 dir
    extract_flash0.py EBOOT.PBP -o /path/to/flash0
    extract_flash0.py --check /path/to/flash0        # just validate an existing tree
    extract_flash0.py --info EBOOT.PBP              # just describe the input
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# Magic numbers, matching Core/ELF/PSPElfTypes.h.
ELF_MAGIC = b"\x7fELF"
PSP_MAGIC = b"~PSP"
SCE_MAGIC = b"~SCE"
PBP_MAGIC = b"\x00PBP"

# Offset of comp_attribute in PSP_Header, see Core/ELF/PrxDecrypter.h. Bit 0 means compressed.
PSP_HEADER_COMP_ATTRIBUTE = 6

# The sub-files of a PBP, in the order their offsets appear in the header at 0x08.
PBP_SUBFILES = ["PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG",
                "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR"]

# What a VSH boot actually reaches for, relative to the flash0 root. Keep in sync with
# docs/XMB.md. (dir, description, fatal-if-missing)
REQUIRED = [
    ("vsh/module/vshmain.prx", "the VSH boot module", True),
    ("vsh/module", "the VSH's sibling modules (paf.prx, common_gui.prx, *_plugin.prx)", True),
    ("vsh/resource", "the XMB's graphics and layout resources (*.rco)", True),
    ("kd", "kernel modules", True),
    ("font", "the system fonts (*.pgf)", False),
    ("data/cert", "certificates", False),
]


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def read_sfo(data):
    """Parses a PARAM.SFO into a dict. Returns {} if it isn't one."""
    if len(data) < 0x14 or data[:4] != b"\x00PSF":
        return {}
    key_off, val_off, count = struct.unpack_from("<III", data, 0x08)
    out = {}
    for i in range(count):
        entry = 0x14 + i * 0x10
        if entry + 0x10 > len(data):
            break
        k_rel, fmt, val_len, _val_max, v_rel = struct.unpack_from("<HHIII", data, entry)
        key_end = data.find(b"\0", key_off + k_rel)
        key = data[key_off + k_rel:key_end].decode("utf-8", "replace")
        raw = data[val_off + v_rel:val_off + v_rel + val_len]
        if fmt == 0x0404:  # int32
            out[key] = struct.unpack("<i", raw[:4])[0] if len(raw) >= 4 else 0
        else:              # utf-8, NUL-terminated
            out[key] = raw.split(b"\0")[0].decode("utf-8", "replace")
    return out


def read_pbp(path):
    """Returns (subfiles, sfo) where subfiles maps name -> (offset, size)."""
    with open(path, "rb") as f:
        header = f.read(0x28)
        if len(header) < 0x28 or header[:4] != PBP_MAGIC:
            return None, {}
        offsets = list(struct.unpack_from("<8I", header, 0x08))
        total = os.path.getsize(path)
        subfiles = {}
        for i, name in enumerate(PBP_SUBFILES):
            start = offsets[i]
            end = offsets[i + 1] if i + 1 < len(offsets) else total
            if start > total:
                continue
            subfiles[name] = (start, max(0, min(end, total) - start))
        sfo = {}
        if "PARAM.SFO" in subfiles and subfiles["PARAM.SFO"][1] > 0:
            f.seek(subfiles["PARAM.SFO"][0])
            sfo = read_sfo(f.read(subfiles["PARAM.SFO"][1]))
        return subfiles, sfo


def describe_input(path):
    """Prints what we can tell about the input without decrypting anything."""
    with open(path, "rb") as f:
        magic = f.read(4)
    print(f"input: {path} ({os.path.getsize(path):,} bytes)")
    if magic == PBP_MAGIC:
        subfiles, sfo = read_pbp(path)
        print("  type:    PBP")
        version = sfo.get("PSP_SYSTEM_VER") or sfo.get("DISC_VERSION")
        if version:
            print(f"  version: {version}")
        if sfo.get("TITLE"):
            print(f"  title:   {sfo['TITLE']}")
        for name, (off, size) in subfiles.items():
            if size:
                print(f"  {name:<10} at 0x{off:08x}, {size:,} bytes")
        if not subfiles.get("DATA.PSAR", (0, 0))[1]:
            fail("this PBP has no DATA.PSAR - it's not a firmware update")
        return version
    if magic == b"PSAR":
        print("  type:    raw PSAR")
        return None
    fail(f"unrecognized input (magic {magic!r}) - expected an EBOOT.PBP or a DATA.PSAR")


def classify(path):
    """How PPSSPP will see this file: 'elf', 'encrypted', 'encrypted+compressed',
    'signed', 'data', or 'empty'."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
    except OSError:
        return "unreadable"
    if not head:
        return "empty"
    if head[:4] == ELF_MAGIC:
        return "elf"
    if head[:4] == SCE_MAGIC:
        return "signed"
    if head[:4] == PSP_MAGIC:
        if len(head) > PSP_HEADER_COMP_ATTRIBUTE + 1:
            comp = struct.unpack_from("<H", head, PSP_HEADER_COMP_ATTRIBUTE)[0]
            if comp & 1:
                return "encrypted+compressed"
        return "encrypted"
    return "data"


def run_pspdecrypt(tool, src, outdir, verbose):
    """Extracts and decrypts the PSAR contents. Returns the directory holding F0/F1."""
    cmd = [tool, "-A", "-O", outdir, src]
    print(f"running: {' '.join(cmd)}")
    try:
        proc = subprocess.run(cmd, capture_output=not verbose, text=True)
    except OSError as e:
        fail(f"could not run {tool}: {e}")
    if proc.returncode != 0:
        if not verbose and proc.stderr:
            sys.stderr.write(proc.stderr)
        fail(f"{tool} exited with {proc.returncode}. Re-run with --verbose for its full output.")
    if not os.path.isdir(os.path.join(outdir, "F0")):
        fail(f"{tool} produced no F0/ directory under {outdir} - nothing to install.\n"
             "       Its output layout may have changed; run it by hand and use --check on the result.")
    return outdir


def install(extracted, flash0dir, flash1dir):
    """Copies the F0/F1 trees into place. Returns the number of files copied."""
    copied = 0
    for src_name, dest in (("F0", flash0dir), ("F1", flash1dir)):
        src = os.path.join(extracted, src_name)
        if not os.path.isdir(src) or dest is None:
            continue
        os.makedirs(dest, exist_ok=True)
        for root, _dirs, files in os.walk(src):
            rel = os.path.relpath(root, src)
            target = os.path.join(dest, rel) if rel != "." else dest
            os.makedirs(target, exist_ok=True)
            for name in files:
                shutil.copy2(os.path.join(root, name), os.path.join(target, name))
                copied += 1
        print(f"installed {src_name}/ -> {dest}")
    return copied


def check(flash0dir):
    """Reports whether this tree can boot, and returns True if it can."""
    print(f"\nchecking {flash0dir}")
    if not os.path.isdir(flash0dir):
        fail(f"{flash0dir} is not a directory")

    ok = True
    for rel, what, fatal in REQUIRED:
        path = os.path.join(flash0dir, *rel.split("/"))
        if os.path.isfile(path):
            present = True
        elif os.path.isdir(path):
            present = any(os.scandir(path))
        else:
            present = False
        if present:
            print(f"  [ok]      {rel:<26} {what}")
        else:
            print(f"  [{'MISSING' if fatal else 'absent ':<7}] {rel:<26} {what}")
            if fatal:
                ok = False

    counts = {}
    still_packed = []
    for root, _dirs, files in os.walk(flash0dir):
        for name in files:
            path = os.path.join(root, name)
            kind = classify(path)
            counts[kind] = counts.get(kind, 0) + 1
            if kind.startswith("encrypted") or kind == "signed":
                still_packed.append((os.path.relpath(path, flash0dir), kind))

    print("\nfile states:")
    for kind in sorted(counts):
        print(f"  {counts[kind]:>5}  {kind}")

    if still_packed:
        compressed = [p for p, k in still_packed if k == "encrypted+compressed"]
        print(f"\n{len(still_packed)} file(s) are still packed. PPSSPP decrypts ~PSP modules itself, but it")
        print("cannot decompress Sony's KL4E/KL3E/2RLZ, so any of these that use it will fail to load")
        print("with \"uses KL4E compression, which PPSSPP can't decompress\".")
        if compressed:
            print(f"{len(compressed)} of them have the compressed bit set, for example:")
            for rel in compressed[:5]:
                print(f"  {rel}")
        print("Decrypt them on the PC side (pspdecrypt without --extract-only) to be safe.")

    # A compressed boot module is fatal on its own, whatever else is in place.
    boot = os.path.join(flash0dir, "vsh", "module", "vshmain.prx")
    boot_kind = classify(boot) if os.path.isfile(boot) else None
    if boot_kind == "encrypted+compressed":
        print("\nVSH boot: will NOT work - vsh/module/vshmain.prx is still compressed, so PPSSPP")
        print("          can't load it. Decrypt the tree on the PC side first.")
        return False

    print("\nVSH boot:", "looks possible" if ok else "will NOT work - see the MISSING entries above")
    return ok


def default_flash0_dir():
    """Where PPSSPP looks for flash0 by default on this platform, best effort."""
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/PPSSPP/flash0")
    if sys.platform.startswith("win"):
        return os.path.join(os.environ.get("APPDATA", ""), "PPSSPP", "flash0")
    return os.path.expanduser("~/.config/ppsspp/PSP/SYSTEM/flash0")


def main():
    ap = argparse.ArgumentParser(
        description="Prepare a flash0 tree for PPSSPP's VSH/XMB boot from a PSP firmware update.",
        epilog="See docs/XMB.md. No firmware is downloaded or included; supply your own update file.")
    ap.add_argument("input", nargs="?", help="an official EBOOT.PBP firmware update, or its DATA.PSAR")
    ap.add_argument("-o", "--outdir", help=f"flash0 directory to install into (default: {default_flash0_dir()})")
    ap.add_argument("--flash1", help="flash1 directory to install into (default: alongside flash0)")
    ap.add_argument("--check", metavar="FLASH0DIR", help="only validate an existing flash0 tree")
    ap.add_argument("--info", action="store_true", help="only describe the input file")
    ap.add_argument("--pspdecrypt", default="pspdecrypt", help="path to the pspdecrypt binary")
    ap.add_argument("--keep-temp", action="store_true", help="keep the intermediate extraction directory")
    ap.add_argument("-v", "--verbose", action="store_true", help="show pspdecrypt's own output")
    args = ap.parse_args()

    if args.check:
        sys.exit(0 if check(args.check) else 1)
    if not args.input:
        ap.error("an input file is required (or use --check)")
    if not os.path.isfile(args.input):
        fail(f"{args.input}: no such file")

    describe_input(args.input)
    if args.info:
        return

    tool = shutil.which(args.pspdecrypt) or (args.pspdecrypt if os.path.isfile(args.pspdecrypt) else None)
    if not tool:
        fail(f"'{args.pspdecrypt}' not found.\n"
             "       The PSAR inside an update is encrypted and KL4E-compressed, which PPSSPP\n"
             "       cannot unpack, so extraction needs pspdecrypt:\n"
             "         git clone https://github.com/John-K/pspdecrypt && cd pspdecrypt && make\n"
             "       Then re-run with --pspdecrypt=/path/to/pspdecrypt (or put it on your PATH).")

    flash0dir = os.path.abspath(args.outdir or default_flash0_dir())
    flash1dir = os.path.abspath(args.flash1) if args.flash1 else \
        os.path.join(os.path.dirname(flash0dir), "flash", "flash1")

    tmp = tempfile.mkdtemp(prefix="ppsspp-flash0-")
    try:
        extracted = run_pspdecrypt(tool, os.path.abspath(args.input), tmp, args.verbose)
        copied = install(extracted, flash0dir, flash1dir)
        print(f"copied {copied} file(s)")
    finally:
        if args.keep_temp:
            print(f"kept extraction directory: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    ok = check(flash0dir)
    if ok:
        print(f"\nTry it:  PPSSPPSDL --vsh          (with flash0Directory = {flash0dir})")
        print(f"     or:  PPSSPPSDL {os.path.join(flash0dir, 'vsh', 'module', 'vshmain.prx')}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

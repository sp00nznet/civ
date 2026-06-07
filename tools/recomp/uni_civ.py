#!/usr/bin/env python3
"""
uni_civ.py -- run the ORIGINAL CIV.EXE under the Unicorn CPU emulator.

A scriptable, headless harness (no DOSBox GUI) to observe the REAL game's
behavior and memory -- ground truth for the static recomp. Modeled on the bolo
recomp's tools/uni_*.py.

CIV.EXE is EXEPACK-compressed with ZERO MZ relocations: the EXEPACK stub at
2A10:0010 decompresses the resident image, applies relocations, and jumps to the
MSC crt0 (~205A:2E7D), which runs C main. The 23 overlay modules (~124 KB) sit
appended after the resident pages and are pulled in on demand by the MSC overlay
manager via INT 3Fh.

We let the stub do the decompression itself (like DOS would), hooking:
  - INT 21h : DOS services (file I/O from the game dir, mem alloc, version, exit)
  - INT 3Fh : MSC overlay manager (logged; can be emulated to load overlays)
  - INT 10h/16h/1Ah : BIOS video/keyboard/timer (stubbed)
and a code hook for CS-transition / target-address tracing + snapshotting.

Usage:
  uni_civ.py [--trace] [--max N] [--stop-open NAME] [--snap-open NAME]
    --trace        print every CS segment transition
    --max N        instruction cap (default 60M)
    --stop-open X  emu_stop the first time file X is opened (substring match)
    --snap-open X  dump memory + a context window the first time X is opened
"""
import os
import struct
import sys
from unicorn import *
from unicorn.x86_const import *

GAME_DIR = r"D:\recomp\pc\civ\extracted\Civilization\civ"
EXE = os.path.join(GAME_DIR, "CIV.EXE")
TOTAL = 0x110000                 # 1 MB + 64 KB (covers seg:off wraparound)
# EXEPACK decompresses to dest = CS - dest_len_paragraphs; a low load segment
# makes that underflow -> "Packed file is corrupt". Load higher (like DOS does
# after COMMAND.COM) so CS > the ~0x3200-paragraph decompressed size.
LOAD_SEG = 0x1000
PSP_SEG = LOAD_SEG - 0x10

R = {n: getattr(sys.modules["unicorn.x86_const"], "UC_X86_REG_" + n.upper())
     for n in ("ax bx cx dx si di bp sp cs ds es ss ip eflags".split())}


def regs(uc):
    return {n: uc.reg_read(r) for n, r in R.items()}


def arg(argv, flag, default=None):
    return argv[argv.index(flag) + 1] if flag in argv else default


def main():
    av = sys.argv
    TRACE = "--trace" in av
    NMAX = int(arg(av, "--max", str(60_000_000)))
    STOP_OPEN = arg(av, "--stop-open")
    SNAP_OPEN = arg(av, "--snap-open")

    raw = open(EXE, "rb").read()
    (magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc,
     e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno) = struct.unpack_from("<14H", raw, 0)
    hdr = e_cparhdr * 16
    img_size = (e_cp - 1) * 512 + (e_cblp or 512) - hdr
    module = raw[hdr:hdr + img_size]
    print(f"MZ: hdr={hdr:#x} resident-module={len(module):#x} relocs={e_crlc} "
          f"CS:IP={e_cs:04x}:{e_ip:04x} SS:SP={e_ss:04x}:{e_sp:04x} "
          f"file={len(raw):#x} overlays~={len(raw)-hdr-img_size:#x}")

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, TOTAL)
    uc.mem_write(LOAD_SEG * 16, module)
    # minimal PSP (top-of-memory segment at +2)
    uc.mem_write(PSP_SEG * 16, b"\xCD\x20")
    uc.mem_write(PSP_SEG * 16 + 2, struct.pack("<H", 0xA000))
    # BIOS data area: conventional memory size = 640 KB (word at 0040:0013, KB)
    uc.mem_write(0x40 * 16 + 0x13, struct.pack("<H", 640))

    cs0 = (LOAD_SEG + e_cs) & 0xFFFF
    ss0 = (LOAD_SEG + e_ss) & 0xFFFF
    for r, v in ((R["cs"], cs0), (R["ip"], e_ip), (R["ss"], ss0), (R["sp"], e_sp),
                 (R["ds"], PSP_SEG), (R["es"], PSP_SEG), (R["ax"], 0), (R["bx"], 0)):
        uc.reg_write(r, v)

    st = {"n": 0, "last_cs": None, "trans": 0, "opens": [], "ovl": 0,
          "mem_free": 0xA000}   # bump alloc ptr; set when program shrinks its block

    def hook_code(uc, address, size, _):
        st["n"] += 1
        cs = uc.reg_read(R["cs"])
        if cs != st["last_cs"]:
            st["trans"] += 1
            if TRACE and st["trans"] <= 200:
                ip = uc.reg_read(R["ip"])
                print(f"[{st['n']:10}] CS {st['last_cs']} -> {cs:04x}:{ip:04x} (lin {address:#07x})")
            st["last_cs"] = cs
        if st["n"] > NMAX:
            st["stop"] = "instruction cap"; uc.emu_stop()

    handles = {}
    def cf(set_):
        fl = uc.reg_read(R["eflags"])
        uc.reg_write(R["eflags"], (fl | 1) if set_ else (fl & ~1))

    def dump_ctx(tag):
        rg = regs(uc)
        ds = rg["ds"]
        print(f"\n=== SNAPSHOT @ {tag}  (insn {st['n']}) ===")
        print("  regs:", {k: hex(v) for k, v in rg.items() if k != "eflags"})
        # decode-context globals the recomp cares about (DS-relative)
        for off, name in ((0x686C, "refill handle"), (0xE84A, "cb_off"),
                          (0xE84C, "cb_seg"), (0xC19E, "buf ptr"), (0x54D8, "buf end")):
            try:
                v = struct.unpack("<H", uc.mem_read((ds << 4) + off, 2))[0]
                print(f"  DS:{off:04X} ({name}) = {v:04X}")
            except UcError:
                pass
        os.makedirs("work", exist_ok=True)
        open("work/uni_snapshot.bin", "wb").write(bytes(uc.mem_read(0, TOTAL)))
        print("  full image -> work/uni_snapshot.bin")

    def do_int(intno):
        """Emulate a real software INT: push FLAGS, CS, IP; jump to IVT[intno].
        Lets the game's own installed handler (e.g. the MSC overlay manager on
        INT 3Fh) run natively. CS:IP already point past the INT instruction."""
        cs = uc.reg_read(R["cs"]); ip = uc.reg_read(R["ip"]); sp = uc.reg_read(R["sp"])
        ss = uc.reg_read(R["ss"]); fl = uc.reg_read(R["eflags"])
        for v in (fl, cs, ip):
            sp = (sp - 2) & 0xFFFF
            uc.mem_write((ss << 4) + sp, struct.pack("<H", v & 0xFFFF))
        uc.reg_write(R["sp"], sp)
        hip = struct.unpack("<H", uc.mem_read(intno * 4, 2))[0]
        hcs = struct.unpack("<H", uc.mem_read(intno * 4 + 2, 2))[0]
        uc.reg_write(R["cs"], hcs); uc.reg_write(R["ip"], hip)

    def hook_intr(uc, intno, _):
        ax = uc.reg_read(R["ax"]); ah = (ax >> 8) & 0xFF
        if intno == 0x21:
            st.setdefault("i21", []).append(ah)
        if intno == 0x3F:
            st["ovl"] += 1
            ivt = struct.unpack("<I", uc.mem_read(0x3F * 4, 4))[0]
            if st["ovl"] <= 6:
                cs = uc.reg_read(R["cs"]); ip = uc.reg_read(R["ip"])
                idx = struct.unpack("<H", uc.mem_read((cs << 4) + ip, 2))[0]
                print(f"  [INT3F] #{st['ovl']} idx={idx:#06x} -> handler {ivt>>16:04x}:{ivt&0xFFFF:04x}")
            if ivt:
                do_int(0x3F)            # dispatch via the game's own overlay manager
            return
        if intno == 0x21:
            if ah == 0x4C:
                seq = st.get("i21", [])
                print("  INT21 AH sequence:", " ".join(f"{h:02X}" for h in seq[-30:]))
                st["stop"] = f"INT21/4C exit {ax & 0xFF}"; uc.emu_stop(); return
            if ah == 0x30:                       # DOS version
                uc.reg_write(R["ax"], 0x0005); return
            if ah == 0x48:                       # ALLOC paragraphs (BX)
                bx = uc.reg_read(R["bx"])
                avail = 0xA000 - st["mem_free"]
                if bx <= avail:
                    seg = st["mem_free"]; st["mem_free"] += bx
                    print(f"  [INT21/48] alloc {bx:#x} para -> seg {seg:04X} (free now {st['mem_free']:04X})")
                    uc.reg_write(R["ax"], seg); cf(False)
                else:
                    print(f"  [INT21/48] alloc {bx:#x} FAIL, avail={avail:#x} para")
                    uc.reg_write(R["ax"], 0x08)          # insufficient memory
                    uc.reg_write(R["bx"], avail); cf(True)  # largest available
                return
            if ah == 0x4A:                       # RESIZE block (ES, BX paragraphs)
                es = uc.reg_read(R["es"]); bx = uc.reg_read(R["bx"])
                print(f"  [INT21/4A] resize ES={es:04X} -> {bx:#x} para (PSP={PSP_SEG:04X})")
                if es == PSP_SEG:                 # program shrinking its own block
                    st["mem_free"] = (es + bx) & 0xFFFF
                cf(False); return
            if ah == 0x25:                       # SET interrupt vector (AL=int, DS:DX)
                al = ax & 0xFF; dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                uc.mem_write(al * 4, struct.pack("<HH", dx, ds)); return
            if ah == 0x35:                       # GET interrupt vector (AL=int -> ES:BX)
                al = ax & 0xFF
                off, seg = struct.unpack("<HH", uc.mem_read(al * 4, 4))
                uc.reg_write(R["bx"], off); uc.reg_write(R["es"], seg); return
            if ah in (0x49, 0x1A, 0x2C, 0x2A, 0x44):
                cf(False); return
            if ah in (0x3D, 0x3C):               # OPEN / CREATE
                dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                a = (ds << 4) + dx
                name = bytes(uc.mem_read(a, 24)).split(b"\x00")[0].decode("latin1", "replace")
                base = os.path.basename(name.replace("\\", "/"))
                st["opens"].append(base)
                print(f"  [INT21/{ah:02X}] OPEN '{name}'")
                path = os.path.join(GAME_DIR, base)
                if os.path.exists(path):
                    fd = len(handles) + 5
                    handles[fd] = open(path, "r+b" if ah == 0x3C else "rb")
                    uc.reg_write(R["ax"], fd); cf(False)
                else:
                    uc.reg_write(R["ax"], 0x02); cf(True)   # not found
                if SNAP_OPEN and SNAP_OPEN.lower() in base.lower():
                    dump_ctx(f"open {base}")
                if STOP_OPEN and STOP_OPEN.lower() in base.lower():
                    st["stop"] = f"opened {base}"; uc.emu_stop()
                return
            if ah == 0x4B:                       # EXEC / load overlay (AL=03)
                al = ax & 0xFF
                dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                bx = uc.reg_read(R["bx"]); es = uc.reg_read(R["es"])
                name = bytes(uc.mem_read((ds << 4) + dx, 24)).split(b"\x00")[0].decode("latin1", "replace")
                base = os.path.basename(name.replace("\\", "/"))
                pb = bytes(uc.mem_read((es << 4) + bx, 4))
                load_seg, reloc = struct.unpack("<HH", pb)
                path = os.path.join(GAME_DIR, base)
                if al == 0x03 and os.path.exists(path):
                    ov = open(path, "rb").read()
                    (om, ocblp, ocp, ocrlc, ocparhdr, *_), olfarlc = \
                        struct.unpack_from("<6H", ov, 0), struct.unpack_from("<H", ov, 0x18)[0]
                    ohdr = ocparhdr * 16
                    oimg = (ocp - 1) * 512 + (ocblp or 512) - ohdr
                    uc.mem_write(load_seg * 16, ov[ohdr:ohdr + oimg])
                    for i in range(ocrlc):          # apply overlay relocations
                        roff, rseg = struct.unpack_from("<HH", ov, olfarlc + i * 4)
                        a = ((load_seg + rseg) & 0xFFFF) * 16 + roff
                        v = (struct.unpack("<H", uc.mem_read(a, 2))[0] + reloc) & 0xFFFF
                        uc.mem_write(a, struct.pack("<H", v))
                    st.setdefault("ovls", []).append(base)
                    print(f"  [INT21/4B03] loaded overlay '{base}' ({oimg}B) @ seg {load_seg:04X} "
                          f"reloc {reloc:04X} ({ocrlc} relocs)")
                    cf(False)
                else:
                    print(f"  [INT21/4B AL={al:02X}] '{base}' not found / unsupported")
                    uc.reg_write(R["ax"], 0x02); cf(True)
                return
            if ah == 0x09:                       # print $-terminated string
                dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                a = (ds << 4) + dx
                s = bytes(uc.mem_read(a, 256)).split(b"$")[0]
                if s.strip():                    # skip screen-clear blank rows
                    print(f"  [INT21/09] PRINT: {s!r}")
                return
            if ah == 0x02:                       # print char
                dl = uc.reg_read(R["dx"]) & 0xFF
                st.setdefault("conout", bytearray()).append(dl)
                return
            if ah == 0x40:                       # WRITE (often the error message)
                bx = uc.reg_read(R["bx"]); cx = uc.reg_read(R["cx"])
                dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                data = bytes(uc.mem_read((ds << 4) + dx, min(cx, 256)))
                print(f"  [INT21/40] WRITE handle={bx} len={cx}: {data!r}")
                uc.reg_write(R["ax"], cx); cf(False); return
            if ah == 0x3F:                       # READ
                bx = uc.reg_read(R["bx"]); cx = uc.reg_read(R["cx"])
                dx = uc.reg_read(R["dx"]); ds = uc.reg_read(R["ds"])
                f = handles.get(bx)
                data = f.read(cx) if f else b""
                uc.mem_write((ds << 4) + dx, data)
                uc.reg_write(R["ax"], len(data)); cf(False); return
            if ah == 0x42:                       # LSEEK
                bx = uc.reg_read(R["bx"]); cx = uc.reg_read(R["cx"]); dx = uc.reg_read(R["dx"])
                al = ax & 0xFF; f = handles.get(bx)
                if f:
                    off = (cx << 16) | dx
                    f.seek(off, al)
                    pos = f.tell()
                    uc.reg_write(R["dx"], (pos >> 16) & 0xFFFF)
                    uc.reg_write(R["ax"], pos & 0xFFFF)
                cf(False); return
            if ah == 0x3E:                       # CLOSE
                bx = uc.reg_read(R["bx"]); f = handles.pop(bx, None)
                if f: f.close()
                cf(False); return
            cf(False); return
        if intno == 0x20:
            st["stop"] = "INT 20h"; uc.emu_stop(); return
        if intno == 0x16:                        # BIOS keyboard
            # scripted keys: video=1, sound=1, input=1, then advance (space/enter)
            KEYS = [(0x31, 0x02), (0x31, 0x02), (0x31, 0x02)] + \
                   [(0x20, 0x39), (0x0D, 0x1C)] * 40
            ki = st.get("ki", 0)
            key = KEYS[ki] if ki < len(KEYS) else (0x20, 0x39)
            word = (key[1] << 8) | key[0]
            if ah in (0x00, 0x10):               # read key (blocking) -> consume
                st["ki"] = ki + 1
                uc.reg_write(R["ax"], word)
            elif ah in (0x01, 0x11):             # status -> key available (ZF=0)
                uc.reg_write(R["ax"], word)
                uc.reg_write(R["eflags"], uc.reg_read(R["eflags"]) & ~0x40)
            return
        # INT 10h/1Ah/33h: stub-succeed (graphics/timer/mouse no-op)
        return

    if TRACE:                       # per-instruction hook is the perf bottleneck
        uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    begin = cs0 * 16 + e_ip
    print(f"start {cs0:04x}:{e_ip:04x} (lin {begin:#07x}); running EXEPACK stub...")
    try:
        uc.emu_start(begin, TOTAL, count=NMAX)
    except UcError as e:
        rg = regs(uc); pc = (rg["cs"] << 4) + rg["ip"]
        print(f"\nUcError: {e} after {st['n']} insns at {rg['cs']:04x}:{rg['ip']:04x}")
        print("  bytes@pc:", bytes(uc.mem_read(pc, 16)).hex(" "))
        print("  opens so far:", st["opens"])
        return
    print(f"\nstopped: {st.get('stop')} after {st['n']} insns, {st['trans']} CS transitions, "
          f"{st['ovl']} INT3F overlay calls")
    print("  file opens:", st["opens"])


if __name__ == "__main__":
    main()

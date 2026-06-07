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
# Top of conventional memory (real DOS: VGA starts at A000 = 640 KB).
MEM_TOP = 0xA000
# The game sizes its heap as (largest-free - 0x100), then loads the graphics
# driver overlay into the tiny region left at the top; its overrun check rejects
# the load if the driver's relocated internal top segment exceeds A000. Report a
# slightly lower allocation ceiling so the heap leaves ~0x300 paras (12 KB) of
# headroom -> the driver loads low enough that even the largest (egraphic ~0x2b5)
# fits. Real-mode VGA RAM above this is unused headless.
ALLOC_CEIL = MEM_TOP - 0x300

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
    uc.mem_write(PSP_SEG * 16 + 2, struct.pack("<H", MEM_TOP))
    # BIOS data area: conventional memory size (word at 0040:0013, in KB),
    # consistent with MEM_TOP so the overlay manager's cross-check doesn't fire.
    uc.mem_write(0x40 * 16 + 0x13, struct.pack("<H", MEM_TOP * 16 // 1024))
    # An IRET stub at 0060:0000 as the default handler for hardware INTs the game
    # hooks-and-chains (timer 8 / 1C, keyboard 9). When the game saves the "old"
    # vector and chains to it, it lands on this harmless IRET instead of garbage.
    uc.mem_write(0x600, b"\xCF")
    for v in (0x08, 0x09, 0x1C):
        uc.mem_write(v * 4, struct.pack("<HH", 0x0000, 0x0060))

    cs0 = (LOAD_SEG + e_cs) & 0xFFFF
    ss0 = (LOAD_SEG + e_ss) & 0xFFFF
    for r, v in ((R["cs"], cs0), (R["ip"], e_ip), (R["ss"], ss0), (R["sp"], e_sp),
                 (R["ds"], PSP_SEG), (R["es"], PSP_SEG), (R["ax"], 0), (R["bx"], 0)):
        uc.reg_write(r, v)

    st = {"n": 0, "last_cs": None, "trans": 0, "opens": [], "ovl": 0,
          "mem_free": MEM_TOP,   # bump alloc ptr; set when program shrinks its block
          "ticks": 0}           # BIOS 18.2 Hz tick (advanced per run-slice)

    import capstone
    _md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)

    def hook_trace(uc, address, size, _):
        if st.get("trace_n", 0) <= 0:
            return
        code = bytes(uc.mem_read(address, min(size, 15)))
        cs = uc.reg_read(R["cs"]); ip = uc.reg_read(R["ip"])
        ins = next(_md.disasm(code, ip), None)
        txt = f"{ins.mnemonic} {ins.op_str}" if ins else code.hex(" ")
        print(f"    {cs:04x}:{ip:04x}  ax={uc.reg_read(R['ax']):04x} bx={uc.reg_read(R['bx']):04x} "
              f"| {txt}")
        st["trace_n"] -= 1
        if st["trace_n"] == 0:
            st["stop"] = "trace done"; uc.emu_stop()

    _ring = []
    def hook_findjump(uc, address, size, _):
        cs = uc.reg_read(R["cs"])
        if cs in (0x0010, 0xFFCC) or address >= 0xFFFF0:   # entered the bad region
            print(f"  --- bad jump into {cs:04x}:{uc.reg_read(R['ip']):04x} "
                  f"(lin {address:#07x}); last sites (skipping zero runs): ---")
            prev = None
            for c, i, mn in _ring[-120:]:
                if mn.startswith("add byte ptr [bx + si], al") and prev == "zero":
                    continue
                prev = "zero" if mn.startswith("add byte ptr [bx + si], al") else None
                tag = " (zeros...)" if prev == "zero" else ""
                print(f"    {c:04x}:{i:04x}  {mn}{tag}")
            st["stop"] = "bad jump found"; uc.emu_stop(); return
        if st.get("record"):
            ip = uc.reg_read(R["ip"])
            code = bytes(uc.mem_read(address, min(size, 8)))
            ins = next(_md.disasm(code, ip), None)
            _ring.append((cs, ip, f"{ins.mnemonic} {ins.op_str}" if ins else code.hex()))
            if len(_ring) > 400:
                del _ring[:200]

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
    # scripted keys: video=1, sound=1, input=1, then advance (enter/space cycling).
    # Paced by the BIOS tick (one new key every KEY_TICKS ticks) so the game's
    # prompt loops settle between keys instead of being flooded every poll.
    KEYS = [(0x31, 0x02), (0x31, 0x02), (0x31, 0x02)] + [(0x0D, 0x1C), (0x20, 0x39)] * 200
    KEY_TICKS = 3
    def next_key(consume):
        idx = st["ticks"] // KEY_TICKS
        return KEYS[idx] if idx < len(KEYS) else (0x20, 0x39)

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
                avail = max(0, MEM_TOP - st["mem_free"])
                # Big heap alloc: reserve headroom so the graphics/sound driver
                # overlays (loaded later just above the heap) fit BELOW VGA (A000).
                # Their relocations use the load segment, so loading them above
                # A000 corrupts them -> bad far call into garbage.
                if avail > 0x5000:
                    avail -= 0xE00
                # Floor small (driver-load) queries: the game loads the driver into
                # (avail-0x100) and its overrun check compares the driver size to
                # it; if avail<=0x100 then [0x53bc]=0 and any driver "overruns".
                avail = max(0x1000, avail)
                if bx <= avail:
                    seg = st["mem_free"]
                    st["mem_free"] = min(MEM_TOP, st["mem_free"] + bx)  # never into VGA
                    print(f"  [INT21/48] alloc {bx:#x} para -> seg {seg:04X} (free now {st['mem_free']:04X})")
                    uc.reg_write(R["ax"], seg); cf(False)
                else:
                    print(f"  [INT21/48] alloc {bx:#x} FAIL, avail={avail:#x} para")
                    uc.reg_write(R["ax"], 0x08)          # insufficient memory
                    uc.reg_write(R["bx"], avail); cf(True)  # largest available
                    if "--tracealloc" in av and not st.get("traced") and avail < 0x800:
                        st["traced"] = 1; st["trace_n"] = 700
                        print("  --- tracing driver-load alloc + overrun check ---")
                return
            if ah == 0x4A:                       # RESIZE block (ES, BX paragraphs)
                es = uc.reg_read(R["es"]); bx = uc.reg_read(R["bx"])
                print(f"  [INT21/4A] resize ES={es:04X} -> {bx:#x} para (PSP={PSP_SEG:04X})")
                if es == PSP_SEG:                 # program shrinking its own block
                    st["mem_free"] = (es + bx) & 0xFFFF
                cf(False); return
            if ah in (0x01, 0x07, 0x08):         # DOS char input (blocking) -> AL=ascii
                st["kc"] = st.get("kc", 0) + 1
                uc.reg_write(R["ax"], (ax & 0xFF00) | (next_key(True)[0]))
                cf(False); return
            if ah == 0x06:                       # direct console I/O
                dl = uc.reg_read(R["dx"]) & 0xFF
                if dl == 0xFF:                   # input poll -> ZF=0 + char
                    uc.reg_write(R["ax"], (ax & 0xFF00) | next_key(False)[0])
                    uc.reg_write(R["eflags"], uc.reg_read(R["eflags"]) & ~0x40)  # ZF=0
                return
            if ah == 0x0B:                       # check input status -> AL=FF (ready)
                uc.reg_write(R["ax"], (ax & 0xFF00) | 0xFF); return
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
                if base.lower() == "credits.txt":
                    st["record"] = 1           # start ring-recording near divergence
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
                    # Reclaim over-granted space: the avail-floor makes the game
                    # alloc a big block for a small driver overlay, over-advancing
                    # mem_free toward VGA so the NEXT driver loads at/above A000
                    # (corrupt). Set mem_free to just past this driver's real image
                    # so subsequent driver overlays load right after it, below A000.
                    ominalloc = struct.unpack_from("<H", ov, 0x0A)[0]
                    end = load_seg + (oimg + 15) // 16 + ominalloc
                    if load_seg < MEM_TOP and end <= MEM_TOP:
                        st["mem_free"] = min(st["mem_free"], end)
                    st.setdefault("ovls", []).append(base)
                    print(f"  [INT21/4B03] loaded overlay '{base}' ({oimg}B) @ seg {load_seg:04X} "
                          f"reloc {reloc:04X} ({ocrlc} relocs) end={end:04X}")
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
            st["i16"] = st.get("i16", 0) + 1
            key = next_key(ah in (0x00, 0x10))
            word = (key[1] << 8) | key[0]
            if ah in (0x00, 0x10):               # read key (blocking)
                uc.reg_write(R["ax"], word)
            elif ah in (0x01, 0x11):             # status -> key available (ZF=0)
                uc.reg_write(R["ax"], word)
                uc.reg_write(R["eflags"], uc.reg_read(R["eflags"]) & ~0x40)
            return
        if intno == 0x1A and ah == 0x00:         # get system time -> CX:DX ticks
            t = st["ticks"]
            uc.reg_write(R["cx"], (t >> 16) & 0xFFFF)
            uc.reg_write(R["dx"], t & 0xFFFF)
            uc.reg_write(R["ax"], 0)
            return
        if intno == 0x10:                        # minimal text-mode (mode 03) BIOS
            def getcur():
                return uc.mem_read(0x450, 1)[0], uc.mem_read(0x451, 1)[0]  # col,row
            def setcur(col, row):
                uc.mem_write(0x450, bytes([col & 0xFF, row & 0xFF]))
            def putcell(col, row, ch, attr=0x07):
                if 0 <= col < 80 and 0 <= row < 25:
                    uc.mem_write(0xB8000 + (row * 80 + col) * 2, bytes([ch & 0xFF, attr]))
            if ah == 0x00:                       # set mode -> clear text screen
                uc.mem_write(0x449, bytes([ax & 0xFF]))
                uc.mem_write(0xB8000, b"\x20\x07" * (80 * 25)); setcur(0, 0)
            elif ah == 0x02:                     # set cursor (DH=row, DL=col)
                dx = uc.reg_read(R["dx"]); setcur(dx & 0xFF, (dx >> 8) & 0xFF)
            elif ah == 0x0E:                     # teletype char
                ch = ax & 0xFF; col, row = getcur()
                if ch == 0x0D: col = 0
                elif ch == 0x0A: row += 1
                elif ch == 0x08: col = max(0, col - 1)
                else:
                    putcell(col, row, ch); col += 1
                if col >= 80: col = 0; row += 1
                if row >= 25: row = 24
                setcur(col, row)
            elif ah == 0x09:                     # write char+attr CX times at cursor
                ch = ax & 0xFF; bl = uc.reg_read(R["bx"]) & 0xFF
                cx = uc.reg_read(R["cx"]) or 1; col, row = getcur()
                for i in range(cx):
                    putcell(col + i, row, ch, bl)
            elif ah == 0x06:                     # scroll/clear window -> clear all
                uc.mem_write(0xB8000, b"\x20\x07" * (80 * 25))
            return
        # INT 33h: mouse no-op
        return

    def hook_mem_invalid(uc, access, address, size, value, _):
        cs = uc.reg_read(R["cs"]); ip = uc.reg_read(R["ip"])
        kind = {16: "READ_U", 17: "WRITE_U", 18: "FETCH_U",
                19: "READ_P", 20: "WRITE_P", 21: "FETCH_P"}.get(access, str(access))
        st.setdefault("badmem", 0)
        st["badmem"] += 1
        if st["badmem"] <= 12:
            print(f"  [MEM-INVALID] {kind} addr={address:#08x} size={size} val={value:#x} "
                  f"at {cs:04x}:{ip:04x}")
        # map the offending 64 KB page and continue, to see where it ends up
        page = address & ~0xFFFF
        try:
            uc.mem_map(page, 0x10000)
        except UcError:
            pass
        return True

    def hook_in(uc, port, size, _):
        # VGA input status (3DA/3BA): toggle bit3 (vsync) + bit0 (display enable)
        # so retrace-wait loops in the intro/graphics code make progress.
        if port in (0x3DA, 0x3BA):
            st["vga"] = st.get("vga", 0) + 1
            v = 0
            if st["vga"] & 1: v |= 0x08
            if st["vga"] & 2: v |= 0x01
            return v
        if port == 0x60:                 # keyboard data port
            return 0x39                  # space scancode
        return 0xFF

    if TRACE:                       # per-instruction hook is the perf bottleneck
        uc.hook_add(UC_HOOK_CODE, hook_code)
    if "--tracealloc" in av:
        uc.hook_add(UC_HOOK_CODE, hook_trace)
    if "--findjump" in av:
        uc.hook_add(UC_HOOK_CODE, hook_findjump)
    uc.hook_add(UC_HOOK_INTR, hook_intr)
    uc.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED
                | UC_HOOK_MEM_FETCH_UNMAPPED, hook_mem_invalid)

    begin = cs0 * 16 + e_ip
    print(f"start {cs0:04x}:{e_ip:04x} (lin {begin:#07x}); running EXEPACK stub...")
    # Run in slices; bump the BIOS 18.2 Hz tick (0040:006C) each slice so the
    # game's timer-polled loops (intro/menus) advance without a per-insn hook.
    SLICE = 1_000_000
    addr, done = begin, 0
    while done < NMAX and not st.get("stop"):
        st["ticks"] += 1
        uc.mem_write(0x46C, struct.pack("<I", st["ticks"]))
        # NOTE: the game hooks INT 8 (PIT) and its graphics driver waits on a
        # frame counter that handler increments (spin at mgraphic 9100:06c7,
        # `cmp [0x440],al; je`). Firing INT 8 every slice here advances the
        # frame-driven intro too fast and breaks it; pacing the timer ISR to the
        # game's real cadence is the next step. An IRET chain-stub for the old
        # vector is installed at 0060:0000 for when that's wired up.
        try:
            uc.emu_start(addr, TOTAL, count=SLICE)
        except UcError as e:
            rg = regs(uc); pc = (rg["cs"] << 4) + rg["ip"]
            print(f"\nUcError: {e} at {rg['cs']:04x}:{rg['ip']:04x} "
                  f"bytes={bytes(uc.mem_read(pc, 12)).hex(' ')}")
            print("  opens so far:", st["opens"]); return
        done += SLICE
        rcs = uc.reg_read(R["cs"]); rip = uc.reg_read(R["ip"])
        addr = (rcs << 4) + rip                                      # resume point
        sl = st.get("slice", 0); st["slice"] = sl + 1
        if sl < 40:
            print(f"  [slice {sl}] resume {rcs:04x}:{rip:04x} opens={len(st['opens'])} ovl={st['ovl']}")
        # if the resume point is stuck (same for several slices), disassemble it
        if addr == st.get("last_resume"):
            st["stuck"] = st.get("stuck", 0) + 1
            # surgical timer: the graphics driver spins on a frame counter at
            # ES:[0x440] that a timer ISR should advance (mgraphic 06c7:
            # `cmp es:[0x440],al; je`). Nothing fires the ISR headless, so when
            # stuck, bump that byte counter directly so the frame-wait exits and
            # the (frame-driven) intro advances one step.
            if st["stuck"] >= 2:
                es = uc.reg_read(R["es"])
                a = (es << 4) + 0x440
                uc.mem_write(a, bytes([(uc.mem_read(a, 1)[0] + 1) & 0xFF]))
                st["stuck"] = 0
            if st["stuck"] == 6 and "--spin" in av:
                import capstone as _c
                md = _c.Cs(_c.CS_ARCH_X86, _c.CS_MODE_16)
                code = bytes(uc.mem_read(addr & ~0xF, 0x60))
                print(f"  --- spin loop @ {rcs:04x}:{rip:04x} (lin {addr:#07x}) ---")
                for ins in md.disasm(code, (addr & ~0xF) - (rcs << 4)):
                    print(f"    {ins.address:04x}: {ins.mnemonic} {ins.op_str}")
                st["stop"] = "spin dumped"; uc.emu_stop()
        else:
            st["stuck"] = 0
        st["last_resume"] = addr
    print(f"\nstopped: {st.get('stop') or 'instruction cap'} after ~{done} insns, "
          f"{st['ovl']} INT3F overlay calls, {st['ticks']} ticks")
    print("  file opens:", st["opens"])
    print(f"  INT16 calls={st.get('i16',0)} keys-consumed={st.get('ki',0)} "
          f"video-modes={st.get('modes',[])}")
    # dump the text-mode screen (0xB8000, 80x25, char in even bytes) so we can SEE
    # which text screen the game is on and feed the right keys.
    print("  --- text screen (B8000) ---")
    vid = bytes(uc.mem_read(0xB8000, 80 * 25 * 2))
    for row in range(25):
        line = "".join(chr(vid[(row * 80 + c) * 2]) if 32 <= vid[(row * 80 + c) * 2] < 127 else " "
                        for c in range(80)).rstrip()
        if line.strip():
            print(f"  |{line}")


if __name__ == "__main__":
    main()

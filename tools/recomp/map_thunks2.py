"""
map_thunks2.py - Map overlay thunks using the 7-byte dual-mode table structure

In MSC 5.x, each thunk table entry is 7 bytes:
  Bytes 0-4: EA xx xx yy yy   (JMP FAR seg:off - for cached direct jump)
  Bytes 5-6: E8 xx             (start of CALL NEAR - for first-call dispatch)

The CALL NEAR at offset+5 extends into the next entry (3 bytes total),
followed by 4 bytes of overlay data at offset+8.

Entry base addresses are at 7-byte intervals starting from TABLE_START.

Call targets:
  - far_0000_XXXX where XXXX = entry_base -> JMP FAR entry
  - far_0000_XXXX where XXXX = entry_base+5 -> CALL NEAR entry

Part of the Civ Recomp project (sp00nznet/civ)
"""

import struct
import sys
import os
import re

LOAD_SEG = 0x0100
TABLE_START = 0x0761  # First thunk entry in the decompressed image

# Overlay module file offsets
OVERLAY_FILE_OFFSETS = {
    1:  0x02B800, 2:  0x02BE00, 3:  0x02DC00, 4:  0x02EC00,
    5:  0x02F400, 6:  0x031600, 7:  0x034000, 8:  0x036200,
    9:  0x037E00, 10: 0x039800, 11: 0x03A200, 12: 0x03B400,
    13: 0x03D400, 14: 0x03E000, 15: 0x040000, 16: 0x040C00,
    17: 0x041400, 18: 0x042800, 19: 0x044400, 20: 0x046600,
    21: 0x048000, 22: 0x048800, 23: 0x049E00,
}


def exepack_decompress(data):
    """Decompress EXEPACK'd MZ executable. Returns decompressed image."""
    hdr_size = struct.unpack_from('<H', data, 8)[0] * 16
    exepack_cs = 0x2A10
    last_page_bytes = struct.unpack_from('<H', data, 2)[0]
    num_pages = struct.unpack_from('<H', data, 4)[0]
    exe_size = num_pages * 512
    if last_page_bytes:
        exe_size -= (512 - last_page_bytes)
    image_size = exe_size - hdr_size

    image = bytearray(0x40000)
    image[0:image_size] = data[hdr_size:hdr_size + image_size]

    hdr_off = exepack_cs * 16
    dest_len = struct.unpack_from('<H', image, hdr_off + 12)[0]
    exepack_size = struct.unpack_from('<H', image, hdr_off + 6)[0]
    exepack_block = bytes(image[hdr_off:hdr_off + exepack_size])

    src = hdr_off
    dst = dest_len * 16
    while src > 0 and image[src - 1] == 0xFF:
        src -= 1

    done = False
    while not done and src >= 3:
        src -= 1; cmd = image[src]
        src -= 1; len_hi = image[src]
        src -= 1; len_lo = image[src]
        count = (len_hi << 8) | len_lo
        if (cmd & 0xFE) == 0xB0:
            src -= 1; fill = image[src]
            for i in range(count):
                if dst == 0: break
                dst -= 1; image[dst] = fill
        elif (cmd & 0xFE) == 0xB2:
            for i in range(count):
                if src == 0 or dst == 0: break
                src -= 1; dst -= 1; image[dst] = image[src]
        if cmd & 0x01: done = True

    RELOC_FROM_HDR = 0x125
    reloc = RELOC_FROM_HDR
    for seg_idx in range(16):
        if reloc + 2 > exepack_size: break
        count = struct.unpack_from('<H', exepack_block, reloc)[0]
        reloc += 2
        for i in range(count):
            if reloc + 2 > exepack_size: break
            offset = struct.unpack_from('<H', exepack_block, reloc)[0]
            reloc += 2
            addr = seg_idx * 0x10000 + offset
            if addr + 1 < dest_len * 16:
                seg_val = struct.unpack_from('<H', image, addr)[0]
                struct.pack_into('<H', image, addr, (seg_val + LOAD_SEG) & 0xFFFF)

    return image, dest_len * 16


def get_overlay_code_start(exe_data, ovl_num):
    """Get the code start file offset for an overlay module."""
    file_off = OVERLAY_FILE_OFFSETS[ovl_num]
    hdr_paras = struct.unpack_from('<H', exe_data, file_off + 8)[0]
    return file_off + hdr_paras * 16


def get_overlay_func_name(ovl_num, rel_off, exe_data):
    """Convert overlay number + relative offset to function name."""
    code_start = get_overlay_code_start(exe_data, ovl_num)
    func_file_off = code_start + rel_off
    return f"ovl{ovl_num:02d}_0{func_file_off:05X}"


def find_overlay_functions(exe_data):
    """Find all function entry points in all overlays by scanning for prologues."""
    all_funcs = {}  # ovl_num -> list of (rel_off, func_name)
    for ovl_num, file_off in sorted(OVERLAY_FILE_OFFSETS.items()):
        hdr_paras = struct.unpack_from('<H', exe_data, file_off + 8)[0]
        hdr_size = hdr_paras * 16
        last_page = struct.unpack_from('<H', exe_data, file_off + 2)[0]
        num_pages = struct.unpack_from('<H', exe_data, file_off + 4)[0]
        exe_size = num_pages * 512
        if last_page: exe_size -= (512 - last_page)
        code_size = exe_size - hdr_size
        code_start = file_off + hdr_size
        code = exe_data[code_start:code_start + code_size]

        funcs = []
        for i in range(len(code) - 2):
            if code[i] == 0x55 and code[i+1] == 0x8B and code[i+2] == 0xEC:
                if i == 0 or code[i-1] in (0xCB, 0xC3, 0x00, 0xCC):
                    func_name = f"ovl{ovl_num:02d}_0{code_start + i:05X}"
                    funcs.append((i, func_name))
        all_funcs[ovl_num] = funcs
    return all_funcs


def collect_all_stubs(recomp_dir):
    """Get ALL far_0000_XXXX addresses from stubs and impl."""
    addrs = set()
    for fname in ['civ_stubs.c', 'civ_impl.c']:
        path = os.path.join(recomp_dir, fname)
        if not os.path.exists(path):
            continue
        with open(path, 'r') as f:
            for line in f:
                m = re.match(r'^void (far_0000_([0-9A-Fa-f]+))\(CPU', line)
                if m:
                    addrs.add(int(m.group(2), 16))
    return addrs


def find_thunk_callers(recomp_dir, thunk_name):
    """Find all files/lines where a thunk function is called."""
    callers = []
    for fname in os.listdir(recomp_dir):
        if not fname.endswith('.c') or fname == 'civ_aliases.c':
            continue
        path = os.path.join(recomp_dir, fname)
        with open(path, 'r') as f:
            for lineno, line in enumerate(f, 1):
                if thunk_name + '(cpu)' in line:
                    # Get the function context
                    callers.append((fname, lineno, line.strip()))
    return callers


def main():
    if len(sys.argv) < 2:
        print("Usage: map_thunks2.py <civ.exe> [RecompiledFuncs/]")
        sys.exit(1)

    exe_path = sys.argv[1]
    recomp_dir = sys.argv[2] if len(sys.argv) >= 3 else 'RecompiledFuncs'

    with open(exe_path, 'rb') as f:
        exe_data = f.read()

    image, image_size = exepack_decompress(exe_data)
    stub_addrs = collect_all_stubs(recomp_dir)

    # Find all overlay functions
    ovl_funcs = find_overlay_functions(exe_data)

    # Find all INT 3F calls
    int3f_by_ovl = {}
    for addr in range(image_size - 4):
        if image[addr] == 0xCD and image[addr + 1] == 0x3F:
            ovl_num = image[addr + 2]
            rel_off = struct.unpack_from('<H', image, addr + 3)[0]
            int3f_by_ovl.setdefault(ovl_num, set()).add(rel_off)

    # Enumerate the 7-byte thunk table
    # Count total entries: from TABLE_START to last known thunk
    last_thunk = max(a for a in stub_addrs if 0x0700 < a < 0x0B00)
    total_entries = (last_thunk - TABLE_START) // 7 + 1

    print(f"Thunk table: 0x{TABLE_START:04X} to 0x{last_thunk:04X}, {total_entries} entries")

    # Map entry index -> (jmp_addr, call_addr, is_jmp_called, is_call_called)
    entries = []
    for i in range(total_entries + 5):  # extra for safety
        base = TABLE_START + i * 7
        if base + 7 > image_size:
            break
        jmp_addr = base
        call_addr = base + 5
        b = bytes(image[base:base+7])

        is_jmp_ea = (b[0] == 0xEA)
        is_jmp_called = jmp_addr in stub_addrs
        is_call_called = call_addr in stub_addrs
        entries.append({
            'idx': i,
            'base': base,
            'bytes': b,
            'is_ea': is_jmp_ea,
            'jmp_called': is_jmp_called,
            'call_called': is_call_called,
        })

    # Print the table
    print(f"\n{'Entry':>5} {'Base':>6} {'Bytes':20} {'JMP':>5} {'CALL':>5}")
    print('-' * 50)
    for e in entries:
        jmp_mark = f"*{e['base']:04X}" if e['jmp_called'] else ""
        call_mark = f"*{e['base']+5:04X}" if e['call_called'] else ""
        print(f"{e['idx']:5d} 0x{e['base']:04X} {e['bytes'].hex(' ')} {jmp_mark:>5} {call_mark:>5}")

    # Now find which thunk entries map to which overlays
    # Strategy: look at callers of each thunk in the recompiled code
    # and match to overlay functions based on context
    print(f"\n{'='*70}")
    print("Thunk caller analysis")
    print(f"{'='*70}")

    thunk_to_callers = {}
    for addr in sorted(stub_addrs):
        if addr < 0x0700 or addr > 0x0B00:
            continue
        name = f"far_0000_{addr:04X}"
        callers = find_thunk_callers(recomp_dir, name)
        if callers:
            thunk_to_callers[name] = callers

    # For each thunk, show callers and try to identify the overlay function
    for name, callers in sorted(thunk_to_callers.items()):
        print(f"\n  {name}: {len(callers)} callers")
        for fname, lineno, line in callers[:5]:
            # Determine if caller is an overlay function
            print(f"    {fname}:{lineno}: {line[:80]}")

    # Build mapping by position in thunk table
    # The linker places thunks in overlay number order, then function order
    # Each overlay's thunks are consecutive in the table
    print(f"\n{'='*70}")
    print("Overlay functions (from prologue scan + INT 3F)")
    print(f"{'='*70}")

    # Combine prologue-found functions with INT 3F-found functions
    all_ovl_funcs_by_num = {}
    for ovl_num in sorted(ovl_funcs.keys()):
        found_offs = set()
        names = {}
        for rel_off, func_name in ovl_funcs[ovl_num]:
            found_offs.add(rel_off)
            names[rel_off] = func_name
        # Add INT 3F functions (might find functions without standard prologue)
        if ovl_num in int3f_by_ovl:
            for rel_off in int3f_by_ovl[ovl_num]:
                if rel_off not in found_offs:
                    func_name = get_overlay_func_name(ovl_num, rel_off, exe_data)
                    names[rel_off] = func_name
                    found_offs.add(rel_off)

        sorted_funcs = sorted(found_offs)
        all_ovl_funcs_by_num[ovl_num] = [(off, names[off]) for off in sorted_funcs]

        int3f_offs = int3f_by_ovl.get(ovl_num, set())
        thunked = [off for off in sorted_funcs if off not in int3f_offs]

        print(f"\n  OVL {ovl_num:2d}: {len(sorted_funcs)} functions, {len(int3f_offs)} via INT3F, {len(thunked)} thunk-only")
        for off in sorted_funcs:
            marker = "INT3F" if off in int3f_offs else "THUNK"
            print(f"    0x{off:04X} -> {names[off]} [{marker}]")

    # Total thunk-only functions across all overlays
    total_thunked = 0
    for ovl_num, funcs in all_ovl_funcs_by_num.items():
        int3f_offs = int3f_by_ovl.get(ovl_num, set())
        for off, name in funcs:
            if off not in int3f_offs:
                total_thunked += 1
    print(f"\nTotal thunk-only overlay functions: {total_thunked}")

    # Count EA entries in the thunk table (active thunks)
    ea_count = sum(1 for e in entries if e['is_ea'])
    called_count = sum(1 for e in entries if e['jmp_called'] or e['call_called'])
    print(f"EA entries in table: {ea_count}")
    print(f"Called entries: {called_count}")

    # The thunk table has entries for ALL overlay functions (not just thunk-only ones)
    # Let me count total overlay functions
    total_ovl_funcs = sum(len(funcs) for funcs in all_ovl_funcs_by_num.values())
    print(f"Total overlay functions: {total_ovl_funcs}")


if __name__ == '__main__':
    main()

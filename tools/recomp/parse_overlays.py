"""
parse_overlays.py - Parse overlay module headers to find function entry points

Each overlay module in CIV.EXE is a complete MZ executable appended after
the main EXEPACK-compressed image. This script parses their MZ headers
and relocation tables to find all exported function entry points.

Part of the Civ Recomp project (sp00nznet/civ)
"""

import struct
import sys

# Overlay module file offsets (from MZ header scan)
OVERLAY_FILE_OFFSETS = {
    1:  0x02B800, 2:  0x02BE00, 3:  0x02DC00, 4:  0x02EC00,
    5:  0x02F400, 6:  0x031600, 7:  0x034000, 8:  0x036200,
    9:  0x037E00, 10: 0x039800, 11: 0x03A200, 12: 0x03B400,
    13: 0x03D400, 14: 0x03E000, 15: 0x040000, 16: 0x040C00,
    17: 0x041400, 18: 0x042800, 19: 0x044400, 20: 0x046600,
    21: 0x048000, 22: 0x048800, 23: 0x049E00,
}


def parse_mz_header(data, offset):
    """Parse MZ header at given file offset."""
    if offset + 0x1C > len(data):
        return None

    sig = struct.unpack_from('<H', data, offset)[0]
    if sig != 0x5A4D:
        return None

    last_page = struct.unpack_from('<H', data, offset + 2)[0]
    num_pages = struct.unpack_from('<H', data, offset + 4)[0]
    num_relocs = struct.unpack_from('<H', data, offset + 6)[0]
    hdr_paras = struct.unpack_from('<H', data, offset + 8)[0]
    min_alloc = struct.unpack_from('<H', data, offset + 10)[0]
    max_alloc = struct.unpack_from('<H', data, offset + 12)[0]
    init_ss = struct.unpack_from('<H', data, offset + 14)[0]
    init_sp = struct.unpack_from('<H', data, offset + 16)[0]
    checksum = struct.unpack_from('<H', data, offset + 18)[0]
    init_ip = struct.unpack_from('<H', data, offset + 20)[0]
    init_cs = struct.unpack_from('<H', data, offset + 22)[0]
    reloc_off = struct.unpack_from('<H', data, offset + 24)[0]
    ovl_num = struct.unpack_from('<H', data, offset + 26)[0]

    exe_size = num_pages * 512
    if last_page:
        exe_size -= (512 - last_page)

    hdr_size = hdr_paras * 16
    code_size = exe_size - hdr_size

    return {
        'sig': sig,
        'exe_size': exe_size,
        'num_relocs': num_relocs,
        'hdr_size': hdr_size,
        'min_alloc': min_alloc,
        'max_alloc': max_alloc,
        'init_ss': init_ss,
        'init_sp': init_sp,
        'init_ip': init_ip,
        'init_cs': init_cs,
        'reloc_off': reloc_off,
        'ovl_num': ovl_num,
        'code_size': code_size,
    }


def parse_reloc_table(data, base_offset, reloc_off, num_relocs):
    """Parse MZ relocation table."""
    relocs = []
    offset = base_offset + reloc_off
    for i in range(num_relocs):
        if offset + 4 > len(data):
            break
        off = struct.unpack_from('<H', data, offset)[0]
        seg = struct.unpack_from('<H', data, offset + 2)[0]
        relocs.append((seg, off))
        offset += 4
    return relocs


def main():
    if len(sys.argv) < 2:
        print("Usage: parse_overlays.py <civ.exe>")
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    print(f"File size: {len(data)} bytes\n")

    for ovl_num in sorted(OVERLAY_FILE_OFFSETS.keys()):
        file_off = OVERLAY_FILE_OFFSETS[ovl_num]
        hdr = parse_mz_header(data, file_off)
        if not hdr:
            print(f"OVL {ovl_num:2d}: Invalid MZ header at 0x{file_off:06X}")
            continue

        code_start = file_off + hdr['hdr_size']

        print(f"OVL {ovl_num:2d} @ 0x{file_off:06X}:")
        print(f"  MZ header: exe_size={hdr['exe_size']} hdr={hdr['hdr_size']} code_size={hdr['code_size']}")
        print(f"  Entry: CS={hdr['init_cs']:04X} IP={hdr['init_ip']:04X}")
        print(f"  Relocs: {hdr['num_relocs']} at offset {hdr['reloc_off']}")
        print(f"  Overlay number in header: {hdr['ovl_num']}")

        # Parse relocation table
        relocs = parse_reloc_table(data, file_off, hdr['reloc_off'], hdr['num_relocs'])

        # Dump first 32 bytes of code
        code = data[code_start:code_start+32]
        print(f"  Code start: {code[:16].hex(' ')}")

        # Look for function prologues (55 8B EC = PUSH BP; MOV BP, SP)
        # Scan the entire overlay code for function entry points
        entry_points = []
        code_data = data[code_start:code_start + hdr['code_size']]
        for i in range(len(code_data) - 2):
            if code_data[i] == 0x55 and code_data[i+1] == 0x8B and code_data[i+2] == 0xEC:
                # Check if this looks like a function start
                # Usually preceded by CB (RETF) or 00 or end of previous function
                if i == 0 or code_data[i-1] in (0xCB, 0xC3, 0x00, 0xCC):
                    func_file_off = code_start + i
                    entry_points.append((i, func_file_off))

        print(f"  Likely function entry points ({len(entry_points)}):")
        for rel_off, file_off_func in entry_points[:20]:
            func_name = f"ovl{ovl_num:02d}_0{file_off_func:05X}"
            print(f"    off=0x{rel_off:04X} -> {func_name}")

        # Also check relocation entries for patterns
        # Group relocations by segment
        seg_groups = {}
        for seg, off in relocs:
            seg_groups.setdefault(seg, []).append(off)

        if relocs:
            print(f"  Relocation segments: {sorted(seg_groups.keys())}")
            for seg in sorted(seg_groups.keys()):
                offs = sorted(seg_groups[seg])
                print(f"    Seg {seg:04X}: {len(offs)} entries: {[f'0x{o:04X}' for o in offs[:10]]}")

        print()


if __name__ == '__main__':
    main()

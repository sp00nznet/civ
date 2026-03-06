"""
map_thunks.py - Map overlay thunk slots to overlay functions

Decompresses the EXEPACK image and reads the INT 3F data at each
far_0000_XXXX thunk address to determine which overlay function
each thunk dispatches to.

Part of the Civ Recomp project (sp00nznet/civ)
"""

import struct
import sys
import os
import re

LOAD_SEG = 0x0100

# Overlay module file offsets (from MZ header scan)
# Each overlay has its own MZ header; code_start = file_offset + header_size
OVERLAY_FILE_OFFSETS = {
    1:  0x02B800,
    2:  0x02BE00,
    3:  0x02DC00,
    4:  0x02EC00,
    5:  0x02F400,
    6:  0x031600,
    7:  0x034000,
    8:  0x036200,
    9:  0x037E00,
    10: 0x039800,
    11: 0x03A200,
    12: 0x03B400,
    13: 0x03D400,
    14: 0x03E000,
    15: 0x040000,
    16: 0x040C00,
    17: 0x041400,
    18: 0x042800,
    19: 0x044400,
    20: 0x046600,
    21: 0x048000,
    22: 0x048800,
    23: 0x049E00,
}


def get_overlay_header_size(data, file_offset):
    """Read the MZ header size for an overlay module."""
    if file_offset + 0x0A > len(data):
        return 0x200  # default
    sig = struct.unpack_from('<H', data, file_offset)[0]
    if sig != 0x5A4D:  # MZ
        return 0x200
    hdr_paras = struct.unpack_from('<H', data, file_offset + 8)[0]
    return hdr_paras * 16


def exepack_decompress(data):
    """
    Decompress EXEPACK'd MZ executable.
    Returns the decompressed image (flat, starting from LOAD_SEG*16 = offset 0).
    """
    # Read MZ header
    if data[0:2] != b'MZ':
        raise ValueError("Not an MZ executable")

    hdr_size = struct.unpack_from('<H', data, 8)[0] * 16  # 0x200

    # The resident image starts at hdr_size in the file
    # At runtime it's loaded at LOAD_SEG:0000
    # EXEPACK CS from MZ header
    exepack_cs = 0x2A10  # from MZ header field

    # Copy the loaded image (everything from hdr_size to the end of resident)
    # The resident portion ends at the overlay start
    # Actually, we need to figure out how much to load.
    # The MZ header tells us: pages * 512 - (512 - last_page_bytes)
    last_page_bytes = struct.unpack_from('<H', data, 2)[0]
    num_pages = struct.unpack_from('<H', data, 4)[0]

    exe_size = num_pages * 512
    if last_page_bytes:
        exe_size -= (512 - last_page_bytes)

    image_size = exe_size - hdr_size
    print(f"[EXEPACK] MZ: exe_size={exe_size}, hdr_size={hdr_size}, image_size={image_size}")

    # Allocate decompressed image (generous size)
    image = bytearray(0x40000)  # 256KB

    # Copy compressed image
    image[0:image_size] = data[hdr_size:hdr_size + image_size]

    # Read EXEPACK header at CS paragraph * 16
    hdr_off = exepack_cs * 16

    real_ip = struct.unpack_from('<H', image, hdr_off + 0)[0]
    real_cs = struct.unpack_from('<H', image, hdr_off + 2)[0]
    exepack_size = struct.unpack_from('<H', image, hdr_off + 6)[0]
    dest_len = struct.unpack_from('<H', image, hdr_off + 12)[0]
    signature = struct.unpack_from('<H', image, hdr_off + 14)[0]

    if signature != 0x4252:
        raise ValueError(f"Bad EXEPACK signature: 0x{signature:04X}")

    print(f"[EXEPACK] Header: dest_len=0x{dest_len:04X} ({dest_len*16} bytes) exepack_size=0x{exepack_size:04X}")
    print(f"[EXEPACK] Real entry: {real_cs:04X}:{real_ip:04X}")

    # Save EXEPACK block before decompression
    exepack_block = bytes(image[hdr_off:hdr_off + exepack_size])

    # Backward decompression
    src = hdr_off
    dst = dest_len * 16

    # Skip 0xFF padding
    while src > 0 and image[src - 1] == 0xFF:
        src -= 1

    blocks = 0
    done = False
    while not done and src >= 3:
        src -= 1; cmd = image[src]
        src -= 1; len_hi = image[src]
        src -= 1; len_lo = image[src]
        count = (len_hi << 8) | len_lo

        if (cmd & 0xFE) == 0xB0:
            # RLE fill
            if src == 0:
                raise ValueError("EXEPACK: underflow in RLE")
            src -= 1
            fill = image[src]
            for i in range(count):
                if dst == 0:
                    break
                dst -= 1
                image[dst] = fill
        elif (cmd & 0xFE) == 0xB2:
            # Literal copy
            for i in range(count):
                if src == 0 or dst == 0:
                    break
                src -= 1
                dst -= 1
                image[dst] = image[src]
        else:
            raise ValueError(f"EXEPACK: bad opcode 0x{cmd:02X} at src={src+1}")

        blocks += 1
        if cmd & 0x01:
            done = True

    print(f"[EXEPACK] Decompressed {blocks} blocks")

    # Save pre-relocation image for analysis
    pre_reloc = bytearray(image)

    # Apply relocations, tracking which addresses are relocated
    RELOC_FROM_HDR = 0x125
    reloc = RELOC_FROM_HDR
    total_relocs = 0
    reloc_addrs = set()

    for seg_idx in range(16):
        if reloc + 2 > exepack_size:
            break
        count = struct.unpack_from('<H', exepack_block, reloc)[0]
        reloc += 2
        for i in range(count):
            if reloc + 2 > exepack_size:
                break
            offset = struct.unpack_from('<H', exepack_block, reloc)[0]
            reloc += 2
            addr = seg_idx * 0x10000 + offset
            if addr + 1 < dest_len * 16:
                seg_val = struct.unpack_from('<H', image, addr)[0]
                seg_val += LOAD_SEG
                struct.pack_into('<H', image, addr, seg_val & 0xFFFF)
                total_relocs += 1
                reloc_addrs.add(addr)

    print(f"[EXEPACK] Applied {total_relocs} relocations")

    return image, pre_reloc, dest_len * 16, real_cs, real_ip, reloc_addrs


def find_all_int3f(image, image_size):
    """Find all CD 3F instructions in the decompressed image."""
    results = []
    for addr in range(image_size - 4):
        if image[addr] == 0xCD and image[addr + 1] == 0x3F:
            ovl_num = image[addr + 2]
            # Next 2 bytes: relative offset within overlay
            rel_off = struct.unpack_from('<H', image, addr + 3)[0]
            # Context bytes
            ctx = bytes(image[addr:min(addr+8, image_size)])
            results.append((addr, ovl_num, rel_off, ctx))
    return results


def get_overlay_func_name(ovl_num, rel_off, exe_data):
    """Convert overlay number + relative offset to function name."""
    if ovl_num not in OVERLAY_FILE_OFFSETS:
        return None
    file_off = OVERLAY_FILE_OFFSETS[ovl_num]
    hdr_size = get_overlay_header_size(exe_data, file_off)
    code_start = file_off + hdr_size
    func_file_off = code_start + rel_off
    return f"ovl{ovl_num:02d}_0{func_file_off:05X}"


def collect_far_stubs(recomp_dir):
    """Get all far_0000_XXXX stub names from civ_stubs.c and civ_impl.c."""
    stubs = set()
    for fname in ['civ_stubs.c', 'civ_impl.c']:
        path = os.path.join(recomp_dir, fname)
        if not os.path.exists(path):
            continue
        with open(path, 'r') as f:
            for line in f:
                m = re.match(r'^void (far_0000_[0-9A-Fa-f]+)\(CPU', line)
                if m:
                    stubs.add(m.group(1))
    return sorted(stubs)


def collect_known_overlay_funcs(recomp_dir):
    """Get all known overlay function names from civ_recomp.h."""
    funcs = {}
    path = os.path.join(recomp_dir, 'civ_recomp.h')
    if not os.path.exists(path):
        return funcs
    with open(path, 'r') as f:
        for line in f:
            m = re.match(r'^void (ovl\d+_\w+)\(CPU \*cpu\);', line)
            if m:
                name = m.group(1)
                funcs[name] = True
    return funcs


def main():
    if len(sys.argv) < 2:
        print("Usage: map_thunks.py <civ.exe> [RecompiledFuncs/]")
        sys.exit(1)

    exe_path = sys.argv[1]
    recomp_dir = sys.argv[2] if len(sys.argv) >= 3 else 'RecompiledFuncs'

    with open(exe_path, 'rb') as f:
        exe_data = f.read()

    # Decompress
    image, pre_reloc, image_size, real_cs, real_ip, reloc_addrs = exepack_decompress(exe_data)

    # Find all INT 3F calls in decompressed image
    int3f_calls = find_all_int3f(image, image_size)
    print(f"\n[INT3F] Found {len(int3f_calls)} INT 3F calls in decompressed image")

    # Get all far_0000_XXXX stubs
    stubs = collect_far_stubs(recomp_dir)
    print(f"[STUBS] Found {len(stubs)} far_0000_XXXX stubs")

    # Get known overlay functions
    known_ovl = collect_known_overlay_funcs(recomp_dir)
    print(f"[KNOWN] Found {len(known_ovl)} known overlay functions")

    # Build mapping: for each far_0000_XXXX, check if there's a CD 3F at that offset
    # The offset in far_0000_XXXX is the offset within segment 0000 (unrelocated).
    # After relocation, segment 0000 becomes segment LOAD_SEG (0x0100).
    # So the address in the decompressed image is just the offset value.
    # Wait - segment 0 unrelocated. At file level, these are at offset XXXX from
    # the start of the image (since segment 0 * 16 = 0).

    # Enumerate ALL 7-byte entries in the thunk table
    # Each entry starts with either EA (JMP FAR) or E8 (CALL NEAR)
    # The table starts at 0x0761 and entries are at 7-byte intervals
    print(f"\n{'='*70}")
    print(f"  Complete Thunk Table Enumeration")
    print(f"{'='*70}")

    # Scan the thunk table region - entries are 7 bytes each
    # Find the table boundaries by looking for EA/E8 patterns
    all_stubs = set()
    for s in stubs:
        off = int(s.split('_')[2], 16)
        all_stubs.add(off)

    # Start from 0x0761 and enumerate every 7 bytes until we stop seeing EA/E8
    table_start = 0x0761
    entry_idx = 0
    groups = []  # list of (group_entries, call_near_entry_or_none)
    current_group = []

    addr = table_start
    while addr < 0x0B00 and addr + 7 <= image_size:
        b = bytes(image[addr:addr+7])

        if b[0] == 0xEA:
            entry_type = "JMP_FAR"
            target_off = struct.unpack_from('<H', b, 1)[0]
            target_seg = struct.unpack_from('<H', b, 3)[0]
            is_stub = addr in all_stubs
            current_group.append((addr, entry_type, is_stub))
            marker = "*" if is_stub else " "
            print(f"  [{entry_idx:3d}] 0x{addr:04X}: EA {target_seg:04X}:{target_off:04X} +{b[5]:02X}{b[6]:02X} {marker}")
        elif b[0] == 0xE8:
            # CALL NEAR
            entry_type = "CALL_NEAR"
            disp = struct.unpack_from('<h', b, 1)[0]
            target = addr + 3 + disp
            data = struct.unpack_from('<I', b, 3)[0]
            is_stub = addr in all_stubs
            # Group separator
            if current_group:
                groups.append((list(current_group), (addr, is_stub)))
                current_group = []
            else:
                groups.append(([], (addr, is_stub)))
            marker = "*" if is_stub else " "
            print(f"  [{entry_idx:3d}] 0x{addr:04X}: E8->0x{target:04X} data={data:08X} {marker}")
        else:
            # Not a thunk entry - end of table
            print(f"  [{entry_idx:3d}] 0x{addr:04X}: {b.hex(' ')} (END OF TABLE)")
            break

        entry_idx += 1
        addr += 7

    if current_group:
        groups.append((list(current_group), None))

    # Summary of groups
    print(f"\n  Table groups (JMP FAR entries per group):")
    for gi, (entries, call_entry) in enumerate(groups):
        ea_count = len(entries)
        call_str = f"+ CALL 0x{call_entry[0]:04X}" if call_entry else ""
        print(f"    Group {gi}: {ea_count} JMP FAR entries {call_str}")
        for addr, etype, is_stub in entries:
            stub_str = f"far_0000_{addr:04X}" if is_stub else f"(0x{addr:04X} - NOT CALLED)"
            print(f"      {stub_str}")

    # Now match groups to overlay functions
    # The INT 3F scan gives us overlay functions sorted by overlay number
    # If groups map 1:1 to overlays by number, we can build the mapping
    print(f"\n  Overlay function counts:")
    by_ovl_unique = {}
    for addr, ovl_num, rel_off, ctx in int3f_calls:
        by_ovl_unique.setdefault(ovl_num, set()).add(rel_off)

    for ovl_num in sorted(by_ovl_unique.keys()):
        funcs = sorted(by_ovl_unique[ovl_num])
        print(f"    OVL {ovl_num:2d}: {len(funcs)} functions: {[f'0x{f:04X}' for f in funcs]}")

    print(f"\n{'='*70}")
    print(f"  Thunk Mapping: far_0000_XXXX -> INT 3F overlay calls")
    print(f"{'='*70}\n")

    mappings = {}  # far_name -> ovl_func_name

    for stub in stubs:
        # Extract offset
        off = int(stub.split('_')[2], 16)

        # Check bytes at this offset in the decompressed image
        if off + 5 > image_size:
            print(f"  {stub}: OUT OF RANGE")
            continue

        b = bytes(image[off:off+8])

        if b[0] == 0xCD and b[1] == 0x3F:
            ovl_num = b[2]
            rel_off = struct.unpack_from('<H', b, 3)[0]
            func_name = get_overlay_func_name(ovl_num, rel_off, exe_data)

            if func_name and func_name in known_ovl:
                mappings[stub] = func_name
                print(f"  {stub} -> CD 3F {ovl_num:02d} off=0x{rel_off:04X} -> {func_name} [MATCHED]")
            elif func_name:
                mappings[stub] = func_name
                print(f"  {stub} -> CD 3F {ovl_num:02d} off=0x{rel_off:04X} -> {func_name} [NOT IN HEADER]")
            else:
                print(f"  {stub} -> CD 3F {ovl_num:02d} off=0x{rel_off:04X} -> UNKNOWN OVERLAY")
        elif b[0] == 0xEA:
            # JMP FAR - already patched by overlay manager? Or uninitialized.
            target = struct.unpack_from('<HH', b, 1)
            print(f"  {stub}: JMP FAR {target[1]:04X}:{target[0]:04X} (not INT 3F)")
        else:
            print(f"  {stub}: bytes={b[:6].hex(' ')} (not INT 3F)")

    print(f"\n{'='*70}")
    print(f"  Summary: {len(mappings)} thunks mapped to overlay functions")
    print(f"{'='*70}")

    # Check for the specific bug: far_0000_07DF
    if 'far_0000_07DF' in mappings:
        print(f"\n  KEY: far_0000_07DF -> {mappings['far_0000_07DF']}")

    # Generate civ_aliases.c
    if mappings:
        alias_path = os.path.join(recomp_dir, 'civ_aliases.c')
        with open(alias_path, 'w') as f:
            f.write('/*\n * civ_aliases.c - Overlay thunk aliases\n')
            f.write(' * AUTO-GENERATED by map_thunks.py\n')
            f.write(' *\n * Maps far_0000_XXXX thunk stubs to their actual overlay functions\n')
            f.write(' * by reading INT 3F data from the EXEPACK-decompressed image.\n */\n\n')
            f.write('#include "recomp/cpu.h"\n\n')

            # Extern declarations
            targets_seen = set()
            for stub, target in sorted(mappings.items()):
                if target not in targets_seen:
                    f.write(f'extern void {target}(CPU *cpu);\n')
                    targets_seen.add(target)

            f.write('\n')

            # Wrapper functions (skip stubs that have impl in civ_impl.c)
            impl_stubs = set()
            impl_path = os.path.join(recomp_dir, 'civ_impl.c')
            if os.path.exists(impl_path):
                with open(impl_path, 'r') as rf:
                    for line in rf:
                        m = re.match(r'^void (far_0000_[0-9A-Fa-f]+)\(CPU', line)
                        if m:
                            impl_stubs.add(m.group(1))

            alias_count = 0
            for stub, target in sorted(mappings.items()):
                if stub in impl_stubs:
                    f.write(f'/* {stub} -> {target} (overridden in civ_impl.c) */\n')
                else:
                    f.write(f'void {stub}(CPU *cpu) {{ {target}(cpu); }}\n')
                    alias_count += 1

            print(f"\n  Wrote {alias_count} aliases to {alias_path}")
            print(f"  ({len(impl_stubs & set(mappings.keys()))} skipped - overridden in civ_impl.c)")

    # Search for overlay descriptor table in the data segment
    # Each overlay module starts at known file offsets. In paragraphs:
    # OVL 1: 0x02B800 -> para 0x2B80
    # These values should appear in the overlay manager's descriptor table
    print(f"\n{'='*70}")
    print(f"  Searching for overlay descriptor table")
    print(f"{'='*70}")

    # Search for paragraph values of overlay starts
    ovl_paras = {}
    for ovl_num, file_off in OVERLAY_FILE_OFFSETS.items():
        para = file_off // 16
        ovl_paras[para] = ovl_num

    # Search the decompressed image for these paragraph values
    for para, ovl_num in sorted(ovl_paras.items()):
        para_bytes = struct.pack('<H', para & 0xFFFF)
        for addr in range(image_size - 2):
            if image[addr:addr+2] == para_bytes:
                # Check context - are other overlay paras nearby?
                context = bytes(image[max(0,addr-8):min(image_size,addr+24)])
                # Check if this is in the data segment area (high addresses)
                if addr > 0x20000:  # likely in DGROUP
                    print(f"  OVL {ovl_num:2d} para 0x{para:04X} found at 0x{addr:05X}: {context.hex(' ')}")

    # Also search for overlay 1 file offset 0x02B800 as a 32-bit value
    for target in [0x02B800, 0x2B80]:
        target_bytes = struct.pack('<I', target)[:4]
        for addr in range(image_size - 4):
            if image[addr:addr+4] == target_bytes[:4]:
                if addr > 0x20000:
                    print(f"  OVL1 offset {target:#x} found at 0x{addr:05X}")

    # Search near the overlay manager code (0x0700-0x0760 area) for a table
    # The overlay manager at 0x0724 references [53B3], [53B2], [53B4], [53B6]
    # These are in the data segment. Let me dump the area around those DS offsets.
    # DS:53B0 area in the DGROUP (which starts at para 0x2A1C from start of image)
    dgroup_off = 0x2A1C * 16  # offset in decompressed image
    print(f"\n  DGROUP at image offset 0x{dgroup_off:05X}")
    print(f"  Overlay manager variables (DS:53B0 area):")
    dump_off = dgroup_off + 0x53A0
    for row in range(4):
        addr = dump_off + row * 16
        if addr + 16 <= image_size:
            hex_str = ' '.join(f'{image[addr+i]:02X}' for i in range(16))
            print(f"    DS:{0x53A0+row*16:04X} ({addr:05X}): {hex_str}")

    # Look for the overlay count or table pointer near the overlay manager
    # The __OVLTAB is usually at a known DS offset
    # Let me search DS for a sequence that looks like an overlay table
    # Each entry might be: (ovl_num:16, file_para:16, entry_off:16, thunk_off:16, ...)
    print(f"\n  Scanning DS for potential overlay table (looking for sequential overlay numbers)...")
    for ds_off in range(0, min(0x6000, image_size - dgroup_off - 20), 2):
        flat = dgroup_off + ds_off
        if flat + 20 > image_size:
            break
        # Check if we see sequential small numbers (1,2,3... or 0,1,2...)
        vals = [struct.unpack_from('<H', image, flat + i*2)[0] for i in range(10)]
        # Look for a pattern where one field increments 1,2,3,...
        for stride in range(2, 8):
            if all(vals[i*stride] == i+1 for i in range(min(5, len(vals)//stride))):
                print(f"    DS:{ds_off:04X}: stride={stride}, vals={vals[:stride*5]}")
                break

    # Also dump ALL INT 3F calls grouped by overlay
    print(f"\n{'='*70}")
    print(f"  All INT 3F calls by overlay")
    print(f"{'='*70}")
    by_ovl = {}
    for addr, ovl_num, rel_off, ctx in int3f_calls:
        by_ovl.setdefault(ovl_num, []).append((addr, rel_off))

    for ovl_num in sorted(by_ovl.keys()):
        calls = by_ovl[ovl_num]
        unique_funcs = sorted(set(rel_off for _, rel_off in calls))
        print(f"\n  Overlay {ovl_num:2d}: {len(calls)} calls to {len(unique_funcs)} unique functions")
        for rel_off in unique_funcs:
            func_name = get_overlay_func_name(ovl_num, rel_off, exe_data)
            status = "OK" if func_name and func_name in known_ovl else "MISSING"
            print(f"    off=0x{rel_off:04X} -> {func_name or '?'} [{status}]")


if __name__ == '__main__':
    main()

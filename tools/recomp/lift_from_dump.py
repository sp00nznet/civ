"""
lift_from_dump.py - Lift functions from decompressed CIV.EXE memory dump

The decompressed dump is created at runtime by startup.c after EXEPACK
decompression. Functions in the compressed region of the binary can only
be accessed from this dump.

Usage:
    python lift_from_dump.py <dump_file> <output_dir> [function_specs...]

Each function_spec is: far_SSSS_OOOO  (segment:offset as in the stub name)
If no specs given, lifts all known missing stubs.
"""

import os
import sys
import struct

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode16 import Decoder, OpType
from lift import Lifter


# Functions to lift: (far_name, segment, offset)
DEFAULT_STUBS = [
    ('far_1DDE_0289', 0x1DDE, 0x0289),
    ('far_1DDE_0102', 0x1DDE, 0x0102),
    ('far_1DDE_009A', 0x1DDE, 0x009A),
    ('far_1DDE_00BA', 0x1DDE, 0x00BA),
    ('far_1DDE_0177', 0x1DDE, 0x0177),
    ('far_1DDE_0208', 0x1DDE, 0x0208),
    ('far_1DDE_0243', 0x1DDE, 0x0243),
    ('far_1DDE_02CD', 0x1DDE, 0x02CD),
    ('far_1DDE_0337', 0x1DDE, 0x0337),
    ('far_1DDE_03CE', 0x1DDE, 0x03CE),
    ('far_1DDE_047D', 0x1DDE, 0x047D),
    ('far_1DDE_04DD', 0x1DDE, 0x04DD),
    ('far_1DDE_0500', 0x1DDE, 0x0500),
    ('far_1DDE_0523', 0x1DDE, 0x0523),
    ('far_1DDE_0698', 0x1DDE, 0x0698),
    ('far_1B05_0E1D', 0x1B05, 0x0E1D),
    ('far_1B05_131A', 0x1B05, 0x131A),
    ('far_1B05_133E', 0x1B05, 0x133E),
    ('far_1B05_135D', 0x1B05, 0x135D),
    ('far_1B05_1380', 0x1B05, 0x1380),
    ('far_1B05_13BF', 0x1B05, 0x13BF),
    ('far_1B05_1406', 0x1B05, 0x1406),
    ('far_1B05_144C', 0x1B05, 0x144C),
    ('far_1B05_14D4', 0x1B05, 0x14D4),
    ('far_1B05_1505', 0x1B05, 0x1505),
    ('far_1B05_1564', 0x1B05, 0x1564),
    ('far_1B05_1579', 0x1B05, 0x1579),
    ('far_1B05_15F5', 0x1B05, 0x15F5),
    ('far_1B05_1647', 0x1B05, 0x1647),
    ('far_1B05_16E2', 0x1B05, 0x16E2),
    ('far_1B05_174E', 0x1B05, 0x174E),
    ('far_1B05_17F3', 0x1B05, 0x17F3),
    ('far_1B05_180D', 0x1B05, 0x180D),
    ('far_1B05_182A', 0x1B05, 0x182A),
    ('far_1B05_1888', 0x1B05, 0x1888),
    ('far_1B05_1931', 0x1B05, 0x1931),
    ('far_1B05_194C', 0x1B05, 0x194C),
    ('far_1B05_0006', 0x1B05, 0x0006),
    ('far_1B05_03B3', 0x1B05, 0x03B3),
    ('far_1B05_0FA7', 0x1B05, 0x0FA7),
    ('far_1B05_11C8', 0x1B05, 0x11C8),
    ('far_15D8_36C0', 0x15D8, 0x36C0),
    ('far_15D8_34EA', 0x15D8, 0x34EA),
    ('far_15D8_3474', 0x15D8, 0x3474),
    ('far_15D8_36EE', 0x15D8, 0x36EE),
    ('far_15D8_389E', 0x15D8, 0x389E),
    ('far_15D8_38F6', 0x15D8, 0x38F6),
    ('far_15D8_3953', 0x15D8, 0x3953),
    ('far_15D8_39BE', 0x15D8, 0x39BE),
    ('far_15D8_3ACA', 0x15D8, 0x3ACA),
    ('far_15D8_432E', 0x15D8, 0x432E),
    ('far_15D8_0004', 0x15D8, 0x0004),
    ('far_15D8_0C98', 0x15D8, 0x0C98),
    ('far_14F4_0737', 0x14F4, 0x0737),
    ('far_14F4_0004', 0x14F4, 0x0004),
    ('far_14F4_04A1', 0x14F4, 0x04A1),
    ('far_14F4_0A30', 0x14F4, 0x0A30),
    ('far_14F4_0AA1', 0x14F4, 0x0AA1),
    ('far_14F4_0D97', 0x14F4, 0x0D97),
    ('far_14F4_0E05', 0x14F4, 0x0E05),
    ('far_14F4_0E1C', 0x14F4, 0x0E1C),
    ('far_14F4_0E33', 0x14F4, 0x0E33),
    ('far_1F67_0088', 0x1F67, 0x0088),
    ('far_1F67_044F', 0x1F67, 0x044F),
    ('far_1F67_0000', 0x1F67, 0x0000),
    ('far_1F67_01AD', 0x1F67, 0x01AD),
    ('far_1F67_0471', 0x1F67, 0x0471),
    ('far_1436_0000', 0x1436, 0x0000),
    ('far_1436_05EE', 0x1436, 0x05EE),
    ('far_1436_0687', 0x1436, 0x0687),
    ('far_1436_06F2', 0x1436, 0x06F2),
    ('far_1436_08C6', 0x1436, 0x08C6),
    ('far_1436_0948', 0x1436, 0x0948),
    ('far_1A1A_0000', 0x1A1A, 0x0000),
    ('far_1A1A_0B58', 0x1A1A, 0x0B58),
    ('far_1A1A_0BBB', 0x1A1A, 0x0BBB),
    ('far_1A1A_0C90', 0x1A1A, 0x0C90),
    ('far_1A1A_0D3F', 0x1A1A, 0x0D3F),
    ('far_1C9E_0000', 0x1C9E, 0x0000),
    ('far_1D1F_0000', 0x1D1F, 0x0000),
    ('far_1D1F_0018', 0x1D1F, 0x0018),
    ('far_1D1F_096C', 0x1D1F, 0x096C),
    ('far_1D1F_0A05', 0x1D1F, 0x0A05),
    ('far_1D1F_0A66', 0x1D1F, 0x0A66),
    ('far_1D1F_0AC9', 0x1D1F, 0x0AC9),
    ('far_1E4B_000E', 0x1E4B, 0x000E),
    ('far_1E4B_111C', 0x1E4B, 0x111C),
    ('far_1E4B_119B', 0x1E4B, 0x119B),
    ('far_1FB6_01A0', 0x1FB6, 0x01A0),
    ('far_1FB6_021E', 0x1FB6, 0x021E),
    ('far_1FB6_0252', 0x1FB6, 0x0252),
    ('far_1FB6_026A', 0x1FB6, 0x026A),
    ('far_1FB6_0286', 0x1FB6, 0x0286),
    ('far_1FB6_04BE', 0x1FB6, 0x04BE),
    ('far_203F_000A', 0x203F, 0x000A),
    ('far_0000_076F', 0x0000, 0x076F),
    ('far_0D06_673D', 0x0D06, 0x673D),
    ('far_0D06_68CC', 0x0D06, 0x68CC),
    ('far_0D06_6AA7', 0x0D06, 0x6AA7),
    ('far_0D06_6B03', 0x0D06, 0x6B03),
    ('far_0D06_6B43', 0x0D06, 0x6B43),
    ('far_0D06_6B7E', 0x0D06, 0x6B7E),
    ('far_0D06_6C32', 0x0D06, 0x6C32),
    ('far_0D06_6C65', 0x0D06, 0x6C65),
    ('far_0D06_6C90', 0x0D06, 0x6C90),
    ('far_0D06_6CC2', 0x0D06, 0x6CC2),
    ('far_0D06_6D56', 0x0D06, 0x6D56),
    ('far_0D06_6E9D', 0x0D06, 0x6E9D),
    ('far_0D06_6F23', 0x0D06, 0x6F23),
    ('far_0D06_6F65', 0x0D06, 0x6F65),
    ('far_0D06_7017', 0x0D06, 0x7017),
    ('far_0D06_710F', 0x0D06, 0x710F),
    ('far_0D06_7249', 0x0D06, 0x7249),
    ('far_0D06_72AD', 0x0D06, 0x72AD),
    # MSC 5.x CRT functions (segment 0x205A)
    ('far_205A_00D8', 0x205A, 0x00D8),
    ('far_205A_0288', 0x205A, 0x0288),
    ('far_205A_02AE', 0x205A, 0x02AE),
    ('far_205A_0566', 0x205A, 0x0566),
    ('far_205A_059A', 0x205A, 0x059A),
    ('far_205A_0696', 0x205A, 0x0696),
    ('far_205A_06C2', 0x205A, 0x06C2),
    ('far_205A_08B4', 0x205A, 0x08B4),
    ('far_205A_08D4', 0x205A, 0x08D4),
    ('far_205A_1A62', 0x205A, 0x1A62),
    ('far_205A_1B8C', 0x205A, 0x1B8C),
    ('far_205A_1D5C', 0x205A, 0x1D5C),
    ('far_205A_1E20', 0x205A, 0x1E20),
    ('far_205A_1E92', 0x205A, 0x1E92),
    ('far_205A_1EAE', 0x205A, 0x1EAE),
    ('far_205A_1F10', 0x205A, 0x1F10),
    ('far_205A_1F68', 0x205A, 0x1F68),
    ('far_205A_1F84', 0x205A, 0x1F84),
    ('far_205A_200C', 0x205A, 0x200C),
    ('far_205A_2142', 0x205A, 0x2142),
    ('far_205A_23FA', 0x205A, 0x23FA),
    ('far_205A_25C6', 0x205A, 0x25C6),
    ('far_205A_25E4', 0x205A, 0x25E4),
    ('far_205A_2638', 0x205A, 0x2638),
    ('far_205A_28EE', 0x205A, 0x28EE),
    ('far_205A_29AA', 0x205A, 0x29AA),
    ('far_205A_2AD6', 0x205A, 0x2AD6),
    ('far_205A_2AE8', 0x205A, 0x2AE8),
    ('far_205A_2988', 0x205A, 0x2988),
    ('far_205A_3060', 0x205A, 0x3060),
    ('far_205A_3108', 0x205A, 0x3108),
    ('far_205A_311E', 0x205A, 0x311E),
    ('far_205A_312C', 0x205A, 0x312C),
    ('far_205A_3140', 0x205A, 0x3140),
    ('far_205A_32B2', 0x205A, 0x32B2),
    ('far_205A_32BE', 0x205A, 0x32BE),
    ('far_205A_32CA', 0x205A, 0x32CA),
    # CRT dependencies (second level)
    ('far_205A_0CD8', 0x205A, 0x0CD8),
    ('far_205A_0DD2', 0x205A, 0x0DD2),
    ('far_205A_0E42', 0x205A, 0x0E42),
    ('far_205A_16A0', 0x205A, 0x16A0),
    ('far_205A_1746', 0x205A, 0x1746),
    ('far_205A_1766', 0x205A, 0x1766),
    ('far_205A_1984', 0x205A, 0x1984),
    ('far_205A_1ED6', 0x205A, 0x1ED6),
    ('far_205A_27CC', 0x205A, 0x27CC),
    ('far_205A_3052', 0x205A, 0x3052),
    ('far_205A_31DC', 0x205A, 0x31DC),
    ('far_205A_3210', 0x205A, 0x3210),
    # CRT dependencies (third level)
    ('far_205A_0A1A', 0x205A, 0x0A1A),
    ('far_205A_0ADC', 0x205A, 0x0ADC),
    ('far_205A_0C3A', 0x205A, 0x0C3A),
    ('far_205A_16DA', 0x205A, 0x16DA),
    ('far_205A_17E0', 0x205A, 0x17E0),
    ('far_205A_2702', 0x205A, 0x2702),
    ('far_205A_2A32', 0x205A, 0x2A32),
    # CRT dependencies (fourth level)
    ('far_205A_1BA0', 0x205A, 0x1BA0),
    ('far_205A_1FE8', 0x205A, 0x1FE8),
    # Resident code far functions (segment 0x085F)
    ('far_085F_0EE4', 0x085F, 0x0EE4),
    ('far_085F_105D', 0x085F, 0x105D),
    ('far_085F_1225', 0x085F, 0x1225),
    ('far_085F_1354', 0x085F, 0x1354),
    ('far_085F_141F', 0x085F, 0x141F),
    ('far_085F_167D', 0x085F, 0x167D),
    ('far_085F_16F9', 0x085F, 0x16F9),
    ('far_085F_1724', 0x085F, 0x1724),
    ('far_085F_24D3', 0x085F, 0x24D3),
    ('far_085F_25BD', 0x085F, 0x25BD),
    ('far_085F_257B', 0x085F, 0x257B),
    ('far_085F_259C', 0x085F, 0x259C),
    # Resident far function (segment 0x0AD4)
    ('far_0AD4_2297', 0x0AD4, 0x2297),
]

# Near (resident) functions to lift - format: (name, flat_offset)
# These are called via NEAR CALL and return with RET (not RETF)
NEAR_STUBS = [
    ('res_01C813', 0x01C813),
    # res_01C49C conflicts with far_1B05_144C at same dump offset - use far version
    # res_01C524 conflicts with far_1B05_14D4 at same dump offset - use far version
    ('res_01C605', 0x01C605),
    ('res_01D221', 0x01D221),
    # res_01F81D conflicts with far_1F67_01AD at same dump offset - use far version
    # res_01FAE1 conflicts with far_1F67_0471 at same dump offset - use far version
    # res_01F670 conflicts with far_1F67_0000 at same dump offset - use far version
]


def find_function_end(data, start, max_scan=4096, is_far=True):
    """Find the end of a function by scanning for the epilogue pattern
    and/or the next prologue."""
    decoder = Decoder(data[start:start + max_scan], base_offset=start)
    instructions = decoder.decode_all()

    ret_mnemonic = 'retf' if is_far else 'ret'
    last_ret = start
    for i, inst in enumerate(instructions):
        # Check for RET/RETF - likely end of function
        if inst.mnemonic == ret_mnemonic or inst.mnemonic == 'retf':
            last_ret = inst.offset + inst.length
            # If the next instruction is a prologue or we're past the return,
            # this is likely the end
            if i + 1 < len(instructions):
                next_inst = instructions[i + 1]
                if (i + 2 < len(instructions) and
                    next_inst.mnemonic == 'push' and
                    next_inst.op1 and next_inst.op1.type == OpType.REG16 and
                    next_inst.op1.reg == 5):  # push bp
                    return last_ret

        # Check for next prologue (push bp / mov bp, sp)
        if (i > 5 and  # not the start
            inst.mnemonic == 'push' and inst.op1 and
            inst.op1.type == OpType.REG16 and inst.op1.reg == 5):
            if i + 1 < len(instructions):
                next_inst = instructions[i + 1]
                if (next_inst.mnemonic == 'mov' and
                    next_inst.op1 and next_inst.op1.type == OpType.REG16 and
                    next_inst.op1.reg == 5 and
                    next_inst.op2 and next_inst.op2.type == OpType.REG16 and
                    next_inst.op2.reg == 4):
                    return inst.offset

    return last_ret if last_ret > start else start + max_scan


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <dump_file> <output_dir> [far_specs...]")
        sys.exit(1)

    dump_path = sys.argv[1]
    output_dir = sys.argv[2]
    specs = sys.argv[3:] if len(sys.argv) > 3 else None

    with open(dump_path, 'rb') as f:
        dump = f.read()
    print(f"Loaded dump: {len(dump)} bytes")

    # Build combined stub list: (name, dump_offset, is_far)
    all_stubs = []
    for name, seg, off in DEFAULT_STUBS:
        all_stubs.append((name, seg * 16 + off, True))
    for name, flat_off in NEAR_STUBS:
        all_stubs.append((name, flat_off, False))

    # Select which stubs to lift
    if specs:
        stubs = []
        for spec in specs:
            for s in all_stubs:
                if s[0] == spec:
                    stubs.append(s)
                    break
            else:
                # Parse far_SSSS_OOOO format
                parts = spec.split('_')
                if len(parts) == 3 and parts[0] == 'far':
                    seg = int(parts[1], 16)
                    off = int(parts[2], 16)
                    stubs.append((spec, seg * 16 + off, True))
                elif len(parts) == 2 and parts[0] == 'res':
                    flat = int(parts[1], 16)
                    stubs.append((spec, flat, False))
    else:
        stubs = all_stubs

    # Build known functions map from dump (prologue scan)
    known_funcs = {}

    lifted_code = []
    all_calls = set()
    errors = 0
    skipped = 0

    for name, dump_off, is_far in stubs:
        if dump_off >= len(dump):
            print(f"  {name}: offset 0x{dump_off:06X} out of range, skipping")
            skipped += 1
            continue

        # Verify prologue
        if dump_off + 3 <= len(dump):
            b = dump[dump_off:dump_off+3]
            if not (b[0] == 0x55 and b[1] == 0x8B and b[2] == 0xEC):
                print(f"  {name}: no prologue at 0x{dump_off:06X} ({b.hex()}), skipping")
                skipped += 1
                continue

        # Find function end
        func_end = find_function_end(dump, dump_off, is_far=is_far)
        func_size = func_end - dump_off
        kind = "FAR" if is_far else "NEAR"
        print(f"  {name} ({kind}): 0x{dump_off:06X} - 0x{func_end:06X} ({func_size} bytes)")

        # Decode and lift
        code = dump[dump_off:func_end]
        decoder = Decoder(code, base_offset=dump_off)
        instructions = decoder.decode_range(0, len(code))

        lifter = Lifter(overlay_bases={}, hdr_size=0, known_funcs=known_funcs)
        try:
            c_code = lifter.lift_function(name, instructions, dump_off, is_far=is_far)
            lifted_code.append((name, dump_off, func_end, c_code))
            all_calls.update(lifter.func_calls)
            all_calls.update(lifter.ovl_calls)
            known_funcs[dump_off] = name
        except Exception as e:
            print(f"  Error lifting {name}: {e}")
            errors += 1

    print(f"\nLifted {len(lifted_code)} functions ({errors} errors, {skipped} skipped)")

    if not lifted_code:
        print("No functions to write.")
        return

    # Post-process: unrelocate segment references in FAR CALL TARGETS.
    # The decompressed dump has RELOCATED segment values (LOAD_SEG added).
    # Subtract LOAD_SEG (0x0100) from far_ segment names to match the
    # naming convention used by the main recomp.py lifter.
    # IMPORTANT: Do NOT modify function names (they already use correct segments).
    import re
    LOAD_SEG = 0x0100

    # Build set of function names to preserve (defined functions and res_ names)
    preserved_names = set(name for name, _, _, _ in lifted_code)
    # Also preserve any res_ names referenced in calls
    preserved_names.update(n for n in all_calls if n.startswith('res_'))

    def unrelocate_far_name(match):
        full = match.group(0)
        if full in preserved_names:
            return full  # don't modify function names
        seg = int(match.group(1), 16)
        off = match.group(2)
        new_seg = seg - LOAD_SEG
        if new_seg < 0:
            return full  # don't modify if underflow
        return f'far_{new_seg:04X}_{off}'

    fixed_code = []
    renames = set()
    for name, start, end, code in lifted_code:
        new_code = re.sub(r'far_([0-9A-Fa-f]{4})_([0-9A-Fa-f]{4})',
                          unrelocate_far_name, code)
        if new_code != code:
            old_refs = set(re.findall(r'far_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}', code))
            new_refs = set(re.findall(r'far_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}', new_code))
            renames.update(old_refs - new_refs)
        fixed_code.append((name, start, end, new_code))
    lifted_code = fixed_code

    if renames:
        print(f"  Unrelocated {len(renames)} far_ segment references (subtracted LOAD_SEG=0x{LOAD_SEG:04X})")

    # Rebuild all_calls with unrelocated names
    all_calls_fixed = set()
    for c in all_calls:
        m = re.match(r'far_([0-9A-Fa-f]{4})_([0-9A-Fa-f]{4})$', c)
        if m:
            seg = int(m.group(1), 16)
            new_seg = seg - LOAD_SEG
            if new_seg >= 0:
                all_calls_fixed.add(f'far_{new_seg:04X}_{m.group(2)}')
            else:
                all_calls_fixed.add(c)
        else:
            all_calls_fixed.add(c)
    all_calls = all_calls_fixed

    # Write output file
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, 'civ_dump_lifted.c')

    # Collect all function names for forward declarations
    all_names = set(name for name, _, _, _ in lifted_code)
    all_referenced = all_calls | all_names

    forward_decls = '\n'.join(
        f'void {n}(CPU *cpu);' for n in sorted(all_referenced)
    )

    with open(output_path, 'w') as out:
        out.write('/*\n')
        out.write(' * civ_dump_lifted.c - Functions lifted from decompressed memory dump\n')
        out.write(' *\n')
        out.write(' * AUTO-GENERATED by lift_from_dump.py - DO NOT EDIT\n')
        out.write(' * Source: civ_decompressed.bin (runtime EXEPACK dump)\n')
        out.write(' */\n\n')
        out.write('#include "recomp/cpu.h"\n\n')
        out.write('/* DOS/BIOS interrupt handlers */\n')
        out.write('extern void dos_int21(CPU *cpu);\n')
        out.write('extern void bios_int10(CPU *cpu);\n')
        out.write('extern void bios_int16(CPU *cpu);\n')
        out.write('extern void mouse_int33(CPU *cpu);\n')
        out.write('extern void int_handler(CPU *cpu, uint8_t num);\n')
        out.write('extern void port_out8(CPU *cpu, uint16_t port, uint8_t value);\n')
        out.write('extern uint8_t port_in8(CPU *cpu, uint16_t port);\n\n')
        out.write('/* Forward declarations */\n')
        out.write(forward_decls)
        out.write('\n\n')

        for name, start, end, code in lifted_code:
            out.write(f'/* Function: {name}\n')
            out.write(f' * Dump offset: 0x{start:06X} - 0x{end:06X} '
                      f'({end - start} bytes)\n')
            out.write(f' */\n')
            out.write(code)
            out.write('\n\n')

    print(f"Written to {output_path}")

    # Report unresolved references
    unresolved = all_calls - all_names
    if unresolved:
        print(f"\nUnresolved references ({len(unresolved)}):")
        for n in sorted(unresolved):
            print(f"  {n}")


if __name__ == '__main__':
    main()

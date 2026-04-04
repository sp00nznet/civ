#!/usr/bin/env python3
"""Generate stubs for dump-lifted code dependencies."""

import sys

SYMBOLS = [
    "far_0000_1041", "far_0000_1080", "far_0000_11FA",
    "far_0181_000C", "far_0181_005E", "far_0181_0088", "far_0181_00B5",
    "far_0181_00F1", "far_0181_0136", "far_0181_014A",
    "far_01A7_02A6", "far_01A7_046E",
    "far_0237_0020", "far_0237_107A",
    "far_0AD4_0194", "far_0AD4_0420", "far_0AD4_1D17", "far_0AD4_22F9",
    "far_0D06_0047",
    "ovl05_0E67", "ovl05_1AEA", "ovl05_1B96", "ovl05_1D0E",
    "ovl06_0000", "ovl06_16AC", "ovl06_16CD", "ovl06_1B33", "ovl06_251D",
    "ovl15_0000", "ovl15_08BA",
    "ovl19_0000", "ovl19_167B",
    "ovl21_0000",
    "ovl22_0967",
    "res_01392C", "res_013B63", "res_013FC5",
    "res_01494E", "res_0149E7", "res_014A52", "res_014C26",
    "res_0153E1", "res_015970", "res_0159E1", "res_015A5D", "res_015C2F",
    "res_0193F1", "res_01951C", "res_0196D3",
    "res_01AE30", "res_01AEDF",
    "res_01BFF7", "res_01C218", "res_01C29F", "res_01C38E", "res_01C5C9",
    "res_01CA8D", "res_01CBB8", "res_01CFF5", "res_01D0C4", "res_01D18F",
    "res_01D665", "res_01DBF5", "res_01DC56", "res_01DE5C", "res_01DFE8",
    "res_01E117", "res_01E478", "res_01EA96", "res_01F0CD",
    "res_01FB68", "res_01FBFC", "res_02013E", "res_020418",
    "res_02120A", "res_02178A", "res_02187C", "res_021A1C",
    "res_021B8E", "res_021BEC", "res_021F13", "res_0220AA", "res_02236A",
    "res_022BE8", "res_0230B0", "res_0230C6", "res_0230D7", "res_0230E2",
    "res_023121", "res_023189", "res_02339F", "res_0233BB", "res_0233D7",
    "res_023470",
]

def main():
    stubs_file = "RecompiledFuncs/civ_stubs.c"

    with open(stubs_file, 'r') as f:
        lines = f.readlines()

    # Find and truncate at marker
    for i, line in enumerate(lines):
        if 'Additional stubs for dump-lifted' in line:
            lines = lines[:i]
            break

    with open(stubs_file, 'w', newline='\n') as f:
        f.writelines(lines)
        f.write('\n/* --- Additional stubs for dump-lifted dependencies --- */\n')
        for sym in SYMBOLS:
            is_far = sym.startswith('far_') or sym.startswith('ovl')
            ret_size = 4 if is_far else 2
            ret_type = 'far' if is_far else 'near'
            f.write(f'\nvoid {sym}(CPU *cpu) {{\n')
            f.write(f'    (void)cpu;\n')
            f.write(f'    static int _count = 0; _count++;\n')
            f.write(f'    if (_count == 1 || (_count % 10000) == 0) fprintf(stderr, "[STUB] {sym} called (n=%d)\\n", _count);\n')
            f.write(f'    cpu->sp += {ret_size}; /* {ret_type} ret */\n')
            f.write(f'}}\n')

    print(f"Generated {len(SYMBOLS)} stubs")

if __name__ == '__main__':
    main()

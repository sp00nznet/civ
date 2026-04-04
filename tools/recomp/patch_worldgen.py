#!/usr/bin/env python3
"""Post-recomp patch for ovl07_034412 world generation function.

Fixes broken jump tables and missing gotos in the auto-generated code.
Run after recomp.py to fix the following issues:
  1. Missing ocean goto (terrain==2 handler) at offset 0x1EA
  2. Jump table 1 at CS:0x272 (8 entries: mountain terrain types)
  3. Missing phase transition goto at offset 0x455
  4. Jump table 2 at CS:0x518 (14 entries: feature terrain types)
  5. Missing feature-skip gotos at offsets 0x49F, 0x4BD, 0x51E
  6. Garbage code from jump table data misinterpreted as instructions
  7. Add #include <stdio.h> if missing
"""

import re
import sys


def patch_file(path):
    with open(path, 'r') as f:
        content = f.read()

    # Ensure stdio is included
    if '#include <stdio.h>' not in content:
        content = content.replace(
            '#include "recomp/cpu.h"',
            '#include "recomp/cpu.h"\n#include <stdio.h>'
        )

    # Fix 1: Ocean handler - replace commented jmp with actual goto
    content = content.replace(
        '    if (cc_ne(cpu)) goto L_ovl07_034412_0001EC; /* jne 0x01EC */\n'
        '    /* jmp out of function to 0x000273 */    /* jmp 0x0273 */',
        '    if (cc_ne(cpu)) goto L_ovl07_034412_0001EC; /* jne 0x01EC */\n'
        '    /* Ocean handler: terrain==2 -> write value 0xC */\n'
        '    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 (layer) */\n'
        '    push16(cpu, cpu->ax);                    /* push ax */\n'
        '    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC (ocean value) */\n'
        '    goto L_ovl07_034412_0001AB;              /* jmp 0x01AB */'
    )

    # Fix 2: Jump table 1 + garbage code removal
    # This is complex - find the xchg/indirect jmp block and replace it
    old_jt1 = (
        '    { uint16_t _t = cpu->ax; cpu->ax = (uint16_t)(cpu->bx); cpu->bx = (uint16_t)(_t); } /* xchg ax, bx */\n'
        '    /* indirect jmp via mem_read16(cpu, cpu->cs, (uint16_t)(cpu->bx + 0x272)) - needs dispatch */ /* jmp word cs:[bx+0x272] */'
    )
    if old_jt1 in content:
        # Find the end of the block to replace (up to and including the garbage code)
        # We need to find from the xchg line through to the line before L_ovl07_034412_00027D
        idx = content.index(old_jt1)
        # Find L_ovl07_034412_00027D which comes after the garbage
        end_marker = 'L_ovl07_034412_00027D:;'
        end_idx = content.index(end_marker, idx)

        replacement = (
            '    { uint16_t _t = cpu->ax; cpu->ax = (uint16_t)(cpu->bx); cpu->bx = (uint16_t)(_t); } /* xchg ax, bx */\n'
            '    /* Jump table 1 dispatch: 8 entries for mountain terrain types.\n'
            '     * Table at CS:0x272: [0xE, 0xE, 0x6, 0x6, 0x7, 0x7, 0xF, 0xF] */\n'
            '    {\n'
            '        static const uint8_t jt1[] = {0xE, 0xE, 0x6, 0x6, 0x7, 0x7, 0xF, 0xF};\n'
            '        uint16_t idx = cpu->bx / 2;\n'
            '        if (idx < 8) {\n'
            '            cpu->ax = (uint16_t)(0x1);\n'
            '            push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(jt1[idx]);\n'
            '        } else {\n'
            '            cpu->ax = (uint16_t)(0x1);\n'
            '            push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0xE); /* default */\n'
            '        }\n'
            '        goto L_ovl07_034412_0001AB;\n'
            '    }\n'
        )
        content = content[:idx] + replacement + content[end_idx:]

    # Fix 3: Phase 4->5 transition goto
    content = content.replace(
        '    /* jmp out of function to 0x000525 */    /* jmp 0x0525 */',
        '    goto L_ovl07_034412_000525;              /* jmp 0x0525 (phase 5 threshold check) */'
    )

    # Fix 4: Jump table 2 + missing gotos + garbage code
    old_jt2_start = '    /* indirect jmp via mem_read16(cpu, cpu->cs, (uint16_t)(cpu->bx + 0x518)) - needs dispatch */'
    if old_jt2_start in content:
        idx = content.index(old_jt2_start)
        # Go back to find the xchg line before it
        xchg_line = '    { uint16_t _t = cpu->ax; cpu->ax = (uint16_t)(cpu->bx); cpu->bx = (uint16_t)(_t); } /* xchg ax, bx */'
        xchg_idx = content.rindex(xchg_line, 0, idx)

        # Find the end - look for the repnz/mov ax, 0x320 line which starts the threshold check
        end_marker = "    cpu->ax = (uint16_t)(0x320);"
        end_idx = content.index(end_marker, idx)

        replacement = (
            '    { uint16_t _t = cpu->ax; cpu->ax = (uint16_t)(cpu->bx); cpu->bx = (uint16_t)(_t); } /* xchg ax, bx */\n'
            '    /* Jump table 2 dispatch: 14 entries for terrain feature placement. */\n'
            '    {\n'
            '        uint16_t idx = cpu->bx / 2;\n'
            '        switch (idx) {\n'
            '        case 0:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0x6); goto L_ovl07_034412_0004F5;\n'
            '        case 1:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0x2); goto L_ovl07_034412_0004F5;\n'
            '        case 2: case 3: case 6: case 7:\n'
            '            goto L_ovl07_034412_000522;\n'
            '        case 4: case 5:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0xC); goto L_ovl07_034412_0004F5;\n'
            '        case 8:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0xD); goto L_ovl07_034412_0004F5;\n'
            '        case 9:\n'
            '            push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xC)));\n'
            '            push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xA)));\n'
            '            push16(cpu, cpu->cs); push16(cpu, 0);\n'
            '            ovl07_0349D4(cpu);\n'
            '            cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4));\n'
            '            goto L_ovl07_034412_000522;\n'
            '        case 10: case 13:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0xB); goto L_ovl07_034412_0004F5;\n'
            '        case 11:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0xA); goto L_ovl07_034412_0004F5;\n'
            '        case 12:\n'
            '            cpu->ax = (uint16_t)(0x1); push16(cpu, cpu->ax);\n'
            '            cpu->ax = (uint16_t)(0x3); goto L_ovl07_034412_0004F5;\n'
            '        default:\n'
            '            goto L_ovl07_034412_000522;\n'
            '        }\n'
            '    }\n'
            'L_ovl07_034412_0004F5:;\n'
            '    push16(cpu, cpu->ax);                    /* push ax */\n'
            '    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xC))); /* push word ss:[bp+-0xC] */\n'
            '    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xA))); /* push word ss:[bp+-0xA] */\n'
            '    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */\n'
            '    far_1B05_17C3(cpu);                      /* call 1B05:17C3 */\n'
            '    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */\n'
            'L_ovl07_034412_000522:;\n'
            '    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xE), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xE)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0xE] */\n'
            'L_ovl07_034412_000525:;\n'
        )
        content = content[:xchg_idx] + replacement + '    ' + content[end_idx:]

    # Fix 5: Replace remaining "jmp out of function to 0x000522" comments with gotos
    content = content.replace(
        '    /* jmp out of function to 0x000522 */    /* jmp 0x0522 */',
        '    goto L_ovl07_034412_000522;              /* jmp 0x0522 */'
    )

    with open(path, 'w', newline='\n') as f:
        f.write(content)

    print(f"Patched {path}")


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'RecompiledFuncs/civ_recomp_007.c'
    patch_file(path)

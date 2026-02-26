/*
 * startup.c - MSC 5.x C Runtime Startup Replacement
 *
 * The original CIV.EXE entry point (CS:IP = 2A10:0010) is the MSC 5.x
 * C runtime startup code (crt0). It initializes the data segment, BSS,
 * stack, and performs relocation fixups before calling main().
 *
 * In our recompilation, the EXE image is loaded into flat memory and
 * the data is already in place. We just need to set up the segment
 * registers and call into the game's resident code.
 *
 * Startup parameters extracted from the MSC header at CS:0000:
 *   DS offset:    0x30C8 (relative to load segment)
 *   SS offset:    0x399B (relative to load segment)
 *   SP:           0x0800
 *   Data to copy: 5353 bytes
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include <stdio.h>

/* MSC startup fields from the CIV.EXE header */
#define CIV_DS_OFFSET  0x30C8   /* DS = LOAD_SEG + this */
#define CIV_SS_OFFSET  0x399B   /* SS = LOAD_SEG + this */
#define CIV_SP_VALUE   0x0800   /* Initial stack pointer */
#define CIV_LOAD_SEG   0x0100   /* DOS load segment */

/*
 * res_02A310 - Entry point replacement
 *
 * This replaces the MSC crt0 startup code. Instead of running the
 * original startup assembly, we directly initialize the CPU state
 * as the startup would have left it, then call the first resident
 * function (which is the MSC __astart that calls main()).
 */
void res_02A310(CPU *cpu)
{
    /* Set segment registers as the startup code would */
    cpu->ds = CIV_LOAD_SEG + CIV_DS_OFFSET;
    cpu->es = cpu->ds;
    cpu->ss = CIV_LOAD_SEG + CIV_SS_OFFSET;
    cpu->sp = CIV_SP_VALUE;

    printf("[STARTUP] DS=%04X ES=%04X SS=%04X SP=%04X\n",
           cpu->ds, cpu->es, cpu->ss, cpu->sp);

    /*
     * The MSC startup copies initialized data from the code segment
     * to the data segment. Since we loaded the entire EXE image into
     * flat memory, the data is already at the correct offsets.
     *
     * The startup then calls the game's main() through the overlay
     * manager. In our recomp, overlay calls are resolved to direct
     * C function calls, so we call the first resident function that
     * represents the game's initialization and main loop.
     *
     * res_000476 is the first detected function in the resident code
     * and represents the beginning of the actual game code after the
     * C library routines.
     *
     * TODO: Phase 4 - Identify the exact game main() function through
     * call graph analysis and connect it properly.
     */

    /* For now, call the first resident function which begins the
     * MSC library initialization chain */
    extern void res_000476(CPU *cpu);
    res_000476(cpu);
}

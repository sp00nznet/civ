/*
 * startup.c - MSC 5.x C Runtime Startup Replacement
 *
 * The original CIV.EXE entry point (CS:IP = 2A10:0010) is the MSC 5.x
 * C runtime startup code (crt0). It initializes the data segment, BSS,
 * stack, copies initialized data, then calls through __astart to main().
 *
 * Startup chain traced from the binary:
 *   1. res_02A310 (crt0) -> copies init data, RETF to __astart
 *   2. __astart (DS:0032, file 0x030EB2) -> CRT init, window setup
 *   3. ovl05_031396 -> C main() -> session setup
 *   4. ovl21_048200 -> Game main loop (unit/turn processing)
 *
 * In our recompilation, the EXE image is loaded into flat memory and
 * the data is already in place. We set up segment registers and call
 * the game's C main() directly. The __astart initialization (window
 * region setup via overlay dispatch table) is handled by init_game_data.
 *
 * Startup parameters extracted from the MSC header at CS:0000:
 *   DS offset:    0x30C8 (relative to load segment)
 *   SS offset:    0x399B (relative to load segment)
 *   SP:           0x0800
 *   Data to copy: 5353 bytes (0x14E9)
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include <stdio.h>
#include <string.h>

/* MSC startup fields from the CIV.EXE header */
#define CIV_DS_OFFSET  0x30C8   /* DS = LOAD_SEG + this */
#define CIV_SS_OFFSET  0x399B   /* SS = LOAD_SEG + this */
#define CIV_SP_VALUE   0x0800   /* Initial stack pointer */
#define CIV_LOAD_SEG   0x0100   /* DOS load segment */

/* The crt0 copies 0x14E9 bytes of initialized data from the crt0
 * segment (CS:0000) to the data segment (DS:0000). This sets up
 * initialized global variables. We replicate this by copying from
 * the source location in the loaded EXE image. */
#define CIV_CRT0_SEG    0x2A10  /* CS value of crt0 segment */
#define CIV_DATA_COPY_SIZE 0x14E9

/*
 * res_02A310 - Entry point replacement
 *
 * Replaces the MSC crt0 startup. Initializes CPU segment registers,
 * copies initialized data to the data segment (as crt0 would), then
 * calls the game's C main() function (ovl05_031396).
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
     * Replicate the crt0 data copy: copy initialized data from the
     * crt0 code segment to the data segment. The source is at
     * (LOAD_SEG + CRT0_SEG):0000 and destination is at DS:0000.
     */
    uint32_t src_flat = seg_off(CIV_LOAD_SEG + CIV_CRT0_SEG, 0);
    uint32_t dst_flat = seg_off(cpu->ds, 0);
    if (src_flat + CIV_DATA_COPY_SIZE <= MEM_SIZE &&
        dst_flat + CIV_DATA_COPY_SIZE <= MEM_SIZE) {
        memcpy(cpu->mem + dst_flat, cpu->mem + src_flat, CIV_DATA_COPY_SIZE);
        printf("[STARTUP] Copied %d bytes of initialized data\n", CIV_DATA_COPY_SIZE);
    }

    /* Clear BSS (zero out remaining data segment after initialized data) */
    uint32_t bss_start = dst_flat + CIV_DATA_COPY_SIZE;
    uint32_t bss_size = 0x2000;  /* Conservative BSS size */
    if (bss_start + bss_size <= MEM_SIZE) {
        memset(cpu->mem + bss_start, 0, bss_size);
    }

    /* Set up the stack frame as MSC expects for main() */
    cpu->bp = cpu->sp;

    printf("[STARTUP] Calling game main (ovl05_031396)...\n");

    /*
     * Call the game's C main() function directly.
     * ovl05_031396 is the game's main() - it performs session setup,
     * then calls ovl21_048200 (the actual game main loop).
     */
    extern void ovl05_031396(CPU *cpu);
    ovl05_031396(cpu);

    printf("[STARTUP] Game main returned, setting halted flag\n");
    cpu->halted = 1;
}

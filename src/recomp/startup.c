/*
 * startup.c - MSC 5.x C Runtime Startup Replacement
 *
 * CIV.EXE is an EXEPACK-compressed MZ executable. The original DOS
 * loading sequence:
 *   1. DOS loads the compressed resident image into memory
 *   2. Entry point (CS:IP = 2A10:0010) is the EXEPACK decompression stub
 *   3. EXEPACK decompresses the code image in-place (~177KB -> ~200KB)
 *   4. EXEPACK applies segment relocations and jumps to real entry point
 *   5. Real crt0 (205A:2E7D) sets DS to DGROUP, checks DOS version
 *   6. __astart clears BSS, sets SS=DS, calls C main
 *
 * In our recompilation, we replicate steps 3-6:
 *   - EXEPACK decompression + relocation
 *   - Set DS = LOAD_SEG + DGROUP (0x2A1C from real crt0)
 *   - Clear BSS, set up stack, call res_001A66 (C main)
 *
 * Full startup chain:
 *   1. res_02A310 (this)    -> EXEPACK decompress, init DS/BSS/stack
 *   2. res_001A66 (C main)  -> title screen, game setup, turn loop
 *     2a. res_001EE4         -> game data initialization
 *     2b. res_0022DA         -> new game/load game handler
 *     2c. ovl05_02F800       -> new game world generation + state setup
 *     2d. res_0023F0 (loop)  -> per-turn processing loop
 *
 * Memory layout within DS (DGROUP at segment 0x2B1C):
 *   0x0000 - 0x64C1: Initialized data (.data) - from EXEPACK decompression
 *   0x64C2 - 0xF7F0: BSS (.bss) - cleared to zero
 *   0xF7F0+:          Stack (grows downward from SP=0xFFEE)
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Constants ---------- */

#define CIV_LOAD_SEG   0x0100   /* Segment where DOS loads the program image */

/* EXEPACK entry point: CS from MZ header (relative to LOAD_SEG) */
#define EXEPACK_CS     0x2A10

/*
 * DGROUP segment offset - the real crt0's MOV AX, 0x2A1C instruction.
 * This is the UNRELOCATED linker value. At runtime: DS = LOAD_SEG + this.
 */
#define CIV_DGROUP     0x2A1C

/* BSS region within DGROUP (from __astart's rep stosb) */
#define CIV_BSS_START  0x64C2   /* First byte of BSS in DS */
#define CIV_BSS_END    0xF7F0   /* One past last byte of BSS in DS */

/* Stack setup (matching __astart) */
#define CIV_SP_INIT    0xFFEE   /* SP after __astart setup */

/* EXEPACK relocation table offset from header start.
 * = 16 (header) + stub bytes + "Packed file is corrupt" string */
#define EXEPACK_RELOC_FROM_HDR  0x125

/* ---------- EXEPACK Decompression ---------- */

/*
 * Decompress the EXEPACK'd resident image in-place.
 *
 * The loaded resident image in CPU memory contains:
 *   LOAD_SEG:0x00000 .. compressed code
 *   LOAD_SEG:EXEPACK_CS*16 .. EXEPACK header + stub + reloc table
 *
 * After decompression, the image expands to dest_len paragraphs.
 * Returns 0 on success, -1 on error.
 */
static int exepack_decompress(CPU *cpu)
{
    uint8_t *image = cpu->mem + seg_off(CIV_LOAD_SEG, 0);

    /* Read EXEPACK header at CS paragraph * 16 within the loaded image */
    uint32_t hdr_off = (uint32_t)EXEPACK_CS * 16;  /* offset from LOAD_SEG */

    uint16_t real_ip      = *(uint16_t *)(image + hdr_off + 0);
    uint16_t real_cs      = *(uint16_t *)(image + hdr_off + 2);
    uint16_t exepack_size = *(uint16_t *)(image + hdr_off + 6);
    uint16_t dest_len     = *(uint16_t *)(image + hdr_off + 12);
    uint16_t signature    = *(uint16_t *)(image + hdr_off + 14);

    if (signature != 0x4252) {  /* "RB" */
        fprintf(stderr, "[EXEPACK] Error: bad signature 0x%04X (expected 0x4252)\n",
                signature);
        return -1;
    }

    printf("[EXEPACK] Header: dest_len=0x%04X (%u bytes) exepack_size=0x%04X\n",
           dest_len, dest_len * 16, exepack_size);
    printf("[EXEPACK] Real entry: %04X:%04X\n", real_cs, real_ip);

    /* Save the EXEPACK block before decompression overwrites it */
    uint8_t *exepack_block = (uint8_t *)malloc(exepack_size);
    if (!exepack_block) {
        fprintf(stderr, "[EXEPACK] Error: malloc failed\n");
        return -1;
    }
    memcpy(exepack_block, image + hdr_off, exepack_size);

    /*
     * Backward decompression (matches the original EXEPACK stub):
     *   - src starts at end of compressed data, moves toward 0
     *   - dst starts at end of decompressed area, moves toward 0
     */
    uint32_t src = hdr_off;
    uint32_t dst = (uint32_t)dest_len * 16;

    /* Skip 0xFF padding at end of compressed data */
    while (src > 0 && image[src - 1] == 0xFF)
        src--;

    int blocks = 0;
    int done = 0;
    while (!done && src >= 3) {
        uint8_t cmd = image[--src];
        uint8_t len_hi = image[--src];
        uint8_t len_lo = image[--src];
        uint16_t count = ((uint16_t)len_hi << 8) | len_lo;

        switch (cmd & 0xFE) {
        case 0xB0: {
            /* RLE fill */
            if (src == 0) { free(exepack_block); return -1; }
            uint8_t fill = image[--src];
            for (uint16_t i = 0; i < count; i++) {
                if (dst == 0) break;
                image[--dst] = fill;
            }
            break;
        }
        case 0xB2:
            /* Literal copy */
            for (uint16_t i = 0; i < count; i++) {
                if (src == 0 || dst == 0) break;
                image[--dst] = image[--src];
            }
            break;
        default:
            fprintf(stderr, "[EXEPACK] Error: bad opcode 0x%02X at src=%u\n",
                    cmd, src + 1);
            free(exepack_block);
            return -1;
        }

        blocks++;
        if (cmd & 0x01) done = 1;
    }

    printf("[EXEPACK] Decompressed %d blocks, remaining prefix: %u bytes\n",
           blocks, src);

    /* Apply relocations from the saved EXEPACK block */
    uint32_t reloc = EXEPACK_RELOC_FROM_HDR;
    int total_relocs = 0;

    for (int seg_idx = 0; seg_idx < 16; seg_idx++) {
        if (reloc + 2 > exepack_size) break;
        uint16_t count = *(uint16_t *)(exepack_block + reloc);
        reloc += 2;
        for (uint16_t i = 0; i < count; i++) {
            if (reloc + 2 > exepack_size) break;
            uint16_t offset = *(uint16_t *)(exepack_block + reloc);
            reloc += 2;
            uint32_t addr = (uint32_t)seg_idx * 0x10000 + offset;
            if (addr + 1 < (uint32_t)dest_len * 16) {
                uint16_t seg_val = *(uint16_t *)(image + addr);
                seg_val += CIV_LOAD_SEG;
                *(uint16_t *)(image + addr) = seg_val;
                total_relocs++;
            }
        }
    }

    printf("[EXEPACK] Applied %d relocations (LOAD_SEG=0x%04X)\n",
           total_relocs, CIV_LOAD_SEG);

    /* Verify DGROUP from the real crt0's MOV AX instruction */
    uint32_t crt0_flat = (uint32_t)(real_cs + CIV_LOAD_SEG) * 16 + real_ip;
    uint32_t load_flat = seg_off(CIV_LOAD_SEG, 0);
    if (crt0_flat >= load_flat && crt0_flat + 10 < load_flat + dest_len * 16) {
        /* Scan first 32 bytes of real crt0 for MOV AX, imm16 (B8 xx xx) */
        uint8_t *crt0 = cpu->mem + crt0_flat;
        for (int i = 0; i < 32; i++) {
            if (crt0[i] == 0xB8) {
                uint16_t dgroup = *(uint16_t *)(crt0 + i + 1);
                printf("[EXEPACK] crt0 DGROUP = 0x%04X (expected 0x%04X)\n",
                       dgroup, CIV_LOAD_SEG + CIV_DGROUP);
                break;
            }
        }
    }

    free(exepack_block);
    return 0;
}

/* ---------- Entry Point ---------- */

/*
 * res_02A310 - Entry point replacement
 *
 * Decompresses the EXEPACK'd resident image, initializes the data
 * segment, clears BSS, sets up the stack, then calls the game's
 * C main() function (res_001A66).
 */
void res_02A310(CPU *cpu)
{
    /*
     * Step 1: Decompress the EXEPACK'd image in-place.
     * This expands the compressed code from ~177KB to ~200KB,
     * populating the DGROUP data segment with correct initialized data.
     */
    printf("[STARTUP] EXEPACK decompression...\n");
    if (exepack_decompress(cpu) != 0) {
        fprintf(stderr, "[STARTUP] EXEPACK decompression failed!\n");
        cpu->halted = 1;
        return;
    }

    /*
     * Step 2: Set segment registers.
     * After EXEPACK decompression, DGROUP is at CIV_DGROUP paragraphs
     * from LOAD_SEG. DS = ES = SS (MSC DGROUP model).
     */
    cpu->ds = CIV_LOAD_SEG + CIV_DGROUP;  /* 0x2B1C */
    cpu->es = cpu->ds;
    cpu->ss = cpu->ds;
    cpu->sp = CIV_SP_INIT;

    printf("[STARTUP] DS=%04X ES=%04X SS=%04X SP=%04X\n",
           cpu->ds, cpu->es, cpu->ss, cpu->sp);

    /*
     * Step 3: Clear BSS - zero out DS:0x64C2 through DS:0xF7F0.
     * The initialized data (DS:0x0000 - DS:0x64C1) is already correct
     * from the EXEPACK decompression - no crt0 data copy needed.
     */
    uint32_t ds_flat = seg_off(cpu->ds, 0);
    uint32_t bss_flat = ds_flat + CIV_BSS_START;
    uint32_t bss_size = CIV_BSS_END - CIV_BSS_START;
    if (bss_flat + bss_size <= MEM_SIZE) {
        memset(cpu->mem + bss_flat, 0, bss_size);
        printf("[STARTUP] Cleared BSS: DS:%04X - DS:%04X (%u bytes)\n",
               CIV_BSS_START, CIV_BSS_END, bss_size);
    }

    /*
     * Step 4: Save CRT state variables that __astart normally stores.
     */
    mem_write16(cpu, cpu->ss, 0x5840, cpu->sp);   /* __astktop */
    mem_write16(cpu, cpu->ss, 0x583C, cpu->sp);   /* __astkbot */
    mem_write16(cpu, cpu->ss, 0x58B1, cpu->ds);   /* __aintdiv saved DS */

    /* Set up the stack frame as MSC expects for main() */
    cpu->bp = 0;

    /* Debug: dump some initialized data to verify decompression */
    printf("[STARTUP] DS:0xAA = 0x%04X (back buffer descriptor)\n",
           mem_read16(cpu, cpu->ds, 0xAA));

    /* Check a known string location for sanity */
    {
        char buf[32];
        for (int i = 0; i < 31; i++) {
            uint8_t c = mem_read8(cpu, cpu->ds, (uint16_t)(0x1A93 + i));
            buf[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            if (c == 0) { buf[i] = 0; break; }
        }
        buf[31] = 0;
        printf("[STARTUP] DS:0x1A93 string: '%s'\n", buf);
    }

    /* Scan decompressed memory for CD 3F (INT 3F overlay calls) in the code */
    {
        uint32_t base = 0x0100 * 16;  /* LOAD_SEG * 16 */
        uint32_t end = base + 0x30C8 * 16;
        printf("[THUNKS] Scanning for CD 3F (INT 3F) in resident code:\n");
        int count = 0;
        for (uint32_t addr = base; addr + 3 <= end && addr + 3 <= MEM_SIZE; addr++) {
            uint8_t *b = &cpu->mem[addr];
            if (b[0] == 0xCD && b[1] == 0x3F) {
                uint16_t off = (uint16_t)(addr - base);
                printf("[INT3F] off=0x%04X ovl=%02d flat=0x%06X ctx=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       off, b[2], addr, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
                count++;
            }
        }
        printf("[THUNKS] Found %d INT 3F calls\n", count);

        /* Also dump the overlay manager data area.
         * In MSC 5.x, the overlay manager stores its dispatch table
         * near the INT 3F handler. The INT 3F vector is at IVT 0x3F*4 = 0xFC */
        uint16_t int3f_off = cpu->mem[0xFC] | (cpu->mem[0xFD] << 8);
        uint16_t int3f_seg = cpu->mem[0xFE] | (cpu->mem[0xFF] << 8);
        printf("[INT3F] Vector: %04X:%04X\n", int3f_seg, int3f_off);
    }

    printf("[STARTUP] Calling C main (res_001A66)...\n");

    /*
     * Step 5: Call the game's C main() function.
     * Push argc, argv, envp (game doesn't use them).
     */
    push16(cpu, 0);  /* envp */
    push16(cpu, 0);  /* argv */
    push16(cpu, 0);  /* argc */

    extern void res_001A66(CPU *cpu);
    push16(cpu, 0);  /* near call return addr */
    res_001A66(cpu);

    printf("[STARTUP] C main returned, setting halted flag\n");
    cpu->halted = 1;
}

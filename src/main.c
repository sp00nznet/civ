/*
 * main.c - Civilization Recompilation Entry Point
 *
 * Initializes the CPU state, loads the game data into flat memory,
 * sets up the DOS compatibility layer and SDL2 platform, then runs
 * the recompiled game code in a frame-driven main loop.
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include "recomp/dos_compat.h"
#include "platform/sdl_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the recompiled function declarations */
#include "civ_recomp.h"

/* Entry point provided by startup.c (replaces MSC crt0) */
extern void res_02A310(CPU *cpu);

/*
 * MZ Header values for CIV.EXE (from binary analysis):
 *   Header size:      0x200 bytes (32 paragraphs)
 *   Code image size:  ~178 KB (resident)
 *   Entry point:      CS:IP = 2A10:0010 (relative to load module)
 *   Stack:            SS:SP from header (relative to load module)
 *   Overlays:         23 modules (resolved at recomp time via INT 3Fh)
 *
 * The load module starts right after the MZ header. In real DOS, it
 * loads after the PSP (256 bytes). We place it at LOAD_SEG:0000.
 */

#define LOAD_SEG        0x0100  /* Segment where DOS loads the program image */
#define MZ_HEADER_SIZE  0x200   /* Size of MZ header to skip when loading data */

/* MZ header field offsets */
#define MZ_SS           0x0E    /* Initial SS (relative to load module) */
#define MZ_SP           0x10    /* Initial SP */
#define MZ_CS           0x16    /* Initial CS (relative to load module) */
#define MZ_IP           0x14    /* Initial IP */

/* Target frame rate for the main loop */
#define TARGET_FPS      30
#define FRAME_TIME_MS   (1000 / TARGET_FPS)

static int load_exe_data(CPU *cpu, const char *exe_path)
{
    FILE *f = fopen(exe_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", exe_path);
        return -1;
    }

    /* Read MZ header to get load parameters */
    uint8_t hdr[64];
    if (fread(hdr, 1, 64, f) < 64) {
        fprintf(stderr, "Error: failed to read MZ header\n");
        fclose(f);
        return -1;
    }

    /* Verify MZ signature */
    if (hdr[0] != 'M' || hdr[1] != 'Z') {
        fprintf(stderr, "Error: not a valid MZ executable\n");
        fclose(f);
        return -1;
    }

    uint16_t hdr_paras = *(uint16_t *)(hdr + 8);
    uint32_t hdr_size = (uint32_t)hdr_paras * 16;
    uint16_t init_ss = *(uint16_t *)(hdr + MZ_SS);
    uint16_t init_sp = *(uint16_t *)(hdr + MZ_SP);
    uint16_t init_cs = *(uint16_t *)(hdr + MZ_CS);
    uint16_t init_ip = *(uint16_t *)(hdr + MZ_IP);

    /* Get total file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);

    /* Load the entire code/data image (after MZ header) into flat memory */
    uint32_t load_addr = seg_off(LOAD_SEG, 0);
    long image_size = file_size - hdr_size;

    if (load_addr + image_size > MEM_SIZE) {
        fprintf(stderr, "Error: EXE image too large (%ld bytes)\n", image_size);
        fclose(f);
        return -1;
    }

    fseek(f, hdr_size, SEEK_SET);
    size_t read = fread(cpu->mem + load_addr, 1, image_size, f);
    fclose(f);

    if ((long)read != image_size) {
        fprintf(stderr, "Error: short read (%zu of %ld)\n", read, image_size);
        return -1;
    }

    /* Set up CPU registers as DOS would after loading */
    cpu->cs = LOAD_SEG + init_cs;
    cpu->ip = init_ip;
    cpu->ss = LOAD_SEG + init_ss;
    cpu->sp = init_sp;

    /* DS and ES point to PSP (LOAD_SEG - 0x10) as DOS convention */
    cpu->ds = LOAD_SEG;
    cpu->es = LOAD_SEG;

    /* Set up a minimal PSP at LOAD_SEG - 0x10 */
    uint16_t psp_seg = LOAD_SEG - 0x10;
    /* INT 20h at PSP:0000 (program terminate) */
    mem_write8(cpu, psp_seg, 0x0000, 0xCD);  /* INT */
    mem_write8(cpu, psp_seg, 0x0001, 0x20);  /* 20h */
    /* Top of memory at PSP:0002 */
    mem_write16(cpu, psp_seg, 0x0002, 0xA000);
    /* Empty command tail at PSP:0080 */
    mem_write8(cpu, psp_seg, 0x0080, 0);
    mem_write8(cpu, psp_seg, 0x0081, 0x0D);

    printf("[MAIN] Loaded EXE: %ld bytes at %04X:0000 (flat 0x%06X)\n",
           image_size, LOAD_SEG, load_addr);
    printf("[MAIN] Entry: %04X:%04X  Stack: %04X:%04X\n",
           cpu->cs, cpu->ip, cpu->ss, cpu->sp);

    return 0;
}

int main(int argc, char *argv[])
{
    printf("============================================================\n");
    printf("  Sid Meier's Civilization - Static Recompilation\n");
    printf("  Original (c) 1991 MicroProse Software, Inc.\n");
    printf("  Recompiled for modern systems by sp00nznet\n");
    printf("============================================================\n\n");

    /* Parse arguments */
    const char *game_dir = NULL;
    const char *exe_path = NULL;
    int scale = WINDOW_SCALE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gamedir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (!exe_path) {
            exe_path = argv[i];
        }
    }

    /* Default paths */
    if (!exe_path)
        exe_path = "CIV.EXE";
    if (!game_dir)
        game_dir = ".";

    printf("[MAIN] EXE path:  %s\n", exe_path);
    printf("[MAIN] Game dir:  %s\n", game_dir);
    printf("[MAIN] Scale:     %dx\n\n", scale);

    /* Initialize CPU */
    CPU cpu;
    cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) < 0)
        return 1;

    /* Load the EXE data segment into memory */
    if (load_exe_data(&cpu, exe_path) < 0) {
        cpu_free(&cpu);
        return 1;
    }

    /* Initialize DOS compatibility layer */
    DosState dos;
    dos_init(&dos, &cpu, game_dir);

    /* Initialize SDL2 platform */
    Platform plat;
    if (platform_init(&plat, scale) < 0) {
        cpu_free(&cpu);
        return 1;
    }

    printf("[MAIN] Starting game...\n\n");

    /*
     * Main loop strategy:
     *
     * The original game is a single-threaded DOS program that runs in
     * a tight loop. In our recompilation, the game code has been
     * translated to C functions that manipulate the CPU state struct.
     *
     * We call the entry point function which runs the MSC runtime
     * startup (initializing DS, BSS, etc.) and then jumps to the
     * game's actual main(). The game code returns when it exits
     * (INT 21h/4Ch sets cpu.halted = 1).
     *
     * Between frames we poll SDL events, update the timer, and render.
     * The game's own timing loops read the timer tick count which we
     * update based on real elapsed time.
     */

    /* Run the entry point - this executes the MSC startup and game main */
    /* For now, we use a frame-based approach where we call the entry
     * point once and let it run. The game's internal loops will call
     * our INT handlers which eventually need to yield for frame updates.
     *
     * TODO: In Phase 4, implement cooperative yielding so the game
     * loop can be interleaved with SDL event processing.
     */

    /*
     * The game's entry point runs the MSC startup, then calls
     * ovl05_031396 (C main), which calls ovl21_048200 (game loop).
     * The game runs as a single blocking call - it loops internally.
     * When it exits (INT 21h/4Ch), cpu.halted is set and it returns.
     *
     * For now, call the entry point once. The game's internal loops
     * will run to completion. DOS API calls (INT 21h, etc.) handle
     * all I/O through our compatibility layer.
     *
     * TODO: Phase 4 - Add cooperative yielding so the game's internal
     * loops interleave with SDL event processing for proper rendering.
     */
    CIV_ENTRY_POINT(&cpu);

    /* If the game returns without halting, run a post-game render loop */
    while (plat.running && !cpu.halted) {
        platform_poll_events(&plat, &dos);
        platform_render(&plat, &cpu, &dos);
        platform_delay(FRAME_TIME_MS);
    }

    printf("\n[MAIN] Game ended.\n");

    /* Cleanup */
    platform_shutdown(&plat);
    cpu_free(&cpu);

    return 0;
}

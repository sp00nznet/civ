/*
 * civ_impl.c - Hand-written implementations for unresolved symbols
 *
 * These replace the auto-generated stubs in civ_stubs.c for functions
 * that need real implementations (I/O, graphics, CRT routines, etc.)
 *
 * NOT auto-generated - safe from recomp.py overwrites.
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include "recomp/dos_compat.h"
#include "hal/input.h"

#include <stdio.h>
#include <string.h>

/* Access the global DOS state for keyboard/file operations */
extern DosState *get_dos_state(CPU *cpu);

/* Forward declarations for functions we reference */
extern void res_020FA0(CPU *cpu);

/* ─── MSC CRT: getch() ─── */
/* far_205A_20AA - Read a character from keyboard without echo.
 * Blocking: pumps SDL event loop while waiting for input.
 * Extended keys (arrows, F-keys): first call returns 0,
 * second call returns the scan code. */
void far_205A_20AA(CPU *cpu)
{
    static uint8_t pending_scan = 0;
    if (pending_scan) {
        cpu->ax = (uint16_t)pending_scan;
        pending_scan = 0;
        return;
    }
    DosState *dos = get_dos_state(cpu);
    while (!keyboard_available(&dos->keyboard)) {
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
    }
    uint16_t key = keyboard_read(&dos->keyboard);
    uint8_t ascii = (uint8_t)(key & 0xFF);
    if (ascii == 0 && key != 0) {
        pending_scan = (uint8_t)(key >> 8);
        cpu->ax = 0;
    } else {
        cpu->ax = (uint16_t)ascii;
    }
    cpu->sp += 4; /* far ret */
}

/* ─── MSC CRT: kbhit() ─── */
/* far_205A_2096 - Check if a key is available in the keyboard buffer.
 * Returns AX=0x00FF if key available, AX=0x0000 if not.
 * Pumps the SDL event loop before checking. */
void far_205A_2096(CPU *cpu)
{
    DosState *dos = get_dos_state(cpu);
    if (dos->poll_events)
        dos->poll_events(dos->platform_ctx, dos, cpu);
    cpu->ax = keyboard_available(&dos->keyboard) ? 0x00FF : 0x0000;
    cpu->sp += 4; /* far ret */
}

/* ─── MSC CRT: fgetc from stdin ─── */
/* res_021BAE - Read a character from stdin file handle.
 * Extracted from the parent function res_021B74 (dead code after return).
 * This is the actual fgetc implementation from MSC 5.x CRT. */
void res_021BAE(CPU *cpu)
{
    push16(cpu, cpu->si);
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x6AB8, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x6AB8), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; }
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AA0));
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2), (uint16_t)(flags_sub16(cpu, mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; }
    if (cc_s(cpu)) goto L_res_021BAE_refill;
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ds, cpu->bx));
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, cpu->bx), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; }
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->ds, cpu->si));
    cpu->ah = (uint8_t)(flags_sub8(cpu, cpu->ah, cpu->ah));
    goto L_res_021BAE_done;
L_res_021BAE_refill:;
    push16(cpu, cpu->bx);
    push16(cpu, 0);  /* near call return addr */
    res_020FA0(cpu);
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2));
L_res_021BAE_done:;
    cpu->si = (uint16_t)(pop16(cpu));
    cpu->sp += 2; /* near ret */
    return;
}

/* res_031BAE - Alias for res_021BAE (address differs by 0x10000) */
void res_031BAE(CPU *cpu)
{
    res_021BAE(cpu);
}

/* ─── Display: end frame ─── */
/* far_01A7_0252 - Copy game's back buffer to VGA framebuffer.
 * Called at end of frame. The game draws to an internal buffer at
 * DS:[DS:0xAA], and we copy it to VGA memory at 0xA0000. */
void far_01A7_0252(CPU *cpu)
{
    uint16_t buf_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    if (buf_ptr != 0) {
        uint32_t src = seg_off(cpu->ds, buf_ptr);
        if (src + 64000 <= MEM_SIZE) {
            memcpy(&cpu->mem[0xA0000], &cpu->mem[src], 64000);
        }
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Display: begin frame ─── */
/* far_01A7_026A - Begin a new display frame (no-op). */
void far_01A7_026A(CPU *cpu)
{
    (void)cpu;
    /* No-op - frame setup handled by platform layer */
    cpu->sp += 4; /* far ret */
}

/* ─── Display: clear/setup ─── */
/* far_085F_257B - Clear or setup the display (no-op). */
void far_085F_257B(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* ─── Display: flush ─── */
/* far_085F_259C - Flush the display, copy back buffer to VGA. */
void far_085F_259C(CPU *cpu)
{
    uint16_t buf_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    if (buf_ptr != 0) {
        uint32_t src = seg_off(cpu->ds, buf_ptr);
        if (src + 64000 <= MEM_SIZE) {
            memcpy(&cpu->mem[0xA0000], &cpu->mem[src], 64000);
        }
    }
    cpu->sp += 4; /* far ret */
}

/* ─── File I/O: _access() ─── */
/* far_0000_065C - Check if a file exists (MSC _access()).
 * Returns AX=0 if file exists, AX=0xFFFF if not.
 * Reads filename pointer from stack. */
void far_0000_065C(CPU *cpu)
{
    /* Stack: [ret_addr 4 bytes] [path_off 2 bytes] [mode 2 bytes] */
    uint16_t path_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    DosState *dos = get_dos_state(cpu);

    char dos_path[260];
    int i = 0;
    while (i < 259) {
        uint8_t c = cpu->mem[seg_off(cpu->ds, (uint16_t)(path_off + i))];
        if (c == 0) break;
        dos_path[i] = (c == '\\') ? '/' : (char)c;
        i++;
    }
    dos_path[i] = 0;

    char native_path[520];
    snprintf(native_path, sizeof(native_path), "%s/%s", dos->game_dir, dos_path);

    FILE *f = fopen(native_path, "rb");
    if (f) {
        fclose(f);
        cpu->ax = 0;
    } else {
        cpu->ax = 0xFFFF;
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Graphics: fill_rect ─── */
/* far_0000_0BEC - Fill a rectangle in the VGA framebuffer.
 * Stack params: buf_offset, x1, y1, x2, y2, color */
void far_0000_0BEC(CPU *cpu)
{
    /* Stack: [ret_addr 4 bytes] [buf 2] [x1 2] [y1 2] [x2 2] [y2 2] [color 2] */
    uint16_t sp = (uint16_t)(cpu->sp + 4); /* skip far return address */
    uint16_t buf = mem_read16(cpu, cpu->ss, sp);
    int16_t x1 = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t y1 = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    int16_t x2 = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    int16_t y2 = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));
    uint8_t color = (uint8_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 10));

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > 320) x2 = 320;
    if (y2 > 200) y2 = 200;

    for (int y = y1; y < y2; y++) {
        uint32_t row_addr = seg_off(cpu->ds, (uint16_t)(buf + y * 320 + x1));
        if (row_addr + (x2 - x1) <= MEM_SIZE) {
            memset(&cpu->mem[row_addr], color, (size_t)(x2 - x1));
        }
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Signal handlers ─── */
/* res_001E52 - MSC signal handler setup (safe no-op). */
void res_001E52(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 2; /* near ret */
}

/* ─── Cursor control ─── */
/* far_0000_0838 - Cursor show/hide (no-op in graphics mode). */
void far_0000_0838(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* ─── Timer/event ─── */
/* far_0402_44E9 - Timer or event polling function (no-op). */
void far_0402_44E9(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* ─── Mega-function mid-entry ─── */
/* res_011CC6 - Mid-function entry into mega-function res_00D093 (no-op). */
void res_011CC6(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 2; /* near ret */
}

/* ─── Overlay file loader ─── */
/* res_000AFC - Overlay file loader (no-op in recompilation).
 * In the original binary, this loads overlay segments from CIV.EXE.
 * In our recompilation, all overlay code is already compiled in. */
void res_000AFC(CPU *cpu)
{
    (void)cpu;
    static int _warned = 0;
    if (!_warned) { fprintf(stderr, "[RUNTIME] res_000AFC (overlay file loader) called\n"); _warned = 1; }
    cpu->sp += 2; /* near ret */
}

/* ─── DOS EXEC ─── */
/* res_000B98 - DOS EXEC (INT 21h AH=4Bh) wrapper.
 * In the original, this loads and runs another EXE.
 * In our recompilation, this is a no-op. */
void res_000B98(CPU *cpu)
{
    (void)cpu;
    static int _warned = 0;
    if (!_warned) { fprintf(stderr, "[RUNTIME] res_000B98 (DOS EXEC) called - skipping\n"); _warned = 1; }
    cpu->sp += 2; /* near ret */
}

/* ─── MSC CRT: strcpy ─── */
/* far_205A_1E60 - Copy null-terminated string from src to dest.
 * Stack: [ret_addr 4] [dest 2] [src 2]
 * Caller cleans up 4 bytes of args (cdecl). Returns dest in AX. */
void far_205A_1E60(CPU *cpu)
{
    /* Stack: [ret_addr 4 bytes] [dest_off 2] [src_off 2] */
    uint16_t dest_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t src_off  = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));

    int i = 0;
    uint8_t c;
    do {
        c = cpu->mem[seg_off(cpu->ds, (uint16_t)(src_off + i))];
        cpu->mem[seg_off(cpu->ds, (uint16_t)(dest_off + i))] = c;
        i++;
    } while (c != 0);

    cpu->ax = dest_off;  /* Return pointer to dest */
    cpu->sp += 4; /* far ret */
}

/* ─── Read character from input ─── */
/* far_0000_09E5 - Read a single character from keyboard input.
 * FAR call, no stack params, returns character in AL.
 * Blocks until input available, pumps SDL event loop. */
void far_0000_09E5(CPU *cpu)
{
    DosState *dos = get_dos_state(cpu);
    while (!keyboard_available(&dos->keyboard)) {
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
    }
    uint16_t key = keyboard_read(&dos->keyboard);
    cpu->al = (uint8_t)(key & 0xFF);
    cpu->sp += 4; /* far ret */
}

/* ─── MSC CRT: __chkstk ─── */
/* far_205A_0264 - Stack frame allocation (MSC __chkstk).
 * Called at function entry with AX = bytes needed for local variables.
 * Subtracts AX from SP to allocate stack space.
 * Original x86: pops return addr, sub sp/ax, pushes return addr back, retf.
 * Net effect: SP = SP_before_call - AX. */
void far_205A_0264(CPU *cpu)
{
    /* SP currently = original_SP - 4 (from simulated far return addr push).
     * We need SP = original_SP - AX after returning.
     * The +4 undoes the return addr push, -AX allocates locals.
     * No additional sp += 4 needed since we already account for it. */
    cpu->sp = (uint16_t)(cpu->sp + 4 - cpu->ax);
}

/* ─── Dialog/message box ─── */
/* far_205A_20C2 - Display dialog or message box.
 * Stack: [ret_addr 4] [type 2] [text1_off 2] [text2_off 2]
 * For now, logs the call. The game should still function if we
 * just return without displaying anything. */
void far_205A_20C2(CPU *cpu)
{
    uint16_t type      = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t text1_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t text2_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));

    /* Read first string for logging */
    char buf[128];
    int i = 0;
    while (i < 127) {
        uint8_t c = cpu->mem[seg_off(cpu->ds, (uint16_t)(text1_off + i))];
        if (c == 0) break;
        buf[i++] = (char)c;
    }
    buf[i] = 0;

    fprintf(stderr, "[DIALOG] type=%u text1=\"%s\" text2_off=0x%04X\n",
            type, buf, text2_off);
    cpu->sp += 4; /* far ret */
}

/* ─── Reference counter increment ─── */
/* res_0311F0 - Increment a reference counter at a memory location.
 * NEAR call, 1 stack param (pointer/offset into DS).
 * Increments the 16-bit word at DS:[param]. */
void res_0311F0(CPU *cpu)
{
    uint16_t ptr = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 2));
    uint16_t val = mem_read16(cpu, cpu->ds, ptr);
    mem_write16(cpu, cpu->ds, ptr, (uint16_t)(val + 1));
    cpu->sp += 2; /* near ret */
}

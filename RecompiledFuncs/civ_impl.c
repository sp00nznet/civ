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
#include "hal/timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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
    fprintf(stderr, "[BLOCK] Waiting for key in far_205A_20AA (getch)\n");
    while (!keyboard_available(&dos->keyboard)) {
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
    }
    uint16_t key = keyboard_read(&dos->keyboard);
    fprintf(stderr, "[KEY] getch: 0x%04X\n", key);
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
    static uint64_t call_count = 0;
    call_count++;
    DosState *dos = get_dos_state(cpu);
    if (dos->poll_events)
        dos->poll_events(dos->platform_ctx, dos, cpu);
    cpu->ax = keyboard_available(&dos->keyboard) ? 0x00FF : 0x0000;
    /* Auto-advance: simulate keypress when no real key available.
     * This lets the game progress past "press any key" screens in testing. */
    if (cpu->ax == 0) {
        keyboard_push(&dos->keyboard, 0x1C, 0x0D); /* inject Enter key */
        cpu->ax = 0x00FF;
    }
    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "[KBHIT] #%llu result=%u\n",
                (unsigned long long)call_count, cpu->ax);
        fflush(stderr);
    }
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
    push16(cpu, cpu->cs); push16(cpu, 0);  /* far call return addr */
    res_020FA0(cpu);
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2));
L_res_021BAE_done:;
    { static int gc=0; gc++; if ((gc >= 30 && gc <= 50) || (gc >= 100 && gc <= 120)) { fprintf(stderr, "[GETC] #%d ch=%02X('%c')\n", gc, cpu->ax, (cpu->ax >= 0x20 && cpu->ax < 0x7F) ? cpu->ax : '.'); fflush(stderr); } }
    cpu->si = (uint16_t)(pop16(cpu));
    cpu->sp += 4; /* far ret - callers use push cs + call near */
    return;
}

/* res_031BAE - Alias for res_021BAE (address differs by 0x10000) */
void res_031BAE(CPU *cpu)
{
    res_021BAE(cpu);
}

/* ─── Display: end frame ─── */
/* far_01A7_0252 - Copy active back buffer page to VGA framebuffer.
 * Called at end of frame. DS:0xAA points to GFX struct.
 * GFX struct[+00] = page flag (0=direct, 1=page1, 2=page2). */
void far_01A7_0252(CPU *cpu)
{
    static int call_count = 0;
    call_count++;
    if (call_count <= 5 || (call_count % 100) == 0)
        fprintf(stderr, "[FRAME] end_frame #%d\n", call_count);

    uint16_t gfx_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    uint16_t page = mem_read16(cpu, cpu->ds, gfx_ptr); /* struct[+00] page flag */
    if (page != 0) {
        uint32_t src = gfx_page_addr(page);
        if (src + 64000 <= MEM_SIZE) {
            memcpy(&cpu->mem[0xA0000], &cpu->mem[src], 64000);
        }
    }

    /* Yield after rendering */
    DosState *dos = get_dos_state(cpu);
    if (dos->poll_events)
        dos->poll_events(dos->platform_ctx, dos, cpu);

    cpu->sp += 4; /* far ret */
}

/* ─── Display: begin frame ─── */
/* far_01A7_026A - Begin a new display frame. */
void far_01A7_026A(CPU *cpu)
{
    static int frame_count = 0;
    frame_count++;
    if (frame_count <= 5 || (frame_count % 100) == 0)
        fprintf(stderr, "[FRAME] begin_frame #%d\n", frame_count);
    cpu->sp += 4; /* far ret */
}

/* ─── Display: clear/setup ─── */
/* far_085F_257B - Clear or setup the display (no-op). */
void far_085F_257B(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* far_0000_08B8 - Begin drawing / set clip rect.
 * WRONG alias in civ_aliases.c maps this to ovl14_03E400 (full map display),
 * causing infinite recursion when called from line-drawing code (res_000FE8).
 * This is a no-arg GFX primitive, stub as no-op until VGA drawing is implemented. */
void far_0000_08B8(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* far_0000_0880 - Set draw color/style.
 * WRONG alias maps to ovl12_03B800 (928-byte map display function).
 * Called from res_000FE8 with color in ax, no pushed args. */
void far_0000_0880(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* far_0000_0879 - End drawing / restore clip.
 * WRONG alias maps to ovl11_03A76A (389-byte display function).
 * Called from res_000FE8 after line drawing, no args. */
void far_0000_0879(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* ─── GFX: char width ─── */
/* far_0000_077D - Get pixel width of a character for text rendering.
 * WRONG alias maps to ovl02_02D9D9 (385-byte complex display function).
 * Called from res_0018ED (strlen-in-pixels). Args: char code, font ptr.
 * Returns char width in AX. We return 8 (standard 8-pixel font). */
void far_0000_077D(CPU *cpu)
{
    cpu->ax = 8;  /* standard 8px char width */
    cpu->sp += 4; /* far ret */
}

/* ─── Story display bypass ─── */
/* far_1FB6_044A -> res_01FF99 - Story display (torch.pic + story.txt).
 * Bypassed: VGA not yet implemented, and overlay thunk mappings for its
 * display primitives (0856, 084F, 0848) are wrong, causing hangs. */
void far_1FB6_044A(CPU *cpu)
{
    fprintf(stderr, "[BYPASS] far_1FB6_044A (story display) skipped\n");
    fflush(stderr);
    cpu->sp += 4; /* far ret */
}

/* ─── Display: flush ─── */
/* far_085F_259C - Flush the display, copy active page to VGA.
 * DS:0xAA points to GFX struct; struct[+00] = page flag. */
void far_085F_259C(CPU *cpu)
{
    uint16_t gfx_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    uint16_t page = mem_read16(cpu, cpu->ds, gfx_ptr); /* struct[+00] page flag */
    if (page != 0) {
        uint32_t src = gfx_page_addr(page);
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
    static int call_count = 0;
    call_count++;
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
    if (call_count <= 10)
        fprintf(stderr, "[ACCESS] #%d off=%04X path='%s' result=%s\n",
                call_count, path_off, native_path, f ? "EXISTS" : "NOT_FOUND");
    cpu->sp += 4; /* far ret */
}

/* ─── Graphics page-flip system ─── */
/*
 * GFX context struct layout (at DS:0x0098, pointed to by DS:[0xAA]):
 *   [+00] page     - active drawing page (0=VGA direct, 1=page1, 2=page2)
 *   [+02] x_origin - added to x param
 *   [+04] y_origin - added to y param
 *   [+06] max_x    - clip right (319)
 *   [+08] max_y    - clip bottom (199)
 *   [+0A] draw_en  - draw enabled flag
 *   [+0C] fg_color - text foreground color
 *
 * Back buffer pages: placed above VGA region to avoid far heap conflicts.
 *   Page 0 = VGA framebuffer (0xA0000)
 *   Page 1 = 0xC0000 (768KB mark, 64KB)
 *   Page 2 = 0xD0000 (832KB mark, 64KB)
 */
#define GFX_PAGE_VGA   0xA0000
#define GFX_PAGE_1     0xC0000
#define GFX_PAGE_2     0xD0000

static uint32_t gfx_page_addr(uint16_t page)
{
    switch (page) {
    case 1:  return GFX_PAGE_1;
    case 2:  return GFX_PAGE_2;
    default: return GFX_PAGE_VGA;
    }
}

/* far_0000_0BEC - Fill a rectangle in the active drawing page.
 * Stack params (cdecl, caller cleans 12 bytes):
 *   [bp+06] gfx_ptr  - DS offset to GFX context struct
 *   [bp+08] x        - left column
 *   [bp+0A] y        - top row
 *   [bp+0C] width    - rectangle width in pixels
 *   [bp+0E] height   - rectangle height in pixels
 *   [bp+10] color    - palette index */
void far_0000_0BEC(CPU *cpu)
{
    static uint64_t call_count = 0;
    call_count++;

    /* Yield periodically so the window stays responsive */
    if (call_count % 50 == 0) {
        DosState *dos = get_dos_state(cpu);
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
        uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC;
        timer_update(&dos->timer, ms);
    }

    /* Stack: [ret_addr 4 bytes] [gfx_ptr 2] [x 2] [y 2] [w 2] [h 2] [color 2] */
    uint16_t sp = (uint16_t)(cpu->sp + 4); /* skip far return address */
    uint16_t gfx_ptr = mem_read16(cpu, cpu->ss, sp);
    int16_t x      = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t y      = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    int16_t width  = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    int16_t height = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));
    uint8_t color  = (uint8_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 10));

    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "[FILL] #%llu gfx=DS:%04X x=%d y=%d w=%d h=%d color=%d\n",
                (unsigned long long)call_count, gfx_ptr, x, y, width, height, color);
    }

    /* Early out: nothing to draw */
    if (width <= 0 || height <= 0) {
        cpu->sp += 4; /* far ret */
        return;
    }

    /* Read GFX context struct from DS */
    uint16_t page     = mem_read16(cpu, cpu->ds, gfx_ptr);       /* [+00] page flag */
    int16_t  x_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ptr + 2)); /* [+02] */
    int16_t  y_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ptr + 4)); /* [+04] */

    /* Apply origin offsets */
    x += x_origin;
    y += y_origin;

    uint32_t buf_base = gfx_page_addr(page);

    /* Clip to screen bounds */
    int16_t x1 = x, y1 = y;
    int16_t x2 = x + width, y2 = y + height;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > 320) x2 = 320;
    if (y2 > 200) y2 = 200;
    if (x1 >= x2 || y1 >= y2) {
        cpu->sp += 4; /* far ret */
        return;
    }

    /* Fill the rectangle */
    for (int row = y1; row < y2; row++) {
        uint32_t row_addr = buf_base + (uint32_t)row * 320 + (uint32_t)x1;
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

/* ─── Page/cursor control ─── */
/* far_0000_0838 - VGA page/cursor control.
 * Stack: [ret_addr 4] [page_or_flag 2]
 * In the original, this interacts with the VGA hardware (cursor, page select).
 * The game code sets struct[+00] directly, so this is a safe no-op. */
void far_0000_0838(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 4; /* far ret */
}

/* ─── Game timer: save/read ─── */
/* far_0000_0330 - Save current timer tick count (start a delay measurement).
 * Used by delay loops (res_001932): saves the current tick count so that
 * far_0000_032C can later return the elapsed ticks.
 * No stack params, no return value. */
static uint32_t delay_start_ticks = 0;
void far_0000_0330(CPU *cpu)
{
    static int call_count = 0;
    call_count++;

    DosState *dos = get_dos_state(cpu);

    /* Update timer with real wall-clock time */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC;
    timer_update(&dos->timer, ms);

    delay_start_ticks = timer_get_ticks(&dos->timer);
    if (call_count <= 5) {
        fprintf(stderr, "[TIMER] save #%d tick=%u\n", call_count, delay_start_ticks);
        fflush(stderr);
    }
    cpu->sp += 4; /* far ret */
}

/* far_0000_032C - Read elapsed ticks since last far_0000_0330 call.
 * Returns elapsed ticks in AX (at 18.2 Hz, each tick ~55ms).
 * Also pumps SDL events so the window stays responsive during delays. */
void far_0000_032C(CPU *cpu)
{
    static uint64_t call_count = 0;
    call_count++;

    DosState *dos = get_dos_state(cpu);

    /* Pump events so the window stays responsive during delay loops */
    if (dos->poll_events)
        dos->poll_events(dos->platform_ctx, dos, cpu);

    /* Update timer with real wall-clock time */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC;
    timer_update(&dos->timer, ms);

    uint32_t now = timer_get_ticks(&dos->timer);
    uint32_t elapsed = now - delay_start_ticks;
    cpu->ax = (uint16_t)(elapsed & 0xFFFF);

    if (call_count <= 5 || (call_count % 1000) == 0) {
        fprintf(stderr, "[TIMER] read #%llu elapsed=%u tick=%u\n",
                (unsigned long long)call_count, (unsigned)elapsed, now);
        fflush(stderr);
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Timer read ─── */
/* far_0000_0A40 - Read BIOS tick counter (INT 1Ah/00h equivalent).
 * Returns the 18.2 Hz tick count in AX (low 16 bits).
 * Called in tight animation/timer loops inside ovl02_02C200,
 * so we also update the timer from wall-clock time and
 * periodically pump SDL events to keep the window responsive. */
void far_0000_0A40(CPU *cpu)
{
    DosState *dos = get_dos_state(cpu);

    /* Update timer from wall-clock time */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC;
    timer_update(&dos->timer, ms);

    uint32_t ticks = timer_get_ticks(&dos->timer);
    cpu->ax = (uint16_t)(ticks & 0xFFFF);

    /* Also write to BIOS data area for consistency */
    mem_write16(cpu, 0x0040, 0x006C, (uint16_t)(ticks & 0xFFFF));
    mem_write16(cpu, 0x0040, 0x006E, (uint16_t)(ticks >> 16));

    /* Pump events periodically to keep window responsive */
    static uint32_t call_count = 0;
    call_count++;
    if ((call_count % 100) == 0 && dos->poll_events) {
        dos->poll_events(dos->platform_ctx, dos, cpu);
    }

    cpu->sp += 4; /* far ret */
}

/* ─── Timer/event yield ─── */
/* far_0402_44E9 - Cooperative yield point.
 * The game calls this ~38 times in its code, primarily after rendering
 * operations and during game logic loops. In the original DOS binary,
 * this processed events and updated timing. In our recompilation, it:
 *   1. Pumps SDL events (keyboard, mouse, window close)
 *   2. Renders the current frame
 *   3. Updates the BIOS timer tick count at 0040:006C
 * Without this, the SDL window freezes during non-I/O game logic. */
void far_0402_44E9(CPU *cpu)
{
    static uint64_t call_count = 0;
    call_count++;

    DosState *dos = get_dos_state(cpu);

    /* Pump SDL events and render the current frame */
    if (dos->poll_events)
        dos->poll_events(dos->platform_ctx, dos, cpu);

    /* Update timer with real wall-clock time */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC;
    timer_update(&dos->timer, ms);

    /* Write BIOS timer tick count to data area at 0040:006C (dword) */
    uint32_t ticks = timer_get_ticks(&dos->timer);
    mem_write16(cpu, 0x0040, 0x006C, (uint16_t)(ticks & 0xFFFF));
    mem_write16(cpu, 0x0040, 0x006E, (uint16_t)(ticks >> 16));

    /* Periodic trace for debugging */
    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "[YIELD] far_0402_44E9 #%llu tick=%u 6AC2=%04X E692=%04X\n",
                (unsigned long long)call_count, ticks,
                mem_read16(cpu, cpu->ds, 0x6AC2),
                mem_read16(cpu, cpu->ds, 0xE692));
        fflush(stderr);
    }

    cpu->sp += 4; /* far ret */
    fprintf(stderr, "[YIELD] RETURNING sp=%04X\n", cpu->sp); fflush(stderr);
}

/* ─── Traced alias wrappers ─── */
/* far_0000_0768 - Alias for ovl02_02C200 (title screen, now bypassed) */
void ovl02_02C200(CPU *cpu);  /* forward decl - defined below */
void far_0000_0768(CPU *cpu)
{
    fprintf(stderr, "[TRACE] far_0000_0768 -> ovl02_02C200 (bypassed) sp=%04X\n",
            cpu->sp);
    ovl02_02C200(cpu);
}

/* far_0000_0792 - Alias for ovl03_02DED7 (civ info screen).
 * Bypassed: VGA not yet implemented, and this screen requires working text
 * rendering with overlay thunks that may have wrong mappings. The function
 * returns AX=1 if user pressed 'c', 0 otherwise. We return 0 (skip). */
void far_0000_0792(CPU *cpu)
{
    fprintf(stderr, "[BYPASS] far_0000_0792 (civ info screen) skipped\n");
    fflush(stderr);
    cpu->ax = 0;  /* no 'c' pressed */
    cpu->sp += 4; /* far ret */
}

/* far_0000_07DF - Alias for ovl05_0307DA (was incorrectly mapped to ovl05_030C49) */
extern void ovl05_0307DA(CPU *cpu);
void far_0000_07DF(CPU *cpu)
{
    fprintf(stderr, "[TRACE] far_0000_07DF -> ovl05_0307DA ENTER sp=%04X\n",
            cpu->sp);
    fflush(stderr);
    ovl05_0307DA(cpu);
    fprintf(stderr, "[TRACE] far_0000_07DF -> ovl05_0307DA EXIT sp=%04X\n",
            cpu->sp);
    fflush(stderr);
}

/* far_0000_0761 - Alias for ovl01_02BA00 (intro function) */
void ovl01_02BA00(CPU *cpu);  /* forward decl - defined below */
void far_0000_0761(CPU *cpu)
{
    ovl01_02BA00(cpu);
}

/* ─── Mega-function mid-entry ─── */
/* res_001CC6 - VGA mode restore (mid-entry in res_001C20).
 * Checks ds:[0x1A3C] (VGA flag) and ds:[0xEE1A] (ref count).
 * In the original, restores VGA mode when ref count == 1.
 * In our recomp, SDL handles display - just decrement ref count. */
void res_001CC6(CPU *cpu)
{
    /* Match the original: if VGA flag set and ref_count == 1, decrement */
    uint16_t vga_flag = mem_read16(cpu, cpu->ds, 0x1A3C);
    uint16_t ref_count = mem_read16(cpu, cpu->ds, 0xEE1A);
    if (vga_flag != 0 && ref_count == 1) {
        /* Original would call far_0000_16FD to restore VGA mode.
         * We skip the actual mode restore (SDL handles it). */
        mem_write16(cpu, cpu->ds, 0xEE1A, (uint16_t)(ref_count - 1));
    }
    cpu->sp += 4; /* far ret */
}

/* res_001CAE - VGA mode set (mid-entry in res_001C20 at offset 0x8E).
 * Increments VGA ref count. If VGA flag set and ref_count becomes 1,
 * calls far_0000_16CD to set VGA mode 13h. */
void res_001CAE(CPU *cpu)
{
    /* inc word ds:[0xEE1A] */
    uint16_t ref_count = mem_read16(cpu, cpu->ds, 0xEE1A);
    ref_count++;
    mem_write16(cpu, cpu->ds, 0xEE1A, ref_count);

    uint16_t vga_flag = mem_read16(cpu, cpu->ds, 0x1A3C);
    fprintf(stderr, "[VGA] res_001CAE: vga_flag=%u ref_count=%u\n",
            vga_flag, ref_count);
    if (vga_flag != 0 && ref_count == 1) {
        fprintf(stderr, "[VGA] Setting mode 13h (320x200x256)\n");
        cpu->ax = 0x0013;
        extern void bios_int10(CPU *cpu);
        bios_int10(cpu);
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Keyboard input availability check ─── */
/* res_021C08 - Check if a keyboard character is available (kbhit variant).
 * Mid-function entry in res_021BD2 at offset 0x36.
 * Checks ds:[0x6AB0] (input buffer flag) and ds:[0x6AB2] (repeat counter).
 * Returns AX=1 if character available, AX=0 if not. Far return. */
void res_021C08(CPU *cpu)
{
    uint16_t buf_flag = mem_read16(cpu, cpu->ds, 0x6AB0);
    if (buf_flag == 0) {
        /* No unget buffer - character available (from DOS kbhit) */
        cpu->ax = 1;
    } else {
        /* Unget buffer active - check countdown */
        int16_t counter = (int16_t)mem_read16(cpu, cpu->ds, 0x6AB2);
        if (counter > 0) {
            mem_write16(cpu, cpu->ds, 0x6AB2, (uint16_t)(counter - 1));
            cpu->ax = 1;
        } else {
            cpu->ax = 0;
        }
    }
    /* Set flags based on AX (callers check with OR AX,AX / JE) */
    if (cpu->ax == 0)
        cpu->flags |= FLAG_ZF;
    else
        cpu->flags &= ~FLAG_ZF;
    cpu->sp += 4; /* far ret */
}

/* ─── Heap segment allocation ─── */
/* res_0222C0 - Allocate a heap segment (sbrk/near heap expansion).
 * Mid-function entry in res_022138 at offset 0x188.
 * Calls through to memory management to allocate a segment.
 * On success: DX != 0xFFFF, ZF clear.
 * On failure: DX = 0xFFFF, ZF set. */
/* Near heap break pointer - starts at BSS end */
static uint16_t heap_break = 0xF7F0;

void res_0222C0(CPU *cpu)
{
    /* The original _nheapgrow: extends the near heap by AX paragraphs.
     * Returns AX = DS offset of new memory, ZF clear on success.
     * Checks against stack pointer to prevent collision. */
    uint16_t paras = cpu->ax;
    if (paras == 0) paras = 1;
    uint16_t bytes = paras * 16;

    uint16_t new_break = heap_break + bytes;
    /* Check for stack collision: leave 64 bytes minimum for stack */
    if (new_break >= cpu->sp - 64 || new_break < heap_break) {
        fprintf(stderr, "[SBRK] FAIL: need %u bytes, break=0x%04X sp=0x%04X\n",
                bytes, heap_break, cpu->sp);
        cpu->dx = 0xFFFF; /* failure */
        cpu->flags |= FLAG_ZF;
        cpu->sp += 2; /* near ret */
        return;
    }

    uint16_t result = heap_break;
    heap_break = new_break;

    fprintf(stderr, "[SBRK] res_0222C0: %u paras (%u bytes) -> DS:0x%04X (break->0x%04X)\n",
            paras, bytes, result, heap_break);

    cpu->ax = result;
    cpu->dx = result; /* caller expects DX != 0xFFFF on success */
    cpu->flags &= ~FLAG_ZF; /* success */
    cpu->sp += 2; /* near ret */
}

/* ─── Turn year display ─── */
/* far_0237_171C - Format and append turn year to text buffer.
 * Formats DS:[0xC150] as a number, appends "BC" or "AD" to DS:0xC936.
 * This is a mid-function entry point. */
void far_0237_171C(CPU *cpu)
{
    int16_t year = (int16_t)mem_read16(cpu, cpu->ds, 0xC150);

    /* Format: number + "BC"/"AD" appended to buffer at DS:0xC936 */
    char num_buf[16];
    int abs_year = (year < 0) ? -year : year;
    int len = 0;
    /* itoa */
    if (abs_year == 0) {
        num_buf[0] = '0';
        len = 1;
    } else {
        int tmp = abs_year;
        while (tmp > 0 && len < 14) {
            num_buf[len++] = '0' + (tmp % 10);
            tmp /= 10;
        }
        /* reverse */
        for (int i = 0; i < len / 2; i++) {
            char c = num_buf[i];
            num_buf[i] = num_buf[len - 1 - i];
            num_buf[len - 1 - i] = c;
        }
    }
    num_buf[len] = 0;

    /* Find end of existing string at DS:0xC936 */
    uint16_t buf_off = 0xC936;
    int pos = 0;
    while (pos < 200 && mem_read8(cpu, cpu->ds, (uint16_t)(buf_off + pos)) != 0)
        pos++;

    /* Right-justify number to 10 chars */
    int pad = 10 - len;
    for (int i = 0; i < pad && pos < 200; i++)
        mem_write8(cpu, cpu->ds, (uint16_t)(buf_off + pos++), ' ');
    for (int i = 0; i < len && pos < 200; i++)
        mem_write8(cpu, cpu->ds, (uint16_t)(buf_off + pos++), (uint8_t)num_buf[i]);

    /* Append "BC" or "AD" */
    const char *suffix = (year < 0) ? "BC" : "AD";
    for (int i = 0; suffix[i] && pos < 200; i++)
        mem_write8(cpu, cpu->ds, (uint16_t)(buf_off + pos++), (uint8_t)suffix[i]);
    mem_write8(cpu, cpu->ds, (uint16_t)(buf_off + pos), 0);

    cpu->sp += 4; /* far ret */
}

/* ─── Heap manager mid-entries ─── */
/* res_022181 - Near heap allocation (mid-entry in res_022138 at offset 0x49).
 *
 * MSC 5.x near heap: contiguous block chain in DGROUP.
 * Layout: [hdr0][data0][hdr1][data1]...[0xFFFE sentinel]
 *
 * Header is 2 bytes. Value = user data size, with bit 0 as status:
 *   - Bit 0 = 1: AVAILABLE (freed/reusable). Size = (hdr-1) or (hdr & ~1).
 *   - Bit 0 = 0: IN USE. Size = hdr exactly.
 *   - 0xFFFE: end sentinel
 * Total block footprint = 2 (header) + (hdr & 0xFFFE) (user data).
 * Next block header at: pos + 2 + (hdr & 0xFFFE).
 *
 * _nfree ORs 0x01 into header byte to mark available.
 * Allocator writes even header value to mark in-use. */
static int heap_chain_fixed = 0;

/* Advance to next block: skip 2-byte header + user data */
#define HEAP_NEXT(pos, hdr) ((uint16_t)((pos) + 2 + ((hdr) & 0xFFFE)))
#define HEAP_UDATA(hdr)     ((uint16_t)((hdr) & 0xFFFE))

void res_022181(CPU *cpu)
{
    uint16_t requested = cpu->cx;
    if (requested == 0) requested = 2;
    /* Match MSC convention: inc cx; and cl, 0xFE */
    uint16_t udata_sz = (uint16_t)((requested + 1) & 0xFFFE);

    uint16_t head = mem_read16(cpu, cpu->ds, cpu->bx);

    /* One-time: connect initial MSC chain to bump-allocated area.
     * MSC init creates: [0x0001][0xFFFE]...(gap)...heap_break
     * We replace the sentinel with a free block spanning to heap_break. */
    if (!heap_chain_fixed && head != 0 && heap_break > head + 4) {
        uint16_t scan = head;
        for (int i = 0; i < 64; i++) {
            uint16_t hdr = mem_read16(cpu, cpu->ds, scan);
            if (hdr == 0xFFFE) {
                /* Replace sentinel with free block spanning to heap_break.
                 * Free block user data size = heap_break - (scan + 2). */
                uint16_t free_udata = heap_break - scan - 2;
                if (free_udata >= 2) {
                    mem_write16(cpu, cpu->ds, scan, (uint16_t)(free_udata | 1));
                    mem_write16(cpu, cpu->ds, heap_break, 0xFFFE);
                    fprintf(stderr, "[HEAP] Chain fix: free @0x%04X udata=%u, sentinel @0x%04X\n",
                            scan, free_udata, heap_break);
                }
                heap_chain_fixed = 1;
                break;
            }
            uint16_t ud = HEAP_UDATA(hdr);
            scan = HEAP_NEXT(scan, hdr);
            if (ud == 0 && !(hdr & 1)) { heap_chain_fixed = 1; break; }
        }
    }

    /* Walk the contiguous block chain from head */
    if (head != 0) {
        uint16_t pos = head;
        for (int i = 0; i < 512; i++) {
            uint16_t hdr = mem_read16(cpu, cpu->ds, pos);
            if (hdr == 0xFFFE) break;

            uint16_t ud = HEAP_UDATA(hdr);
            int is_avail = (hdr & 1);

            if (is_avail && ud > 0) {
                /* Coalesce with following available blocks */
                uint16_t coal_ud = ud;
                uint16_t next = HEAP_NEXT(pos, hdr);
                while (1) {
                    uint16_t nh = mem_read16(cpu, cpu->ds, next);
                    if (nh == 0xFFFE || !(nh & 1)) break;
                    uint16_t nud = HEAP_UDATA(nh);
                    coal_ud += 2 + nud; /* absorb next block's header + data */
                    next = HEAP_NEXT(next, nh);
                }
                if (coal_ud != ud) {
                    mem_write16(cpu, cpu->ds, pos, (uint16_t)(coal_ud | 1));
                    ud = coal_ud;
                }

                if (ud >= udata_sz) {
                    /* Use this block */
                    uint16_t remainder = ud - udata_sz;
                    if (remainder >= 4) {
                        /* Split: allocate udata_sz, leave remainder */
                        mem_write16(cpu, cpu->ds, pos, udata_sz); /* in-use */
                        uint16_t rem_hdr_pos = (uint16_t)(pos + 2 + udata_sz);
                        uint16_t rem_ud = remainder - 2;
                        mem_write16(cpu, cpu->ds, rem_hdr_pos, (uint16_t)(rem_ud | 1));
                    } else {
                        /* Use entire block */
                        mem_write16(cpu, cpu->ds, pos, (uint16_t)(ud & 0xFFFE));
                    }
                    uint16_t ptr = pos + 2;
                    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 2),
                                HEAP_NEXT(pos, udata_sz));
                    fprintf(stderr, "[HEAP] Reused %u bytes at DS:0x%04X\n", udata_sz, ptr);
                    cpu->ax = ptr;
                    cpu->dx = cpu->ds;
                    cpu->flags &= ~FLAG_ZF;
                    cpu->sp += 2;
                    return;
                }
            }
            pos = HEAP_NEXT(pos, hdr);
        }
    }

    /* No available block - bump allocate from heap_break */
    uint16_t block_total = 2 + udata_sz; /* header + user data */
    uint16_t alloc_end = heap_break + block_total;
    if (alloc_end >= cpu->sp - 64 || alloc_end < heap_break) {
        uint16_t need_paras = (uint16_t)((block_total + 15) / 16);
        uint16_t save_ax = cpu->ax;
        cpu->ax = need_paras;
        res_0222C0(cpu);
        cpu->sp -= 2;
        if (cpu->flags & FLAG_ZF) {
            fprintf(stderr, "[HEAP] FAIL: need %u bytes, no room\n", block_total);
            cpu->ax = 0; cpu->dx = 0; cpu->flags |= FLAG_ZF;
            uint16_t h = mem_read16(cpu, cpu->ds, cpu->bx);
            mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 2), h);
            cpu->sp += 2; return;
        }
        cpu->ax = save_ax;
        alloc_end = heap_break + block_total;
        if (alloc_end >= cpu->sp - 64 || alloc_end < heap_break) {
            fprintf(stderr, "[HEAP] FAIL: still no room after sbrk\n");
            cpu->ax = 0; cpu->dx = 0; cpu->flags |= FLAG_ZF;
            uint16_t h = mem_read16(cpu, cpu->ds, cpu->bx);
            mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 2), h);
            cpu->sp += 2; return;
        }
    }

    /* Bump: [header=udata_sz in-use][user data][sentinel] */
    uint16_t hdr_off = heap_break;
    uint16_t ptr = hdr_off + 2;
    mem_write16(cpu, cpu->ds, hdr_off, udata_sz); /* in-use (even) */
    heap_break = (uint16_t)(hdr_off + 2 + udata_sz);
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 2), heap_break);
    mem_write16(cpu, cpu->ds, heap_break, 0xFFFE);

    fprintf(stderr, "[HEAP] Allocated %u+2 bytes at DS:0x%04X (break->0x%04X)\n",
            udata_sz, ptr, heap_break);
    cpu->ax = ptr;
    cpu->dx = cpu->ds;
    cpu->flags &= ~FLAG_ZF;
    cpu->sp += 2;
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
    static int call_count = 0;
    call_count++;
    DosState *dos = get_dos_state(cpu);

    /* Show a visible prompt on screen so the user knows the game is waiting */
    char prompt[81];
    snprintf(prompt, sizeof(prompt), " Press Space to continue... (%d) ", call_count);
    uint32_t row_base = 0xB8000 + 12 * 80 * 2; /* Center of screen */
    /* Center the prompt */
    int len = (int)strlen(prompt);
    int start = (80 - len) / 2;
    for (int i = 0; i < 80; i++) {
        int pi = i - start;
        uint8_t ch = (pi >= 0 && pi < len) ? (uint8_t)prompt[pi] : ' ';
        uint8_t attr = (pi >= 0 && pi < len) ? 0x1F : 0x00; /* White on blue / black */
        cpu->mem[row_base + i * 2]     = ch;
        cpu->mem[row_base + i * 2 + 1] = attr;
    }

    fprintf(stderr, "[BLOCK] Waiting for key in far_0000_09E5 (#%d)\n", call_count);
    while (!keyboard_available(&dos->keyboard)) {
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
    }
    uint16_t key = keyboard_read(&dos->keyboard);
    cpu->al = (uint8_t)(key & 0xFF);

    /* Clear the prompt after key press */
    for (int i = 0; i < 80; i++) {
        cpu->mem[row_base + i * 2]     = 0;
        cpu->mem[row_base + i * 2 + 1] = 0;
    }

    fprintf(stderr, "[KEY] far_0000_09E5 #%d: 0x%04X (ascii='%c')\n",
            call_count, key, (cpu->al >= 32 && cpu->al < 127) ? cpu->al : '.');
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
/* far_205A_20C2 - Display dialog or message box (interactive).
 * Stack: [ret_addr 4] [type 2] [ctrl_off 2] [text_off 2]
 *
 * Control struct at DS:ctrl_off:
 *   byte 0: option count (e.g. 3)
 *   byte 1: flags
 * Result written to DS:[ctrl_off - 2] (16-bit selection index).
 *
 * When type=0x10, this is an input dialog that blocks for a keypress.
 * The caller reads the result from the 2 bytes before ctrl_off. */
void far_205A_20C2(CPU *cpu)
{
    uint16_t type     = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t ctrl_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t text_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));

    uint8_t opt_count = cpu->mem[seg_off(cpu->ds, ctrl_off)];
    uint8_t flags     = cpu->mem[seg_off(cpu->ds, (uint16_t)(ctrl_off + 1))];

    fprintf(stderr, "[DIALOG] type=%u opts=%u flags=0x%02X ctrl=0x%04X text=0x%04X\n",
            type, opt_count, flags, ctrl_off, text_off);

    if (type == 0x10 && opt_count > 0) {
        /* Selection dialog - check if we're in VGA graphics mode yet.
         * During text-mode intro (before mode 13h), auto-select option 1
         * since we can't render the menu text. Once in graphics mode,
         * show a prompt and wait for real input. */
        DosState *dos = get_dos_state(cpu);

        /* Check if there's already a key available (e.g. from KBHIT auto-inject) */
        if (keyboard_available(&dos->keyboard)) {
            uint16_t key = keyboard_read(&dos->keyboard);
            uint8_t ascii = (uint8_t)(key & 0xFF);
            uint16_t selection = 1;
            if (ascii >= '1' && ascii <= '9')
                selection = (uint16_t)(ascii - '0');
            if (selection > opt_count)
                selection = opt_count;
            mem_write16(cpu, cpu->ds, (uint16_t)(ctrl_off - 2), selection);
            fprintf(stderr, "[DIALOG] result=%u (key=0x%04X ascii='%c')\n",
                    selection, key, (ascii >= 32 && ascii < 127) ? ascii : '.');
        } else {
            /* No key available - auto-select option 1 */
            mem_write16(cpu, cpu->ds, (uint16_t)(ctrl_off - 2), 1);
            fprintf(stderr, "[DIALOG] auto-selected 1 (no key available)\n");
        }
    } else if (type == 0x10) {
        /* Display-only dialog (opts=0) - set result=0, don't block.
         * The game uses far_0000_09E5 for the actual "press key" wait.
         * The original function renders menu text that we can't reproduce yet.
         * Auto-inject a '1' key so digit-validation loops in the intro
         * don't get stuck (the game expects digit keys, not Space). */
        DosState *dos = get_dos_state(cpu);
        mem_write16(cpu, cpu->ds, (uint16_t)(ctrl_off - 2), 0);
        memset(&cpu->mem[0xB8000], 0, 80 * 25 * 2);
        mem_write8(cpu, 0x0040, 0x0050, 0);
        mem_write8(cpu, 0x0040, 0x0051, 0);

        /* Pre-load '1' key (scancode 0x02, ascii '1') into keyboard buffer
         * so the next far_0000_09E5 call reads it immediately. */
        if (!keyboard_available(&dos->keyboard)) {
            keyboard_push(&dos->keyboard, 0x02, '1');
        }
    }

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

/* ─── MSC CRT: memcpy ─── */
/* far_205A_2A06 - Copy bytes from source to destination.
 * Stack: [ret_addr 4] [dest 2] [src 2] [count 2]
 * Copies count bytes from DS:src to DS:dest. Returns dest in AX. */
void far_205A_2A06(CPU *cpu)
{
    uint16_t dest  = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t src   = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t count = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));

    uint32_t d = seg_off(cpu->ds, dest);
    uint32_t s = seg_off(cpu->ds, src);
    if (d + count <= MEM_SIZE && s + count <= MEM_SIZE) {
        memmove(&cpu->mem[d], &cpu->mem[s], count);
    }

    cpu->ax = dest;  /* Return dest pointer */
    cpu->sp += 4; /* far ret */
}

/* ─── VGA detection ─── */
/* far_0000_1630 - Detect VGA hardware.
 * Returns AX=1 if VGA present, AX=0 if not.
 * The game stores this in DS:0x1A3C as the VGA flag.
 * If this returns 0, the game skips VGA mode 13h setup entirely. */
void far_0000_1630(CPU *cpu)
{
    fprintf(stderr, "[VGA] far_0000_1630: VGA detection -> returning 1 (present)\n");
    cpu->ax = 1;  /* VGA is present */
    cpu->sp += 4; /* far ret */
}

/* ─── Intro bypass ─── */
/*
 * ovl01_02BA00 - Intro / configuration screen (overlay 1 entry point).
 *
 * The original reads intro.txt to display sound driver and video options,
 * then waits for user input to select them. The CRT buffered I/O chain
 * has broken "jmp out of function" cases (e.g., res_0224EE/itoa) that
 * prevent intro.txt from being read correctly. Rather than fix the entire
 * MSC 5.x CRT I/O chain, we bypass the intro and set config directly.
 *
 * Config variables set:
 *   DS:0x1A30 - Sound driver selection (0 = no sound)
 *   DS:0x1A3C - VGA flag (1 = VGA present, use mode 13h)
 */
void ovl01_02BA00(CPU *cpu)
{
    fprintf(stderr, "[INTRO] Bypassing intro screen (VGA=1, sound=none) DS=%04X\n",
            cpu->ds);

    /* No sound driver */
    mem_write8(cpu, cpu->ds, 0x1A30, 0);

    /* VGA present - use mode 13h (320x200x256) */
    mem_write16(cpu, cpu->ds, 0x1A3C, 1);

    /* Verify it was written */
    uint16_t check = mem_read16(cpu, cpu->ds, 0x1A3C);
    fprintf(stderr, "[INTRO] DS:0x1A3C = %u (expected 1)\n", check);

    cpu->sp += 4; /* far ret */
}

/* ─── CRT itoa replacement ─── */
/*
 * res_0224EE - MSC 5.x _itoa() implementation.
 *
 * The original code sets up parameters (value, buffer, radix) in registers
 * then jumps to shared number-to-string conversion code at offset 0x0B04
 * within the code segment. The lifter can't follow cross-function jumps,
 * so the generated code has "jmp out of function to 0x000B04" which falls
 * through into garbage instructions. This hand-written replacement provides
 * the standard itoa behavior.
 *
 * Far call: itoa(int value, char *buf, int radix)
 *   [bp+6]  = value (int16_t)
 *   [bp+8]  = buffer pointer (near, in DS)
 *   [bp+0xA] = radix (2-36)
 * Returns: AX = buffer pointer
 */
void res_0224EE(CPU *cpu)
{
    /* Read arguments from stack (far call: bp not yet pushed, args at sp+4) */
    int16_t value = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t buf_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t radix = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
    uint32_t buf_flat = seg_off(cpu->ds, buf_off);

    if (radix < 2 || radix > 36 || buf_flat + 20 > MEM_SIZE) {
        /* Invalid args - return empty string */
        cpu->mem[buf_flat] = 0;
        cpu->ax = buf_off;
        cpu->sp += 4; /* far ret */
        return;
    }

    /* Convert integer to string */
    char tmp[20];
    int neg = 0;
    uint16_t uval;

    if (radix == 10 && value < 0) {
        neg = 1;
        uval = (uint16_t)(-(int)value);
    } else {
        uval = (uint16_t)value;
    }

    int pos = 0;
    do {
        uint16_t digit = uval % radix;
        tmp[pos++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        uval /= radix;
    } while (uval > 0 && pos < 18);

    if (neg) tmp[pos++] = '-';

    /* Reverse into destination buffer */
    for (int i = 0; i < pos; i++) {
        cpu->mem[buf_flat + i] = (uint8_t)tmp[pos - 1 - i];
    }
    cpu->mem[buf_flat + pos] = 0;

    cpu->ax = buf_off;
    cpu->sp += 4; /* far ret */
}

/* ─── Title screen bypass ─── */
/* ovl02_02C200 - Title screen animation + main menu.
 * The original function runs a 3-minute animated title sequence,
 * then waits for the user to press N/L/E/C to select a game mode.
 * We bypass this entirely and select "New Game" immediately.
 *
 * Key state set by the original function:
 *   DS:0x6AC2 = game mode (0=New, 1=Load, 2=Earth, 3=Customize)
 *   DS:0xEDEA = number of valid save game slots (0-7)
 *   DS:0xE692 = multiplayer flag
 *   DS:0xC13E = earth map flag
 */
void ovl02_02C200(CPU *cpu)
{
    fprintf(stderr, "[TITLE] Bypassing title screen -> New Game\n");

    /* Set game mode to "New Game" */
    mem_write16(cpu, cpu->ds, 0x6AC2, 0x0000);

    /* No valid save slots */
    mem_write16(cpu, cpu->ds, 0xEDEA, 0x0000);

    /* Single player mode */
    mem_write16(cpu, cpu->ds, 0xE692, 0x0000);

    /* Not earth mode */
    mem_write16(cpu, cpu->ds, 0xC13E, 0x0000);

    cpu->sp += 4; /* far ret */
}

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
#include "platform/font8x8.h"  /* CP437 8x8 font for far_0000_07F4 text rendering */

#include <stdio.h>
#include <stdlib.h>  /* getenv - without this, the implicit int decl truncates the 64-bit pointer */
#include <string.h>
#include <time.h>

/* Access the global DOS state for keyboard/file operations */
extern DosState *get_dos_state(CPU *cpu);

/* Forward declarations for functions we reference */
extern void res_020FA0(CPU *cpu);

/* ─── MSC CRT: fopen / fclose ─── */
/* Hand-implemented to bypass broken CRT internal FILE struct allocation.
 *
 * The FILE struct does NOT live in the guest's DS — the game allocates
 * a runtime stack that overruns its declared BSS boundary, so any DS
 * offset within ~0xEB00..0xFFEE gets clobbered by stack writes during
 * deeply-recursive code paths (e.g. the planet2.pic civ-select dialog).
 *
 * Instead we keep FILE state in a host-side table and hand the game an
 * opaque token (0xF200 | slot) as the FILE*. Lifted code never reads
 * FILE fields directly — only our own fread/fclose dereference, and
 * they go through the host table.
 */
#define CIV_FILE_MAX     8
#define CIV_FILE_TOKEN_BASE 0xF200   /* token = base | (slot*8); not a real DS offset */

typedef struct {
    uint8_t  dos_handle;
    uint8_t  in_use;
} CivFileSlot;

static CivFileSlot g_civ_files[CIV_FILE_MAX];

static int civ_file_token_to_slot(uint16_t tok) {
    if ((tok & 0xFFC7) != CIV_FILE_TOKEN_BASE) return -1; /* not our token */
    int slot = (tok >> 3) & 0x07;
    if (!g_civ_files[slot].in_use) return -1;
    return slot;
}

static uint16_t civ_file_slot_to_token(int slot) {
    return (uint16_t)(CIV_FILE_TOKEN_BASE | (slot * 8));
}

/* Most-recently-loaded PIC palette (6-bit RGB), applied by far_0000_07E6.
 * Captured directly from the .pic file's 'M0' chunk at open time, because the
 * game's transient header buffer at DS:0xC936 is overwritten by the LZW
 * dictionary during decode (so it can't be read at row-blit time). */
uint8_t g_pic_pal[768];
int     g_pic_pal_valid = 0;

/* Parse the 'M0' (0x304D) palette chunk out of an LBM-style Civ .pic file. */
static void civ_load_pic_palette(const char *path) {
    size_t n = strlen(path);
    if (n < 4) return;
    const char *ext = path + n - 4;
    if (!(ext[0]=='.' && (ext[1]=='p'||ext[1]=='P') &&
          (ext[2]=='i'||ext[2]=='I') && (ext[3]=='c'||ext[3]=='C'))) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t buf[65536];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    size_t o = 0;
    while (o + 4 <= len) {
        uint16_t tag = (uint16_t)(buf[o] | (buf[o+1] << 8));
        if ((tag & 0xFF) == 0x58) break;            /* 'X' image chunk -> done */
        uint16_t clen = (uint16_t)(buf[o+2] | (buf[o+3] << 8));
        if (tag == 0x304D && clen >= 0x0302 && o + 6 + 768 <= len) {
            memcpy(g_pic_pal, buf + o + 6, 768);     /* skip tag,len,2-byte hdr */
            g_pic_pal_valid = 1;
            return;
        }
        o += 4 + (clen & ~1u);
    }
}

void res_01FB68(CPU *cpu) {
    /* Args: [sp+4]=filename_ptr, [sp+6]=mode (0=read binary) */
    uint16_t fn_off = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));

    /* Build path string */
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        uint8_t c = mem_read8(cpu, cpu->ds, (uint16_t)(fn_off + i));
        path[i] = (char)c;
        if (!c) break;
    }
    path[255] = 0;

    /* Capture this PIC's palette (if any) for far_0000_07E6 to upload. */
    civ_load_pic_palette(path);

    /* Diagnostic: flag when the title background (arch.pic) loads, so main.c can
     * snapshot the title/menu screen (CIV_DUMPALL). */
    { extern int g_arch_pic_loaded; if (strstr(path, "arch")) g_arch_pic_loaded = 1; }

    /* Open via DOS INT 21h/AH=3D (read-only) */
    DosState *dos = get_dos_state(cpu);
    uint16_t saved_bx = cpu->bx;
    uint16_t saved_cx = cpu->cx, saved_dx = cpu->dx;

    cpu->ah = 0x3D; cpu->al = 0x00; /* open read-only */
    cpu->dx = fn_off; /* DS:DX = filename */
    dos_int21(cpu);

    if (cpu->flags & FLAG_CF) {
        fprintf(stderr, "[FOPEN] '%s' FAILED\n", path);
        cpu->ax = 0; /* return NULL */
        cpu->bx = saved_bx; cpu->cx = saved_cx; cpu->dx = saved_dx;
        cpu->sp += 4; /* far ret */
        return;
    }

    uint8_t handle = (uint8_t)(cpu->ax & 0xFF);

    /* Find a free slot in the host-side table */
    int slot = -1;
    for (int s = 0; s < CIV_FILE_MAX; s++) {
        if (!g_civ_files[s].in_use) { slot = s; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "[FOPEN] '%s' no free FILE slots\n", path);
        cpu->ah = 0x3E; cpu->bx = handle; dos_int21(cpu); /* close */
        cpu->ax = 0;
        cpu->bx = saved_bx; cpu->cx = saved_cx; cpu->dx = saved_dx;
        cpu->sp += 4;
        return;
    }

    g_civ_files[slot].dos_handle = handle;
    g_civ_files[slot].in_use     = 1;

    uint16_t token = civ_file_slot_to_token(slot);

    /* Store as current FILE for the game's PIC decoder fallback path. */
    mem_write16(cpu, cpu->ds, 0x54DA, token);

    fprintf(stderr, "[FOPEN] '%s' -> handle %d, token %04X (slot %d)\n",
            path, handle, token, slot);

    cpu->ax = token;
    cpu->bx = saved_bx; cpu->cx = saved_cx; cpu->dx = saved_dx;
    cpu->sp += 4; /* far ret */
}

/* res_01FBFC - fclose */
void res_01FBFC(CPU *cpu) {
    uint16_t token = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    int slot = civ_file_token_to_slot(token);
    if (slot >= 0) {
        uint8_t handle = g_civ_files[slot].dos_handle;
        uint16_t saved_ax = cpu->ax, saved_bx = cpu->bx;
        cpu->ah = 0x3E; cpu->bx = handle;
        dos_int21(cpu);
        cpu->ax = saved_ax; cpu->bx = saved_bx;
        g_civ_files[slot].in_use = 0;
        g_civ_files[slot].dos_handle = 0;
        fprintf(stderr, "[FCLOSE] token %04X handle %d (slot %d)\n", token, handle, slot);
    }
    cpu->ax = 0;
    cpu->sp += 4; /* far ret */
}

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

    /* Auto-inject a key after 200 polls if no real key pressed.
     * This advances past "press any key" screens during startup.
     * Default is Space (any-key screens). Set CIV_AUTOKEY=<char> to inject a
     * specific ASCII key instead - e.g. CIV_AUTOKEY=n auto-selects New Game at
     * the title menu (getkey strips the scancode, so ASCII is what matters).
     * Real keyboard input always takes precedence; this only fires when the
     * buffer is empty. */
    /* CIV_AUTOKEY: a sequence of ASCII keys auto-injected (cycling) when the
     * buffer is empty, to drive menus headlessly. Each screen consumes the key
     * it accepts and ignores the rest; cycling lets a multi-screen flow advance
     * (e.g. CIV_AUTOKEY=n4 -> 'n' New Game at the title, '4' difficulty at setup).
     * Default (env unset) injects a single Space for "press any key" screens.
     * Real keyboard input always wins. */
    static const char *ak_seq = 0; static int ak_init = 0, ak_period = 200, ak_idx = 0;
    if (!ak_init) {
        ak_init = 1;
        ak_seq = getenv("CIV_AUTOKEY");
        if (ak_seq && ak_seq[0]) ak_period = 5; else ak_seq = " ";
    }
    if (!keyboard_available(&dos->keyboard) && call_count >= (uint64_t)ak_period &&
        (call_count % ak_period) == 0) {
        int len = (int)strlen(ak_seq);
        uint8_t a = (uint8_t)ak_seq[ak_idx % (len ? len : 1)];
        ak_idx++;
        keyboard_push(&dos->keyboard, 0, a);
        fprintf(stderr, "[KBHIT] Auto-injected key 0x%02X at poll #%llu\n",
                a, (unsigned long long)call_count);
    }

    /* With a deterministic key script, always report "key available" so the
     * script getkey (far_0000_09DE) is reached on every poll. */
    { const char *ks = getenv("CIV_KEYSCRIPT");
      if (ks && ks[0]) { cpu->ax = 0x00FF; cpu->sp += 4; return; } }
    cpu->ax = keyboard_available(&dos->keyboard) ? 0x00FF : 0x0000;
    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "[KBHIT] #%llu result=%u\n",
                (unsigned long long)call_count, cpu->ax);
        fflush(stderr);
    }
    cpu->sp += 4; /* far ret */
}

/* far_0000_09DE - blocking keyboard read (INT 16h AH=0 equivalent).
 * Returns AX = (scancode<<8) | ascii and consumes one key from the buffer.
 *
 * Was MIS-ALIASED to ovl12_03D15C (a score-string builder: itoa+strcat) by the
 * naive link-order thunk assignment. getkey (far_1D1F_0AC9) calls this, so it
 * read garbage and never returned N/L/E/C; meanwhile the real key sat unconsumed
 * in the buffer, so kbhit (far_205A_2096) reported "key available" forever and
 * the title menu spun without ever advancing. Verified by contract, not order
 * (see the standing warning about the 0x0761 runtime-patched thunk table). */
void far_0000_09DE(CPU *cpu)
{
    extern void platform_delay(uint32_t ms);
    DosState *dos = get_dos_state(cpu);
    /* Deterministic key script: CIV_KEYSCRIPT=<chars> returns one char per getkey
     * call (reproducible New-Game -> sp299 repro, vs the timing-based autokey).
     * '_' = a 1ms-ish "no real wait" filler returning Space. Returns AL=ascii,
     * AH=a plausible scancode for the common menu/setup keys. */
    { const char *ks = getenv("CIV_KEYSCRIPT");
      if (ks && ks[0]) {
        static unsigned ksi = 0;
        char c = ks[ksi] ? ks[ksi] : ' ';
        if (ks[ksi]) ksi++;
        uint8_t sc = 0x39;                 /* default scancode = space */
        if (c == '\r' || c == '\n') { c = '\r'; sc = 0x1C; }
        else if (c == ' ')          sc = 0x39;
        else if (c >= '1' && c <= '9') sc = 0x02 + (c - '1');
        else if (c == 'N' || c == 'n') sc = 0x31;
        cpu->ax = (uint16_t)((sc << 8) | (uint8_t)c);
        static int _ks=0; if (++_ks <= 40)
            fprintf(stderr, "[KEYSCRIPT] #%d getkey -> 0x%04X '%c'\n", _ks, cpu->ax, (c>=32&&c<127)?c:'.');
        fflush(stderr);
        cpu->sp += 4; return;
      } }
    /* Block until a key is available, pumping SDL so the window stays live and
     * a small delay keeps us off 100% CPU. The title menu only calls this after
     * kbhit confirms a key, so in practice it rarely waits. */
    while (!keyboard_available(&dos->keyboard)) {
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
        platform_delay(2);
    }
    cpu->ax = keyboard_read(&dos->keyboard);
    static int _kc = 0;
    if (++_kc <= 8) {
        fprintf(stderr, "[KEY] far_0000_09DE getkey raw=0x%04X\n", cpu->ax);
        fflush(stderr);
    }
    cpu->sp += 4; /* far ret */
}

/* ─── New-Game setup input chain (lifted from dump) ───
 * far_0000_16C6 / far_01A7_0225 / far_1436_0918 are the setup screen's mouse +
 * keyboard input path. They were no-op STUBs, so the setup's input-wait never
 * blocked/returned a key and the setup screen spun. Hand-lifted faithfully; all
 * their deps are implemented (far_0402_44E9 yield, far_205A_2096 kbhit,
 * far_205A_20AA getch). The cosmetic menu-box draw (far_0181_000C -> the still-
 * undefined far_0000_0FFC) stays stubbed. See plan_newgame_menu_subsystem.md. */
extern void far_0402_44E9(CPU *cpu);
extern void far_205A_2096(CPU *cpu);
extern void far_205A_20AA(CPU *cpu);

/* far_0000_16C6 - read+clear the mouse-click latch DS:[0x5824] (0 = none). */
void far_0000_16C6(CPU *cpu)
{
    cpu->ax = mem_read16(cpu, cpu->ds, 0x5824);
    mem_write16(cpu, cpu->ds, 0x5824, 0);
    cpu->sp += 4; /* far ret */
}

/* far_01A7_0225 - snapshot mouse/cursor state into DS:[0xEA6A/0xEA6C/0xEA6E].
 * @0x1C95: if VGA([0x1A3C]): EA6A = mouse_clicks|[0x5822]; EA6C=[0x581E];
 * EA6E=[0x5820]; else zero all three. */
void far_01A7_0225(CPU *cpu)
{
    if (mem_read16(cpu, cpu->ds, 0x1A3C) != 0) {
        push16(cpu, cpu->cs); push16(cpu, 0);
        far_0000_16C6(cpu);                       /* ax = mouse-click latch */
        cpu->ax |= mem_read16(cpu, cpu->ds, 0x5822);
        mem_write16(cpu, cpu->ds, 0xEA6A, cpu->ax);
        mem_write16(cpu, cpu->ds, 0xEA6C, mem_read16(cpu, cpu->ds, 0x581E));
        mem_write16(cpu, cpu->ds, 0xEA6E, mem_read16(cpu, cpu->ds, 0x5820));
    } else {
        mem_write16(cpu, cpu->ds, 0xEA6E, 0);
        mem_write16(cpu, cpu->ds, 0xEA6C, 0);
        mem_write16(cpu, cpu->ds, 0xEA6A, 0);
    }
    cpu->sp += 4; /* far ret */
}

/* far_1436_0918 - the setup screen's input wait (@0x14C78). Yields, then loops
 * polling mouse + keyboard until input, then reads the key via getch.
 *   yield; loop{ mouse(); if EA6A!=0 break; if kbhit() break; }
 *   if EA6A==0 { k=getch(); if k==0 getch(); }  ; returns key in AX */
void far_1436_0918(CPU *cpu)
{
    push16(cpu, cpu->cs); push16(cpu, 0); far_0402_44E9(cpu);   /* yield */
    for (;;) {
        push16(cpu, cpu->cs); push16(cpu, 0); far_01A7_0225(cpu); /* mouse */
        if (mem_read16(cpu, cpu->ds, 0xEA6A) != 0) break;
        push16(cpu, cpu->cs); push16(cpu, 0); far_205A_2096(cpu); /* kbhit */
        if (cpu->ax != 0) break;
    }
    if (mem_read16(cpu, cpu->ds, 0xEA6A) == 0) {
        push16(cpu, cpu->cs); push16(cpu, 0); far_205A_20AA(cpu); /* getch */
        if (cpu->ax == 0) {
            push16(cpu, cpu->cs); push16(cpu, 0); far_205A_20AA(cpu); /* extended */
        }
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
    uint16_t gfx_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    uint16_t page = mem_read16(cpu, cpu->ds, gfx_ptr); /* struct[+00] page flag */
    if (page != 0) {
        uint32_t src = gfx_page_addr(page);
        if (src + 64000 <= MEM_SIZE) {
            memcpy(&cpu->mem[0xA0000], &cpu->mem[src], 64000);
        }
    }

    if (call_count <= 5 || (call_count % 100) == 0) {
        /* Scan VGA framebuffer for non-zero pixels */
        int nonzero = 0;
        for (int i = 0; i < 64000 && nonzero < 20; i++) {
            if (cpu->mem[0xA0000 + i] != 0) nonzero++;
        }
        fprintf(stderr, "[FRAME] end_frame #%d page=%d gfx=%04X nonzero=%d\n",
                call_count, page, gfx_ptr, nonzero);
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

/* far_0000_0825 - Timer/VBlank interrupt handler.
 * Original code: sets CS:[0x065A] = 1 and does IRET.
 * This flag is polled by wait loops. In the recompiled version,
 * we set the flag in memory at the relocated address.
 * Pre-relocation CS=0, relocated CS=LOAD_SEG=0x100, so
 * CS:0x065A = real address 0x100*16 + 0x065A = 0x165A. */
void far_0000_0825(CPU *cpu)
{
    /* Set the timer/VBlank flag */
    mem_write16(cpu, 0x0100, 0x065A, 1);
    cpu->sp += 4; /* far ret (original uses IRET but we just return) */
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

/* ─── GFX sprite store: screen-grab capture + blit (far_0000_076F / _083F) ───
 * The original sprite system: far_0000_076F snapshots a w*h rect of the active
 * draw page into an off-screen buffer and returns a handle; far_0000_083F later
 * blits a sprite (by handle, or by a DS-relative pointer to file-loaded sprite
 * data) onto a gfx page at (x,y). Both ends were no-op stubs, so nothing drew.
 *
 * Screen-grab sprites are OURS end-to-end (we control the handle format), so we
 * implement them with a host-side store keyed by an opaque handle in the 0x4000
 * range (distinct from real DS offsets, which are small). File-loaded sprites
 * (logo/font/map tiles) use the GAME's on-disk sprite format, which still needs
 * RE — far_0000_083F logs those headers (for the next session) and skips them. */
#define SPRITE_STORE_MAX 256
typedef struct { int used; int w, h; uint8_t *px; } HostSprite;
static HostSprite g_sprites[SPRITE_STORE_MAX];
static int sprite_is_handle(uint16_t v) { return (v & 0xF000) == 0x4000; }

/* far_0000_076F - Capture a rectangle of the active draw page into a host
 * sprite and return a handle.
 *   Stack params (cdecl): [sp+04] flag  [sp+06] x  [sp+08] y
 *                         [sp+0A] width [sp+0C] height
 * The active draw page is the one referenced by the GFX struct at DS:[0xAA].
 * The civ-select dialog ovl02_02CDD7 calls this 24x in a 6x4 grid to snapshot
 * portrait cells; the title/menu uses it to save areas under cursors/highlights.
 * (The old thunk alias wrongly pointed 0x076F at its caller ovl02_02CDD7 ->
 * recursion; this is the real screen-grab primitive.) */
void far_0000_076F(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4); /* skip far return address */
    int16_t  flag   = (int16_t)mem_read16(cpu, cpu->ss, sp);
    int16_t  x      = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t  y      = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    int16_t  width  = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    int16_t  height = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));
    (void)flag;

    /* Source = active draw page (GFX struct at DS:[0xAA]); apply its origin. */
    uint16_t gfx_ptr = mem_read16(cpu, cpu->ds, 0xAA);
    uint16_t page    = mem_read16(cpu, cpu->ds, gfx_ptr);
    int16_t  xo      = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ptr + 2));
    int16_t  yo      = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ptr + 4));
    uint32_t base    = gfx_page_addr(page);
    int16_t  gx = (int16_t)(x + xo), gy = (int16_t)(y + yo);

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < SPRITE_STORE_MAX; i++) if (!g_sprites[i].used) { slot = i; break; }
    if (slot < 0 || width <= 0 || height <= 0 || width > 320 || height > 200) {
        cpu->ax = 0; cpu->sp += 4; return;
    }

    HostSprite *s = &g_sprites[slot];
    free(s->px);
    s->px = (uint8_t *)malloc((size_t)width * height);
    s->w = width; s->h = height; s->used = 1;
    if (s->px) {
        for (int r = 0; r < height; r++) {
            int srow = gy + r;
            for (int c = 0; c < width; c++) {
                int scol = gx + c;
                uint8_t v = 0;
                if (srow >= 0 && srow < 200 && scol >= 0 && scol < 320) {
                    uint32_t a = base + (uint32_t)srow * 320 + (uint32_t)scol;
                    if (a < MEM_SIZE) v = cpu->mem[a];
                }
                s->px[r * width + c] = v;
            }
        }
    }
    cpu->ax = (uint16_t)(0x4000 | slot);  /* opaque host handle */
    { static int _g=0; if(++_g<=6) fprintf(stderr,"[SPRITE] grab slot=%d %dx%d @(%d,%d) pg%d\n",slot,width,height,gx,gy,page); }
    cpu->sp += 4; /* far ret */
}

/* far_0000_083F - Sprite blitter (thunk; the long-stubbed keystone, 64 callers).
 *   Stack params (cdecl, caller cleans 8): [sp+04] gfx_ctx  [sp+06] x
 *                                          [sp+08] y       [sp+0A] sprite
 * If `sprite` is a host handle (0x4000 range) from far_0000_076F, blit our
 * captured pixels onto the gfx_ctx page at (x,y), opaque (screen-grab restore).
 * Otherwise `sprite` is a DS pointer to file-loaded sprite data in the GAME's
 * format (logo/font/tiles) which we can't decode yet -> log its header for RE
 * and skip. */
void far_0000_083F(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4);
    uint16_t gfx_ctx = mem_read16(cpu, cpu->ss, sp);
    int16_t  x       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t  y       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    uint16_t sprite  = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));

    if (sprite_is_handle(sprite)) {
        int slot = sprite & (SPRITE_STORE_MAX - 1);
        HostSprite *s = &g_sprites[slot];
        if (slot < SPRITE_STORE_MAX && s->used && s->px) {
            uint16_t page = mem_read16(cpu, cpu->ds, gfx_ctx);
            int16_t  xo   = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ctx + 2));
            int16_t  yo   = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ctx + 4));
            uint32_t base = gfx_page_addr(page);
            int16_t  dx = (int16_t)(x + xo), dy = (int16_t)(y + yo);
            for (int r = 0; r < s->h; r++) {
                int drow = dy + r; if (drow < 0 || drow >= 200) continue;
                for (int c = 0; c < s->w; c++) {
                    int dcol = dx + c; if (dcol < 0 || dcol >= 320) continue;
                    uint32_t a = base + (uint32_t)drow * 320 + (uint32_t)dcol;
                    if (a < MEM_SIZE) cpu->mem[a] = s->px[r * s->w + c];
                }
            }
            { static int _b=0; if(++_b<=6) fprintf(stderr,"[SPRITE] blit handle slot=%d %dx%d @(%d,%d)\n",slot,s->w,s->h,x,y); }
        }
    } else {
        /* File-loaded sprite: capture its header bytes once per distinct offset
         * so the on-disk sprite format can be reverse-engineered next session. */
        static int logged = 0;
        if (logged < 12) {
            logged++;
            uint32_t a = seg_off(cpu->ds, sprite);
            fprintf(stderr, "[SPRITE] file-sprite blit gfx=0x%04X x=%d y=%d off=0x%04X hdr:",
                    gfx_ctx, x, y, sprite);
            for (int i = 0; i < 16 && a + i < MEM_SIZE; i++)
                fprintf(stderr, " %02X", cpu->mem[a + i]);
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
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
    /* Dump game state on first few minimap fills to diagnose the loop */
    if (x == 160 && y == 0 && width == 80 && height == 50) {
        static int minimap_count = 0;
        minimap_count++;
        if (minimap_count <= 5 || minimap_count == 100 || minimap_count == 1000) {
            fprintf(stderr, "[DIAG] minimap_fill #%d: EB78=%04X 6B1A=%04X E71E=%04X 9102=%04X EE90=%04X sp=%04X\n",
                    minimap_count,
                    mem_read16(cpu, cpu->ds, 0xEB78),
                    mem_read16(cpu, cpu->ds, 0x6B1A),
                    mem_read16(cpu, cpu->ds, 0xE71E),
                    mem_read16(cpu, cpu->ds, 0x9102),
                    mem_read16(cpu, cpu->ds, 0xEE90),
                    cpu->sp);
            fflush(stderr);
        }
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

/* ─── Blit: far_0000_07ED ─── */
/* far_0000_07ED - Copy rectangle between GFX page buffers.
 * Stack params (cdecl, caller cleans 0x10 = 16 bytes):
 *   [sp+04] src_gfx  - DS offset to source GFX context struct
 *   [sp+06] sx       - source x coordinate
 *   [sp+08] sy       - source y coordinate
 *   [sp+0A] width    - rectangle width in pixels
 *   [sp+0C] height   - rectangle height in pixels
 *   [sp+0E] dst_gfx  - DS offset to destination GFX context struct
 *   [sp+10] dx       - destination x coordinate
 *   [sp+12] dy       - destination y coordinate
 *
 * Both src_gfx and dst_gfx point to GFX structs with:
 *   [+00] page (0=VGA, 1=page1, 2=page2)
 *   [+02] x_origin
 *   [+04] y_origin
 */
void far_0000_07ED(CPU *cpu)
{
    static uint64_t call_count = 0;
    call_count++;

    /* Yield periodically */
    if (call_count % 50 == 0) {
        DosState *dos = get_dos_state(cpu);
        if (dos->poll_events)
            dos->poll_events(dos->platform_ctx, dos, cpu);
    }

    uint16_t sp = (uint16_t)(cpu->sp + 4); /* skip far return address */
    uint16_t src_gfx = mem_read16(cpu, cpu->ss, sp);
    int16_t sx       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t sy       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    int16_t width    = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    int16_t height   = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));
    uint16_t dst_gfx = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 10));
    int16_t dx       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 12));
    int16_t dy       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 14));

    if (call_count <= 10 || (call_count % 1000) == 0) {
        uint16_t sp2 = mem_read16(cpu, cpu->ds, src_gfx);
        uint16_t dp2 = mem_read16(cpu, cpu->ds, dst_gfx);
        fprintf(stderr, "[BLIT] #%llu src=DS:%04X(pg%d,%d,%d) %dx%d -> dst=DS:%04X(pg%d,%d,%d)\n",
                (unsigned long long)call_count, src_gfx, sp2, sx, sy, width, height, dst_gfx, dp2, dx, dy);
        fflush(stderr);
    }

    if (width <= 0 || height <= 0) {
        cpu->sp += 4; /* far ret */
        return;
    }

    /* Read source GFX context */
    uint16_t src_page     = mem_read16(cpu, cpu->ds, src_gfx);
    int16_t  src_x_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(src_gfx + 2));
    int16_t  src_y_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(src_gfx + 4));

    /* Read destination GFX context */
    uint16_t dst_page     = mem_read16(cpu, cpu->ds, dst_gfx);
    int16_t  dst_x_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(dst_gfx + 2));
    int16_t  dst_y_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(dst_gfx + 4));

    /* Apply origin offsets */
    sx += src_x_origin;
    sy += src_y_origin;
    dx += dst_x_origin;
    dy += dst_y_origin;

    uint32_t src_base = gfx_page_addr(src_page);
    uint32_t dst_base = gfx_page_addr(dst_page);

    /* Clip to screen bounds (320x200) */
    int16_t src_x1 = sx, src_y1 = sy;
    int16_t dst_x1 = dx, dst_y1 = dy;
    int16_t copy_w = width, copy_h = height;

    /* Clip left edge */
    if (src_x1 < 0) { copy_w += src_x1; dst_x1 -= src_x1; src_x1 = 0; }
    if (dst_x1 < 0) { copy_w += dst_x1; src_x1 -= dst_x1; dst_x1 = 0; }
    /* Clip right edge */
    if (src_x1 + copy_w > 320) copy_w = 320 - src_x1;
    if (dst_x1 + copy_w > 320) copy_w = 320 - dst_x1;
    /* Clip top edge */
    if (src_y1 < 0) { copy_h += src_y1; dst_y1 -= src_y1; src_y1 = 0; }
    if (dst_y1 < 0) { copy_h += dst_y1; src_y1 -= dst_y1; dst_y1 = 0; }
    /* Clip bottom edge */
    if (src_y1 + copy_h > 200) copy_h = 200 - src_y1;
    if (dst_y1 + copy_h > 200) copy_h = 200 - dst_y1;

    if (copy_w <= 0 || copy_h <= 0) {
        cpu->sp += 4; /* far ret */
        return;
    }

    /* Copy scanlines */
    for (int row = 0; row < copy_h; row++) {
        uint32_t s = src_base + (uint32_t)(src_y1 + row) * 320 + (uint32_t)src_x1;
        uint32_t d = dst_base + (uint32_t)(dst_y1 + row) * 320 + (uint32_t)dst_x1;
        if (s + copy_w <= MEM_SIZE && d + copy_w <= MEM_SIZE) {
            memmove(&cpu->mem[d], &cpu->mem[s], (size_t)copy_w);
        }
    }

    cpu->sp += 4; /* far ret */
}

/* ─── PIC row blitter: far_0000_07E6 ───
 * Copies one decoded image row (a linear DS buffer) onto a destination GFX page.
 * The PIC display loop far_1FB6_01A0 decodes each row into DS:0xF0AE then calls
 * this to place it. Was MIS-ALIASED to ovl05_02FFC2 (which treated arg1, the
 * source buffer offset, as a page index and dropped the pixels).
 * Stack params (cdecl far, caller cleans 0xA = 5 words):
 *   [sp+04] src_off  - DS offset of the source row buffer
 *   [sp+06] dst_gfx  - DS offset of the destination GFX context (page@+0,
 *                      x_origin@+2, y_origin@+4) — same convention as 0000_07ED
 *   [sp+08] x        - destination column
 *   [sp+0A] y        - destination row
 *   [sp+0C] width    - pixels to copy */
void far_0000_07E6(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4);
    uint16_t src_off = mem_read16(cpu, cpu->ss, sp);
    uint16_t dst_gfx = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t  x       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    int16_t  y       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    int16_t  width   = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));

    /* Upload the current PIC's palette. far_0000_1080 parsed the PIC's LBM-style
     * chunks into the header buffer at DS:0xC936; the 'M0' (0x304D) chunk is the
     * 256-colour VGA palette: [tag 0x304D][len 0x0302][2-byte hdr 00 FF][768 bytes
     * of 6-bit RGB]. The game never uploads it (its only DAC uploader, the fade
     * engine, is unwired), so PICs rendered black. Scan for the chunk and copy the
     * palette into dos->video.palette (already 6-bit). Idempotent per row. */
    if (g_pic_pal_valid) {
        DosState *dos = get_dos_state(cpu);
        for (int i = 0; i < 256; i++) {
            dos->video.palette[i][0] = g_pic_pal[i*3 + 0] & 0x3F;
            dos->video.palette[i][1] = g_pic_pal[i*3 + 1] & 0x3F;
            dos->video.palette[i][2] = g_pic_pal[i*3 + 2] & 0x3F;
        }
    }

    uint16_t page = mem_read16(cpu, cpu->ds, dst_gfx);
    int16_t  xo   = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(dst_gfx + 2));
    int16_t  yo   = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(dst_gfx + 4));
    uint32_t base = gfx_page_addr(page);
    int16_t  dx = (int16_t)(x + xo), dy = (int16_t)(y + yo);

    if (width > 0 && dy >= 0 && dy < 200) {
        uint32_t src = seg_off(cpu->ds, src_off);
        for (int c = 0; c < width; c++) {
            int cx = dx + c;
            if (cx < 0 || cx >= 320) continue;
            uint32_t a = base + (uint32_t)dy * 320 + (uint32_t)cx;
            if (a < MEM_SIZE && src + c < MEM_SIZE)
                cpu->mem[a] = cpu->mem[src + c];
        }
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Text: far_0000_07F4 - render a string to the active GFX page ───
 * Was MIS-ALIASED to ovl05_0307DA (the planet2/civ-select builder) — the same
 * wrong-thunk class as far_0000_076F — so ALL game text rendered nothing.
 * Called by res_001828 (char/string draw) which is reached from res_0018B1
 * (centered text) used throughout menus, the world-reveal status line, the
 * civ-select dialog, etc.
 *   Stack params (cdecl, caller cleans 8 bytes):
 *     [sp+04] gfx_ctx  - DS offset to GFX context struct
 *     [sp+06] x        - left pixel column
 *     [sp+08] y        - top pixel row
 *     [sp+0A] str_off  - DS offset to a NUL-terminated string
 * The GFX struct: [+00]=page, [+02]=x_origin, [+04]=y_origin, [+0C]=fg_color.
 * The game assumes 8px-wide glyphs (far_0000_077D char-width returns 8), so we
 * render with the built-in CP437 8x8 font. Pixels are written as palette
 * indices to the active page; color 0 is treated as transparent so text can
 * overlay existing graphics (matches the original's masked glyph blit). */
void far_0000_07F4(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4); /* skip far return address */
    uint16_t gfx_ctx = mem_read16(cpu, cpu->ss, sp);
    int16_t  x       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int16_t  y       = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    uint16_t str_off = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));

    uint16_t page     = mem_read16(cpu, cpu->ds, gfx_ctx);                       /* [+00] */
    int16_t  x_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ctx + 2)); /* [+02] */
    int16_t  y_origin = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ctx + 4)); /* [+04] */
    uint8_t  color    = (uint8_t)mem_read16(cpu, cpu->ds, (uint16_t)(gfx_ctx + 0xC)); /* [+0C] fg */
    if (color == 0) color = 15; /* avoid invisible text if fg unset */

    x += x_origin;
    y += y_origin;
    uint32_t base = gfx_page_addr(page);

    int cx = x;
    for (int i = 0; i < 256; i++) {
        uint8_t ch = mem_read8(cpu, cpu->ds, (uint16_t)(str_off + i));
        if (ch == 0) break;
        const uint8_t *glyph = font8x8_cp437[ch];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= 200) continue;
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(bits & (0x80 >> gx))) continue;
                int px = cx + gx;
                if (px < 0 || px >= 320) continue;
                uint32_t a = base + (uint32_t)py * 320 + (uint32_t)px;
                if (a < MEM_SIZE) cpu->mem[a] = color;
            }
        }
        cx += 8;
    }
    cpu->sp += 4; /* far ret */
}

/* Forward decls for helpers called by the lifted ovl07_035B6E (defined later
 * in this file or in the generated recomp/stub units). */
extern void far_0000_032C(CPU *cpu); extern void far_0000_0330(CPU *cpu);
extern void far_0000_0374(CPU *cpu); extern void far_0000_03EC(CPU *cpu);
extern void far_0000_041D(CPU *cpu); extern void far_0000_049C(CPU *cpu);
extern void far_0000_04C6(CPU *cpu); extern void far_0000_07ED(CPU *cpu);
extern void far_0000_0838(CPU *cpu); extern void far_0000_0A40(CPU *cpu);
extern void far_0000_0BEC(CPU *cpu); extern void far_1DDE_007C(CPU *cpu);
extern void far_1FB6_0252(CPU *cpu); extern void far_1FB6_0286(CPU *cpu);
extern void far_205A_1E60(CPU *cpu); extern void far_205A_2096(CPU *cpu);
extern void res_0018B1(CPU *cpu); extern void res_001932(CPU *cpu);
extern void res_001ECA(CPU *cpu); extern void res_020B20(CPU *cpu);
extern void res_020C1C(CPU *cpu); extern void res_020E3A(CPU *cpu);
extern void res_022418(CPU *cpu);

/* ─── ovl07_035B6E: terrain-reveal state machine (un-stubbed 2026-05-30) ───
 * Previously a no-op stub that returned AX=0 without advancing state, which
 * (a) left the ovl07_034412 map-reveal loops spinning forever and (b) drew
 * nothing. This is the faithful body produced by the project lifter
 * (tools/recomp on CIV.EXE @0x35B6E, 1044 bytes). It advances the phase
 * counter [0x3B12] (0->2, 2->5), renders via res_0018B1/res_001932 (the
 * 0x181:xx tile draws) + far_0000_0BEC fill + far_0000_07ED blit, and polls
 * input via far_205A_2096 (which also pumps the SDL render). Returns AX=0
 * once [0x3B12]==2 (reveal complete) so the waiting loops terminate.
 * A few auxiliary draw thunks (far_0000_0374/03EC/041D/049C/04C6) are still
 * stubs and no-op harmlessly; they do not affect loop-termination counters. */
void ovl07_035B6E(CPU *cpu)
{
    push16(cpu, cpu->bp);                    /* push bp */
    cpu->bp = (uint16_t)(cpu->sp);           /* mov bp, sp */
    cpu->sp = (uint16_t)(flags_sub16(cpu, cpu->sp, 0x1A)); /* sub sp, 0x1A */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x0)); /* mov word ds:[bx], 0x0 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x2); /* cmp word ds:[0x3B12], 0x2 */
    if (cc_l(cpu)) goto L_ovl07_035B6E_00008B; /* jl 0x008B */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_00008B; /* jne 0x008B */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x5); /* cmp word ds:[0x3B12], 0x5 */
    if (cc_ge(cpu)) goto L_ovl07_035B6E_000051; /* jge 0x0051 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B12, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B12] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x5); /* cmp word ds:[0x3B12], 0x5 */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_000080; /* jne 0x0080 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x8);               /* mov ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xF0);              /* mov ax, 0xF0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x28);              /* mov ax, 0x28 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
    goto L_ovl07_035B6E_000080;              /* jmp 0x0080 */
L_ovl07_035B6E_000051:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_032C(cpu);                      /* call 0000:032C */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    cpu->cx = (uint16_t)(0x3C);              /* mov cx, 0x3C */
    { int32_t _n = (int32_t)(((uint32_t)cpu->dx << 16) | cpu->ax); int16_t _d = (int16_t)cpu->cx; cpu->ax = (uint16_t)(int16_t)(_n / _d); cpu->dx = (uint16_t)(int16_t)(_n % _d); } /* idiv cx */
    mem_write16(cpu, cpu->ds, 0x6792, (uint16_t)(cpu->ax)); /* mov word ds:[0x6792], ax */
    flags_logic8(cpu, mem_read8(cpu, cpu->ds, 0x6792) & 0x1); /* test byte ds:[0x6792], 0x1 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_00006B; /* je 0x006B */
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
    goto L_ovl07_035B6E_00006E;              /* jmp 0x006E */
L_ovl07_035B6E_00006B:;
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
L_ovl07_035B6E_00006E:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3AD8);            /* mov ax, 0x3AD8 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
L_ovl07_035B6E_000080:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x1)); /* mov word ds:[bx], 0x1 */
    goto L_ovl07_035B6E_00040E;              /* jmp 0x040E */
L_ovl07_035B6E_00008B:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x0); /* cmp word ds:[0x3B12], 0x0 */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_0000D2; /* jne 0x00D2 */
    cpu->ax = (uint16_t)(0x3AEE);            /* mov ax, 0x3AEE */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3AF1);            /* mov ax, 0x3AF1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_020C1C(cpu);                         /* call 205A:0696 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    mem_write16(cpu, cpu->ds, 0x6796, (uint16_t)(cpu->ax)); /* mov word ds:[0x6796], ax */
    mem_write16(cpu, cpu->ds, 0x3B12, (uint16_t)(0x1)); /* mov word ds:[0x3B12], 0x1 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    mem_write16(cpu, cpu->ds, 0x6798, (uint16_t)(cpu->ax)); /* mov word ds:[0x6798], ax */
    mem_write16(cpu, cpu->ds, 0x6792, (uint16_t)(cpu->ax)); /* mov word ds:[0x6792], ax */
    cpu->ax = (uint16_t)(0x3AFB);            /* mov ax, 0x3AFB */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E60(cpu);                      /* call 205A:1E60 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(0x7)); /* mov word ds:[bx+0x10], 0x7 */
    mem_write16(cpu, cpu->ds, 0x6794, (uint16_t)(0x0)); /* mov word ds:[0x6794], 0x0 */
L_ovl07_035B6E_0000D2:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x6794), 0x0); /* cmp word ds:[0x6794], 0x0 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_0000E1; /* je 0x00E1 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    mem_write16(cpu, cpu->ds, 0x6792, (uint16_t)(cpu->ax)); /* mov word ds:[0x6792], ax */
L_ovl07_035B6E_0000E1:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6798)); /* mov ax, word ds:[0x6798] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x6792), cpu->ax); /* cmp word ds:[0x6792], ax */
    if (cc_ge(cpu)) goto L_ovl07_035B6E_0000F5; /* jge 0x00F5 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x1)); /* mov word ds:[bx], 0x1 */
    goto L_ovl07_035B6E_000409;              /* jmp 0x0409 */
L_ovl07_035B6E_0000F5:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x2); /* cmp word ds:[0x3B12], 0x2 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_000124; /* je 0x0124 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x6794), 0x28); /* cmp word ds:[0x6794], 0x28 */
    if (cc_ge(cpu)) goto L_ovl07_035B6E_000124; /* jge 0x0124 */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->ax = (uint16_t)(0x5);               /* mov ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl07_035B6E_000124:;
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3AFC);            /* mov ax, 0x3AFC */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x6796)); /* push word ds:[0x6796] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_020E3A(cpu);                         /* call 205A:08B4 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x18), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x18], ax */
    flags_cmp16(cpu, cpu->ax, 0xFFFF);       /* cmp ax, 0xFFFF */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_000143; /* jne 0x0143 */
    goto L_ovl07_035B6E_00039B;              /* jmp 0x039B */
L_ovl07_035B6E_000143:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl07_035B6E_000156; /* je 0x0156 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x678E), 0x0); /* cmp word ds:[0x678E], 0x0 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_000156; /* je 0x0156 */
    goto L_ovl07_035B6E_00039B;              /* jmp 0x039B */
L_ovl07_035B6E_000156:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B14), 0x1); /* cmp word ds:[0x3B14], 0x1 */
    if (cc_le(cpu)) goto L_ovl07_035B6E_000183; /* jle 0x0183 */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x8);               /* mov ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl07_035B6E_000183:;
    cpu->ax = (uint16_t)(0xA);               /* mov ax, 0xA */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->ax = (uint16_t)(0x5);               /* mov ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(0xB);               /* mov ax, 0xB */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    mem_write16(cpu, cpu->ds, 0x6798, (uint16_t)(0x0)); /* mov word ds:[0x6798], 0x0 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_022418(cpu);                         /* call 205A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    flags_cmp16(cpu, cpu->ax, 0x3);          /* cmp ax, 0x3 */
    if (cc_le(cpu)) goto L_ovl07_035B6E_00020C; /* jle 0x020C */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x6794), 0x0); /* cmp word ds:[0x6794], 0x0 */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_000200; /* jne 0x0200 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0330(cpu);                      /* call 0000:0330 */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001ECA(cpu);                         /* call 01A7:046E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(0x4);               /* mov ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001ECA(cpu);                         /* call 01A7:046E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl07_035B6E_000200:;
    mem_write16(cpu, cpu->ds, 0x6798, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x6798), 0x2))); /* add word ds:[0x6798], 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x6794, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x6794), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x6794] */
    goto L_ovl07_035B6E_00035A;              /* jmp 0x035A */
L_ovl07_035B6E_00020C:;
    cpu->ax = (uint16_t)(0x3B03);            /* mov ax, 0x3B03 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x10)); /* lea ax, word ss:[bp+-0x10] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E60(cpu);                      /* call 205A:1E60 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B14)); /* mov ax, word ds:[0x3B14] */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B14, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B14), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B14] */
    mem_write8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xB), (uint8_t)(flags_add8(cpu, mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0xB)), cpu->al))); /* add byte ss:[bp+-0xB], al */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_00024A; /* je 0x024A */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(0x1)); /* mov word ss:[bp+-0x16], 0x1 */
    goto L_ovl07_035B6E_000242;              /* jmp 0x0242 */
L_ovl07_035B6E_000234:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16))); /* push word ss:[bp+-0x16] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_041D(cpu);                      /* call 0000:041D */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x16] */
L_ovl07_035B6E_000242:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B18)); /* mov ax, word ds:[0x3B18] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), cpu->ax); /* cmp word ss:[bp+-0x16], ax */
    if (cc_le(cpu)) goto L_ovl07_035B6E_000234; /* jle 0x0234 */
L_ovl07_035B6E_00024A:;
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x10)); /* lea ax, word ss:[bp+-0x10] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x2);               /* mov ax, 0x2 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_0252(cpu);                      /* call 1FB6:0252 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_000272; /* je 0x0272 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x8);               /* mov ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_04C6(cpu);                      /* call 0000:04C6 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
L_ovl07_035B6E_000272:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_00029C; /* jne 0x029C */
    goto L_ovl07_035B6E_00035A;              /* jmp 0x035A */
L_ovl07_035B6E_00029C:;
    cpu->ax = (uint16_t)(0x3B0E);            /* mov ax, 0x3B0E */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x9)); /* lea ax, word ss:[bp+-0x9] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E60(cpu);                      /* call 205A:1E60 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0xD4EE);            /* mov ax, 0xD4EE */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x10)); /* lea ax, word ss:[bp+-0x10] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_0286(cpu);                      /* call 1FB6:0286 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0xD4EE);            /* mov ax, 0xD4EE */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x8);               /* mov ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_049C(cpu);                      /* call 0000:049C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    mem_write16(cpu, cpu->ds, 0x3B18, (uint16_t)(0x0)); /* mov word ds:[0x3B18], 0x0 */
    goto L_ovl07_035B6E_000329;              /* jmp 0x0329 */
L_ovl07_035B6E_0002D4:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B16)); /* mov bx, word ds:[0x3B16] */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B16, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B16), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B16] */
    cpu->es = (uint16_t)(mem_read16(cpu, cpu->ds, 0x63B4)); /* mov es, word ds:[0x63B4] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->es, cpu->bx)); /* mov al, byte es:[bx] */
    cpu->ah = (uint8_t)(flags_sub8(cpu, cpu->ah, cpu->ah)); /* sub ah, ah */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x12), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x12], ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B16)); /* mov bx, word ds:[0x3B16] */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B16, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B16), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B16] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->es, cpu->bx)); /* mov al, byte es:[bx] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x14), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x14], ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B16)); /* mov bx, word ds:[0x3B16] */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B16, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B16), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B16] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->es, cpu->bx)); /* mov al, byte es:[bx] */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    cpu->ax = (uint16_t)(0x12C);             /* mov ax, 0x12C */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    { uint32_t _n = ((uint32_t)cpu->dx << 16) | cpu->ax; uint16_t _d = cpu->cx; cpu->ax = (uint16_t)(_n / _d); cpu->dx = (uint16_t)(_n % _d); } /* div cx */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x1A), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x1A], ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x14))); /* push word ss:[bp+-0x14] */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x12))); /* push word ss:[bp+-0x12] */
    push16(cpu, cpu->ax);                    /* push ax */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B18, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B18), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B18] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x3B18)); /* push word ds:[0x3B18] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0374(cpu);                      /* call 0000:0374 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
L_ovl07_035B6E_000329:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B16)); /* mov bx, word ds:[0x3B16] */
    cpu->es = (uint16_t)(mem_read16(cpu, cpu->ds, 0x63B4)); /* mov es, word ds:[0x63B4] */
    flags_cmp8(cpu, mem_read8(cpu, cpu->es, cpu->bx), 0x0); /* cmp byte es:[bx], 0x0 */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_0002D4; /* jne 0x02D4 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x3B16, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x3B16), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x3B16] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(0x1)); /* mov word ss:[bp+-0x16], 0x1 */
    goto L_ovl07_035B6E_000352;              /* jmp 0x0352 */
L_ovl07_035B6E_000344:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16))); /* push word ss:[bp+-0x16] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_03EC(cpu);                      /* call 0000:03EC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x16] */
L_ovl07_035B6E_000352:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B18)); /* mov ax, word ds:[0x3B18] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), cpu->ax); /* cmp word ss:[bp+-0x16], ax */
    if (cc_le(cpu)) goto L_ovl07_035B6E_000344; /* jle 0x0344 */
L_ovl07_035B6E_00035A:;
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_022418(cpu);                         /* call 205A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    flags_cmp16(cpu, cpu->ax, 0x13);         /* cmp ax, 0x13 */
    if (cc_le(cpu)) goto L_ovl07_035B6E_00036F; /* jle 0x036F */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x6798, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x6798), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x6798] */
L_ovl07_035B6E_00036F:;
    cpu->ax = (uint16_t)(0x7FFF);            /* mov ax, 0x7FFF */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x2D);              /* mov ax, 0x2D */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x6798); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x6798] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ds, 0x6792))); /* add ax, word ds:[0x6792] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6794)); /* mov bx, word ds:[0x6794] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->es = (uint16_t)(mem_read16(cpu, cpu->ds, 0x63B4)); /* mov es, word ds:[0x63B4] */
    push16(cpu, mem_read16(cpu, cpu->es, (uint16_t)(cpu->bx + 0x83))); /* push word es:[bx+0x83] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1DDE_007C(cpu);                      /* call 1DDE:007C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    mem_write16(cpu, cpu->ds, 0x6798, (uint16_t)(cpu->ax)); /* mov word ds:[0x6798], ax */
    goto L_ovl07_035B6E_0003EF;              /* jmp 0x03EF */
L_ovl07_035B6E_00039B:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x18)), 0xFFFF); /* cmp word ss:[bp+-0x18], 0xFFFF */
    if (cc_ne(cpu)) goto L_ovl07_035B6E_0003AD; /* jne 0x03AD */
    cpu->ax = (uint16_t)(0xB4);              /* mov ax, 0xB4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl07_035B6E_0003AD:;
    push16(cpu, mem_read16(cpu, cpu->ds, 0x6796)); /* push word ds:[0x6796] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_020B20(cpu);                         /* call 205A:059A */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001ECA(cpu);                         /* call 01A7:046E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_0003E9; /* je 0x03E9 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(0x1)); /* mov word ss:[bp+-0x16], 0x1 */
    goto L_ovl07_035B6E_0003E1;              /* jmp 0x03E1 */
L_ovl07_035B6E_0003D3:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16))); /* push word ss:[bp+-0x16] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_041D(cpu);                      /* call 0000:041D */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x16] */
L_ovl07_035B6E_0003E1:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x3B18)); /* mov ax, word ds:[0x3B18] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x16)), cpu->ax); /* cmp word ss:[bp+-0x16], ax */
    if (cc_le(cpu)) goto L_ovl07_035B6E_0003D3; /* jle 0x03D3 */
L_ovl07_035B6E_0003E9:;
    mem_write16(cpu, cpu->ds, 0x3B12, (uint16_t)(0x2)); /* mov word ds:[0x3B12], 0x2 */
L_ovl07_035B6E_0003EF:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0838(cpu);                      /* call 0000:0838 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x1)); /* mov word ds:[bx], 0x1 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x3B12), 0x2); /* cmp word ds:[0x3B12], 0x2 */
    if (cc_e(cpu)) goto L_ovl07_035B6E_00040E; /* je 0x040E */
L_ovl07_035B6E_000409:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    goto L_ovl07_035B6E_000410;              /* jmp 0x0410 */
L_ovl07_035B6E_00040E:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_ovl07_035B6E_000410:;
    cpu->sp = (uint16_t)(cpu->bp);           /* mov sp, bp */
    cpu->bp = (uint16_t)(pop16(cpu));        /* pop bp */
    cpu->sp += 4; return;                    /* retf */
}

/* ─── File read: far_205A_30E4 ─── */
/* far_205A_30E4 - DOS file read wrapper (_dos_read).
 * Stack: [ret_addr 4] [handle 2] [buf_off 2] [buf_seg 2] [size 2] [result_ptr 2]
 * Reads 'size' bytes from file 'handle' into buf_seg:buf_off.
 * Stores bytes-read count at SS:result_ptr.
 * Returns AX = bytes read (or 0 on error). */
void far_205A_30E4(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4);
    uint16_t handle_or_fp = mem_read16(cpu, cpu->ss, sp);
    uint16_t buf_off   = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    uint16_t buf_seg   = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 4));
    uint16_t size      = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 6));
    uint16_t res_ptr   = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 8));

    /* The first arg may be a small DOS handle, our FILE* token, or some
     * garbage (e.g. lifted code that uses _lastiob indirectly). Resolve
     * via the host-side table when it looks like a token. */
    uint16_t handle = handle_or_fp;
    int slot = civ_file_token_to_slot(handle_or_fp);
    if (slot >= 0) {
        handle = g_civ_files[slot].dos_handle;
    } else if (handle_or_fp > 20) {
        /* Try DS:0x686C (game's stored "current FILE") */
        uint16_t fp = mem_read16(cpu, cpu->ds, 0x686C);
        slot = civ_file_token_to_slot(fp);
        if (slot >= 0) {
            handle = g_civ_files[slot].dos_handle;
        } else {
            /* Last resort: any open slot */
            for (int s = 0; s < CIV_FILE_MAX; s++) {
                if (g_civ_files[s].in_use) {
                    handle = g_civ_files[s].dos_handle;
                    break;
                }
            }
        }
    }

    DosState *dos = get_dos_state(cpu);
    uint16_t got = 0;

    if (handle < DOS_MAX_HANDLES && dos->file_table.files[handle]) {
        uint32_t dest = seg_off(buf_seg, buf_off);
        if (dest + size <= MEM_SIZE) {
            size_t n = fread(cpu->mem + dest, 1, size, dos->file_table.files[handle]);
            got = (uint16_t)n;
            static int rc = 0; rc++;
            if (rc <= 8) fprintf(stderr, "[READ] h=%d %u bytes -> %zu\n", handle, size, n);
        }
    } else {
        static int fc = 0; fc++;
        if (fc <= 5) {
            fprintf(stderr, "[READ] FAIL: hfp=%04X handle=%d 686C=%04X slot=%d\n",
                    handle_or_fp, handle, mem_read16(cpu, cpu->ds, 0x686C), slot);
        }
    }

    /* Store result at SS:result_ptr */
    mem_write16(cpu, cpu->ss, res_ptr, got);
    cpu->ax = got;
    cpu->sp += 4; /* far ret */
}

/* ─── MSC 5.x CRT text-stream chain (scanf %s/%[ field reader) ───────────────
 * Implemented (2026-05-31) so the real intro/title (ovl02_02C200) can parse
 * credits.txt and reach the menu. The hand-implemented fopen returns an opaque
 * host token (not a real FILE struct with buffer fields), so MSC's buffered
 * getc/_filbuf path reads garbage and never hits EOF. These reimplement the
 * chain against the host FILE table (DOS handle -> host C FILE*), bypassing the
 * MSC buffer. All are FAR (RETF -> cpu->sp += 4). Stream state in DGROUP:
 *   [0x6AA0] FILE*(token)  [0x6AA4] err/eof flag  [0x6AA8] scanset mode
 *   [0x6AAA] eof count     [0x6AAC] dest arg ptr  [0x6AAE] scanset table
 *   [0x6AB0]/[0x6AB2] width  [0x6AB4] no-store  [0x6AB6] fields read  [0x6AB8] chars
 * _ctype[] table is the static data at DS:0x5A59 (ctype[c] = [0x5A59+c]). */
void res_021BC8(CPU *cpu);  void res_021BEC(CPU *cpu);
void res_021C22(CPU *cpu);  void far_215A_16DA(CPU *cpu);

/* getc: next byte from the FILE token at DS:[0x6AA0], or EOF (-1). */
void res_021BC8(CPU *cpu)
{
    mem_write16(cpu, cpu->ds, 0x6AB8,
                (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AB8) + 1)); /* inc char count */
    uint16_t tok = mem_read16(cpu, cpu->ds, 0x6AA0);
    int slot = civ_file_token_to_slot(tok);
    int c = -1;
    if (slot >= 0) {
        DosState *dos = get_dos_state(cpu);
        uint8_t h = g_civ_files[slot].dos_handle;
        if (h < DOS_MAX_HANDLES && dos->file_table.files[h])
            c = fgetc(dos->file_table.files[h]);
    }
    cpu->ax = (c < 0) ? 0xFFFF : (uint16_t)(c & 0xFF);
    cpu->sp += 4; /* far ret */
}

/* ungetc(char, FILE): push a char back onto the host stream. Args (cdecl):
 * [sp+4]=char, [sp+6]=FILE token. */
void far_215A_16DA(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4);
    int16_t  ch  = (int16_t)mem_read16(cpu, cpu->ss, sp);
    uint16_t tok = mem_read16(cpu, cpu->ss, (uint16_t)(sp + 2));
    int slot = civ_file_token_to_slot(tok);
    if (slot >= 0 && ch >= 0) {
        DosState *dos = get_dos_state(cpu);
        uint8_t h = g_civ_files[slot].dos_handle;
        if (h < DOS_MAX_HANDLES && dos->file_table.files[h])
            ungetc((int)(ch & 0xFF), dos->file_table.files[h]);
    }
    cpu->ax = (uint16_t)(ch & 0xFF);
    cpu->sp += 4; /* far ret */
}

/* field-width check: AX=1 if more chars allowed, 0 if width exhausted. */
void res_021C22(CPU *cpu)
{
    if (mem_read16(cpu, cpu->ds, 0x6AB0) == 0) {
        cpu->ax = 1;
    } else if ((int16_t)mem_read16(cpu, cpu->ds, 0x6AB2) > 0) {
        mem_write16(cpu, cpu->ds, 0x6AB2,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AB2) - 1));
        cpu->ax = 1;
    } else {
        cpu->ax = 0;
    }
    cpu->sp += 4; /* far ret */
}

/* skip leading whitespace, peek the next char, push it back (ungetc). */
void res_021BEC(CPU *cpu)
{
    uint16_t si;
L_loop:
    push16(cpu, cpu->cs); push16(cpu, 0); res_021BC8(cpu);   /* getc */
    si = cpu->ax;
    if (mem_read8(cpu, cpu->ds, (uint16_t)(si + 0x5A59)) & 8) goto L_loop; /* ws */
    if (si != 0xFFFF) {
        mem_write16(cpu, cpu->ds, 0x6AB8,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AB8) - 1));
        push16(cpu, mem_read16(cpu, cpu->ds, 0x6AA0)); /* FILE */
        push16(cpu, si);                                /* char */
        push16(cpu, cpu->cs); push16(cpu, 0); far_215A_16DA(cpu);
        cpu->sp = (uint16_t)(cpu->sp + 4);              /* clean 2 args */
    } else {
        mem_write16(cpu, cpu->ds, 0x6AAA,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AAA) + 1)); /* eof++ */
    }
    cpu->ax = si;
    cpu->sp += 4; /* far ret */
}

/* scanf field reader: reads a whitespace/scanset-delimited field into the
 * destination buffer, with width limits + ungetc of the delimiter. Faithful
 * port of the 0x2178A routine. Arg [sp+4] = store-result flag. */
void res_02178A(CPU *cpu)
{
    uint16_t arg = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t bx  = mem_read16(cpu, cpu->ds, 0x6AAC);
    uint16_t si  = mem_read16(cpu, cpu->ds, bx);   /* dest buffer pos */
    uint16_t si_start = si;
    uint16_t di = 0;
    uint16_t ax = 0;
    if (mem_read16(cpu, cpu->ds, 0x6AB4) == 0)
        mem_write16(cpu, cpu->ds, 0x6AAC, (uint16_t)(bx + 2));
    if (mem_read16(cpu, cpu->ds, 0x6AA4) != 0) goto Lcleanup;
    if (arg == 0) goto L217e3;
    if (mem_read16(cpu, cpu->ds, 0x6AA8) != 0) goto L217e3;
    push16(cpu, cpu->cs); push16(cpu, 0); res_021BEC(cpu);  /* skip leading ws */
    goto L217e3;
L217c4:
    { uint8_t cl = (uint8_t)(mem_read8(cpu, cpu->ds, (uint16_t)(di + 0x5A59)) & 8);
      ax = (cl == 0) ? 1u : 0u; }                 /* not-whitespace ? 1 : 0 */
L217d3:
    if (ax == 0) goto L2180a;                      /* whitespace -> end field */
L217d7:
    if (mem_read16(cpu, cpu->ds, 0x6AB4) != 0) goto L217e3;
    mem_write8(cpu, cpu->ds, si, (uint8_t)(di & 0xFF));  /* store char */
    si = (uint16_t)(si + 1);
L217e3:
    push16(cpu, cpu->cs); push16(cpu, 0); res_021C22(cpu);
    if (cpu->ax == 0) goto L2180a;                 /* width exhausted */
    push16(cpu, cpu->cs); push16(cpu, 0); res_021BC8(cpu);  /* getc */
    di = cpu->ax;
    if ((uint16_t)(di + 1) == 0) goto L2180a;      /* EOF */
    if (arg == 0) goto L217d7;
    if (mem_read16(cpu, cpu->ds, 0x6AA8) == 0) goto L217c4;
    { uint16_t sb = mem_read16(cpu, cpu->ds, 0x6AAE);
      int8_t v = (int8_t)mem_read8(cpu, cpu->ds, (uint16_t)(sb + di));
      ax = (uint16_t)(int16_t)v; }                 /* scanset membership (cwde) */
    goto L217d3;
L2180a:
    if ((uint16_t)(di + 1) == 0) {                 /* di == -1 (EOF) */
        mem_write16(cpu, cpu->ds, 0x6AAA,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AAA) + 1));
        goto L2185c;
    }
    if (arg == 0) goto L2185c;
    if (mem_read16(cpu, cpu->ds, 0x6AB0) == 0) goto L2184b;
    if (mem_read16(cpu, cpu->ds, 0x6AB2) != 0) goto L2184b;
    if (mem_read16(cpu, cpu->ds, 0x6AA8) != 0) {
        uint16_t sb = mem_read16(cpu, cpu->ds, 0x6AAE);
        uint8_t v = mem_read8(cpu, cpu->ds, (uint16_t)(sb + di));
        ax = (v == 0) ? 1u : 0u;
    } else {
        ax = (uint16_t)(mem_read8(cpu, cpu->ds, (uint16_t)(di + 0x5A59)) & 8);
    }
    if (ax == 0) goto L2185c;
L2184b:
    mem_write16(cpu, cpu->ds, 0x6AB8,
                (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AB8) - 1));
    push16(cpu, mem_read16(cpu, cpu->ds, 0x6AA0));
    push16(cpu, di);
    push16(cpu, cpu->cs); push16(cpu, 0); far_215A_16DA(cpu);
    cpu->sp = (uint16_t)(cpu->sp + 4);
L2185c:
    if (mem_read16(cpu, cpu->ds, 0x6AB4) != 0) goto Lcleanup;
    if (arg != 0) mem_write8(cpu, cpu->ds, si, 0);   /* null-terminate */
    if (si_start != si)
        mem_write16(cpu, cpu->ds, 0x6AB6,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x6AB6) + 1));
Lcleanup:
    cpu->sp += 4; /* far ret */
}

/* ─── Climate/temperature: far_205A_2AC0 ─── */
/* far_205A_2AC0 - Returns abs(arg). Used in terrain type assignment to
 * convert signed latitude offset to unsigned distance from equator.
 * Stack: [ret_addr 4] [latitude 2]
 * Returns AX = abs(latitude). */
void far_205A_2AC0(CPU *cpu)
{
    uint16_t sp = (uint16_t)(cpu->sp + 4);
    int16_t arg = (int16_t)mem_read16(cpu, cpu->ss, sp);
    cpu->ax = (uint16_t)(arg < 0 ? -arg : arg);
    cpu->sp += 4; /* far ret */
}

/* ─── Timer delay: far_1DDE_007C ─── */
/* far_1DDE_007C - Animation timing/delay function.
 * Stack: [ret_addr 4] [delay_target 2] [current_time 2] [max_time 2]
 * Returns AX = next timing value. Used in animation loop timing.
 * With animation skipped, this rarely gets called but we implement it
 * to avoid issues if other code paths use it. */
/* far_1DDE_007C == res_01DE5C (resident 0x1DE5C): signed int clamp.
 *   return min(max(value, lo), hi)
 * Args (far): [sp+4]=value, [sp+6]=lo, [sp+8]=hi.
 *
 * Previously mis-implemented as a "delay" that returned arg3 (the hi bound).
 * The terrain-reveal state machine (ovl07_035B6E) stores this return into its
 * timer target 0x6798 via clamp(per_tile, 0x2D*0x6798+timer, 0x7FFF); returning
 * 0x7FFF made the target ~32767 ticks (~30 min) so the reveal's "wait until
 * timer>=target" loop spun forever and the game never reached the map. The
 * correct clamp yields a small next-step target so the reveal advances. */
void far_1DDE_007C(CPU *cpu)
{
    int16_t value = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    int16_t lo    = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    int16_t hi    = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    cpu->ax = (uint16_t)value;
    cpu->sp += 4; /* far ret */
}

/* ─── Signal handlers ─── */
/* res_001E52 - MSC signal handler setup (safe no-op). */
void res_001E52(CPU *cpu)
{
    (void)cpu;
    cpu->sp += 2; /* near ret */
}

/* ─── MSC CRT: _aFchkstk (far stack check) ─── */
/* far_205A_1B8C - MSC _aFchkstk (far stack available check).
 * Original: POP CX; POP DX (save return addr); check stack limit at DS:58EA;
 * return AX = available stack space (or 0 if overflow); PUSH DX; PUSH CX; RETF.
 * NOT an allocator - just checks available space. We always report plenty. */
void far_205A_1B8C(CPU *cpu)
{
    uint16_t ret_ip = pop16(cpu);
    uint16_t ret_cs = pop16(cpu);
    uint16_t limit = mem_read16(cpu, cpu->ds, 0x58EA);
    if (limit >= cpu->sp) {
        cpu->ax = 0;
    } else {
        cpu->ax = (uint16_t)(cpu->sp - limit);
    }
    static int cc = 0; cc++;
    if (cc <= 5)
        fprintf(stderr, "[CHKSTK] #%d sp=%04X limit=%04X avail=%04X\n",
                cc, cpu->sp, limit, cpu->ax);
    push16(cpu, ret_cs);
    push16(cpu, ret_ip);
    cpu->sp += 4; /* retf */
}

/* ─── MSC CRT: res_0207C7 (near stack helper) ─── */
/* res_0207C7 - Near internal CRT helper for buffer management.
 * This is a mid-function entry point - just return cleanly. */
void res_0207C7(CPU *cpu)
{
    cpu->sp += 2; /* near ret */
}

/* ─── MSC CRT: res_0220AA (buffer write/flush) ─── */
/* res_0220AA - Internal CRT subroutine to write buffer to file via INT 21h.
 * Called as NEAR from within far_205A_1A62 (_read/_write implementation).
 * Original: PUSH AX/BX/CX; CX = DI-DX (bytes); INT 21h/AH=40h (write);
 *           ADD [BP-2],AX (count); POP CX/BX/AX; DI=DX (reset). */
void res_0220AA(CPU *cpu) {
    uint16_t saved_ax = cpu->ax;
    uint16_t saved_bx = cpu->bx;
    uint16_t saved_cx = cpu->cx;

    uint16_t count = (uint16_t)(cpu->di - cpu->dx); /* bytes to write */
    if (count == 0) {
        cpu->sp += 2; /* near ret */
        return;
    }

    /* Call DOS write: AH=40h, BX=handle from [BP+6], CX=count, DS:DX=buffer */
    uint16_t handle = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6));
    cpu->bx = handle;
    cpu->cx = count;
    cpu->ah = 0x40;
    dos_int21(cpu);

    if (!(cpu->flags & FLAG_CF)) {
        /* Success: add bytes written to [BP-2] */
        uint16_t prev = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2));
        mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2), (uint16_t)(prev + cpu->ax));
    }

    /* Reset buffer pointer */
    cpu->di = cpu->dx;

    /* Restore registers */
    cpu->cx = saved_cx;
    cpu->bx = saved_bx;
    cpu->ax = saved_ax;

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

/* Timer speed multiplier - speeds up animation during world gen */
static int timer_speed = 20;

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

    /* Update timer with real wall-clock time (scaled by speed multiplier) */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC * timer_speed;
    timer_update(&dos->timer, ms);

    delay_start_ticks = timer_get_ticks(&dos->timer);
    if (call_count <= 5) {
        fprintf(stderr, "[TIMER] save #%d tick=%u (speed=%dx)\n", call_count, delay_start_ticks, timer_speed);
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

    /* Update timer with real wall-clock time (scaled by speed multiplier) */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC * timer_speed;
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

    /* Update timer from wall-clock time (scaled by speed multiplier) */
    uint64_t ms = (uint64_t)clock() * 1000ULL / CLOCKS_PER_SEC * timer_speed;
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

/* ─── res_02120A: MSC _getbuf (un-stubbed 2026-05-31, task #7) ───
 * Allocates a FILE's 512-byte stream buffer on first read and inits the
 * descriptor (cnt=0, ptr=buf) so the getc/fread chain then calls the real
 * _filbuf to read. Faithful lift from the dump (CIV.EXE _getbuf @0x2120A,
 * NEAR). The far call to 0x215A:0x1BB2 is the MSC buffer allocator, lifted as
 * res_022138 (far). king.txt (opened by the REAL MSC fopen far_205A_0696) is
 * read through this path; the prior no-op stub left FILE cnt/ptr garbage. */
void res_022138(CPU *cpu);  /* MSC malloc/_getbuf-core (far), civ_recomp_006.c */
void res_02120A(CPU *cpu)
{
    push16(cpu, cpu->bp);                    /* push bp */
    cpu->bp = (uint16_t)(cpu->sp);           /* mov bp, sp */
    cpu->sp = (uint16_t)(flags_sub16(cpu, cpu->sp, 0x2)); /* sub sp, 0x2 */
    push16(cpu, cpu->si);                    /* push si */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x4))); /* mov ax, word ss:[bp+0x4] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x590A)); /* sub ax, 0x590A */
    cpu->cl = (uint8_t)(0x3);                /* mov cl, 0x3 */
    { int16_t _v = (int16_t)cpu->ax; uint8_t _c = cpu->cl; int16_t _r = _v >> _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (_c - 1)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, (uint16_t)_r); cpu->ax = (uint16_t)((uint16_t)_r); } /* sar ax, cl */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    { uint16_t _v = cpu->ax; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* shl ax, 0x1 */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, cpu->cx)); /* add ax, cx */
    { uint16_t _v = cpu->ax; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* shl ax, 0x1 */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x59AA)); /* add ax, 0x59AA */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x2], ax */
    cpu->ax = (uint16_t)(0x200);             /* mov ax, 0x200 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_022138(cpu);                         /* call 215A:1BB2 (MSC buffer alloc) */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x4))); /* mov bx, word ss:[bp+0x4] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x4), (uint16_t)(cpu->ax)); /* mov word ds:[bx+0x4], ax */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_res_02120A_000044; /* je 0x0044 */
    { uint8_t _r = mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x6)) | 0x8; flags_logic8(cpu, _r); mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x6), (uint8_t)(_r)); } /* or byte ds:[bx+0x6], 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2))); /* mov bx, word ss:[bp+-0x2] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2), (uint16_t)(0x200)); /* mov word ds:[bx+0x2], 0x200 */
    goto L_res_02120A_00005A;                /* jmp 0x005A */
L_res_02120A_000044:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x4))); /* mov bx, word ss:[bp+0x4] */
    { uint8_t _r = mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x6)) | 0x4; flags_logic8(cpu, _r); mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x6), (uint8_t)(_r)); } /* or byte ds:[bx+0x6], 0x4 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2))); /* mov ax, word ss:[bp+-0x2] */
    { int _cf = cf(cpu); cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 1)); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc ax */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x4), (uint16_t)(cpu->ax)); /* mov word ds:[bx+0x4], ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2))); /* mov bx, word ss:[bp+-0x2] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2), (uint16_t)(0x1)); /* mov word ds:[bx+0x2], 0x1 */
L_res_02120A_00005A:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x4))); /* mov bx, word ss:[bp+0x4] */
    cpu->si = (uint16_t)(cpu->bx);           /* mov si, bx */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->si + 0x4))); /* mov ax, word ds:[si+0x4] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2), (uint16_t)(0x0)); /* mov word ds:[bx+0x2], 0x0 */
    cpu->si = (uint16_t)(pop16(cpu));        /* pop si */
    cpu->sp = (uint16_t)(cpu->bp);           /* mov sp, bp */
    cpu->bp = (uint16_t)(pop16(cpu));        /* pop bp */
    cpu->sp += 2; return;                    /* ret */
}

/* ─── Traced alias wrappers ─── */
/* far_0000_0768 - Alias for ovl02_02C200 (title screen, now un-bypassed) */
void ovl02_02C200(CPU *cpu);  /* forward decl - defined below */
/* far_0000_0768 - Free-memory query (resident offset 0x0768), called in
 * res_001A66 AFTER the title (ovl02_02C200) to decide whether to show the
 * *LOMEM low-memory warning. res_001A66 compares the return value (AX) against
 * a threshold (0xFA0 or 0x1F40); if AX < threshold it loads king.txt's *LOMEM
 * message and shows a dialog that then blocks the whole startup.
 *
 * This was previously (wrongly) aliased to ovl02_02C200: it happened to work
 * only while the title was a stub returning AX=0x6000. With the title now
 * UN-BYPASSED, calling ovl02_02C200 here both re-ran the title and returned a
 * small AX -> LOMEM false alarm. We have ample host memory, so report a large
 * free-memory figure (>= the 0x1F40 graphics threshold) to skip LOMEM. */
void far_0000_0768(CPU *cpu)
{
    cpu->ax = 0x6000;   /* plenty of free memory; skip *LOMEM warning */
    cpu->sp += 4;       /* far ret */
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
    ovl05_0307DA(cpu);
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

/* ─── Overlay manager init: res_000A54 ─── */
/* res_000A54 - MSC overlay manager initialization.
 * Original: allocates memory, loads overlay EXE via INT 21h/4Bh fn 03h,
 * sets up the far heap descriptor at DS:0x53B8, and patches INT 3Fh thunks.
 * In our recompilation all overlay code is compiled in, so we just allocate
 * a memory block for the heap descriptor and return the segment.
 *
 * Stack (far call, cdecl, caller cleans 4 bytes):
 *   [sp+04] filename_offset  (DS-relative pointer to overlay filename)
 *   [sp+06] flags            (0 = don't call res_000AFC)
 * Returns: AX = allocated segment.
 */
void res_000A54(CPU *cpu)
{
    /* Query available DOS memory */
    cpu->ah = 0x48;
    cpu->bx = 0xFFFF;
    dos_int21(cpu);  /* CF set, BX = available paragraphs */

    /* Allocate available - 0x100 paragraphs */
    uint16_t alloc_size = cpu->bx - 0x100;
    cpu->ah = 0x48;
    cpu->bx = alloc_size;
    dos_int21(cpu);  /* AX = segment, CF clear on success */
    if (cpu->flags & FLAG_CF) {
        fprintf(stderr, "[FATAL] res_000A54: memory allocation failed\n");
        exit(1);
    }

    uint16_t seg = cpu->ax;
    mem_write16(cpu, cpu->ds, 0x53BC, alloc_size);  /* store size */
    mem_write16(cpu, cpu->ds, 0x53B8, seg);          /* heap base */
    mem_write16(cpu, cpu->ds, 0x53BA, seg);          /* heap base copy */

    /* Zero the overlay header area so res_000B4E reads safe values */
    uint32_t base = (uint32_t)seg * 16;
    for (int i = 0; i < 0x40 && base + i < MEM_SIZE; i++)
        cpu->mem[base + i] = 0;

    fprintf(stderr, "[RUNTIME] res_000A54: allocated %u paras at seg 0x%04X\n",
            alloc_size, seg);

    cpu->ax = seg;
    cpu->sp += 4; /* far ret */
}

/* ─── Overlay thunk table init: res_000B4E ─── */
/* res_000B4E - Patches INT 3Fh thunk table from loaded overlay header.
 * In our recompilation overlay thunks are direct C function calls,
 * so this is a no-op.
 * Stack (far call, cdecl, caller cleans 2 bytes):
 *   [sp+04] segment of loaded overlay
 */
void res_000B4E(CPU *cpu)
{
    (void)cpu;
    fprintf(stderr, "[RUNTIME] res_000B4E (thunk table init) - skipped\n");
    cpu->sp += 4; /* far ret */
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
            /* No key available - auto-select option 0 (New Game on main menu).
             * The game's main menu maps: 0=New Game, 1=Load, 2=Scenario, etc.
             * Previously stubs returned garbage AX which fell through to
             * the new-game path; now that dialog code is real, we must
             * explicitly select 0. */
            mem_write16(cpu, cpu->ds, (uint16_t)(ctrl_off - 2), 0);
            fprintf(stderr, "[DIALOG] auto-selected 0 (no key available)\n");
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
        /* (Removed the '1' auto-inject: it was a hack for the now-bypassed
         * intro's digit-validation loop, but it pollutes the title menu's
         * getkey, which only accepts N/L/E/C. Real input / CIV_AUTOKEY drive
         * the menu now.) */
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

    /* The real intro establishes MCGA mode 13h (320x200x256) before any
     * graphics are drawn; main's res_001CAE (the proper mode setter) sits
     * after the title attract loop and is never reached, so replicate the
     * intro's side effect here. Set the mode ONCE: the attract loop re-enters
     * this bypass every iteration, and re-running the BIOS mode-set would
     * memset A0000 and wipe the title that was just drawn. Only do the
     * mode set (and its framebuffer clear) on the first transition into 13h. */
    if (mem_read8(cpu, 0x0040, 0x0049) != 0x13) {
        extern void bios_int10(CPU *cpu);
        uint16_t saved_ax = cpu->ax;
        cpu->ax = 0x0013;       /* AH=00 set mode, AL=13h */
        bios_int10(cpu);
        cpu->ax = saved_ax;
        /* Keep res_001CAE's VGA ref count consistent so a later real call
         * (ref 0->1) won't re-issue the mode set and clear the screen. */
        mem_write16(cpu, cpu->ds, 0xEE1A, 1);
        fprintf(stderr, "[INTRO] Set MCGA mode 13h (A0000 graphics)\n");
    }

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
/* ovl02_02C200 - Title screen + intro + main menu (N/L/E/C key handler).
 * UN-BYPASSED 2026-05-31 (task #6): real lifted body (CIV.EXE @0x2C200,
 * 1121 insts). Runs logo/birth/credits via the CRT text chain, then the
 * menu at the tail (sets [0x6AC2] from N/L/E/C). To re-bypass, restore the
 * stub from civ_impl.c.prebypass.bak. */
extern void far_0000_0000(CPU *cpu);
extern void far_0000_0330(CPU *cpu);
extern void far_0000_0374(CPU *cpu);
extern void far_0000_03EC(CPU *cpu);
extern void far_0000_041D(CPU *cpu);
extern void far_0000_065C(CPU *cpu);
extern void far_0000_0761(CPU *cpu);
extern void far_0000_076F(CPU *cpu);
extern void far_0000_0792(CPU *cpu);
extern void far_0000_07A7(CPU *cpu);
extern void far_0000_07DF(CPU *cpu);
extern void far_0000_07ED(CPU *cpu);
extern void far_0000_0810(CPU *cpu);
extern void far_0000_081E(CPU *cpu);
extern void far_0000_0838(CPU *cpu);
extern void far_0000_083F(CPU *cpu);
extern void far_0000_0864(CPU *cpu);
extern void far_0000_0A1D(CPU *cpu);
extern void far_0000_0A40(CPU *cpu);
extern void far_0000_0BEC(CPU *cpu);
extern void far_0402_44E9(CPU *cpu);
extern void far_1D1F_0AC9(CPU *cpu);
extern void far_1DDE_0042(CPU *cpu);
extern void far_1DDE_0523(CPU *cpu);
extern void far_1FB6_021E(CPU *cpu);
extern void far_1FB6_026A(CPU *cpu);
extern void far_205A_1E60(CPU *cpu);
extern void far_205A_2096(CPU *cpu);
extern void far_205A_20AA(CPU *cpu);
extern void ovl02_02DBA3(CPU *cpu);
extern void res_000A54(CPU *cpu);
extern void res_000B4E(CPU *cpu);
extern void res_00102D(CPU *cpu);
extern void res_0018B1(CPU *cpu);
extern void res_001932(CPU *cpu);
extern void res_001A0F(CPU *cpu);
extern void res_001A5A(CPU *cpu);
extern void res_001D02(CPU *cpu);
extern void res_001ECA(CPU *cpu);
extern void res_002339(CPU *cpu);
extern void res_020C1C(CPU *cpu);
extern void res_022418(CPU *cpu);

void ovl02_02C200(CPU *cpu)
{
    push16(cpu, cpu->bp);                    /* push bp */
    cpu->bp = (uint16_t)(cpu->sp);           /* mov bp, sp */
    cpu->sp = (uint16_t)(flags_sub16(cpu, cpu->sp, 0x4E)); /* sub sp, 0x4E */
    push16(cpu, cpu->si);                    /* push si */
    mem_write16(cpu, cpu->ds, 0xEDEA, (uint16_t)(0x1)); /* mov word ds:[0xEDEA], 0x1 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x1)); /* mov word ss:[bp+-0x48], 0x1 */
L_ovl02_02C200_000012:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001D02(cpu);                         /* call 01A7:02A6 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_000035; /* je 0x0035 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* mov ax, word ss:[bp+-0x48] */
    { int _cf = cf(cpu); cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 1)); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc ax */
    mem_write16(cpu, cpu->ds, 0xEDEA, (uint16_t)(cpu->ax)); /* mov word ds:[0xEDEA], ax */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x8); /* cmp word ss:[bp+-0x48], 0x8 */
    if (cc_l(cpu)) goto L_ovl02_02C200_000012; /* jl 0x0012 */
L_ovl02_02C200_000035:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4C), (uint16_t)(0x0)); /* mov word ss:[bp+-0x4C], 0x0 */
L_ovl02_02C200_00003A:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x31FA);            /* mov ax, 0x31FA */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_065C(cpu);                      /* call 0000:065C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    { int _cf = cf(cpu); cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 1)); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_000051; /* je 0x0051 */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    goto L_ovl02_02C200_000053;              /* jmp 0x0053 */
L_ovl02_02C200_000051:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_ovl02_02C200_000053:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4C), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x4C], ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_002339(cpu);                         /* call 01A7:08DD */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x1)); /* mov word ss:[bp+-0x48], 0x1 */
L_ovl02_02C200_00006A:;
    push16(cpu, mem_read16(cpu, cpu->ds, 0x31F8)); /* push word ds:[0x31F8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001A5A(cpu);                         /* call 0198:00EE */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xC); /* cmp word ss:[bp+-0x48], 0xC */
    if (cc_le(cpu)) goto L_ovl02_02C200_00006A; /* jle 0x006A */
    cpu->ax = (uint16_t)(0x3203);            /* mov ax, 0x3203 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xD);               /* mov ax, 0xD */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001A0F(cpu);                         /* call 0198:00A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4C)), 0x0); /* cmp word ss:[bp+-0x4C], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_0000AE; /* je 0x00AE */
    cpu->ax = (uint16_t)(0x3206);            /* mov ax, 0x3206 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1D);              /* mov ax, 0x1D */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x10);              /* mov ax, 0x10 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001A0F(cpu);                         /* call 0198:00A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    goto L_ovl02_02C200_0000E5;              /* jmp 0x00E5 */
L_ovl02_02C200_0000AE:;
    cpu->ax = (uint16_t)(0x321B);            /* mov ax, 0x321B */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E60(cpu);                      /* call 205A:1E60 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0x6);               /* mov ax, 0x6 */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    cpu->cx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xEDEA)); /* mov cx, word ds:[0xEDEA] */
    { int32_t _n = (int32_t)(((uint32_t)cpu->dx << 16) | cpu->ax); int16_t _d = (int16_t)cpu->cx; cpu->ax = (uint16_t)(int16_t)(_n / _d); cpu->dx = (uint16_t)(int16_t)(_n % _d); } /* idiv cx */
    mem_write8(cpu, cpu->ds, 0xC949, (uint8_t)(flags_add8(cpu, mem_read8(cpu, cpu->ds, 0xC949), cpu->al))); /* add byte ds:[0xC949], al */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1D);              /* mov ax, 0x1D */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x10);              /* mov ax, 0x10 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001A0F(cpu);                         /* call 0198:00A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_20AA(cpu);                      /* call 205A:20AA */
L_ovl02_02C200_0000E5:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4C)), 0x0); /* cmp word ss:[bp+-0x4C], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0000EE; /* jne 0x00EE */
    goto L_ovl02_02C200_00003A;              /* jmp 0x003A */
L_ovl02_02C200_0000EE:;
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, 0x1A22), 0x6D); /* cmp byte ds:[0x1A22], 0x6D */
    if (cc_e(cpu)) goto L_ovl02_02C200_0000FC; /* je 0x00FC */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, 0x1A22), 0x4D); /* cmp byte ds:[0x1A22], 0x4D */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000101; /* jne 0x0101 */
L_ovl02_02C200_0000FC:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    goto L_ovl02_02C200_000103;              /* jmp 0x0103 */
L_ovl02_02C200_000101:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_ovl02_02C200_000103:;
    mem_write16(cpu, cpu->ds, 0xE692, (uint16_t)(cpu->ax)); /* mov word ds:[0xE692], ax */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, 0x1A22), 0x65); /* cmp byte ds:[0x1A22], 0x65 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000114; /* je 0x0114 */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, 0x1A22), 0x45); /* cmp byte ds:[0x1A22], 0x45 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000119; /* jne 0x0119 */
L_ovl02_02C200_000114:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    goto L_ovl02_02C200_00011B;              /* jmp 0x011B */
L_ovl02_02C200_000119:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_ovl02_02C200_00011B:;
    mem_write16(cpu, cpu->ds, 0xC13E, (uint16_t)(cpu->ax)); /* mov word ds:[0xC13E], ax */
    cpu->ax = (uint16_t)(0x3231);            /* mov ax, 0x3231 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1A22);            /* mov ax, 0x1A22 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_000A54(cpu);                         /* call 0000:0A68 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_000B4E(cpu);                         /* call 0000:0B62 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1A30);            /* mov ax, 0x1A30 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_000A54(cpu);                         /* call 0000:0A68 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_000B4E(cpu);                         /* call 0000:0B62 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->bx = (uint16_t)(flags_sub16(cpu, cpu->bx, cpu->bx)); /* sub bx, bx */
    cpu->es = (uint16_t)(cpu->bx);           /* mov es, bx */
    cpu->bx = (uint16_t)(0x417);             /* mov bx, 0x417 */
    { uint8_t _r = mem_read8(cpu, cpu->es, cpu->bx) & 0xDF; flags_logic8(cpu, _r); mem_write8(cpu, cpu->es, cpu->bx, (uint8_t)(_r)); } /* and byte es:[bx], 0xDF */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(0x1)); /* mov word ds:[bx+0x10], 0x1 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0864(cpu);                      /* call 0000:0864 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write16(cpu, cpu->ds, 0x6D8C, (uint16_t)(0x0)); /* mov word ds:[0x6D8C], 0x0 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000180; /* je 0x0180 */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    goto L_ovl02_02C200_000183;              /* jmp 0x0183 */
L_ovl02_02C200_000180:;
    cpu->ax = (uint16_t)(0x4);               /* mov ax, 0x4 */
L_ovl02_02C200_000183:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x2], ax */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x0)); /* mov word ss:[bp+-0x48], 0x0 */
    goto L_ovl02_02C200_000196;              /* jmp 0x0196 */
L_ovl02_02C200_00018D:;
    mem_write16(cpu, cpu->ds, 0x6D8C, (uint16_t)(0x1)); /* mov word ds:[0x6D8C], 0x1 */
L_ovl02_02C200_000193:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_000196:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2))); /* mov ax, word ss:[bp+-0x2] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), cpu->ax); /* cmp word ss:[bp+-0x48], ax */
    if (cc_ge(cpu)) goto L_ovl02_02C200_0001BE; /* jge 0x01BE */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0761(cpu);                      /* call 0000:0761 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write16(cpu, cpu->ds, 0x3286, (uint16_t)(cpu->ax)); /* mov word ds:[0x3286], ax */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_00018D; /* je 0x018D */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0810(cpu);                      /* call 0000:0810 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    goto L_ovl02_02C200_000193;              /* jmp 0x0193 */
L_ovl02_02C200_0001BE:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0000(cpu);                      /* call 0000:0000 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A1D(cpu);                      /* call 0000:0A1D */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1DDE_0042(cpu);                      /* call 1DDE:0042 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00020A; /* jne 0x020A */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19FC)); /* push word ds:[0x19FC] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
L_ovl02_02C200_00020A:;
    cpu->ax = (uint16_t)(0x19FE);            /* mov ax, 0x19FE */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_081E(cpu);                      /* call 0000:081E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07DF(cpu);                      /* call 0000:07DF */
    cpu->ax = (uint16_t)(0x323A);            /* mov ax, 0x323A */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_021E(cpu);                      /* call 1FB6:021E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0x50);              /* mov ax, 0x50 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_076F(cpu);                      /* call 0000:076F */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xA)); /* add sp, 0xA */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4E), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x4E], ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0792(cpu);                      /* call 0000:0792 */
    cpu->ax = (uint16_t)(0x3243);            /* mov ax, 0x3243 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_021E(cpu);                      /* call 1FB6:021E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x4C);              /* mov ax, 0x4C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(0x324E);            /* mov ax, 0x324E */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x2);               /* mov ax, 0x2 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_021E(cpu);                      /* call 1FB6:021E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x4C);              /* mov ax, 0x4C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(0x3259);            /* mov ax, 0x3259 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1FB6_026A(cpu);                      /* call 1FB6:026A */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
    cpu->ax = (uint16_t)(0x98);              /* mov ax, 0x98 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x92);              /* mov ax, 0x92 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xE);               /* mov ax, 0xE */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0374(cpu);                      /* call 0000:0374 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_00031C; /* je 0x031C */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x1)); /* mov word ss:[bp+-0x48], 0x1 */
L_ovl02_02C200_000308:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_03EC(cpu);                      /* call 0000:03EC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x3); /* cmp word ss:[bp+-0x48], 0x3 */
    if (cc_le(cpu)) goto L_ovl02_02C200_000308; /* jle 0x0308 */
L_ovl02_02C200_00031C:;
    { uint8_t _r = mem_read8(cpu, cpu->ds, 0x19C0) | 0x10; flags_logic8(cpu, _r); mem_write8(cpu, cpu->ds, 0x19C0, (uint8_t)(_r)); } /* or byte ds:[0x19C0], 0x10 */
    cpu->ax = (uint16_t)(0x3262);            /* mov ax, 0x3262 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3265);            /* mov ax, 0x3265 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_020C1C(cpu);                         /* call 205A:0696 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    mem_write16(cpu, cpu->ds, 0xE698, (uint16_t)(cpu->ax)); /* mov word ds:[0xE698], ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0330(cpu);                      /* call 0000:0330 */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001ECA(cpu);                         /* call 01A7:046E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x0)); /* mov word ss:[bp+-0x48], 0x0 */
    goto L_ovl02_02C200_0004D2;              /* jmp 0x04D2 */
L_ovl02_02C200_000352:;
    cpu->ax = (uint16_t)(0x7);               /* mov ax, 0x7 */
L_ovl02_02C200_000355:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB8);              /* mov ax, 0xB8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000376; /* je 0x0376 */
    cpu->ax = (uint16_t)(0xF8);              /* mov ax, 0xF8 */
    goto L_ovl02_02C200_000379;              /* jmp 0x0379 */
L_ovl02_02C200_000376:;
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
L_ovl02_02C200_000379:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB6);              /* mov ax, 0xB6 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_00039A; /* je 0x039A */
    cpu->ax = (uint16_t)(0xFA);              /* mov ax, 0xFA */
    goto L_ovl02_02C200_00039D;              /* jmp 0x039D */
L_ovl02_02C200_00039A:;
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
L_ovl02_02C200_00039D:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB7);              /* mov ax, 0xB7 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44))); /* mov ax, word ss:[bp+-0x44] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
L_ovl02_02C200_0003BB:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0003D4; /* jne 0x03D4 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, cpu->bx), 0x0); /* cmp word ds:[bx], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_0003CF; /* je 0x03CF */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    goto L_ovl02_02C200_0003D2;              /* jmp 0x03D2 */
L_ovl02_02C200_0003CF:;
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
L_ovl02_02C200_0003D2:;
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
L_ovl02_02C200_0003D4:;
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    cpu->si = (uint16_t)(cpu->ax);           /* mov si, ax */
    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->si);                    /* push si */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(0x4C);              /* mov ax, 0x4C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(0x64);              /* mov ax, 0x64 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->si);                    /* push si */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x58);              /* mov ax, 0x58 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000476; /* jne 0x0476 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, cpu->bx)); /* mov ax, word ds:[bx] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x44], ax */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x0)); /* mov word ds:[bx], 0x0 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->bx);                    /* push bx */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_00102D(cpu);                         /* call 0000:1041 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44))); /* mov ax, word ss:[bp+-0x44] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0838(cpu);                      /* call 0000:0838 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000476:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    cpu->ax = (uint16_t)(0x15);              /* mov ax, 0x15 */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ss:[bp+-0x48] */
    cpu->bx = (uint16_t)(cpu->cx);           /* mov bx, cx */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    cpu->cx = (uint16_t)(0x2);               /* mov cx, 0x2 */
    { int16_t _v = (int16_t)cpu->ax; uint8_t _c = cpu->cl; int16_t _r = _v >> _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (_c - 1)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, (uint16_t)_r); cpu->ax = (uint16_t)((uint16_t)_r); } /* sar ax, cl */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    flags_cmp16(cpu, cpu->ax, cpu->bx);      /* cmp ax, bx */
    if (cc_ge(cpu)) goto L_ovl02_02C200_0004A0; /* jge 0x04A0 */
    flags_logic8(cpu, mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)) & 0x1); /* test byte ss:[bp+-0x48], 0x1 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0004A0; /* jne 0x04A0 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_0004A0:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    cpu->ax = (uint16_t)(0x15);              /* mov ax, 0x15 */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ss:[bp+-0x48] */
    cpu->bx = (uint16_t)(cpu->cx);           /* mov bx, cx */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    cpu->cx = (uint16_t)(0x2);               /* mov cx, 0x2 */
    { int16_t _v = (int16_t)cpu->ax; uint8_t _c = cpu->cl; int16_t _r = _v >> _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (_c - 1)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, (uint16_t)_r); cpu->ax = (uint16_t)((uint16_t)_r); } /* sar ax, cl */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    flags_cmp16(cpu, cpu->ax, cpu->bx);      /* cmp ax, bx */
    if (cc_g(cpu)) goto L_ovl02_02C200_0004A0; /* jg 0x04A0 */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_0004CF; /* je 0x04CF */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x3E7)); /* mov word ss:[bp+-0x48], 0x3E7 */
L_ovl02_02C200_0004CF:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_0004D2:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x140); /* cmp word ss:[bp+-0x48], 0x140 */
    if (cc_l(cpu)) goto L_ovl02_02C200_0004DC; /* jl 0x04DC */
    goto L_ovl02_02C200_000635;              /* jmp 0x0635 */
L_ovl02_02C200_0004DC:;
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    cpu->si = (uint16_t)(cpu->ax);           /* mov si, ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->si);                    /* push si */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->si);                    /* push si */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xA); /* cmp word ss:[bp+-0x48], 0xA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00053C; /* jne 0x053C */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(0x5)); /* mov word ds:[bx+0x10], 0x5 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_00053C:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x32); /* cmp word ss:[bp+-0x48], 0x32 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000547; /* jne 0x0547 */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
L_ovl02_02C200_000547:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x46); /* cmp word ss:[bp+-0x48], 0x46 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000558; /* jne 0x0558 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000558:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x78); /* cmp word ss:[bp+-0x48], 0x78 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000569; /* jne 0x0569 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000569:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xA0); /* cmp word ss:[bp+-0x48], 0xA0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000575; /* jne 0x0575 */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
L_ovl02_02C200_000575:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xAA); /* cmp word ss:[bp+-0x48], 0xAA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000587; /* jne 0x0587 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000587:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xBE); /* cmp word ss:[bp+-0x48], 0xBE */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000599; /* jne 0x0599 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000599:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xD2); /* cmp word ss:[bp+-0x48], 0xD2 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0005AB; /* jne 0x05AB */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0005AB:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xE6); /* cmp word ss:[bp+-0x48], 0xE6 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0005BD; /* jne 0x05BD */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0005BD:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xFA); /* cmp word ss:[bp+-0x48], 0xFA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0005CF; /* jne 0x05CF */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0005CF:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x10E); /* cmp word ss:[bp+-0x48], 0x10E */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0005E1; /* jne 0x05E1 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0005E1:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x122); /* cmp word ss:[bp+-0x48], 0x122 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0005F3; /* jne 0x05F3 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0005F3:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x136); /* cmp word ss:[bp+-0x48], 0x136 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000605; /* jne 0x0605 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000605:;
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_022418(cpu);                         /* call 205A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000618; /* jne 0x0618 */
    goto L_ovl02_02C200_0003BB;              /* jmp 0x03BB */
L_ovl02_02C200_000618:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, cpu->bx)); /* mov ax, word ds:[bx] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x44], ax */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x1)); /* mov word ds:[bx], 0x1 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00062F; /* jne 0x062F */
    goto L_ovl02_02C200_000352;              /* jmp 0x0352 */
L_ovl02_02C200_00062F:;
    cpu->ax = (uint16_t)(0xFC);              /* mov ax, 0xFC */
    goto L_ovl02_02C200_000355;              /* jmp 0x0355 */
L_ovl02_02C200_000635:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x0)); /* mov word ss:[bp+-0x48], 0x0 */
    goto L_ovl02_02C200_00083B;              /* jmp 0x083B */
L_ovl02_02C200_00063D:;
    cpu->ax = (uint16_t)(0x7);               /* mov ax, 0x7 */
L_ovl02_02C200_000640:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB8);              /* mov ax, 0xB8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000661; /* je 0x0661 */
    cpu->ax = (uint16_t)(0xF8);              /* mov ax, 0xF8 */
    goto L_ovl02_02C200_000664;              /* jmp 0x0664 */
L_ovl02_02C200_000661:;
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
L_ovl02_02C200_000664:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB6);              /* mov ax, 0xB6 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000685; /* je 0x0685 */
    cpu->ax = (uint16_t)(0xFA);              /* mov ax, 0xFA */
    goto L_ovl02_02C200_000688;              /* jmp 0x0688 */
L_ovl02_02C200_000685:;
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
L_ovl02_02C200_000688:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB7);              /* mov ax, 0xB7 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA0);              /* mov ax, 0xA0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_0018B1(cpu);                         /* call 0181:00B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44))); /* mov ax, word ss:[bp+-0x44] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
L_ovl02_02C200_0006A6:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0006BF; /* jne 0x06BF */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, cpu->bx), 0x0); /* cmp word ds:[bx], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_0006BA; /* je 0x06BA */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    goto L_ovl02_02C200_0006BD;              /* jmp 0x06BD */
L_ovl02_02C200_0006BA:;
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
L_ovl02_02C200_0006BD:;
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
L_ovl02_02C200_0006BF:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x140); /* cmp word ss:[bp+-0x48], 0x140 */
    if (cc_ge(cpu)) goto L_ovl02_02C200_0006EE; /* jge 0x06EE */
    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_0006EE:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x0); /* cmp word ss:[bp+-0x48], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_00071A; /* je 0x071A */
    cpu->ax = (uint16_t)(0xC);               /* mov ax, 0xC */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_00071A:;
    cpu->ax = (uint16_t)(0x4C);              /* mov ax, 0x4C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x140); /* cmp word ss:[bp+-0x48], 0x140 */
    if (cc_ge(cpu)) goto L_ovl02_02C200_000770; /* jge 0x0770 */
    cpu->ax = (uint16_t)(0x64);              /* mov ax, 0x64 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x58);              /* mov ax, 0x58 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x58);              /* mov ax, 0x58 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_000770:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x0); /* cmp word ss:[bp+-0x48], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_00079C; /* je 0x079C */
    cpu->ax = (uint16_t)(0x64);              /* mov ax, 0x64 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x58);              /* mov ax, 0x58 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_00079C:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0007D0; /* jne 0x07D0 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, cpu->bx)); /* mov ax, word ds:[bx] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x44], ax */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x0)); /* mov word ds:[bx], 0x0 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->bx);                    /* push bx */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_00102D(cpu);                         /* call 0000:1041 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44))); /* mov ax, word ss:[bp+-0x44] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(cpu->ax)); /* mov word ds:[bx], ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0838(cpu);                      /* call 0000:0838 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0007D0:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    cpu->ax = (uint16_t)(0x15);              /* mov ax, 0x15 */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x1A40)); /* add ax, 0x1A40 */
    cpu->bx = (uint16_t)(cpu->cx);           /* mov bx, cx */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    cpu->cx = (uint16_t)(0x2);               /* mov cx, 0x2 */
    { int16_t _v = (int16_t)cpu->ax; uint8_t _c = cpu->cl; int16_t _r = _v >> _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (_c - 1)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, (uint16_t)_r); cpu->ax = (uint16_t)((uint16_t)_r); } /* sar ax, cl */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    flags_cmp16(cpu, cpu->ax, cpu->bx);      /* cmp ax, bx */
    if (cc_ge(cpu)) goto L_ovl02_02C200_0007FD; /* jge 0x07FD */
    flags_logic8(cpu, mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)) & 0x1); /* test byte ss:[bp+-0x48], 0x1 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0007FD; /* jne 0x07FD */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_0007FD:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00082A; /* jne 0x082A */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    cpu->cx = (uint16_t)(cpu->ax);           /* mov cx, ax */
    cpu->ax = (uint16_t)(0x15);              /* mov ax, 0x15 */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x1A40)); /* add ax, 0x1A40 */
    cpu->bx = (uint16_t)(cpu->cx);           /* mov bx, cx */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    cpu->cx = (uint16_t)(0x2);               /* mov cx, 0x2 */
    { int16_t _v = (int16_t)cpu->ax; uint8_t _c = cpu->cl; int16_t _r = _v >> _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (_c - 1)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, (uint16_t)_r); cpu->ax = (uint16_t)((uint16_t)_r); } /* sar ax, cl */
    { uint16_t _r = cpu->ax ^ cpu->dx; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* xor ax, dx */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->dx)); /* sub ax, dx */
    flags_cmp16(cpu, cpu->ax, cpu->bx);      /* cmp ax, bx */
    if (cc_g(cpu)) goto L_ovl02_02C200_0007FD; /* jg 0x07FD */
L_ovl02_02C200_00082A:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_000838; /* je 0x0838 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x3E7)); /* mov word ss:[bp+-0x48], 0x3E7 */
L_ovl02_02C200_000838:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_00083B:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x140); /* cmp word ss:[bp+-0x48], 0x140 */
    if (cc_l(cpu)) goto L_ovl02_02C200_000845; /* jl 0x0845 */
    goto L_ovl02_02C200_0009D7;              /* jmp 0x09D7 */
L_ovl02_02C200_000845:;
    if (cc_ge(cpu)) goto L_ovl02_02C200_000870; /* jge 0x0870 */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_000870:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x0); /* cmp word ss:[bp+-0x48], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_00089F; /* je 0x089F */
    cpu->ax = (uint16_t)(0xB0);              /* mov ax, 0xB0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)))); /* sub ax, word ss:[bp+-0x48] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    cpu->ax = (uint16_t)(0x18);              /* mov ax, 0x18 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_ovl02_02C200_00089F:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xA); /* cmp word ss:[bp+-0x48], 0xA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008B0; /* jne 0x08B0 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0008B0:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x1E); /* cmp word ss:[bp+-0x48], 0x1E */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008C1; /* jne 0x08C1 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0008C1:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x32); /* cmp word ss:[bp+-0x48], 0x32 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008CC; /* jne 0x08CC */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
L_ovl02_02C200_0008CC:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x40); /* cmp word ss:[bp+-0x48], 0x40 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008DD; /* jne 0x08DD */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0008DD:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x54); /* cmp word ss:[bp+-0x48], 0x54 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008EE; /* jne 0x08EE */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_0008EE:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x74); /* cmp word ss:[bp+-0x48], 0x74 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0008F9; /* jne 0x08F9 */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
L_ovl02_02C200_0008F9:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x82); /* cmp word ss:[bp+-0x48], 0x82 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00090B; /* jne 0x090B */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_00090B:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x96); /* cmp word ss:[bp+-0x48], 0x96 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00091D; /* jne 0x091D */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_00091D:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xAA); /* cmp word ss:[bp+-0x48], 0xAA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00092F; /* jne 0x092F */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_00092F:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xBE); /* cmp word ss:[bp+-0x48], 0xBE */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000941; /* jne 0x0941 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000941:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xD2); /* cmp word ss:[bp+-0x48], 0xD2 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000953; /* jne 0x0953 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000953:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xE6); /* cmp word ss:[bp+-0x48], 0xE6 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000965; /* jne 0x0965 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000965:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0xFA); /* cmp word ss:[bp+-0x48], 0xFA */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000977; /* jne 0x0977 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000977:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x10E); /* cmp word ss:[bp+-0x48], 0x10E */
    if (cc_ne(cpu)) goto L_ovl02_02C200_000989; /* jne 0x0989 */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000989:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x122); /* cmp word ss:[bp+-0x48], 0x122 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_00099B; /* jne 0x099B */
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    ovl02_02DBA3(cpu);                       /* call 0x19A3 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_00099B:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x136); /* cmp word ss:[bp+-0x48], 0x136 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0009A7; /* jne 0x09A7 */
    mem_write8(cpu, cpu->ds, 0xC936, (uint8_t)(0x0)); /* mov byte ds:[0xC936], 0x0 */
L_ovl02_02C200_0009A7:;
    cpu->ax = (uint16_t)(0xC936);            /* mov ax, 0xC936 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_022418(cpu);                         /* call 205A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0009BA; /* jne 0x09BA */
    goto L_ovl02_02C200_0006A6;              /* jmp 0x06A6 */
L_ovl02_02C200_0009BA:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, cpu->bx)); /* mov ax, word ds:[bx] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x44), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x44], ax */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x1)); /* mov word ds:[bx], 0x1 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_ne(cpu)) goto L_ovl02_02C200_0009D1; /* jne 0x09D1 */
    goto L_ovl02_02C200_00063D;              /* jmp 0x063D */
L_ovl02_02C200_0009D1:;
    cpu->ax = (uint16_t)(0xFC);              /* mov ax, 0xFC */
    goto L_ovl02_02C200_000640;              /* jmp 0x0640 */
L_ovl02_02C200_0009D7:;
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0xFFFE)); /* mov word ds:[0x6AC2], 0xFFFE */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A0F; /* je 0x0A0F */
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0xFFFF)); /* mov word ds:[0x6AC2], 0xFFFF */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1D1F_0AC9(cpu);                      /* call 1D1F:0AC9 */
    flags_cmp16(cpu, cpu->ax, 0x4E);         /* cmp ax, 0x4E */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A09; /* je 0x0A09 */
    if (cc_g(cpu)) goto L_ovl02_02C200_000A3D; /* jg 0x0A3D */
    flags_cmp16(cpu, cpu->ax, 0x43);         /* cmp ax, 0x43 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A35; /* je 0x0A35 */
    flags_cmp16(cpu, cpu->ax, 0x45);         /* cmp ax, 0x45 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A2D; /* je 0x0A2D */
    flags_cmp16(cpu, cpu->ax, 0x4C);         /* cmp ax, 0x4C */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A25; /* je 0x0A25 */
    goto L_ovl02_02C200_000A0F;              /* jmp 0x0A0F */
L_ovl02_02C200_000A09:;
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0x0)); /* mov word ds:[0x6AC2], 0x0 */
L_ovl02_02C200_000A0F:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x6AC2), 0xFFFE); /* cmp word ds:[0x6AC2], 0xFFFE */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A19; /* je 0x0A19 */
    goto L_ovl02_02C200_000B26;              /* jmp 0x0B26 */
L_ovl02_02C200_000A19:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0A40(cpu);                      /* call 0000:0A40 */
    flags_cmp16(cpu, cpu->ax, 0xD87);        /* cmp ax, 0xD87 */
    if (cc_ge(cpu)) goto L_ovl02_02C200_000A53; /* jge 0x0A53 */
    goto L_ovl02_02C200_000A19;              /* jmp 0x0A19 */
L_ovl02_02C200_000A25:;
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0x1)); /* mov word ds:[0x6AC2], 0x1 */
    goto L_ovl02_02C200_000A0F;              /* jmp 0x0A0F */
L_ovl02_02C200_000A2D:;
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0x2)); /* mov word ds:[0x6AC2], 0x2 */
    goto L_ovl02_02C200_000A0F;              /* jmp 0x0A0F */
L_ovl02_02C200_000A35:;
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0x3)); /* mov word ds:[0x6AC2], 0x3 */
    goto L_ovl02_02C200_000A0F;              /* jmp 0x0A0F */
L_ovl02_02C200_000A3D:;
    flags_cmp16(cpu, cpu->ax, 0x63);         /* cmp ax, 0x63 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A35; /* je 0x0A35 */
    flags_cmp16(cpu, cpu->ax, 0x65);         /* cmp ax, 0x65 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A2D; /* je 0x0A2D */
    flags_cmp16(cpu, cpu->ax, 0x6C);         /* cmp ax, 0x6C */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A25; /* je 0x0A25 */
    flags_cmp16(cpu, cpu->ax, 0x6E);         /* cmp ax, 0x6E */
    if (cc_e(cpu)) goto L_ovl02_02C200_000A09; /* je 0x0A09 */
    goto L_ovl02_02C200_000A0F;              /* jmp 0x0A0F */
L_ovl02_02C200_000A53:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4E))); /* push word ss:[bp+-0x4E] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_083F(cpu);                      /* call 0000:083F */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000AB4; /* je 0x0AB4 */
    cpu->ax = (uint16_t)(0xEF);              /* mov ax, 0xEF */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xE0);              /* mov ax, 0xE0 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xE);               /* mov ax, 0xE */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x4);               /* mov ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0374(cpu);                      /* call 0000:0374 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->ax = (uint16_t)(0x4);               /* mov ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_03EC(cpu);                      /* call 0000:03EC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
L_ovl02_02C200_000AB4:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x0)); /* mov word ss:[bp+-0x48], 0x0 */
L_ovl02_02C200_000AB9:;
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x50);              /* mov ax, 0x50 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x4);               /* mov ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x4))); /* add word ss:[bp+-0x48], 0x4 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x140); /* cmp word ss:[bp+-0x48], 0x140 */
    if (cc_l(cpu)) goto L_ovl02_02C200_000AB9; /* jl 0x0AB9 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x0)); /* mov word ss:[bp+-0x48], 0x0 */
    goto L_ovl02_02C200_000B0C;              /* jmp 0x0B0C */
L_ovl02_02C200_000AFD:;
    cpu->ax = (uint16_t)(0x5);               /* mov ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    res_001932(cpu);                         /* call 0181:0136 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_000B0C:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x64); /* cmp word ss:[bp+-0x48], 0x64 */
    if (cc_ge(cpu)) goto L_ovl02_02C200_000B1B; /* jge 0x0B1B */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2096(cpu);                      /* call 205A:2096 */
    { uint16_t _r = cpu->ax | cpu->ax; flags_logic16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* or ax, ax */
    if (cc_e(cpu)) goto L_ovl02_02C200_000AFD; /* je 0x0AFD */
L_ovl02_02C200_000B1B:;
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0402_44E9(cpu);                      /* call 0402:44E9 */
    mem_write16(cpu, cpu->ds, 0x6AC2, (uint16_t)(0xFFFF)); /* mov word ds:[0x6AC2], 0xFFFF */
L_ovl02_02C200_000B26:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE692), 0x0); /* cmp word ds:[0xE692], 0x0 */
    if (cc_e(cpu)) goto L_ovl02_02C200_000B4A; /* je 0x0B4A */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(0x1)); /* mov word ss:[bp+-0x48], 0x1 */
    goto L_ovl02_02C200_000B37;              /* jmp 0x0B37 */
L_ovl02_02C200_000B34:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x48] */
L_ovl02_02C200_000B37:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48)), 0x4); /* cmp word ss:[bp+-0x48], 0x4 */
    if (cc_g(cpu)) goto L_ovl02_02C200_000B52; /* jg 0x0B52 */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x48))); /* push word ss:[bp+-0x48] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_041D(cpu);                      /* call 0000:041D */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    goto L_ovl02_02C200_000B34;              /* jmp 0x0B34 */
L_ovl02_02C200_000B4A:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, cpu->bx, (uint16_t)(0x0)); /* mov word ds:[bx], 0x0 */
L_ovl02_02C200_000B52:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0BEC(cpu);                      /* call 0000:0BEC */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xC)); /* add sp, 0xC */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4E))); /* push word ss:[bp+-0x4E] */
    cpu->ax = (uint16_t)(0x40);              /* mov ax, 0x40 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_083F(cpu);                      /* call 0000:083F */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->ax = (uint16_t)(0x14);              /* mov ax, 0x14 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07A7(cpu);                      /* call 0000:07A7 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0xC8);              /* mov ax, 0xC8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x140);             /* mov ax, 0x140 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19D4)); /* push word ds:[0x19D4] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0000:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_0838(cpu);                      /* call 0000:0838 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(0x1)); /* mov word ds:[bx+0x10], 0x1 */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x4E))); /* push word ss:[bp+-0x4E] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1DDE_0523(cpu);                      /* call 1DDE:0523 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->si = (uint16_t)(pop16(cpu));        /* pop si */
    cpu->sp = (uint16_t)(cpu->bp);           /* mov sp, bp */
    cpu->bp = (uint16_t)(pop16(cpu));        /* pop bp */
    cpu->sp += 4; return;                    /* retf */
}

/* ─── Random number module (overlay segment 0x1DDE) ─── */
/*
 * The 1DDE overlay contains the game's random number generator.
 * MSC 5.x uses: seed = seed * 214013 + 2531011; return (seed>>16) & 0x7FFF
 * We use the same LCG for reproducibility.
 */
static uint32_t rng_seed = 0;
static int rng_seeded = 0;

static uint16_t civ_rand(void)
{
    if (!rng_seeded) {
        rng_seed = (uint32_t)time(NULL);
        rng_seeded = 1;
    }
    rng_seed = rng_seed * 214013u + 2531011u;
    return (uint16_t)((rng_seed >> 16) & 0x7FFF);
}

/* far_1DDE_0042 - srand(seed): Seed the random number generator.
 * Stack: [ret4] [seed 2 bytes] */
void far_1DDE_0042(CPU *cpu)
{
    uint16_t seed = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    rng_seed = (uint32_t)seed;
    rng_seeded = 1;
    fprintf(stderr, "[RNG] srand(%u)\n", seed);
    cpu->sp += 4; /* far ret */
}

/* far_1DDE_005D - random(max): Return random int in [0, max).
 * Stack: [ret4] [max 2 bytes]
 * Returns: AX = random value in [0, max) */
void far_1DDE_005D(CPU *cpu)
{
    uint16_t max_val = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    if (max_val == 0) {
        cpu->ax = 0;
    } else {
        cpu->ax = civ_rand() % max_val;
    }
    cpu->sp += 4; /* far ret */
}

/* ─── Map access thunks (overlay segment 0x1B05 / thunk 0x07C3) ─── */

/* far_0000_07C3 - Map terrain query (thunk table entry #14).
 * Stack: [ret4] [layer 2] [col 2] [row 2]
 * Returns: AX = terrain value at (col, row) for given layer.
 *
 * From res_00AB99 call analysis: arg1=layer, arg2=col, arg3=row.
 * The map is 80 columns x 50 rows, wrapping E-W. Data stored at DS offsets
 * organized by layer. Return the byte from the map data array. */
void far_0000_07C3(CPU *cpu)
{
    static int call_count = 0;
    call_count++;

    uint16_t arg1 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t arg2 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t arg3 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));

    if (call_count <= 10) {
        fprintf(stderr, "[MAP_Q] far_0000_07C3 #%d args=(%u, %u, %u)\n",
                call_count, arg1, arg2, arg3);
        fflush(stderr);
    }

    uint16_t layer = arg1;
    uint16_t col = arg2 % 80;  /* E-W wrap */
    uint16_t row = arg3;

    /* Map data: 80 columns x 50 rows, 6 layers.
     * Base offsets per layer (from code analysis):
     *   Layer 0: DS:0x5864 (terrain type)
     *   Layer 1: DS:0x6824 (terrain modifiers)
     *   Layer 2: DS:0x77E4 (visibility/ownership)
     * Each layer = 80*50 = 4000 bytes */
    if (row < 50 && layer < 6) {
        uint16_t base;
        switch (layer) {
        case 0: base = 0x5864; break;
        case 1: base = 0x6824; break;
        case 2: base = 0x77E4; break;
        case 3: base = 0x87A4; break;
        case 4: base = 0x9764; break;
        case 5: base = 0xA724; break;
        default: base = 0x5864; break;
        }
        uint16_t offset = base + row * 80 + col;
        cpu->ax = (uint16_t)mem_read8(cpu, cpu->ds, offset);
    } else {
        cpu->ax = 0;
    }

    cpu->sp += 4; /* far ret */
}

/* res_01DE5C - integer clamp (lifted from dump @0x1DE5C). FAR function, invoked
 * via the MSC `push cs; call near` idiom, so args sit at sp+4/+6/+8:
 *   clamp(value=[sp+4], lo=[sp+6], hi=[sp+8]) -> AX
 *     if value < lo: value = lo;  if value > hi: value = hi;
 * far_1DDE_03CE uses it to clamp the title blit's width/height. The previous
 * no-op stub left AX undefined AND returned as near (sp+=2), which both produced
 * the degenerate 0x0 blit and corrupted the caller's stack frame. */
void res_01DE5C(CPU *cpu)
{
    int16_t value = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    int16_t lo    = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    int16_t hi    = (int16_t)mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    cpu->ax = (uint16_t)value;
    cpu->sp += 4; /* far ret */
}

/* far_1B05_17C3 - Map terrain write.
 * Stack: [ret4] [arg1 2] [arg2 2] [arg3 2] [arg4 2]
 * Writes a value to the map at given coordinates and layer. */
void far_1B05_17C3(CPU *cpu)
{
    static int call_count = 0;
    call_count++;

    uint16_t arg1 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4));
    uint16_t arg2 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t arg3 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
    uint16_t arg4 = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 10));

    if (call_count <= 10) {
        fprintf(stderr, "[MAP_W] far_1B05_17C3 #%d args=(%u, %u, %u, %u)\n",
                call_count, arg1, arg2, arg3, arg4);
        fflush(stderr);
    }

    /* Write value to map: arg1=col, arg2=row, arg3=value, arg4=layer */
    uint16_t col = arg1 % 80;  /* E-W wrap */
    uint16_t row = arg2;
    uint16_t value = arg3;
    uint16_t layer = arg4;

    if (row < 50 && layer < 6) {
        uint16_t base;
        switch (layer) {
        case 0: base = 0x5864; break;
        case 1: base = 0x6824; break;
        case 2: base = 0x77E4; break;
        case 3: base = 0x87A4; break;
        case 4: base = 0x9764; break;
        case 5: base = 0xA724; break;
        default: base = 0x5864; break;
        }
        uint16_t offset = base + row * 80 + col;
        mem_write8(cpu, cpu->ds, offset, (uint8_t)(value & 0xFF));
    }

    cpu->sp += 4; /* far ret */
}

/* ─── PIC LZW Decoder: internal subroutines of res_0011E6 ─── */
/* These are internal near-call subroutines within the res_0011E6 code range
 * (0x0011E6-0x0013A6). They implement LZW decompression for .PIC files.
 *
 * The decoder uses the emulated stack as a LIFO buffer for LZW-decompressed
 * characters, switching between normal stack and decode stack via
 * xchg sp, [0x687A].
 *
 * Data structures (all in DS segment):
 *   0x6874: image width (pixels per row)
 *   0x6876: image height (number of rows)
 *   0x687A: saved SP for stack switching (decode stack at 0x6A8D)
 *   0x687C: pixels remaining in current row
 *   0x687E: RLE repeat count
 *   0x687F: RLE repeat byte
 *   0x6880: current LZW code bit width
 *   0x6881: max LZW code bit width
 *   0x6882: LZW max code value ((1 << bitwidth) - 1)
 *   0x6884: next LZW dictionary entry index
 *   0x6886: bit buffer (16-bit)
 *   0x6888: bits remaining in bit buffer
 *   0x6889: 4-bit mode flag (for EGA/CGA)
 *   0x688A: previous LZW code
 *   0x688C: first character of previous code
 *   0xC19E: read position in compressed data buffer
 *   0x54D8: end of compressed data buffer
 *   0xE84A: callback to refill compressed data buffer
 *   0xC936-: LZW dictionary area
 *   DS-0x36CA: dictionary parent pointers (word, indexed by code*3)
 *   DS-0x36C8: dictionary character values (byte, indexed by code*3)
 */

/* Forward declare callback function */
extern void res_020191(CPU *cpu);

/* Helper: refill the compressed data buffer via the E84A callback */
static void pic_refill_buffer(CPU *cpu)
{
    push16(cpu, cpu->bx);
    push16(cpu, cpu->cx);
    push16(cpu, cpu->dx);
    uint16_t cb_off = mem_read16(cpu, cpu->ds, 0xE84A);
    uint16_t cb_seg = mem_read16(cpu, cpu->ds, 0xE84C);
    /* The refill callback is far_1FB6_0642 (== res_020191): read 512B of the .pic
     * via far_205A_30E4 using the file token at DS:0x686C. The title/intro PICs
     * set the callback seg to the unrelocated 0x1FB6; sp299.pic (and the other
     * in-game sprite sheets) use the relocated 0x20B6 (== 0x1FB6+LOAD_SEG). Both
     * forms are dispatched. The sprite-sheet decode used to corrupt DS:0x686C
     * (a self-referential LZW dict chain overran the decode stack) — fixed in
     * res_0012F6 (KwKwK walks prev_code, not dx; prev_code save uses cx). */
    if (cb_off == 0x0642 && (cb_seg == 0x1FB6 || cb_seg == 0x20B6)) {
        push16(cpu, cpu->cs); push16(cpu, 0);
        res_020191(cpu);
    } else {
        static int _w=0; if(++_w<=2) fprintf(stderr, "[WARN] PIC: Unknown callback %04X:%04X\n", cb_seg, cb_off);
    }
    cpu->dx = pop16(cpu);
    cpu->cx = pop16(cpu);
    cpu->bx = pop16(cpu);
    cpu->si = mem_read16(cpu, cpu->ds, 0xC19E);
}

/* Helper: Read next variable-width LZW code from bitstream.
 * Returns the code value. Manages bit buffer at DS:0x6886/0x6888. */
static uint16_t pic_read_code(CPU *cpu)
{
    uint16_t bit_buf = mem_read16(cpu, cpu->ds, 0x6886);
    uint8_t bits_avail = mem_read8(cpu, cpu->ds, 0x6888);
    uint8_t code_bits = mem_read8(cpu, cpu->ds, 0x6880);

    /* Shift out already-consumed bits */
    uint8_t shift = 16 - bits_avail;
    uint16_t bx = (shift < 16) ? (bit_buf >> shift) : 0;
    uint8_t cl = bits_avail;

    /* Read more words until we have enough bits */
    while ((int8_t)cl < (int8_t)code_bits) {
        /* Refill from compressed data buffer if needed */
        if (cpu->si >= mem_read16(cpu, cpu->ds, 0x54D8)) {
            pic_refill_buffer(cpu);
        }
        /* Read next 16 bits */
        uint16_t word = mem_read16(cpu, cpu->ds, cpu->si);
        cpu->si += 2;
        mem_write16(cpu, cpu->ds, 0x6886, word);
        bx |= (uint16_t)(word << cl);
        cl += 16;
    }

    /* Extract the code */
    cl -= code_bits;
    mem_write8(cpu, cpu->ds, 0x6888, cl);
    uint16_t mask = mem_read16(cpu, cpu->ds, 0x6882);
    return bx & mask;
}

/* lzw_dict_reset - the dictionary-only reset (original entry 0x1262): set
 * code_bits=9, max_code=0x1FF, next_free=0x100, clear the 0x800 parent slots and
 * re-init the 256 single-byte codes. Critically it does NOT read a stream word or
 * touch the bit buffer (0x6886/0x6888) — the bitstream continues uninterrupted.
 * Reached two ways in the original: fall-through from res_00124E (after the word
 * read, init only) and `call 0x1262` from res_0012F6's dict-full path. */
static void lzw_dict_reset(CPU *cpu)
{
    mem_write8(cpu, cpu->ds, 0x6880, 0x9);
    mem_write16(cpu, cpu->ds, 0x6882, 0x1FF);
    cpu->dx = 0x100;
    mem_write16(cpu, cpu->ds, 0x6884, cpu->dx);
    /* Clear dictionary parent pointers to 0xFFFF (unused) */
    for (int i = 0; i < 0x800; i++) {
        mem_write16(cpu, cpu->ds, (uint16_t)((i * 3) + (uint16_t)(-0x36CA & 0xFFFF)), 0xFFFF);
    }
    /* Init single-byte codes 0-255 */
    for (int i = 0; i < 0x100; i++) {
        mem_write8(cpu, cpu->ds, (uint16_t)((i * 3) + (uint16_t)(-0x36C8 & 0xFFFF)), (uint8_t)i);
    }
}

/* res_00124E - LZW (re)init that reads a fresh param word (original 0x124E):
 * read a 16-bit word from the stream — low byte = max code width (clamped 0x0B),
 * the word also seeds the bit buffer (bits_avail=8 → only the high byte is data) —
 * then do the dictionary reset. Used at decode start (via res_001205).
 * Near call (sp += 2 on return). */
void res_00124E(CPU *cpu)
{
    /* Read new LZW parameters */
    if (cpu->si >= mem_read16(cpu, cpu->ds, 0x54D8))
        pic_refill_buffer(cpu);
    cpu->ax = mem_read16(cpu, cpu->ds, cpu->si);
    cpu->si += 2;
    mem_write16(cpu, cpu->ds, 0xC19E, cpu->si);
    if (cpu->al > 0x0B) cpu->al = 0x0B;
    mem_write8(cpu, cpu->ds, 0x6881, cpu->al);
    mem_write16(cpu, cpu->ds, 0x6886, cpu->ax);
    mem_write8(cpu, cpu->ds, 0x6888, 0x8);
    lzw_dict_reset(cpu);
    cpu->sp += 2; /* near ret */
}

/* res_001205 - Initialize LZW decoder state.
 * Near call (sp += 2 on return). */
void res_001205(CPU *cpu)
{
    uint16_t w = mem_read16(cpu, cpu->ds, 0x6874);
    uint16_t h = mem_read16(cpu, cpu->ds, 0x6876);
    if ((w | h) == 0) {
        cpu->sp += 2; return;
    }
    /* Initialize RLE state */
    mem_write8(cpu, cpu->ds, 0x687E, 0x0);
    mem_write8(cpu, cpu->ds, 0x687F, 0x0);
    /* Set decode stack base */
    mem_write16(cpu, cpu->ds, 0x687A, 0x6A8D);
    /* Read initial parameters and init dictionary */
    cpu->si = mem_read16(cpu, cpu->ds, 0xC19E);
    push16(cpu, 0);
    res_00124E(cpu);
    cpu->sp += 2; /* near ret */
}

/* res_0012F6 - LZW decode: return next decompressed byte.
 * This is the core LZW decoder. It maintains a decode stack (at DS:0x6A8D)
 * where multi-byte LZW sequences are expanded. Each call returns one byte.
 *
 * The original code uses the x86 stack as the decode buffer (xchg sp, [687A]),
 * pushing decoded chars onto it and popping them one at a time.
 * We simulate this using the emulated memory stack.
 *
 * Returns decoded byte in AL.
 * Near call (sp += 2 on return). */
void res_0012F6(CPU *cpu)
{
    /* Dictionary layout: 3 bytes per entry at DS:[entry*3 - 0x36CA]
     *   word: parent code (0xFFFF = root/single-byte)
     *   byte: character value
     * Entries 0-255 are single-byte codes. */
    uint16_t dict_base_parent = (uint16_t)(-0x36CA & 0xFFFF);
    uint16_t dict_base_char   = (uint16_t)(-0x36C8 & 0xFFFF);

    /* Check if decode stack has buffered characters */
    uint16_t decode_sp = mem_read16(cpu, cpu->ds, 0x687A);
    if (decode_sp != 0x6A8D) {
        /* Pop one byte from decode stack */
        cpu->al = mem_read8(cpu, cpu->ds, decode_sp);
        decode_sp += 2;  /* stack grows down, pop = sp += 2 (word-sized) */
        mem_write16(cpu, cpu->ds, 0x687A, decode_sp);
        cpu->sp += 2; /* near ret */
        return;
    }

    /* Decode stack empty - need to decode next LZW code */
    uint16_t dx = mem_read16(cpu, cpu->ds, 0x6884); /* next free dict entry */
    uint16_t code = pic_read_code(cpu);
    uint16_t cx = code;

    /* Walk starts from the code itself for a normal (already-defined) code. */
    uint16_t walk_code = code;

    /* Handle code >= next free entry (KwKwK case).
     * Output = prev_string + first_char(prev_string). The original
     * (0x135F-0x1369) sets cx=dx, pushes the OLD first char [0x688C] as the
     * trailing "K", then walks from the PREVIOUS code [0x688A] — NOT dx (which
     * is the not-yet-defined entry; walking it reads a stale/self-referential
     * dict slot -> infinite chain -> decode-stack overrun). */
    if ((int16_t)code >= (int16_t)dx) {
        cx = dx;
        uint8_t prev_first_char = mem_read8(cpu, cpu->ds, 0x688C);
        decode_sp -= 2;
        mem_write8(cpu, cpu->ds, decode_sp, prev_first_char);
        walk_code = mem_read16(cpu, cpu->ds, 0x688A);  /* prev_code */
    }

    /* Walk the dictionary chain, pushing characters onto decode stack.
     * A valid LZW chain strictly descends (parent < child) and ends at a
     * root (parent==0xFFFF); the guard below is a defensive backstop against a
     * corrupt/circular chain so it can never overrun the state region. */
    int _walk_n = 0;
    while (1) {
        if (decode_sp <= 0x6890 || ++_walk_n > 4096) {
            static int _ov=0; if (++_ov <= 8)
                fprintf(stderr, "[LZWOVR] decode_sp=%04X walk_code=%04X parent=%04X code=%04X dx=%04X n=%d\n",
                        decode_sp, walk_code,
                        mem_read16(cpu, cpu->ds, (uint16_t)(walk_code*3+dict_base_parent)),
                        code, dx, _walk_n);
            break;
        }
        uint16_t parent = mem_read16(cpu, cpu->ds, (uint16_t)(walk_code * 3 + dict_base_parent));
        uint8_t ch = mem_read8(cpu, cpu->ds, (uint16_t)(walk_code * 3 + dict_base_char));
        if ((int16_t)(parent + 1) == 0) {
            /* Root code (parent == 0xFFFF): this is the first character */
            /* Save as first char for dictionary update */
            mem_write8(cpu, cpu->ds, 0x688C, ch);
            /* Push it (will be popped last = output first) */
            decode_sp -= 2;
            mem_write8(cpu, cpu->ds, decode_sp, ch);
            break;
        }
        /* Push character and continue walking */
        decode_sp -= 2;
        mem_write8(cpu, cpu->ds, decode_sp, ch);
        walk_code = parent;
    }

    /* Add new dictionary entry: prev_code + first_char_of_current */
    uint8_t first_char = mem_read8(cpu, cpu->ds, 0x688C);
    mem_write8(cpu, cpu->ds, (uint16_t)(dx * 3 + dict_base_char), first_char);
    uint16_t prev_code = mem_read16(cpu, cpu->ds, 0x688A);
    mem_write16(cpu, cpu->ds, (uint16_t)(dx * 3 + dict_base_parent), prev_code);
    dx++;

    /* Check if dictionary needs to grow bit width */
    uint16_t max_code = mem_read16(cpu, cpu->ds, 0x6882);
    if ((int16_t)dx > (int16_t)max_code) {
        uint8_t cur_bits = mem_read8(cpu, cpu->ds, 0x6880);
        cur_bits++;
        uint8_t max_bits = mem_read8(cpu, cpu->ds, 0x6881);
        if ((int8_t)cur_bits > (int8_t)max_bits) {
            /* Dictionary full - reset. The original calls 0x1262 (dict-only reset)
             * here, NOT the full res_00124E (0x124E): it must NOT consume a stream
             * word or reset the bit buffer mid-stream, or the bitstream desyncs and
             * everything after the first overflow decodes to garbage. */
            mem_write16(cpu, cpu->ds, 0x6884, dx);
            lzw_dict_reset(cpu);
            dx = mem_read16(cpu, cpu->ds, 0x6884);
        } else {
            mem_write8(cpu, cpu->ds, 0x6880, cur_bits);
            /* rcl word [0x6882], 1 → max_code = max_code * 2 + CF */
            /* CF was set by stc before this in original code */
            max_code = (max_code << 1) | 1;
            mem_write16(cpu, cpu->ds, 0x6882, max_code);
        }
    }

    mem_write16(cpu, cpu->ds, 0x6884, dx);
    /* Original (0x13B3) saves CX, which is `code` for a normal code but `dx`
     * for the KwKwK case — saving `code` there seeds a wrong prev_code and
     * creates self/forward-referential dict entries. */
    mem_write16(cpu, cpu->ds, 0x688A, cx);

    /* Pop first byte from decode stack */
    cpu->al = mem_read8(cpu, cpu->ds, decode_sp);
    decode_sp += 2;
    mem_write16(cpu, cpu->ds, 0x687A, decode_sp);

    cpu->sp += 2; /* near ret */
}

/* res_001284 - Decode one row of PIC data (RLE + LZW).
 * Reads CX decompressed bytes via res_0012F6 (LZW) and applies
 * PackBits RLE decoding (0x90 escape). Output goes to ES:DI.
 *
 * CX = pixel count (set by caller), DI = output buffer, SI = read ptr.
 * Near call (sp += 2 on return). */
void res_001284(CPU *cpu)
{
    uint8_t is_4bit = mem_read8(cpu, cpu->ds, 0x6889);

    /* Adjust pixel count for 4-bit mode */
    if (is_4bit) {
        cpu->cx++;
        cpu->cx >>= 1;
    }
    mem_write16(cpu, cpu->ds, 0x687C, cpu->cx);

    /* Main decode loop: decode pixels one at a time */
    while (mem_read16(cpu, cpu->ds, 0x687C) != 0) {
        if (mem_read8(cpu, cpu->ds, 0x687E) != 0) {
            /* RLE repeat: output previous byte again */
            cpu->al = mem_read8(cpu, cpu->ds, 0x687F);
            mem_write8(cpu, cpu->ds, 0x687E,
                       (uint8_t)(mem_read8(cpu, cpu->ds, 0x687E) - 1));
        } else {
            /* Get next decompressed byte from LZW */
            push16(cpu, 0);
            res_0012F6(cpu);

            if (cpu->al == 0x90) {
                /* PackBits RLE escape */
                push16(cpu, 0);
                res_0012F6(cpu);
                if (cpu->al == 0) {
                    /* Literal 0x90 byte */
                    cpu->al = 0x90;
                    mem_write8(cpu, cpu->ds, 0x687F, cpu->al);
                } else {
                    /* Repeat previous byte (count-1) more times */
                    cpu->al--;
                    mem_write8(cpu, cpu->ds, 0x687E, cpu->al);
                    cpu->al = mem_read8(cpu, cpu->ds, 0x687F);
                    mem_write8(cpu, cpu->ds, 0x687E,
                               (uint8_t)(mem_read8(cpu, cpu->ds, 0x687E) - 1));
                }
            } else {
                mem_write8(cpu, cpu->ds, 0x687F, cpu->al);
            }
        }

        /* Output pixel(s) */
        if (is_4bit) {
            cpu->ah = cpu->al;
            cpu->al &= 0x0F;
            cpu->ah >>= 4;
            mem_write8(cpu, cpu->es, cpu->di, cpu->al);
            mem_write8(cpu, cpu->es, (uint16_t)(cpu->di + 1), cpu->ah);
            cpu->di += 2;
        } else {
            mem_write8(cpu, cpu->es, cpu->di, cpu->al);
            cpu->di++;
        }

        mem_write16(cpu, cpu->ds, 0x687C,
                    (uint16_t)(mem_read16(cpu, cpu->ds, 0x687C) - 1));
    }

    cpu->sp += 2; /* near ret */
}

/* ─── Resident PIC decoder aliases (the dump-lifted copy the runtime actually
 * calls via far_0000_11FA / far_0000_1080) ───
 * The resident and overlay copies of the LZW+RLE PIC decoder were lifted at a
 * +0x14 offset, so the resident entry points (res_001219/1262/1298/130A) got
 * separate STUBS while the working implementations live under the overlay names
 * (res_001205/124E/1284/12F6). Both copies share the SAME DGROUP state (dict
 * @DS:0xC936, bit buffer @0x6886, decode stack @0x687A, counts @0x6874..0x688C),
 * so delegating the resident entries to the implemented twins decodes correctly.
 * Verified res_001219's disasm matches res_001205 exactly (guard -> RLE state ->
 * stack base 0x6A8D -> max-width read -> dict init). This was why every PIC
 * (logo/birth/title) opened+read but produced no pixels (screen stayed blank). */
void res_001219(CPU *cpu) { res_001205(cpu); }  /* LZW state init */
void res_001262(CPU *cpu) { lzw_dict_reset(cpu); cpu->sp += 2; }  /* dict-only reset (0x1262) */
void res_001298(CPU *cpu) { res_001284(cpu); }  /* RLE+LZW row decode */
void res_00130A(CPU *cpu) { res_0012F6(cpu); }  /* LZW next byte */

/* res_01D665 - menu accelerator-table builder (lifted). Called by res_01D221
 * to parse the menu string and fill the per-item accel table at DS:0xEB7A so
 * first-letter select works (was a no-op stub -> empty table -> menu spun).
 * Cosmetic menu-box draws (far_0181_*) stay no-op stubs. */
void far_0181_002C(CPU *cpu) { cpu->sp += 4; }
void res_01DB5C(CPU *cpu) { cpu->sp += 2; }
void res_01D665(CPU *cpu);
void res_01DB5C(CPU *cpu);
void res_01DBF5(CPU *cpu);

/* Function: res_01D665
 * Dump offset: 0x01D665 - 0x01DB5C (1271 bytes)
 */
void res_01D665(CPU *cpu)
{
    push16(cpu, cpu->bp);                    /* push bp */
    cpu->bp = (uint16_t)(cpu->sp);           /* mov bp, sp */
    cpu->sp = (uint16_t)(flags_sub16(cpu, cpu->sp, 0x5C)); /* sub sp, 0x5C */
    push16(cpu, cpu->di);                    /* push di */
    push16(cpu, cpu->si);                    /* push si */
    mem_write16(cpu, cpu->ds, 0x64E0, (uint16_t)(0x0)); /* mov word ds:[0x64E0], 0x0 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE212), 0x1); /* cmp word ds:[0xE212], 0x1 */
    if (cc_e(cpu)) goto L_res_01D665_000018; /* je 0x0018 */
    goto L_res_01D665_00036A;                /* jmp 0x036A */
L_res_01D665_000018:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    push16(cpu, mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10))); /* push word ds:[bx+0x10] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07B5(cpu);                      /* call 0100:07B5 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write16(cpu, cpu->ds, 0x2F54, (uint16_t)(cpu->ax)); /* mov word ds:[0x2F54], ax */
    flags_cmp16(cpu, cpu->ax, 0x9);          /* cmp ax, 0x9 */
    if (cc_ne(cpu)) goto L_res_01D665_000033; /* jne 0x0033 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x2F54, (uint16_t)(flags_sub16(cpu, mem_read16(cpu, cpu->ds, 0x2F54), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* dec word ds:[0x2F54] */
L_res_01D665_000033:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x38E6), 0xFFFF); /* cmp word ds:[0x38E6], 0xFFFF */
    if (cc_e(cpu)) goto L_res_01D665_000040; /* je 0x0040 */
    mem_write16(cpu, cpu->ds, 0x2F54, (uint16_t)(0x8)); /* mov word ds:[0x2F54], 0x8 */
L_res_01D665_000040:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(0x0)); /* mov word ss:[bp+-0x56], 0x0 */
L_res_01D665_000045:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx - 0x1486), (uint8_t)(0xFF)); /* mov byte ds:[bx+-0x1486], 0xFF */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x56] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56)), 0x20); /* cmp word ss:[bp+-0x56], 0x20 */
    if (cc_l(cpu)) goto L_res_01D665_000045; /* jl 0x0045 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x5A], ax */
    mem_write16(cpu, cpu->ds, 0x6520, (uint16_t)(cpu->ax)); /* mov word ds:[0x6520], ax */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x54], ax */
    mem_write16(cpu, cpu->ds, 0xE3F8, (uint16_t)(cpu->ax)); /* mov word ds:[0xE3F8], ax */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x56], ax */
    goto L_res_01D665_0000C6;                /* jmp 0x00C6 */
L_res_01D665_000069:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54)), 0x0); /* cmp word ss:[bp+-0x54], 0x0 */
    if (cc_ne(cpu)) goto L_res_01D665_0000A7; /* jne 0x00A7 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov si, word ss:[bp+0x6] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si))); /* mov al, byte ds:[bx+si] */
    mem_write8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5C), (uint8_t)(cpu->al)); /* mov byte ss:[bp+-0x5C], al */
    flags_cmp8(cpu, cpu->al, 0x20);          /* cmp al, 0x20 */
    if (cc_e(cpu)) goto L_res_01D665_000082; /* je 0x0082 */
    flags_cmp8(cpu, cpu->al, 0x5F);          /* cmp al, 0x5F */
    if (cc_ne(cpu)) goto L_res_01D665_0000A7; /* jne 0x00A7 */
L_res_01D665_000082:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 0x20); /* cmp word ss:[bp+-0x5A], 0x20 */
    if (cc_ge(cpu)) goto L_res_01D665_000097; /* jge 0x0097 */
    cpu->si = (uint16_t)(cpu->bx);           /* mov si, bx */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov bx, word ss:[bp+0x6] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si + 0x1))); /* mov al, byte ds:[bx+si+0x1] */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A))); /* mov bx, word ss:[bp+-0x5A] */
    mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx - 0x1486), (uint8_t)(cpu->al)); /* mov byte ds:[bx+-0x1486], al */
L_res_01D665_000097:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xC11C), 0xFFFF); /* cmp word ds:[0xC11C], 0xFFFF */
    if (cc_ne(cpu)) goto L_res_01D665_0000A4; /* jne 0x00A4 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6520)); /* mov ax, word ds:[0x6520] */
    mem_write16(cpu, cpu->ds, 0xC11C, (uint16_t)(cpu->ax)); /* mov word ds:[0xC11C], ax */
L_res_01D665_0000A4:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x5A] */
L_res_01D665_0000A7:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov si, word ss:[bp+0x6] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si))); /* mov al, byte ds:[bx+si] */
    cpu->ax = (uint16_t)(int16_t)(int8_t)cpu->al; /* cbw */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    push16(cpu, mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10))); /* push word ds:[bx+0x10] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_077D(cpu);                      /* call 0100:077D */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54)), cpu->ax))); /* add word ss:[bp+-0x54], ax */
L_res_01D665_0000C3:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x56] */
L_res_01D665_0000C6:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* push word ss:[bp+0x6] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E92(cpu);                      /* call 215A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    flags_cmp16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* cmp ax, word ss:[bp+-0x56] */
    if (cc_le(cpu)) goto L_res_01D665_000108; /* jle 0x0108 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov si, word ss:[bp+0x6] */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si)), 0xA); /* cmp byte ds:[bx+si], 0xA */
    if (cc_ne(cpu)) goto L_res_01D665_000069; /* jne 0x0069 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xE3F8)); /* mov ax, word ds:[0xE3F8] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54)), cpu->ax); /* cmp word ss:[bp+-0x54], ax */
    if (cc_le(cpu)) goto L_res_01D665_0000EF; /* jle 0x00EF */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54))); /* mov ax, word ss:[bp+-0x54] */
    mem_write16(cpu, cpu->ds, 0xE3F8, (uint16_t)(cpu->ax)); /* mov word ds:[0xE3F8], ax */
L_res_01D665_0000EF:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x54), (uint16_t)(0x0)); /* mov word ss:[bp+-0x54], 0x0 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ds, 0x6520, (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ds, 0x6520), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ds:[0x6520] */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6520)); /* mov bx, word ds:[0x6520] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov ax, word ss:[bp+-0x56] */
    { int _cf = cf(cpu); cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 1)); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc ax */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x64E0), (uint16_t)(cpu->ax)); /* mov word ds:[bx+0x64E0], ax */
    goto L_res_01D665_0000C3;                /* jmp 0x00C3 */
L_res_01D665_000108:;
    cpu->ax = (uint16_t)(0xC0);              /* mov ax, 0xC0 */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* sub ax, word ss:[bp+0xA] */
    cpu->dx = (cpu->ax & 0x8000) ? 0xFFFF : 0x0000; /* cwd */
    cpu->cx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x2F54)); /* mov cx, word ds:[0x2F54] */
    { int32_t _n = (int32_t)(((uint32_t)cpu->dx << 16) | cpu->ax); int16_t _d = (int16_t)cpu->cx; cpu->ax = (uint16_t)(int16_t)(_n / _d); cpu->dx = (uint16_t)(int16_t)(_n % _d); } /* idiv cx */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x6520)); /* push word ds:[0x6520] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1DDE_007C(cpu);                      /* call 1EDE:007C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    mem_write16(cpu, cpu->ds, 0x6520, (uint16_t)(cpu->ax)); /* mov word ds:[0x6520], ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ds, 0xE3F8))); /* add ax, word ds:[0xE3F8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x8)); /* add ax, 0x8 */
    mem_write16(cpu, cpu->ds, 0xED3E, (uint16_t)(cpu->ax)); /* mov word ds:[0xED3E], ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6520)); /* mov ax, word ds:[0x6520] */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x2F54); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x2F54] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* add ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x6)); /* add ax, 0x6 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x52], ax */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F50), 0x0); /* cmp word ds:[0x2F50], 0x0 */
    if (cc_e(cpu)) goto L_res_01D665_000150; /* je 0x0150 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52)), 0x2))); /* add word ss:[bp+-0x52], 0x2 */
L_res_01D665_000150:;
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* push word ss:[bp+0x6] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E92(cpu);                      /* call 215A:1E92 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->si = (uint16_t)(cpu->ax);           /* mov si, ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov bx, word ss:[bp+0x6] */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si - 0x1)), 0xA); /* cmp byte ds:[bx+si+-0x1], 0xA */
    if (cc_e(cpu)) goto L_res_01D665_000169; /* je 0x0169 */
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A), (uint16_t)(flags_sub16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* dec word ss:[bp+-0x5A] */
L_res_01D665_000169:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A))); /* mov ax, word ss:[bp+-0x5A] */
    mem_write16(cpu, cpu->ds, 0xE138, (uint16_t)(cpu->ax)); /* mov word ds:[0xE138], ax */
    flags_logic8(cpu, mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)) & 0x1); /* test byte ss:[bp+0xA], 0x1 */
    if (cc_e(cpu)) goto L_res_01D665_000178; /* je 0x0178 */
    goto L_res_01D665_0002E6;                /* jmp 0x02E6 */
L_res_01D665_000178:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F4E), 0xFFFF); /* cmp word ds:[0x2F4E], 0xFFFF */
    if (cc_ne(cpu)) goto L_res_01D665_00019E; /* jne 0x019E */
    cpu->ax = (uint16_t)(0x7);               /* mov ax, 0x7 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52))); /* mov ax, word ss:[bp+-0x52] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* sub ax, word ss:[bp+0xA] */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xED3E)); /* mov ax, word ds:[0xED3E] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8)))); /* sub ax, word ss:[bp+0x8] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA))); /* push word ss:[bp+0xA] */
    push16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* push word ss:[bp+0x8] */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    res_01DB5C(cpu);                         /* call 0x04F7 */
    goto L_res_01D665_0002E3;                /* jmp 0x02E3 */
L_res_01D665_00019E:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x2F4E)); /* mov bx, word ds:[0x2F4E] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    push16(cpu, mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x2F56))); /* push word ds:[bx+0x2F56] */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x50)); /* lea ax, word ss:[bp+-0x50] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E60(cpu);                      /* call 215A:1E60 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F4E), 0x2); /* cmp word ds:[0x2F4E], 0x2 */
    if (cc_g(cpu)) goto L_res_01D665_0001CB; /* jg 0x01CB */
    cpu->ax = (uint16_t)(0x2F64);            /* mov ax, 0x2F64 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x50)); /* lea ax, word ss:[bp+-0x50] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_1E20(cpu);                      /* call 215A:1E20 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x4)); /* add sp, 0x4 */
L_res_01D665_0001CB:;
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x50)); /* lea ax, word ss:[bp+-0x50] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_00F1(cpu);                      /* call 0281:00F1 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x58), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x58], ax */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov si, word ss:[bp+0x8] */
    cpu->si = (uint16_t)(flags_add16(cpu, cpu->si, cpu->ax)); /* add si, ax */
    cpu->si = (uint16_t)(flags_add16(cpu, cpu->si, 0x8)); /* add si, 0x8 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xED3E), cpu->si); /* cmp word ds:[0xED3E], si */
    if (cc_ge(cpu)) goto L_res_01D665_0001EC; /* jge 0x01EC */
    mem_write16(cpu, cpu->ds, 0xED3E, (uint16_t)(cpu->si)); /* mov word ds:[0xED3E], si */
L_res_01D665_0001EC:;
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52))); /* mov si, word ss:[bp+-0x52] */
    cpu->si = (uint16_t)(flags_sub16(cpu, cpu->si, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* sub si, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(0x7);               /* mov ax, 0x7 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3E7);             /* mov ax, 0x3E7 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x3D);              /* mov ax, 0x3D */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)((uint16_t)(cpu->si + 0x8)); /* lea ax, word ds:[si+0x8] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_1DDE_007C(cpu);                      /* call 1EDE:007C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x6)); /* add sp, 0x6 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xED3E)); /* mov ax, word ds:[0xED3E] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8)))); /* sub ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x2A)); /* add ax, 0x2A */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA))); /* mov ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x8)); /* sub ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x2A)); /* sub ax, 0x2A */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    res_01DB5C(cpu);                         /* call 0x04F7 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xA)); /* add sp, 0xA */
    cpu->ax = (uint16_t)((uint16_t)(cpu->si - 0x34)); /* lea ax, word ds:[si+-0x34] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x58), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x58], ax */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F4E), 0x2); /* cmp word ds:[0x2F4E], 0x2 */
    if (cc_g(cpu)) goto L_res_01D665_00025D; /* jg 0x025D */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0x2F4E)); /* mov bx, word ds:[0x2F4E] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    push16(cpu, mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx - 0x116E))); /* push word ds:[bx+-0x116E] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA))); /* mov ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x5)); /* sub ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x28)); /* sub ax, 0x28 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_083F(cpu);                      /* call 0100:083F */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    goto L_res_01D665_0002A0;                /* jmp 0x02A0 */
L_res_01D665_00025D:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x58)), 0x0); /* cmp word ss:[bp+-0x58], 0x0 */
    if (cc_le(cpu)) goto L_res_01D665_000269; /* jle 0x0269 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x58))); /* mov ax, word ss:[bp+-0x58] */
    { int _cf = cf(cpu); cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 1)); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* dec ax */
    goto L_res_01D665_00026B;                /* jmp 0x026B */
L_res_01D665_000269:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_res_01D665_00026B:;
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* add ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x6)); /* sub ax, 0x6 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x28)); /* sub ax, 0x28 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0xAA)); /* push word ds:[0xAA] */
    cpu->ax = (uint16_t)(0x3C);              /* mov ax, 0x3C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x28);              /* mov ax, 0x28 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x8C);              /* mov ax, 0x8C */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x28);              /* mov ax, 0x28 */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x2F4E); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x2F4E] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x28)); /* add ax, 0x28 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, mem_read16(cpu, cpu->ds, 0x19E8)); /* push word ds:[0x19E8] */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0000_07ED(cpu);                      /* call 0100:07ED */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x10)); /* add sp, 0x10 */
L_res_01D665_0002A0:;
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov si, word ss:[bp+0x8] */
    cpu->si = (uint16_t)(flags_add16(cpu, cpu->si, 0x5)); /* add si, 0x5 */
    cpu->ax = (uint16_t)(0xF);               /* mov ax, 0xF */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA))); /* mov ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x4)); /* sub ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->si);                    /* push si */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x50)); /* lea ax, word ss:[bp+-0x50] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_005E(cpu);                      /* call 0281:005E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->di = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA))); /* mov di, word ss:[bp+0xA] */
    cpu->di = (uint16_t)(flags_add16(cpu, cpu->di, 0x3)); /* add di, 0x3 */
    cpu->ax = (uint16_t)(0xB);               /* mov ax, 0xB */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->di);                    /* push di */
    cpu->ax = (uint16_t)((uint16_t)(cpu->bp - 0x50)); /* lea ax, word ss:[bp+-0x50] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_00F1(cpu);                      /* call 0281:00F1 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8)))); /* add ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->di);                    /* push di */
    push16(cpu, cpu->si);                    /* push si */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_000C(cpu);                      /* call 0281:000C */
L_res_01D665_0002E3:;
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xA)); /* add sp, 0xA */
L_res_01D665_0002E6:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F50), 0x0); /* cmp word ds:[0x2F50], 0x0 */
    if (cc_e(cpu)) goto L_res_01D665_000324; /* je 0x0324 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10))); /* mov ax, word ds:[bx+0x10] */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x56], ax */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(0x2)); /* mov word ds:[bx+0x10], 0x2 */
    cpu->ax = (uint16_t)(0xA);               /* mov ax, 0xA */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52))); /* mov ax, word ss:[bp+-0x52] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x4)); /* sub ax, 0x4 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xED3E)); /* mov ax, word ds:[0xED3E] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x4A)); /* sub ax, 0x4A */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x2F6D);            /* mov ax, 0x2F6D */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_005E(cpu);                      /* call 0281:005E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov ax, word ss:[bp+-0x56] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x10), (uint16_t)(cpu->ax)); /* mov word ds:[bx+0x10], ax */
L_res_01D665_000324:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x7FF6), 0x0); /* cmp word ds:[0x7FF6], 0x0 */
    if (cc_e(cpu)) goto L_res_01D665_00036A; /* je 0x036A */
    cpu->ax = (uint16_t)(0xB);               /* mov ax, 0xB */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52))); /* mov ax, word ss:[bp+-0x52] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x8)); /* sub ax, 0x8 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xED3E)); /* mov ax, word ds:[0xED3E] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x11)); /* sub ax, 0x11 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x2F7E);            /* mov ax, 0x2F7E */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_002C(cpu);                      /* call 0281:002C */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->ax = (uint16_t)(0xB);               /* mov ax, 0xB */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0xA);               /* mov ax, 0xA */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(0x14);              /* mov ax, 0x14 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x52))); /* mov ax, word ss:[bp+-0x52] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0xA)); /* sub ax, 0xA */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0xED3E)); /* mov ax, word ds:[0xED3E] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, 0x14)); /* sub ax, 0x14 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs);                    /* push cs */
    push16(cpu, 0);                          /* near call return addr */
    res_01DBF5(cpu);                         /* call 0x0590 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0xA)); /* add sp, 0xA */
L_res_01D665_00036A:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov bx, word ss:[bp+0x6] */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, cpu->bx), 0x20); /* cmp byte ds:[bx], 0x20 */
    if (cc_e(cpu)) goto L_res_01D665_000377; /* je 0x0377 */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, cpu->bx), 0x5F); /* cmp byte ds:[bx], 0x5F */
    if (cc_ne(cpu)) goto L_res_01D665_00037B; /* jne 0x037B */
L_res_01D665_000377:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    goto L_res_01D665_00037E;                /* jmp 0x037E */
L_res_01D665_00037B:;
    cpu->ax = (uint16_t)(0xFFFF);            /* mov ax, 0xFFFF */
L_res_01D665_00037E:;
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A), (uint16_t)(cpu->ax)); /* mov word ss:[bp+-0x5A], ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, 0xAA)); /* mov bx, word ds:[0xAA] */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x64DE)); /* mov ax, word ds:[0x64DE] */
    mem_write16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0xC), (uint16_t)(cpu->ax)); /* mov word ds:[bx+0xC], ax */
    mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(0x0)); /* mov word ss:[bp+-0x56], 0x0 */
    goto L_res_01D665_000488;                /* jmp 0x0488 */
L_res_01D665_000393:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    cpu->cl = (uint8_t)(mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A))); /* mov cl, byte ss:[bp+-0x5A] */
    { uint16_t _v = cpu->ax; uint8_t _c = cpu->cl; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* shl ax, cl */
    flags_logic16(cpu, mem_read16(cpu, cpu->ds, 0xC1A6) & cpu->ax); /* test word ds:[0xC1A6], ax */
    if (cc_e(cpu)) goto L_res_01D665_0003A6; /* je 0x03A6 */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    goto L_res_01D665_0003A8;                /* jmp 0x03A8 */
L_res_01D665_0003A6:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_res_01D665_0003A8:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov ax, word ss:[bp+-0x56] */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x2F54); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x2F54] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* add ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->di);                    /* push di */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_005E(cpu);                      /* call 0281:005E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->si + 0x64E0))); /* mov bx, word ds:[si+0x64E0] */
    cpu->bx = (uint16_t)(flags_add16(cpu, cpu->bx, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6)))); /* add bx, word ss:[bp+0x6] */
    mem_write8(cpu, cpu->ds, cpu->bx, (uint8_t)(0x20)); /* mov byte ds:[bx], 0x20 */
    goto L_res_01D665_000459;                /* jmp 0x0459 */
L_res_01D665_0003D4:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 0x0); /* cmp word ss:[bp+-0x5A], 0x0 */
    if (cc_ge(cpu)) goto L_res_01D665_00040E; /* jge 0x040E */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0x2F54), 0x9); /* cmp word ds:[0x2F54], 0x9 */
    if (cc_le(cpu)) goto L_res_01D665_00040E; /* jle 0x040E */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov ax, word ss:[bp+-0x56] */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x2F54); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x2F54] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* add ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x6)); /* add ax, 0x6 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x64E0))); /* mov ax, word ds:[bx+0x64E0] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6)))); /* add ax, word ss:[bp+0x6] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_005E(cpu);                      /* call 0281:005E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
L_res_01D665_00040E:;
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 0x0); /* cmp word ss:[bp+-0x5A], 0x0 */
    if (cc_ge(cpu)) goto L_res_01D665_000419; /* jge 0x0419 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x64DE)); /* mov ax, word ds:[0x64DE] */
    goto L_res_01D665_00042E;                /* jmp 0x042E */
L_res_01D665_000419:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    cpu->cl = (uint8_t)(mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A))); /* mov cl, byte ss:[bp+-0x5A] */
    { uint16_t _v = cpu->ax; uint8_t _c = cpu->cl; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* shl ax, cl */
    flags_logic16(cpu, mem_read16(cpu, cpu->ds, 0xC1A6) & cpu->ax); /* test word ds:[0xC1A6], ax */
    if (cc_e(cpu)) goto L_res_01D665_00042C; /* je 0x042C */
    cpu->ax = (uint16_t)(0x3);               /* mov ax, 0x3 */
    goto L_res_01D665_00042E;                /* jmp 0x042E */
L_res_01D665_00042C:;
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, cpu->ax)); /* sub ax, ax */
L_res_01D665_00042E:;
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov ax, word ss:[bp+-0x56] */
    { int32_t _r = (int32_t)(int16_t)cpu->ax * (int16_t)mem_read16(cpu, cpu->ds, 0x2F54); cpu->ax = (uint16_t)_r; cpu->dx = (uint16_t)((uint32_t)_r >> 16); cpu->flags = (cpu->flags & ~(FLAG_CF|FLAG_OF)) | ((uint32_t)_r != (uint32_t)(int32_t)(int16_t)_r ? FLAG_CF|FLAG_OF : 0); } /* imul word ds:[0x2F54] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xA)))); /* add ax, word ss:[bp+0xA] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x8))); /* mov ax, word ss:[bp+0x8] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, 0x5)); /* add ax, 0x5 */
    push16(cpu, cpu->ax);                    /* push ax */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x64E0))); /* mov ax, word ds:[bx+0x64E0] */
    cpu->ax = (uint16_t)(flags_add16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6)))); /* add ax, word ss:[bp+0x6] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_0181_005E(cpu);                      /* call 0281:005E */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x8)); /* add sp, 0x8 */
L_res_01D665_000459:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x64E2))); /* mov si, word ds:[bx+0x64E2] */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov bx, word ss:[bp+0x6] */
    mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si - 0x1), (uint8_t)(0xA)); /* mov byte ds:[bx+si+-0x1], 0xA */
L_res_01D665_000469:;
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov bx, word ss:[bp+-0x56] */
    { uint16_t _v = cpu->bx; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->bx = (uint16_t)(_r); } /* shl bx, 0x1 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->bx + 0x64E2))); /* mov bx, word ds:[bx+0x64E2] */
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6))); /* mov si, word ss:[bp+0x6] */
    cpu->al = (uint8_t)(mem_read8(cpu, cpu->ds, (uint16_t)(cpu->bx + cpu->si))); /* mov al, byte ds:[bx+si] */
    mem_write8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5C), (uint8_t)(cpu->al)); /* mov byte ss:[bp+-0x5C], al */
    flags_cmp8(cpu, cpu->al, 0x20);          /* cmp al, 0x20 */
    if (cc_e(cpu)) goto L_res_01D665_000482; /* je 0x0482 */
    flags_cmp8(cpu, cpu->al, 0x5F);          /* cmp al, 0x5F */
    if (cc_ne(cpu)) goto L_res_01D665_000485; /* jne 0x0485 */
L_res_01D665_000482:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x5A] */
L_res_01D665_000485:;
    { int _cf = cf(cpu); mem_write16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56), (uint16_t)(flags_add16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56)), 1))); if (_cf) cpu->flags |= FLAG_CF; else cpu->flags &= ~FLAG_CF; } /* inc word ss:[bp+-0x56] */
L_res_01D665_000488:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x6520)); /* mov ax, word ds:[0x6520] */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56)), cpu->ax); /* cmp word ss:[bp+-0x56], ax */
    if (cc_ge(cpu)) goto L_res_01D665_0004EE; /* jge 0x04EE */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ds, 0xE212), 0x0); /* cmp word ds:[0xE212], 0x0 */
    if (cc_ne(cpu)) goto L_res_01D665_0004AB; /* jne 0x04AB */
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xC))); /* mov ax, word ss:[bp+0xC] */
    cpu->ax = (uint16_t)(flags_sub16(cpu, cpu->ax, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)))); /* sub ax, word ss:[bp+-0x5A] */
    push16(cpu, cpu->ax);                    /* push ax */
    push16(cpu, cpu->cs); push16(cpu, 0);    /* far call return addr */
    far_205A_2AC0(cpu);                      /* call 215A:2AC0 */
    cpu->sp = (uint16_t)(flags_add16(cpu, cpu->sp, 0x2)); /* add sp, 0x2 */
    flags_cmp16(cpu, cpu->ax, 0x2);          /* cmp ax, 0x2 */
    if (cc_ge(cpu)) goto L_res_01D665_000469; /* jge 0x0469 */
L_res_01D665_0004AB:;
    cpu->si = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x56))); /* mov si, word ss:[bp+-0x56] */
    { uint16_t _v = cpu->si; uint8_t _c = 0x1; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->si = (uint16_t)(_r); } /* shl si, 0x1 */
    cpu->bx = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->si + 0x64E2))); /* mov bx, word ds:[si+0x64E2] */
    cpu->bx = (uint16_t)(flags_add16(cpu, cpu->bx, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6)))); /* add bx, word ss:[bp+0x6] */
    mem_write8(cpu, cpu->ds, (uint16_t)(cpu->bx - 0x1), (uint8_t)(0x0)); /* mov byte ds:[bx+-0x1], 0x0 */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 0x0); /* cmp word ss:[bp+-0x5A], 0x0 */
    if (cc_ge(cpu)) goto L_res_01D665_0004C4; /* jge 0x04C4 */
    goto L_res_01D665_0003D4;                /* jmp 0x03D4 */
L_res_01D665_0004C4:;
    cpu->ax = (uint16_t)(0x1);               /* mov ax, 0x1 */
    cpu->cl = (uint8_t)(mem_read8(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A))); /* mov cl, byte ss:[bp+-0x5A] */
    { uint16_t _v = cpu->ax; uint8_t _c = cpu->cl; uint16_t _r = _v << _c; cpu->flags = (cpu->flags & ~FLAG_CF) | ((_v >> (16 - _c)) & 1 ? FLAG_CF : 0); flags_shift16(cpu, _r); cpu->ax = (uint16_t)(_r); } /* shl ax, cl */
    flags_logic16(cpu, mem_read16(cpu, cpu->ds, 0xE722) & cpu->ax); /* test word ds:[0xE722], ax */
    if (cc_ne(cpu)) goto L_res_01D665_0004D5; /* jne 0x04D5 */
    goto L_res_01D665_0003D4;                /* jmp 0x03D4 */
L_res_01D665_0004D5:;
    cpu->di = (uint16_t)(mem_read16(cpu, cpu->ds, (uint16_t)(cpu->si + 0x64E0))); /* mov di, word ds:[si+0x64E0] */
    cpu->di = (uint16_t)(flags_add16(cpu, cpu->di, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0x6)))); /* add di, word ss:[bp+0x6] */
    mem_write8(cpu, cpu->ds, cpu->di, (uint8_t)(0x5E)); /* mov byte ds:[di], 0x5E */
    flags_cmp16(cpu, mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x5A)), 0x0); /* cmp word ss:[bp+-0x5A], 0x0 */
    if (cc_l(cpu)) goto L_res_01D665_0004E8; /* jl 0x04E8 */
    goto L_res_01D665_000393;                /* jmp 0x0393 */
L_res_01D665_0004E8:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ds, 0x64DE)); /* mov ax, word ds:[0x64DE] */
    goto L_res_01D665_0003A8;                /* jmp 0x03A8 */
L_res_01D665_0004EE:;
    cpu->ax = (uint16_t)(mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp + 0xC))); /* mov ax, word ss:[bp+0xC] */
    cpu->si = (uint16_t)(pop16(cpu));        /* pop si */
    cpu->di = (uint16_t)(pop16(cpu));        /* pop di */
    cpu->sp = (uint16_t)(cpu->bp);           /* mov sp, bp */
    cpu->bp = (uint16_t)(pop16(cpu));        /* pop bp */
    cpu->sp += 4; return;                    /* retf */
}

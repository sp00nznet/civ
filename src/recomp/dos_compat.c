/*
 * dos_compat.c - DOS API Compatibility Layer
 *
 * Implements INT 21h DOS services, INT 10h video BIOS, INT 16h keyboard,
 * and INT 33h mouse driver for the Civilization static recompilation.
 *
 * DOS API functions used by CIV.EXE (from binary analysis):
 *   File I/O: 3Ch create, 3Dh open, 3Eh close, 3Fh read, 40h write,
 *             42h seek, 41h delete
 *   Memory:   48h alloc, 49h free, 4Ah resize
 *   Console:  02h char out, 08h char in, 09h print, 0Bh check input
 *   System:   19h get drive, 25h set vector, 2Ah date, 2Ch time,
 *             30h DOS version, 35h get vector, 47h get dir
 *   Exit:     00h/4Ch terminate
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/dos_compat.h"
#include "hal/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global DOS state - accessible from interrupt handlers */
static DosState *g_dos = NULL;

DosState *get_dos_state(CPU *cpu)
{
    (void)cpu;
    return g_dos;
}

/* ─── File path translation ─── */

static void dos_path_to_native(const CPU *cpu, uint16_t seg, uint16_t off,
                                char *out, int out_size)
{
    /* Read DOS path string from memory */
    char dos_path[260];
    int i = 0;
    while (i < 259) {
        uint8_t c = cpu->mem[seg_off(seg, (uint16_t)(off + i))];
        if (c == 0) break;
        /* Convert backslash to forward slash */
        dos_path[i] = (c == '\\') ? '/' : (char)c;
        i++;
    }
    dos_path[i] = 0;

    /* Prepend game directory */
    snprintf(out, out_size, "%s/%s", g_dos->game_dir, dos_path);
}

/* ─── DOS File Handle Management ─── */

static int dos_alloc_handle(FILE *f)
{
    DosFileTable *ft = &g_dos->file_table;
    for (int i = 5; i < DOS_MAX_HANDLES; i++) {  /* 0-4 reserved for stdin/out/err/aux/prn */
        if (ft->files[i] == NULL) {
            ft->files[i] = f;
            return i;
        }
    }
    return -1;
}

static FILE *dos_get_file(int handle)
{
    if (handle < 0 || handle >= DOS_MAX_HANDLES) return NULL;
    return g_dos->file_table.files[handle];
}

static void dos_close_handle(int handle)
{
    if (handle >= 5 && handle < DOS_MAX_HANDLES) {
        if (g_dos->file_table.files[handle]) {
            fclose(g_dos->file_table.files[handle]);
            g_dos->file_table.files[handle] = NULL;
        }
    }
}

/* ─── Initialization ─── */

void dos_init(DosState *ds, CPU *cpu, const char *game_dir)
{
    memset(ds, 0, sizeof(*ds));
    strncpy(ds->game_dir, game_dir, sizeof(ds->game_dir) - 1);
    g_dos = ds;

    /* Initialize subsystems */
    video_init(&ds->video);
    keyboard_init(&ds->keyboard);
    mouse_init(&ds->mouse);
    timer_init(&ds->timer);

    /* Set up standard file handles */
    ds->file_table.files[0] = stdin;
    ds->file_table.files[1] = stdout;
    ds->file_table.files[2] = stderr;
    ds->file_table.files[3] = NULL;  /* AUX */
    ds->file_table.files[4] = NULL;  /* PRN */

    /* Set up BIOS data area */
    /* Equipment word at 0040:0010 */
    mem_write16(cpu, 0x0040, 0x0010, 0x0021);  /* Color, 1 floppy */
    /* Memory size at 0040:0013 (in KB) */
    mem_write16(cpu, 0x0040, 0x0013, 640);
    /* Video mode at 0040:0049 - start in text mode (game switches to 13h) */
    mem_write8(cpu, 0x0040, 0x0049, 0x03);  /* Mode 3: 80x25 text */
    /* Screen columns at 0040:004A */
    mem_write16(cpu, 0x0040, 0x004A, 80);

    ds->mem_top = 0x4000;  /* Start of free far memory (after program code+data) */

    printf("[DOS] Initialized with game dir: %s\n", game_dir);
}

/* ─── INT 21h - DOS API ─── */

void dos_int21(CPU *cpu)
{
    uint8_t ah = cpu->ah;

    switch (ah) {
    case 0x00: /* Terminate program */
        printf("[DOS] Program terminated (INT 21h/00)\n");
        cpu->halted = 1;
        break;

    case 0x01: /* Character input with echo (blocking) */ {
        KeyboardState *ks = &g_dos->keyboard;
        fprintf(stderr, "[BLOCK] Waiting for key in INT 21h/01\n");
        while (!keyboard_available(ks)) {
            if (g_dos->poll_events)
                g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
        }
        uint16_t key = keyboard_read(ks);
        cpu->al = (uint8_t)(key & 0xFF);
        fprintf(stderr, "[KEY] INT 21h/01: 0x%04X (ascii='%c')\n",
                key, (cpu->al >= 32 && cpu->al < 127) ? cpu->al : '.');
        break;
    }

    case 0x02: /* Character output */
        /* Silently consume - game text goes via INT 10h to VGA memory */
        break;

    case 0x08: /* Character input without echo */
    case 0x07: {
        KeyboardState *ks = &g_dos->keyboard;
        fprintf(stderr, "[BLOCK] Waiting for key in INT 21h/%02Xh\n", ah);
        while (!keyboard_available(ks)) {
            if (g_dos->poll_events)
                g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
        }
        uint16_t key = keyboard_read(ks);
        cpu->al = (uint8_t)(key & 0xFF);
        fprintf(stderr, "[KEY] INT 21h/%02Xh: 0x%04X\n", ah, key);
        break;
    }

    case 0x09: { /* Print string (terminated by '$') */
        /* Output to text mode video memory via INT 10h teletype.
         * Safety: skip if the first byte isn't printable text -
         * the game's overlay init calls this with pointers to binary data. */
        uint32_t addr = seg_off(cpu->ds, cpu->dx);
        if (addr >= MEM_SIZE) break;
        uint8_t first = cpu->mem[addr];
        /* Only print if first char is printable ASCII, space, or common control */
        if (first != '$' && (first >= 0x20 && first < 0x7F) || first == 0x0D || first == 0x0A || first == 0x09) {
            int count = 0;
            while (addr < MEM_SIZE && cpu->mem[addr] != '$' && count < 2000) {
                cpu->al = cpu->mem[addr];
                uint8_t save_ah = cpu->ah;
                cpu->ah = 0x0E;
                bios_int10(cpu);
                cpu->ah = save_ah;
                addr++;
                count++;
            }
        }
        break;
    }

    case 0x0A: { /* Buffered input */
        /* Read a line into buffer at DS:DX */
        uint8_t max_len = ds_read8(cpu, cpu->dx);
        char buf[256];
        if (fgets(buf, max_len, stdin)) {
            int len = (int)strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[--len] = '\r';
            ds_write8(cpu, (uint16_t)(cpu->dx + 1), (uint8_t)len);
            for (int i = 0; i < len; i++)
                ds_write8(cpu, (uint16_t)(cpu->dx + 2 + i), (uint8_t)buf[i]);
            ds_write8(cpu, (uint16_t)(cpu->dx + 2 + len), 0x0D);
        }
        break;
    }

    case 0x0B: /* Check keyboard input status */
        if (g_dos->poll_events)
            g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
        cpu->al = keyboard_available(&g_dos->keyboard) ? 0xFF : 0x00;
        break;

    case 0x0E: /* Select disk */
        cpu->al = 5;  /* Return 5 logical drives */
        break;

    case 0x11: /* Find first (FCB) */
    case 0x12: /* Find next (FCB) */
        cpu->al = 0xFF;  /* Not found */
        break;

    case 0x19: /* Get current disk */
        cpu->al = 2;  /* C: drive */
        break;

    case 0x1A: /* Set DTA address */
        /* Store DTA at DS:DX - we just track the pointer */
        break;

    case 0x25: /* Set interrupt vector */
        /* AL = interrupt number, DS:DX = handler */
        g_dos->ivt[cpu->al] = ((uint32_t)cpu->ds << 16) | cpu->dx;
        break;

    case 0x2A: { /* Get date */
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        cpu->cx = (uint16_t)(tm->tm_year + 1900);
        cpu->dh = (uint8_t)(tm->tm_mon + 1);
        cpu->dl = (uint8_t)tm->tm_mday;
        cpu->al = (uint8_t)tm->tm_wday;
        break;
    }

    case 0x2C: { /* Get time */
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        cpu->ch = (uint8_t)tm->tm_hour;
        cpu->cl = (uint8_t)tm->tm_min;
        cpu->dh = (uint8_t)tm->tm_sec;
        cpu->dl = 0;  /* hundredths */
        break;
    }

    case 0x30: /* Get DOS version */
        cpu->al = 5;   /* DOS 5.0 */
        cpu->ah = 0;
        cpu->bx = 0;
        cpu->cx = 0;
        break;

    case 0x35: { /* Get interrupt vector */
        uint32_t vec = g_dos->ivt[cpu->al];
        cpu->es = (uint16_t)(vec >> 16);
        cpu->bx = (uint16_t)(vec & 0xFFFF);
        break;
    }

    case 0x3C: { /* Create file */
        char path[512];
        dos_path_to_native(cpu, cpu->ds, cpu->dx, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (f) {
            int handle = dos_alloc_handle(f);
            if (handle >= 0) {
                cpu->ax = (uint16_t)handle;
                cpu->flags &= ~FLAG_CF;  /* Success */
            } else {
                fclose(f);
                cpu->ax = 4;  /* Too many open files */
                cpu->flags |= FLAG_CF;
            }
        } else {
            cpu->ax = 3;  /* Path not found */
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    case 0x3D: { /* Open file */
        char path[512];
        dos_path_to_native(cpu, cpu->ds, cpu->dx, path, sizeof(path));
        const char *mode;
        switch (cpu->al & 3) {
            case 0: mode = "rb"; break;     /* Read only */
            case 1: mode = "r+b"; break;    /* Write only */
            case 2: mode = "r+b"; break;    /* Read/write */
            default: mode = "rb"; break;
        }
        FILE *f = fopen(path, mode);
        if (f) {
            int handle = dos_alloc_handle(f);
            if (handle >= 0) {
                cpu->ax = (uint16_t)handle;
                cpu->flags &= ~FLAG_CF;
                fprintf(stderr, "[FILE] Open '%s' -> handle %d\n", path, handle);
            } else {
                fclose(f);
                cpu->ax = 4;
                cpu->flags |= FLAG_CF;
                fprintf(stderr, "[FILE] Open '%s' FAIL (no handles)\n", path);
            }
        } else {
            cpu->ax = 2;  /* File not found */
            cpu->flags |= FLAG_CF;
            fprintf(stderr, "[FILE] Open '%s' FAIL (not found) DS:DX=%04X:%04X raw=",
                    path, cpu->ds, cpu->dx);
            for (int j = 0; j < 16; j++)
                fprintf(stderr, "%02X ", cpu->mem[seg_off(cpu->ds, (uint16_t)(cpu->dx + j))]);
            fprintf(stderr, "\n");
        }
        break;
    }

    case 0x3E: { /* Close file */
        fprintf(stderr, "[FILE] Close handle %d\n", cpu->bx);
        dos_close_handle(cpu->bx);
        cpu->flags &= ~FLAG_CF;
        break;
    }

    case 0x3F: { /* Read file */
        if (cpu->bx == 0) {
            /* Handle 0 = stdin (CON device).
             * Non-blocking: return any available keyboard input, or 0 (EOF).
             * This handles both intentional stdin reads and corrupted FILE
             * structs that default to handle 0. */
            uint32_t dest = seg_off(cpu->ds, cpu->dx);
            uint16_t count = cpu->cx;
            uint16_t got = 0;
            /* Pump events so keys can arrive */
            if (g_dos->poll_events)
                g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
            while (got < count && keyboard_available(&g_dos->keyboard)) {
                uint16_t key = keyboard_read(&g_dos->keyboard);
                uint8_t ascii = (uint8_t)(key & 0xFF);
                if (ascii == 0) continue;  /* skip extended keys */
                cpu->mem[dest + got] = ascii;
                got++;
                if (ascii == 0x0D) break;
            }
            cpu->ax = got;
            cpu->flags &= ~FLAG_CF;
            { static int _rc0 = 0; _rc0++;
              if (_rc0 <= 10) fprintf(stderr, "[FILE] Read stdin %u bytes -> %u\n", count, got);
              if (_rc0 == 1) {
                  /* Dump VGA text mode buffer and key DS variables on first stdin read */
                  fprintf(stderr, "[DIAG] VGA text at first stdin read:\n");
                  for (int row = 0; row < 25; row++) {
                      char line[81];
                      int any_nonspace = 0;
                      for (int col = 0; col < 80; col++) {
                          uint8_t ch = cpu->mem[0xB8000 + row * 160 + col * 2];
                          line[col] = (ch >= 32 && ch < 127) ? (char)ch : '.';
                          if (ch > 32 && ch < 127) any_nonspace = 1;
                      }
                      line[80] = 0;
                      if (any_nonspace) fprintf(stderr, "[VGA] %02d: %s\n", row, line);
                  }
                  /* Dump key DS offsets */
                  fprintf(stderr, "[DIAG] DS:0x0098=[%04X] DS:0xAA=[%04X] DS:0x6AC2=[%04X]\n",
                      mem_read16(cpu, cpu->ds, 0x0098),
                      mem_read16(cpu, cpu->ds, 0x00AA),
                      mem_read16(cpu, cpu->ds, 0x6AC2));
                  fprintf(stderr, "[DIAG] video mode=0x%02X SP=%04X BP=%04X\n",
                      cpu->mem[0x449], cpu->sp, cpu->bp);
              }
            }
        } else {
            FILE *f = dos_get_file(cpu->bx);
            if (f) {
                uint32_t dest = seg_off(cpu->ds, cpu->dx);
                size_t n = fread(cpu->mem + dest, 1, cpu->cx, f);
                cpu->ax = (uint16_t)n;
                cpu->flags &= ~FLAG_CF;
                { static int _rc = 0; _rc++;
                  if (_rc <= 20 || n == 0) fprintf(stderr, "[FILE] Read h=%d %u bytes -> %zu (call #%d)\n", cpu->bx, cpu->cx, n, _rc);
                }
            } else {
                cpu->ax = 6;  /* Invalid handle */
                cpu->flags |= FLAG_CF;
                fprintf(stderr, "[FILE] Read h=%d FAIL (invalid)\n", cpu->bx);
            }
        }
        break;
    }

    case 0x40: { /* Write file */
        FILE *f = dos_get_file(cpu->bx);
        if (f) {
            uint32_t src = seg_off(cpu->ds, cpu->dx);
            size_t n = fwrite(cpu->mem + src, 1, cpu->cx, f);
            cpu->ax = (uint16_t)n;
            cpu->flags &= ~FLAG_CF;
        } else if (cpu->bx == 1 || cpu->bx == 2) {
            /* stdout/stderr */
            uint32_t src = seg_off(cpu->ds, cpu->dx);
            fwrite(cpu->mem + src, 1, cpu->cx, cpu->bx == 1 ? stdout : stderr);
            cpu->ax = cpu->cx;
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = 6;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    case 0x41: { /* Delete file */
        char path[512];
        dos_path_to_native(cpu, cpu->ds, cpu->dx, path, sizeof(path));
        if (remove(path) == 0) {
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = 2;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    case 0x42: { /* Move file pointer (seek) */
        FILE *f = dos_get_file(cpu->bx);
        if (f) {
            long offset = (long)(((uint32_t)cpu->cx << 16) | cpu->dx);
            int whence;
            switch (cpu->al) {
                case 0: whence = SEEK_SET; break;
                case 1: whence = SEEK_CUR; break;
                case 2: whence = SEEK_END; break;
                default: whence = SEEK_SET; break;
            }
            fseek(f, offset, whence);
            long pos = ftell(f);
            cpu->ax = (uint16_t)(pos & 0xFFFF);
            cpu->dx = (uint16_t)((pos >> 16) & 0xFFFF);
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = 6;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    case 0x47: { /* Get current directory */
        /* DS:SI = buffer for path, DL = drive (0=default) */
        uint32_t dest = seg_off(cpu->ds, cpu->si);
        cpu->mem[dest] = 0;  /* Root directory */
        cpu->flags &= ~FLAG_CF;
        break;
    }

    case 0x48: { /* Allocate memory */
        /* BX = paragraphs requested */
        uint16_t paras = cpu->bx;
        if (g_dos->mem_top + paras < 0xA000) {
            cpu->ax = g_dos->mem_top;
            fprintf(stderr, "[DOS] Alloc %u paras (%u bytes) -> seg 0x%04X\n",
                    paras, (unsigned)paras * 16, cpu->ax);
            g_dos->mem_top += paras;
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = 8;  /* Insufficient memory */
            cpu->bx = (uint16_t)(0xA000 - g_dos->mem_top);
            fprintf(stderr, "[DOS] Alloc FAIL: %u paras requested, %u available\n",
                    paras, cpu->bx);
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    case 0x43: { /* Get/Set file attributes */
        if (cpu->al == 0x00) {
            /* Get file attributes: DS:DX = filename */
            /* Return CX = normal file attributes */
            cpu->cx = 0x0020; /* Archive bit set (normal file) */
            cpu->flags &= ~FLAG_CF;
        } else {
            /* Set file attributes - just succeed */
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    case 0x49: /* Free memory */
        /* ES = segment to free - simplified, just succeed */
        cpu->flags &= ~FLAG_CF;
        break;

    case 0x4A: /* Resize memory block */
        /* ES = segment, BX = new size in paragraphs */
        cpu->flags &= ~FLAG_CF;  /* Always succeed */
        break;

    case 0x44: { /* IOCTL - Get/Set device information */
        uint8_t al = cpu->al;
        switch (al) {
        case 0x00: /* Get device information */
            if (cpu->bx <= 4) {
                /* Standard handles 0-4 are character devices */
                /* Bit 7 = is device, Bit 0 = stdin, Bit 1 = stdout/stderr */
                uint16_t info = 0x80; /* is device */
                if (cpu->bx == 0) info |= 0x01;  /* stdin */
                if (cpu->bx == 1 || cpu->bx == 2) info |= 0x02; /* stdout/stderr */
                cpu->dx = info;
                cpu->flags &= ~FLAG_CF;
            } else {
                /* File handle - return disk file info */
                cpu->dx = 0x0000; /* disk file, drive A (bits 5-0) */
                cpu->flags &= ~FLAG_CF;
            }
            break;
        case 0x01: /* Set device information */
            cpu->flags &= ~FLAG_CF; /* just succeed */
            break;
        case 0x06: /* Get input status */
            cpu->al = 0xFF; /* ready */
            cpu->flags &= ~FLAG_CF;
            break;
        case 0x07: /* Get output status */
            cpu->al = 0xFF; /* ready */
            cpu->flags &= ~FLAG_CF;
            break;
        default:
            fprintf(stderr, "[DOS] Unhandled IOCTL sub-function AL=%02Xh BX=%04Xh\n", al, cpu->bx);
            cpu->flags &= ~FLAG_CF;
            break;
        }
        break;
    }

    case 0x4C: /* Terminate with return code */
        printf("[DOS] Program exit with code %d\n", cpu->al);
        cpu->halted = 1;
        break;

    case 0x62: /* Get PSP segment */
        cpu->bx = 0x0100;  /* PSP at segment 0100h */
        break;

    default:
        fprintf(stderr, "[DOS] Unhandled INT 21h AH=%02Xh AL=%02Xh BX=%04Xh CX=%04Xh DX=%04Xh\n",
                ah, cpu->al, cpu->bx, cpu->cx, cpu->dx);
        break;
    }
}

/* ─── INT 10h - Video BIOS ─── */

/* Text mode video memory at 0xB8000, 80x25, 2 bytes per cell (char + attr) */
#define TEXT_MODE_BASE  0xB8000
#define TEXT_COLS       80
#define TEXT_ROWS       25

void bios_int10(CPU *cpu)
{
    switch (cpu->ah) {
    case 0x00: /* Set video mode */
        fprintf(stderr, "[VIDEO] Set mode 0x%02X\n", cpu->al);
        /* Store current mode in BIOS data area */
        mem_write8(cpu, 0x0040, 0x0049, cpu->al);
        if (cpu->al == 0x13) {
            /* Mode 13h: 320x200x256 - clear VGA framebuffer */
            memset(&cpu->mem[0xA0000], 0, 64000);
            mem_write16(cpu, 0x0040, 0x004A, 40);
        } else if (cpu->al <= 0x03) {
            /* Text mode - clear text mode buffer */
            memset(&cpu->mem[TEXT_MODE_BASE], 0, TEXT_COLS * TEXT_ROWS * 2);
            mem_write16(cpu, 0x0040, 0x004A, 80);
            /* Reset cursor to top-left */
            mem_write8(cpu, 0x0040, 0x0050, 0);
            mem_write8(cpu, 0x0040, 0x0051, 0);
        }
        break;

    case 0x02: /* Set cursor position */
        /* BH = page, DH = row, DL = column */
        mem_write8(cpu, 0x0040, 0x0050, cpu->dl);
        mem_write8(cpu, 0x0040, 0x0051, cpu->dh);
        break;

    case 0x03: /* Get cursor position */
        /* BH = page → returns DH=row, DL=column, CH/CL=cursor shape */
        cpu->dl = mem_read8(cpu, 0x0040, 0x0050);
        cpu->dh = mem_read8(cpu, 0x0040, 0x0051);
        cpu->ch = 0;
        cpu->cl = 0;
        break;

    case 0x09: /* Write character and attribute at cursor */
        /* AL = character, BL = attribute, CX = count */
        /* Write to text mode video memory (no cursor advance) */ {
        uint8_t col = mem_read8(cpu, 0x0040, 0x0050);
        uint8_t row = mem_read8(cpu, 0x0040, 0x0051);
        for (int i = 0; i < cpu->cx; i++) {
            int pos = (row * TEXT_COLS + col + i) % (TEXT_COLS * TEXT_ROWS);
            uint32_t addr = TEXT_MODE_BASE + pos * 2;
            if (addr + 1 < MEM_SIZE) {
                cpu->mem[addr] = cpu->al;
                cpu->mem[addr + 1] = (uint8_t)cpu->bl;
            }
        }
        break;
        }

    case 0x0E: /* Teletype output */
        /* Write character and advance cursor */ {
        uint8_t col = mem_read8(cpu, 0x0040, 0x0050);
        uint8_t row = mem_read8(cpu, 0x0040, 0x0051);
        if (cpu->al == 0x0D) {
            col = 0;  /* Carriage return */
        } else if (cpu->al == 0x0A) {
            row++;    /* Line feed */
        } else if (cpu->al == 0x08) {
            if (col > 0) col--;  /* Backspace */
        } else {
            int pos = row * TEXT_COLS + col;
            uint32_t addr = TEXT_MODE_BASE + pos * 2;
            if (addr + 1 < MEM_SIZE) {
                cpu->mem[addr] = cpu->al;
                cpu->mem[addr + 1] = 0x07; /* Default attribute */
            }
            col++;
            if (col >= TEXT_COLS) { col = 0; row++; }
        }
        if (row >= TEXT_ROWS) row = TEXT_ROWS - 1;
        mem_write8(cpu, 0x0040, 0x0050, col);
        mem_write8(cpu, 0x0040, 0x0051, row);
        break;
        }

    case 0x0F: /* Get video mode */
        cpu->al = mem_read8(cpu, 0x0040, 0x0049);
        if (cpu->al == 0) cpu->al = 0x03; /* Default to mode 3 */
        cpu->ah = (cpu->al == 0x13) ? 40 : TEXT_COLS;
        cpu->bh = 0;     /* Page */
        break;

    default:
        break;
    }
}

/* ─── INT 16h - Keyboard BIOS ─── */

void bios_int16(CPU *cpu)
{
    KeyboardState *ks = &g_dos->keyboard;

    switch (cpu->ah) {
    case 0x00: /* Read key (blocking) */
    case 0x10: /* Extended read key */
        fprintf(stderr, "[BLOCK] Waiting for key in INT 16h/%02Xh\n", cpu->ah);
        while (!keyboard_available(ks)) {
            if (g_dos->poll_events)
                g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
        }
        cpu->ax = keyboard_read(ks);
        fprintf(stderr, "[KEY] INT 16h/%02Xh: 0x%04X\n", cpu->ah, cpu->ax);
        break;

    case 0x01: /* Check for key */
    case 0x11:
        /* Pump events before checking */
        if (g_dos->poll_events)
            g_dos->poll_events(g_dos->platform_ctx, g_dos, cpu);
        if (keyboard_available(ks)) {
            cpu->ax = ks->keybuf[ks->head];
            cpu->flags &= ~FLAG_ZF;  /* Key available */
        } else {
            cpu->flags |= FLAG_ZF;   /* No key */
        }
        break;

    case 0x02: /* Get shift flags */
        cpu->al = 0;  /* No shift keys */
        break;

    default:
        break;
    }
}

/* ─── INT 33h - Mouse Driver ─── */

void mouse_int33(CPU *cpu)
{
    MouseState *ms = &g_dos->mouse;

    switch (cpu->ax) {
    case 0x0000: /* Reset / detect mouse */
        mouse_init(ms);
        cpu->ax = 0xFFFF;  /* Mouse installed */
        cpu->bx = 3;       /* 3 buttons */
        break;

    case 0x0001: /* Show cursor */
        ms->visible = 1;
        break;

    case 0x0002: /* Hide cursor */
        ms->visible = 0;
        break;

    case 0x0003: /* Get position and button status */
        cpu->bx = ms->buttons;
        cpu->cx = (uint16_t)ms->x;
        cpu->dx = (uint16_t)ms->y;
        break;

    case 0x0004: /* Set position */
        ms->x = (int16_t)cpu->cx;
        ms->y = (int16_t)cpu->dx;
        break;

    case 0x0007: /* Set horizontal range */
        ms->min_x = (int16_t)cpu->cx;
        ms->max_x = (int16_t)cpu->dx;
        break;

    case 0x0008: /* Set vertical range */
        ms->min_y = (int16_t)cpu->cx;
        ms->max_y = (int16_t)cpu->dx;
        break;

    case 0x000C: /* Set event handler */
        /* CX = event mask, ES:DX = handler
         * We don't call the handler directly in recomp;
         * instead we poll in the main loop */
        break;

    default:
        break;
    }
}

/* ─── Generic interrupt handler ─── */

void int_handler(CPU *cpu, uint8_t num)
{
    static uint64_t call_count = 0;
    call_count++;
    if (call_count <= 3 || (call_count % 5000) == 0)
        fprintf(stderr, "[INT] #%llu int=0x%02X\n", (unsigned long long)call_count, num);
    switch (num) {
    case 0x08: /* Timer tick - update timer state */
        timer_update(&g_dos->timer, 0);  /* Will use SDL tick in main loop */
        break;

    case 0x20: /* Terminate */
        cpu->halted = 1;
        break;

    default:
        /* Most other interrupts are safe to ignore */
        break;
    }
}

/* ─── Port I/O ─── */

void port_out8(CPU *cpu, uint16_t port, uint8_t value)
{
    DosState *ds = g_dos;

    /* VGA DAC palette ports */
    if (port >= 0x3C7 && port <= 0x3C9) {
        video_port_write(&ds->video, port, value);
        return;
    }

    /* PIT timer ports */
    if (port == 0x40 || port == 0x43) {
        timer_port_write(&ds->timer, port, value);
        return;
    }

    /* PIC - acknowledge interrupt */
    if (port == 0x20) {
        return;  /* EOI - ignore */
    }

    /* All other ports: silently ignore */
    (void)cpu;
}

uint8_t port_in8(CPU *cpu, uint16_t port)
{
    DosState *ds = g_dos;

    /* VGA status register */
    if (port == 0x3DA) {
        return video_port_read(&ds->video, port);
    }

    /* VGA DAC read */
    if (port == 0x3C9) {
        return video_port_read(&ds->video, port);
    }

    /* PIT timer read */
    if (port == 0x40) {
        return timer_port_read(&ds->timer, port);
    }

    /* DMA channel word count registers (0x01, 0x03, 0x05, 0x07) */
    /* Port 0x0004 is not a standard DMA port but CIV.EXE reads it in a
     * timing loop, expecting the value to change.  Return a fast-moving
     * counter so the loop exits quickly. */
    if (port <= 0x07 || port == 0x0004) {
        static uint8_t dma_counter = 0;
        return dma_counter++;
    }

    /* Keyboard data port */
    if (port == 0x60) {
        return 0;  /* No key */
    }

    (void)cpu;
    return 0;
}

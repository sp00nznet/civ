/*
 * picdecode - Civilization .PIC/.PAL image format analyzer
 *
 * Civilization uses a custom MicroProse image format for all its
 * artwork: title screens, unit sprites, terrain tiles, city views,
 * diplomacy portraits, wonder images, and map graphics.
 *
 * The .PIC files contain 256-color or 16-color image data, often
 * with an embedded or separate .PAL palette. Multiple graphics
 * driver executables (egraphic.exe for EGA, mgraphic.exe for MCGA/VGA,
 * tgraphic.exe for Tandy) handle the actual display.
 *
 * This tool analyzes and attempts to decode the .PIC format for
 * format documentation and eventual reimplementation.
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static void dump_hex(const uint8_t *data, int len, int offset)
{
    for (int i = 0; i < len; i += 16) {
        printf("  %06X: ", offset + i);
        for (int j = 0; j < 16 && i + j < len; j++)
            printf("%02X ", data[i + j]);
        for (int j = len - i; j < 16; j++)
            printf("   ");
        printf(" ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            uint8_t b = data[i + j];
            putchar(b >= 32 && b < 127 ? b : '.');
        }
        putchar('\n');
    }
}

static void analyze_pal(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    printf("\n  Palette: %s (%ld bytes)\n", path, sz);

    /* VGA palette: 256 entries * 3 bytes (R,G,B) = 768 bytes
     * VGA DAC uses 6-bit values (0-63) */
    if (sz == 768) {
        printf("  Format: Standard VGA palette (256 colors, 6-bit RGB)\n");
        printf("  First 16 colors:\n");
        for (int i = 0; i < 16; i++) {
            printf("    Color %2d: R=%2d G=%2d B=%2d (8-bit: #%02X%02X%02X)\n",
                   i, data[i*3], data[i*3+1], data[i*3+2],
                   (data[i*3] * 255 / 63),
                   (data[i*3+1] * 255 / 63),
                   (data[i*3+2] * 255 / 63));
        }
    } else if (sz == 48) {
        printf("  Format: EGA palette (16 colors, 6-bit RGB)\n");
        for (int i = 0; i < 16; i++) {
            printf("    Color %2d: R=%2d G=%2d B=%2d\n",
                   i, data[i*3], data[i*3+1], data[i*3+2]);
        }
    } else {
        printf("  Format: Unknown (%ld bytes)\n", sz);
        dump_hex(data, sz < 128 ? (int)sz : 128, 0);
    }

    free(data);
}

static void write_bmp(const char *path, const uint8_t *pixels, int width, int height,
                      const uint8_t *palette, int pal_entries)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;

    uint32_t row_bytes = ((width + 3) / 4) * 4;
    uint32_t img_size = row_bytes * height;
    uint32_t pal_size = pal_entries * 4;
    uint32_t offset = 14 + 40 + pal_size;
    uint32_t file_size = offset + img_size;

    /* BMP file header */
    uint8_t bfh[14] = {'B','M'};
    *(uint32_t *)(bfh + 2) = file_size;
    *(uint32_t *)(bfh + 10) = offset;
    fwrite(bfh, 1, 14, f);

    /* BMP info header */
    uint8_t bih[40] = {0};
    *(uint32_t *)(bih + 0) = 40;
    *(int32_t *)(bih + 4) = width;
    *(int32_t *)(bih + 8) = height;
    *(uint16_t *)(bih + 12) = 1;
    *(uint16_t *)(bih + 14) = 8;
    *(uint32_t *)(bih + 20) = img_size;
    *(uint32_t *)(bih + 32) = pal_entries;
    fwrite(bih, 1, 40, f);

    /* palette (BGRA) */
    for (int i = 0; i < pal_entries; i++) {
        uint8_t bgra[4];
        if (palette) {
            /* convert 6-bit VGA to 8-bit */
            bgra[2] = (palette[i*3 + 0] * 255 / 63); /* R */
            bgra[1] = (palette[i*3 + 1] * 255 / 63); /* G */
            bgra[0] = (palette[i*3 + 2] * 255 / 63); /* B */
        } else {
            bgra[0] = bgra[1] = bgra[2] = (uint8_t)(i * 255 / (pal_entries - 1));
        }
        bgra[3] = 0;
        fwrite(bgra, 1, 4, f);
    }

    /* pixel data (bottom-up) */
    uint8_t *row = (uint8_t *)calloc(1, row_bytes);
    for (int y = height - 1; y >= 0; y--) {
        memcpy(row, pixels + y * width, width);
        fwrite(row, 1, row_bytes, f);
    }

    free(row);
    fclose(f);
    printf("  Wrote BMP: %s (%dx%d)\n", path, width, height);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "picdecode - Civilization .PIC/.PAL Image Analyzer\n"
            "Part of the Civ Recomp project (sp00nznet/civ)\n\n"
            "Usage: picdecode <file.pic> [file.pal] [-decode]\n"
            "\nAnalyzes MicroProse .PIC image format and optionally\n"
            "decodes to BMP using the associated .PAL palette.\n");
        return 1;
    }

    const char *pic_path = argv[1];
    const char *pal_path = NULL;
    int do_decode = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-decode") == 0) do_decode = 1;
        else pal_path = argv[i];
    }

    FILE *f = fopen(pic_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", pic_path); return 1; }
    fseek(f, 0, SEEK_END);
    long pic_sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pic = (uint8_t *)malloc(pic_sz);
    fread(pic, 1, pic_sz, f);
    fclose(f);

    printf("=== PIC File Analysis: %s (%ld bytes) ===\n", pic_path, pic_sz);

    /* header analysis */
    printf("\n  Header (first 64 bytes):\n");
    dump_hex(pic, pic_sz < 64 ? (int)pic_sz : 64, 0);

    /* check first byte for graphics mode indicator */
    char mode = (char)pic[0];
    printf("\n  First byte: 0x%02X ('%c')\n", pic[0], mode >= 32 && mode < 127 ? mode : '.');

    /* heuristic: check if first few bytes contain VGA palette data (values 0-63) */
    int looks_like_palette = 1;
    for (int i = 0; i < 48 && i < pic_sz; i++) {
        if (pic[i] > 63) { looks_like_palette = 0; break; }
    }

    if (looks_like_palette && pic_sz > 768) {
        printf("  Note: First bytes appear to be VGA palette data (values 0-63)\n");
    }

    /* look for known patterns */
    /* MicroProse PIC files appear to have a variable-length header
     * possibly starting with a mode byte, palette, then RLE-compressed pixels */

    /* check for RLE patterns: look for repeated bytes or escape sequences */
    int rle_escapes = 0;
    int raw_runs = 0;
    for (long i = 4; i < pic_sz - 1; i++) {
        if (pic[i] == pic[i+1]) raw_runs++;
        if (pic[i] == 0x80 || pic[i] == 0x00) rle_escapes++;
    }
    printf("  Repeated byte pairs: %d (%.1f%%)\n", raw_runs, 100.0 * raw_runs / pic_sz);
    printf("  Potential RLE markers (0x00/0x80): %d\n", rle_escapes);

    /* byte frequency analysis */
    int freq[256] = {0};
    for (long i = 0; i < pic_sz; i++) freq[pic[i]]++;

    printf("\n  Top 10 most frequent bytes:\n");
    for (int pass = 0; pass < 10; pass++) {
        int max_val = 0, max_byte = 0;
        for (int i = 0; i < 256; i++) {
            if (freq[i] > max_val) { max_val = freq[i]; max_byte = i; }
        }
        printf("    0x%02X: %5d occurrences (%.1f%%)\n",
               max_byte, max_val, 100.0 * max_val / pic_sz);
        freq[max_byte] = 0;
    }

    /* if a .pal file was given, analyze it too */
    if (pal_path) {
        analyze_pal(pal_path);
    } else {
        /* try to find matching .pal automatically */
        char auto_pal[512];
        strncpy(auto_pal, pic_path, sizeof(auto_pal) - 1);
        char *dot = strrchr(auto_pal, '.');
        if (dot) {
            strcpy(dot, ".pal");
            FILE *pf = fopen(auto_pal, "rb");
            if (pf) {
                fclose(pf);
                printf("\n  Found matching palette: %s\n", auto_pal);
                analyze_pal(auto_pal);
                pal_path = auto_pal;
            }
        }
    }

    /* attempt raw decode if requested */
    if (do_decode && pic_sz > 768) {
        printf("\n  Attempting raw decode (320x200 VGA assumed)...\n");

        /* load palette if available */
        uint8_t *palette = NULL;
        if (pal_path) {
            FILE *pf = fopen(pal_path, "rb");
            if (pf) {
                fseek(pf, 0, SEEK_END);
                long psz = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                if (psz >= 768) {
                    palette = (uint8_t *)malloc(768);
                    fread(palette, 1, 768, pf);
                }
                fclose(pf);
            }
        }

        /* try several offsets as potential pixel data start */
        int offsets[] = {0, 2, 4, 768, 770, 772};
        for (int t = 0; t < 6; t++) {
            int off = offsets[t];
            if (off + 320 * 200 > pic_sz) continue;

            char bmp_path[512];
            snprintf(bmp_path, sizeof(bmp_path), "%s_raw_off%d.bmp", pic_path, off);
            write_bmp(bmp_path, pic + off, 320, 200, palette, 256);
        }

        free(palette);
    }

    free(pic);
    return 0;
}

/*
 * mzparse - MZ DOS executable analyzer for Sid Meier's Civilization
 *
 * Parses the MZ header, identifies Microsoft C overlay modules,
 * maps the overlay dispatch table (INT 3Fh), and extracts the
 * complete binary structure for static recompilation.
 *
 * Civilization (1991) was compiled with Microsoft C 5.x and uses
 * the MSC overlay manager for demand-loading code segments.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;          /* 'MZ' */
    uint16_t last_page_bytes;
    uint16_t pages;
    uint16_t reloc_count;
    uint16_t header_paragraphs;
    uint16_t min_extra;
    uint16_t max_extra;
    uint16_t init_ss;
    uint16_t init_sp;
    uint16_t checksum;
    uint16_t init_ip;
    uint16_t init_cs;
    uint16_t reloc_offset;
    uint16_t overlay_num;
} MZ_Header;
#pragma pack(pop)

typedef struct {
    uint32_t file_offset;
    uint32_t image_size;
    uint16_t pages;
    uint16_t last_page;
    uint16_t header_paras;
    uint16_t init_cs;
    uint16_t init_ip;
    int      overlay_index;
} OverlayModule;

typedef struct {
    uint8_t  overlay_num;
    uint16_t offset;
    uint32_t call_site;
    int      call_count;
} OverlayCall;

static uint8_t *g_data = NULL;
static uint32_t g_size = 0;

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    g_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_data = (uint8_t *)malloc(g_size);
    if (!g_data) {
        fclose(f);
        return -1;
    }
    fread(g_data, 1, g_size, f);
    fclose(f);
    return 0;
}

static void print_mz_header(const MZ_Header *hdr, uint32_t offset)
{
    uint32_t hdr_size = hdr->header_paragraphs * 16;
    uint32_t img_size = hdr->pages > 0
        ? (hdr->last_page_bytes
            ? (uint32_t)(hdr->pages - 1) * 512 + hdr->last_page_bytes
            : (uint32_t)hdr->pages * 512)
        : 0;

    printf("  File offset:     0x%06X\n", offset);
    printf("  Image size:      %u bytes (%.1f KB)\n", img_size, img_size / 1024.0);
    printf("  Pages:           %u (%u bytes)\n", hdr->pages, hdr->pages * 512);
    printf("  Last page bytes: %u\n", hdr->last_page_bytes);
    printf("  Header size:     %u bytes (%u paragraphs)\n", hdr_size, hdr->header_paragraphs);
    printf("  Relocations:     %u (table at 0x%04X)\n", hdr->reloc_count, hdr->reloc_offset);
    printf("  Min extra:       %u paragraphs (%u bytes)\n", hdr->min_extra, hdr->min_extra * 16);
    printf("  Max extra:       0x%04X\n", hdr->max_extra);
    printf("  Initial SS:SP:   %04X:%04X\n", hdr->init_ss, hdr->init_sp);
    printf("  Initial CS:IP:   %04X:%04X\n", hdr->init_cs, hdr->init_ip);
    printf("  Overlay number:  %u\n", hdr->overlay_num);
}

static int find_overlays(OverlayModule *ovls, int max_ovls)
{
    const MZ_Header *main_hdr = (const MZ_Header *)g_data;
    uint32_t img_size = main_hdr->last_page_bytes
        ? (uint32_t)(main_hdr->pages - 1) * 512 + main_hdr->last_page_bytes
        : (uint32_t)main_hdr->pages * 512;
    int count = 0;

    /* scan for embedded MZ headers after main image (align to 512-byte page) */
    uint32_t scan_start = (img_size + 0x1FFu) & ~0x1FFu;
    for (uint32_t pos = scan_start; pos + sizeof(MZ_Header) < g_size; pos += 0x200) {
        if (g_data[pos] == 'M' && g_data[pos + 1] == 'Z') {
            const MZ_Header *oh = (const MZ_Header *)(g_data + pos);
            if (oh->pages > 0 && oh->pages < 500 &&
                oh->header_paragraphs > 0 && oh->header_paragraphs < 100) {
                if (count < max_ovls) {
                    uint32_t oisz = oh->last_page_bytes
                        ? (uint32_t)(oh->pages - 1) * 512 + oh->last_page_bytes
                        : (uint32_t)oh->pages * 512;
                    ovls[count].file_offset = pos;
                    ovls[count].image_size = oisz;
                    ovls[count].pages = oh->pages;
                    ovls[count].last_page = oh->last_page_bytes;
                    ovls[count].header_paras = oh->header_paragraphs;
                    ovls[count].init_cs = oh->init_cs;
                    ovls[count].init_ip = oh->init_ip;
                    ovls[count].overlay_index = count + 1;
                    count++;
                }
            }
        }
    }
    return count;
}

typedef struct {
    uint8_t  ovl_num;
    uint16_t offset;
    int      count;
} DispatchEntry;

static int analyze_overlay_calls(DispatchEntry *entries, int max_entries)
{
    int count = 0;
    uint32_t hdr_size = ((const MZ_Header *)g_data)->header_paragraphs * 16;

    for (uint32_t i = hdr_size; i + 4 < g_size; i++) {
        if (g_data[i] == 0xCD && g_data[i + 1] == 0x3F) {
            uint8_t ovl = g_data[i + 2];
            uint16_t off = *(uint16_t *)(g_data + i + 3);

            /* check if we already have this target */
            int found = 0;
            for (int j = 0; j < count; j++) {
                if (entries[j].ovl_num == ovl && entries[j].offset == off) {
                    entries[j].count++;
                    found = 1;
                    break;
                }
            }
            if (!found && count < max_entries) {
                entries[count].ovl_num = ovl;
                entries[count].offset = off;
                entries[count].count = 1;
                count++;
            }
        }
    }
    return count;
}

static void find_strings(uint32_t start, uint32_t end, int min_len)
{
    int str_count = 0;
    uint32_t str_start = 0;
    int in_string = 0;
    int run = 0;

    for (uint32_t i = start; i < end && i < g_size; i++) {
        uint8_t b = g_data[i];
        if (b >= 32 && b < 127) {
            if (!in_string) {
                str_start = i;
                in_string = 1;
                run = 0;
            }
            run++;
        } else {
            if (in_string && run >= min_len) {
                printf("  0x%06X: ", str_start);
                for (uint32_t j = str_start; j < i && j < str_start + 80; j++)
                    putchar(g_data[j]);
                putchar('\n');
                str_count++;
            }
            in_string = 0;
        }
    }
    printf("  [%d strings found]\n", str_count);
}

static void analyze_interrupts(void)
{
    uint32_t hdr_size = ((const MZ_Header *)g_data)->header_paragraphs * 16;
    int int_counts[256] = {0};

    for (uint32_t i = hdr_size; i + 1 < g_size; i++) {
        if (g_data[i] == 0xCD) {
            int_counts[g_data[i + 1]]++;
        }
    }

    static const struct { uint8_t num; const char *name; } int_names[] = {
        {0x08, "TIMER"},      {0x09, "KEYBOARD_HW"}, {0x10, "VIDEO"},
        {0x13, "DISK"},       {0x16, "KEYBOARD"},     {0x1A, "CLOCK"},
        {0x21, "DOS"},        {0x2F, "MULTIPLEX"},    {0x33, "MOUSE"},
        {0x3F, "MSC_OVERLAY"},{0x67, "EMS"},
    };

    printf("\n=== DOS/BIOS Interrupt Usage ===\n");
    for (int n = 0; n < 256; n++) {
        if (int_counts[n] == 0) continue;
        const char *name = "";
        for (int j = 0; j < (int)(sizeof(int_names)/sizeof(int_names[0])); j++) {
            if (int_names[j].num == n) { name = int_names[j].name; break; }
        }
        printf("  INT %02Xh %-14s %4d occurrences\n", n, name, int_counts[n]);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "mzparse - Civilization DOS MZ Executable Analyzer\n"
            "Part of the Civ Recomp project (sp00nznet/civ)\n\n"
            "Usage: mzparse <civ.exe> [-strings] [-disasm]\n");
        return 1;
    }

    int show_strings = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-strings") == 0) show_strings = 1;
    }

    if (load_file(argv[1]) != 0) return 1;
    if (g_size < sizeof(MZ_Header) || g_data[0] != 'M' || g_data[1] != 'Z') {
        fprintf(stderr, "Error: not a valid MZ executable\n");
        free(g_data);
        return 1;
    }

    const MZ_Header *hdr = (const MZ_Header *)g_data;
    uint32_t hdr_size = hdr->header_paragraphs * 16;
    uint32_t img_size = hdr->last_page_bytes
        ? (uint32_t)(hdr->pages - 1) * 512 + hdr->last_page_bytes
        : (uint32_t)hdr->pages * 512;

    printf("================================================================\n");
    printf("  Sid Meier's Civilization (1991) - Binary Analysis\n");
    printf("  Compiled with Microsoft C 5.x (1988 Runtime)\n");
    printf("================================================================\n\n");

    printf("=== Main MZ Header ===\n");
    printf("  File:            %s (%u bytes, %.1f KB)\n", argv[1], g_size, g_size / 1024.0);
    print_mz_header(hdr, 0);
    printf("  Code start:      0x%06X\n", hdr_size);
    printf("  Code size:       %u bytes (%.1f KB)\n", img_size - hdr_size, (img_size - hdr_size) / 1024.0);
    printf("  Overlay data:    %u bytes (%.1f KB)\n", g_size - img_size, (g_size - img_size) / 1024.0);

    /* analyze interrupts */
    analyze_interrupts();

    /* find overlay modules */
    OverlayModule ovls[64];
    int ovl_count = find_overlays(ovls, 64);
    uint32_t total_ovl_code = 0;

    printf("\n=== Overlay Modules (%d found) ===\n", ovl_count);
    printf("  %-4s  %-10s  %-10s  %-8s  %-12s\n",
           "#", "Offset", "Size", "Pages", "CS:IP");
    printf("  %-4s  %-10s  %-10s  %-8s  %-12s\n",
           "----", "----------", "----------", "--------", "------------");
    for (int i = 0; i < ovl_count; i++) {
        printf("  %-4d  0x%08X  %6u B    %3u pg   %04X:%04X\n",
               ovls[i].overlay_index,
               ovls[i].file_offset,
               ovls[i].image_size,
               ovls[i].pages,
               ovls[i].init_cs, ovls[i].init_ip);
        total_ovl_code += ovls[i].image_size;
    }
    printf("\n  Total overlay code: %u bytes (%.1f KB)\n", total_ovl_code, total_ovl_code / 1024.0);
    printf("  Total code (resident + overlays): %u bytes (%.1f KB)\n",
           (img_size - hdr_size) + total_ovl_code,
           ((img_size - hdr_size) + total_ovl_code) / 1024.0);

    /* analyze overlay dispatch table */
    DispatchEntry entries[512];
    int dispatch_count = analyze_overlay_calls(entries, 512);

    printf("\n=== Overlay Dispatch Table (%d unique targets) ===\n", dispatch_count);

    /* group by overlay */
    for (int ovl = 1; ovl <= 0x17; ovl++) {
        int func_count = 0;
        int call_total = 0;
        for (int j = 0; j < dispatch_count; j++) {
            if (entries[j].ovl_num == ovl) {
                func_count++;
                call_total += entries[j].count;
            }
        }
        if (func_count > 0) {
            printf("  OVL %02X: %3d functions, %4d call sites\n", ovl, func_count, call_total);
        }
    }

    /* summary */
    int total_funcs = dispatch_count;
    int total_calls = 0;
    for (int j = 0; j < dispatch_count; j++) total_calls += entries[j].count;

    printf("\n=== Binary Summary ===\n");
    printf("  Compiler:          Microsoft C 5.x (1988)\n");
    printf("  Architecture:      16-bit x86 real mode (DOS)\n");
    printf("  Overlay manager:   Microsoft C INT 3Fh\n");
    printf("  Resident code:     %u bytes (%.1f KB)\n", img_size - hdr_size, (img_size - hdr_size) / 1024.0);
    printf("  Overlay modules:   %d (%u bytes, %.1f KB)\n", ovl_count, total_ovl_code, total_ovl_code / 1024.0);
    printf("  Total code:        %u bytes (%.1f KB)\n",
           (img_size - hdr_size) + total_ovl_code,
           ((img_size - hdr_size) + total_ovl_code) / 1024.0);
    printf("  Overlay functions: %d (%d call sites)\n", total_funcs, total_calls);
    printf("  DOS API calls:     INT 21h\n");
    printf("  Video:             INT 10h + direct VGA\n");
    printf("  Input:             INT 16h (keyboard) + INT 33h (mouse)\n");
    printf("  Sound:             AdLib/SB via driver EXEs\n");

    if (show_strings) {
        printf("\n=== String Table ===\n");
        find_strings(hdr_size, g_size, 6);
    }

    free(g_data);
    return 0;
}

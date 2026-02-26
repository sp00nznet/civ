/*
 * ovldump - Microsoft C Overlay Module Extractor
 *
 * Extracts individual overlay modules from CIV.EXE into separate
 * binary files for analysis. Each overlay is a self-contained MZ
 * executable that gets demand-loaded by the MSC overlay manager
 * via INT 3Fh.
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
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

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "ovldump - Civilization Overlay Module Extractor\n"
            "Part of the Civ Recomp project (sp00nznet/civ)\n\n"
            "Usage: ovldump <civ.exe> [output_dir]\n"
            "\nExtracts overlay modules to individual files.\n"
            "Output: ovl_XX.bin for each overlay module.\n");
        return 1;
    }

    const char *outdir = argc >= 3 ? argv[2] : ".";

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    uint32_t fsize = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(fsize);
    fread(data, 1, fsize, f);
    fclose(f);

    if (fsize < sizeof(MZ_Header) || data[0] != 'M' || data[1] != 'Z') {
        fprintf(stderr, "Error: not a valid MZ executable\n");
        free(data);
        return 1;
    }

    const MZ_Header *hdr = (const MZ_Header *)data;
    uint32_t img_size = hdr->last_page_bytes
        ? (uint32_t)(hdr->pages - 1) * 512 + hdr->last_page_bytes
        : (uint32_t)hdr->pages * 512;

    /* extract resident code (non-overlay portion) */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/resident.bin", outdir);
        FILE *out = fopen(path, "wb");
        if (out) {
            uint32_t hdr_sz = hdr->header_paragraphs * 16;
            fwrite(data + hdr_sz, 1, img_size - hdr_sz, out);
            fclose(out);
            printf("Extracted resident code: %s (%u bytes)\n", path, img_size - hdr_sz);
        }
    }

    /* scan for overlay modules - align to 512-byte page boundaries */
    int ovl_count = 0;
    uint32_t total_extracted = 0;
    uint32_t scan_start = (img_size + 0x1FF) & ~0x1FFu; /* align up to next 512 boundary */

    for (uint32_t pos = scan_start; pos + sizeof(MZ_Header) < fsize; pos += 0x200) {
        if (data[pos] != 'M' || data[pos + 1] != 'Z') continue;

        const MZ_Header *oh = (const MZ_Header *)(data + pos);
        if (oh->pages == 0 || oh->pages >= 500) continue;
        if (oh->header_paragraphs == 0 || oh->header_paragraphs >= 100) continue;

        uint32_t oisz = oh->last_page_bytes
            ? (uint32_t)(oh->pages - 1) * 512 + oh->last_page_bytes
            : (uint32_t)oh->pages * 512;

        ovl_count++;

        /* extract just the code (skip overlay MZ header) */
        uint32_t ohdr_sz = oh->header_paragraphs * 16;
        uint32_t code_sz = oisz > ohdr_sz ? oisz - ohdr_sz : 0;

        char path[512];
        snprintf(path, sizeof(path), "%s/ovl_%02d.bin", outdir, ovl_count);
        FILE *out = fopen(path, "wb");
        if (out) {
            if (code_sz > 0 && pos + ohdr_sz + code_sz <= fsize) {
                fwrite(data + pos + ohdr_sz, 1, code_sz, out);
            }
            fclose(out);
            printf("Extracted overlay %2d: %s (%u bytes code, file offset 0x%06X)\n",
                   ovl_count, path, code_sz, pos);
            total_extracted += code_sz;
        }

        /* also dump the full overlay with header for reference */
        snprintf(path, sizeof(path), "%s/ovl_%02d_full.bin", outdir, ovl_count);
        out = fopen(path, "wb");
        if (out) {
            uint32_t write_sz = oisz;
            if (pos + write_sz > fsize) write_sz = fsize - pos;
            fwrite(data + pos, 1, write_sz, out);
            fclose(out);
        }
    }

    printf("\nExtracted %d overlay modules (%u bytes total code)\n", ovl_count, total_extracted);
    printf("Resident + overlay code: %u bytes (%.1f KB)\n",
           (img_size - hdr->header_paragraphs * 16) + total_extracted,
           ((img_size - hdr->header_paragraphs * 16) + total_extracted) / 1024.0);

    free(data);
    return 0;
}

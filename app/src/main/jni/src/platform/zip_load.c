/*
 * zip_load.c
 * Extract the first Atari ROM (.a26/.a78/.bin) from a ZIP archive.
 *
 * Algorithm:
 *  1. Find the End-of-Central-Directory (EOCD) record by scanning the last
 *     65558 bytes of the file.
 *  2. Follow the EOCD's central-directory offset to scan file entries.
 *  3. For the first entry whose filename ends in .a26/.a78/.bin, read its
 *     compressed size / uncompressed size / local-header offset from the
 *     central directory record (not the local header — local sizes are zero
 *     in bit-3 data-descriptor ZIPs).
 *  4. Seek to the local file header to get the actual data offset (local
 *     header has variable-length filename + extra-field), then extract:
 *     METHOD_STORED (0)  — memcpy
 *     METHOD_DEFLATE (8) — puff()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>    /* strncasecmp */
#include "zip_load.h"
#include "puff.h"
#include "machine.h"    /* MACHINE_2600, MACHINE_7800 */

#define SIG_EOCD    0x06054B50UL
#define SIG_CD      0x02014B50UL
#define SIG_LOCAL   0x04034B50UL

#define METHOD_STORED   0
#define METHOD_DEFLATE  8

/* Maximum ZIP comment length (65535) + EOCD size (22) */
#define EOCD_SCAN_LEN  65557
#define EOCD_SIZE      22

/* Maximum ROM size accepted from a ZIP entry (512 KiB) */
#define MAX_ROM_SIZE   (512 * 1024UL)

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Machine type from a filename's extension; -1 if not a supported ROM. */
static int ext_to_mtype(const char *name, int namelen)
{
    if (namelen < 4) return -1;
    const char *ext = name + namelen - 4;
    if (strncasecmp(ext, ".a26", 4) == 0) return MACHINE_2600;
    if (strncasecmp(ext, ".bin", 4) == 0) return MACHINE_2600;
    if (strncasecmp(ext, ".a78", 4) == 0) return MACHINE_7800;
    return -1;
}

/*
 * Read the last min(EOCD_SCAN_LEN, file_size) bytes into a malloc'd buffer
 * and search backward for the EOCD signature.
 * Returns the file offset of the EOCD record, or -1 on failure.
 */
static long find_eocd(FILE *f, long file_size)
{
    long scan = EOCD_SCAN_LEN < file_size ? EOCD_SCAN_LEN : file_size;
    long start = file_size - scan;

    uint8_t *buf = (uint8_t *)malloc((size_t)scan);
    if (!buf) return -1;

    if (fseek(f, start, SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)scan, f) != (size_t)scan) {
        free(buf);
        return -1;
    }

    long off = -1;
    for (long i = scan - EOCD_SIZE; i >= 0; i--) {
        if (buf[i]   == 0x50 && buf[i+1] == 0x4B &&
            buf[i+2] == 0x05 && buf[i+3] == 0x06) {
            off = start + i;
            break;
        }
    }
    free(buf);
    return off;
}

/*
 * Shared: open the central directory and scan for the first ROM entry.
 * Fills *local_off, *cmp_size, *uncmp_size, *method, *mtype on success.
 * Returns 0 on success, -1 if no suitable entry found or file error.
 */
static int find_first_rom(FILE *f,
                           uint32_t *local_off_out,
                           uint32_t *cmp_size_out,
                           uint32_t *uncmp_size_out,
                           uint16_t *method_out,
                           int      *mtype_out)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long file_size = ftell(f);
    if (file_size < EOCD_SIZE) return -1;

    long eocd_off = find_eocd(f, file_size);
    if (eocd_off < 0) return -1;

    uint8_t eocd[EOCD_SIZE];
    if (fseek(f, eocd_off, SEEK_SET) != 0) return -1;
    if (fread(eocd, 1, EOCD_SIZE, f) != EOCD_SIZE) return -1;

    uint16_t num_entries = u16le(eocd + 10);
    uint32_t cd_offset   = u32le(eocd + 16);

    if (num_entries == 0 || (long)cd_offset >= file_size) return -1;

    uint32_t cd_pos = cd_offset;
    for (uint16_t i = 0; i < num_entries; i++) {
        uint8_t cde[46];
        if (fseek(f, (long)cd_pos, SEEK_SET) != 0) return -1;
        if (fread(cde, 1, 46, f) != 46) return -1;
        if (u32le(cde) != SIG_CD) return -1;

        uint16_t method      = u16le(cde + 10);
        uint32_t cmp_size    = u32le(cde + 20);
        uint32_t uncmp_size  = u32le(cde + 24);
        uint16_t fname_len   = u16le(cde + 28);
        uint16_t extra_len   = u16le(cde + 30);
        uint16_t comment_len = u16le(cde + 32);
        uint32_t local_off   = u32le(cde + 42);

        /* Read filename (cap at 511 to stay in stack bounds) */
        char fname[512];
        int flen = fname_len < 511 ? (int)fname_len : 511;
        if (fread(fname, 1, (size_t)flen, f) != (size_t)flen) return -1;
        fname[flen] = '\0';

        int mtype = ext_to_mtype(fname, flen);

        if (mtype >= 0 &&
            (method == METHOD_STORED || method == METHOD_DEFLATE) &&
            uncmp_size > 0 && uncmp_size <= MAX_ROM_SIZE) {
            *local_off_out  = local_off;
            *cmp_size_out   = cmp_size;
            *uncmp_size_out = uncmp_size;
            *method_out     = method;
            *mtype_out      = mtype;
            return 0;
        }

        cd_pos += 46u + fname_len + extra_len + comment_len;
    }
    return -1;
}

int zip_extract_rom(const char *zip_path,
                    unsigned char **data_out,
                    unsigned long  *size_out,
                    int            *mtype_out)
{
    uint32_t local_off, cmp_size, uncmp_size;
    uint16_t method;
    int mtype;

    FILE *f = fopen(zip_path, "rb");
    if (!f) return -1;

    int rc = find_first_rom(f, &local_off, &cmp_size, &uncmp_size, &method, &mtype);
    if (rc != 0) { fclose(f); return -1; }

    /* Read local file header to get the actual data offset.
     * We use sizes from the central directory (already captured above), not
     * from the local header — the local sizes may be zero for bit-3 entries. */
    uint8_t lhdr[30];
    if (fseek(f, (long)local_off, SEEK_SET) != 0 ||
        fread(lhdr, 1, 30, f) != 30 ||
        u32le(lhdr) != SIG_LOCAL) {
        fclose(f);
        return -1;
    }
    uint16_t lname_len  = u16le(lhdr + 26);
    uint16_t lextra_len = u16le(lhdr + 28);
    long data_off = (long)local_off + 30 + lname_len + lextra_len;

    if (fseek(f, data_off, SEEK_SET) != 0) { fclose(f); return -1; }

    unsigned char *out = (unsigned char *)malloc(uncmp_size);
    if (!out) { fclose(f); return -1; }

    if (method == METHOD_STORED) {
        if (cmp_size != uncmp_size ||
            fread(out, 1, uncmp_size, f) != uncmp_size) {
            free(out); fclose(f); return -1;
        }
    } else {
        /* METHOD_DEFLATE via puff */
        unsigned char *cmp = (unsigned char *)malloc(cmp_size);
        if (!cmp) { free(out); fclose(f); return -1; }

        if (fread(cmp, 1, cmp_size, f) != cmp_size) {
            free(cmp); free(out); fclose(f); return -1;
        }
        unsigned long destlen   = uncmp_size;
        unsigned long sourcelen = cmp_size;
        int pret = puff(out, &destlen, cmp, &sourcelen);
        free(cmp);
        if (pret != 0 || destlen != uncmp_size) {
            free(out); fclose(f); return -1;
        }
    }

    fclose(f);
    *data_out  = out;
    *size_out  = uncmp_size;
    *mtype_out = mtype;
    return 0;
}

int zip_probe_machine_type(const char *zip_path)
{
    uint32_t local_off, cmp_size, uncmp_size;
    uint16_t method;
    int mtype = -1;

    FILE *f = fopen(zip_path, "rb");
    if (!f) return -1;

    find_first_rom(f, &local_off, &cmp_size, &uncmp_size, &method, &mtype);
    fclose(f);
    return mtype;
}

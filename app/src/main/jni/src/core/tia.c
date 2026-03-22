/*
 * tia.c
 *
 * Television Interface Adaptor (TIA) — EMU7800-style batch renderer
 *
 * Faithful port of EMU7800 TIA.cs + TIATables.cs by Mike Murphy.
 * Uses lookup tables for playfield, player, missile, ball rendering
 * and collision detection. Lazy rendering: TIA catches up on read/write.
 *
 * Improvements over EMU7800 reference:
 * - 2D pixel addressing (row * FB_WIDTH + col) instead of FrameBufferIndex++
 * - VBLANK-OFF anchor for stable vertical display position
 * - Double-buffered frame output
 *
 * C port Copyright (c) 2024-2026 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include "tia.h"
#include "tiasound.h"
#include "machine.h"

/* External logging */
extern void log_msg(const char *msg);

/* ========================================================================
 * Debug Logging Implementation
 * ======================================================================== */

uint32_t g_tia_dbg_flags = 0;
int g_tia_dbg_frame_ok = 0;
int g_tia_dbg_pixel_sl = -1;

static int g_tia_dbg_frame_start = 0;
static int g_tia_dbg_frame_end = 2;
static int g_tia_dbg_frame_num = 0;
static int g_tia_dbg_total_chars = 0;
#define TIA_DBG_MAX_CHARS 25000

void tia_dbg_init(uint32_t flags, int frame_start, int frame_end, int pixel_scanline)
{
    g_tia_dbg_flags = flags;
    g_tia_dbg_frame_start = frame_start;
    g_tia_dbg_frame_end = frame_end;
    g_tia_dbg_pixel_sl = pixel_scanline;
    g_tia_dbg_frame_ok = 0;
    g_tia_dbg_total_chars = 0;
}

void tia_dbg_set_frame(int frame_num)
{
    g_tia_dbg_frame_num = frame_num;
    g_tia_dbg_frame_ok = (g_tia_dbg_flags != 0) &&
                          (frame_num >= g_tia_dbg_frame_start) &&
                          (frame_num <= g_tia_dbg_frame_end);
}

void tia_dbg_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int len;

    if (g_tia_dbg_total_chars >= TIA_DBG_MAX_CHARS)
        return;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_tia_dbg_total_chars += len;
    log_msg(buf);

    if (g_tia_dbg_total_chars >= TIA_DBG_MAX_CHARS) {
        log_msg("--- LOG CUTOFF: 25000 chars reached ---");
        g_tia_dbg_flags = 0;
    }
}

/* ========================================================================
 * TIA Register Addresses (Write)
 * ======================================================================== */

#define VSYNC   0x00
#define VBLANK  0x01
#define WSYNC   0x02
#define RSYNC   0x03
#define NUSIZ0  0x04
#define NUSIZ1  0x05
#define COLUP0  0x06
#define COLUP1  0x07
#define COLUPF  0x08
#define COLUBK  0x09
#define CTRLPF  0x0A
#define REFP0   0x0B
#define REFP1   0x0C
#define PF0     0x0D
#define PF1     0x0E
#define PF2     0x0F
#define RESP0   0x10
#define RESP1   0x11
#define RESM0   0x12
#define RESM1   0x13
#define RESBL   0x14
#define AUDC0   0x15
#define AUDC1   0x16
#define AUDF0   0x17
#define AUDF1   0x18
#define AUDV0   0x19
#define AUDV1   0x1A
#define GRP0    0x1B
#define GRP1    0x1C
#define ENAM0   0x1D
#define ENAM1   0x1E
#define ENABL   0x1F
#define HMP0    0x20
#define HMP1    0x21
#define HMM0    0x22
#define HMM1    0x23
#define HMBL    0x24
#define VDELP0  0x25
#define VDELP1  0x26
#define VDELBL  0x27
#define RESMP0  0x28
#define RESMP1  0x29
#define HMOVE   0x2A
#define HMCLR   0x2B
#define CXCLR   0x2C

/* TIA Read Register Addresses */
#define CXM0P   0x00
#define CXM1P   0x01
#define CXP0FB  0x02
#define CXP1FB  0x03
#define CXM0FB  0x04
#define CXM1FB  0x05
#define CXBLPF  0x06
#define CXPPMM  0x07
#define INPT0   0x08
#define INPT1   0x09
#define INPT2   0x0A
#define INPT3   0x0B
#define INPT4   0x0C
#define INPT5   0x0D

/* ========================================================================
 * Constants
 * ======================================================================== */

#define H_CLOCKS       228
#define H_BLANK_CLOCKS  68
#define H_PIXEL        160

/* Collision pair flags (matches EMU7800 TIACxPairFlags) */
#define CXP_M0P1  (1 << 0)
#define CXP_M0P0  (1 << 1)
#define CXP_M1P0  (1 << 2)
#define CXP_M1P1  (1 << 3)
#define CXP_P0PF  (1 << 4)
#define CXP_P0BL  (1 << 5)
#define CXP_P1PF  (1 << 6)
#define CXP_P1BL  (1 << 7)
#define CXP_M0PF  (1 << 8)
#define CXP_M0BL  (1 << 9)
#define CXP_M1PF  (1 << 10)
#define CXP_M1BL  (1 << 11)
#define CXP_BLPF  (1 << 12)
#define CXP_P0P1  (1 << 13)
#define CXP_M0M1  (1 << 14)

/* Collision object flags (6-bit, used to index collision mask table) */
#define CXF_PF  (1 << 0)
#define CXF_BL  (1 << 1)
#define CXF_M0  (1 << 2)
#define CXF_M1  (1 << 3)
#define CXF_P0  (1 << 4)
#define CXF_P1  (1 << 5)

/* ========================================================================
 * Frame buffer — 160 x 320 (double-buffered)
 * ======================================================================== */

#define FB_WIDTH  160
#define FB_HEIGHT 320

static uint8_t g_frame_buffers[2][FB_WIDTH * FB_HEIGHT];
static uint8_t *g_frame_buffer = g_frame_buffers[0];
static uint8_t *g_display_buffer = g_frame_buffers[1];

/* Sampled input state — captured once at frame start */
static int g_frame_trigger[2];
static int g_inpt4_log_count = 0;

/* Frame start TIA clock — for mid-frame audio position */
static uint64_t g_frame_start_tia_clock = 0;

/* ========================================================================
 * NTSC Color Palette
 * ======================================================================== */

const uint32_t tia_ntsc_palette[256] = {
    0x000000, 0x000000, 0x4a4a4a, 0x4a4a4a, 0x6f6f6f, 0x6f6f6f, 0x8e8e8e, 0x8e8e8e,
    0xaaaaaa, 0xaaaaaa, 0xc0c0c0, 0xc0c0c0, 0xd6d6d6, 0xd6d6d6, 0xececec, 0xececec,
    0x484800, 0x484800, 0x69690f, 0x69690f, 0x86861d, 0x86861d, 0xa2a22a, 0xa2a22a,
    0xbbbb35, 0xbbbb35, 0xd2d240, 0xd2d240, 0xe8e84a, 0xe8e84a, 0xfcfc54, 0xfcfc54,
    0x7c2c00, 0x7c2c00, 0x904811, 0x904811, 0xa26221, 0xa26221, 0xb47a30, 0xb47a30,
    0xc3903d, 0xc3903d, 0xd2a44a, 0xd2a44a, 0xdfb755, 0xdfb755, 0xecc860, 0xecc860,
    0x901c00, 0x901c00, 0xa33915, 0xa33915, 0xb55328, 0xb55328, 0xc66c3a, 0xc66c3a,
    0xd5824a, 0xd5824a, 0xe39759, 0xe39759, 0xf0aa67, 0xf0aa67, 0xfcbc74, 0xfcbc74,
    0x940000, 0x940000, 0xa71a1a, 0xa71a1a, 0xb83232, 0xb83232, 0xc84848, 0xc84848,
    0xd65c5c, 0xd65c5c, 0xe46f6f, 0xe46f6f, 0xf08080, 0xf08080, 0xfc9090, 0xfc9090,
    0x840064, 0x840064, 0x97197a, 0x97197a, 0xa8308f, 0xa8308f, 0xb846a2, 0xb846a2,
    0xc659b3, 0xc659b3, 0xd46cc3, 0xd46cc3, 0xe07cd2, 0xe07cd2, 0xec8ce0, 0xec8ce0,
    0x500084, 0x500084, 0x68199a, 0x68199a, 0x7d30ad, 0x7d30ad, 0x9246c0, 0x9246c0,
    0xa459d0, 0xa459d0, 0xb56ce0, 0xb56ce0, 0xc57cee, 0xc57cee, 0xd48cfc, 0xd48cfc,
    0x140090, 0x140090, 0x331aa3, 0x331aa3, 0x4e32b5, 0x4e32b5, 0x6848c6, 0x6848c6,
    0x7f5cd5, 0x7f5cd5, 0x956fe3, 0x956fe3, 0xa980f0, 0xa980f0, 0xbc90fc, 0xbc90fc,
    0x000094, 0x000094, 0x181aa7, 0x181aa7, 0x2d32b8, 0x2d32b8, 0x4248c8, 0x4248c8,
    0x545cd6, 0x545cd6, 0x656fe4, 0x656fe4, 0x7580f0, 0x7580f0, 0x8490fc, 0x8490fc,
    0x001c88, 0x001c88, 0x183b9d, 0x183b9d, 0x2d57b0, 0x2d57b0, 0x4272c2, 0x4272c2,
    0x548ad2, 0x548ad2, 0x65a0e1, 0x65a0e1, 0x75b5ef, 0x75b5ef, 0x84c8fc, 0x84c8fc,
    0x003064, 0x003064, 0x185080, 0x185080, 0x2d6d98, 0x2d6d98, 0x4288b0, 0x4288b0,
    0x54a0c5, 0x54a0c5, 0x65b7d9, 0x65b7d9, 0x75cceb, 0x75cceb, 0x84e0fc, 0x84e0fc,
    0x004030, 0x004030, 0x18624e, 0x18624e, 0x2d8169, 0x2d8169, 0x429e82, 0x429e82,
    0x54b899, 0x54b899, 0x65d1ae, 0x65d1ae, 0x75e7c2, 0x75e7c2, 0x84fcd4, 0x84fcd4,
    0x004400, 0x004400, 0x1a661a, 0x1a661a, 0x328432, 0x328432, 0x48a048, 0x48a048,
    0x5cba5c, 0x5cba5c, 0x6fd26f, 0x6fd26f, 0x80e880, 0x80e880, 0x90fc90, 0x90fc90,
    0x143c00, 0x143c00, 0x355f18, 0x355f18, 0x527e2d, 0x527e2d, 0x6e9c42, 0x6e9c42,
    0x87b754, 0x87b754, 0x9ed065, 0x9ed065, 0xb4e775, 0xb4e775, 0xc8fc84, 0xc8fc84,
    0x303800, 0x303800, 0x505916, 0x505916, 0x6d762b, 0x6d762b, 0x88923e, 0x88923e,
    0xa0ab4f, 0xa0ab4f, 0xb7c25f, 0xb7c25f, 0xccd86e, 0xccd86e, 0xe0ec7c, 0xe0ec7c,
    0x482c00, 0x482c00, 0x694d14, 0x694d14, 0x866a26, 0x866a26, 0xa28638, 0xa28638,
    0xbb9f47, 0xbb9f47, 0xd2b656, 0xd2b656, 0xe8cc63, 0xe8cc63, 0xfce070, 0xfce070
};

/* GRP reflect table (bit reversal) */
const uint8_t tia_grp_reflect[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};

/* ========================================================================
 * Lookup Tables (port of TIATables.cs)
 * ======================================================================== */

/* Playfield mask: [reflection_state][pixel_pos] -> bit mask */
static uint32_t g_pf_mask[2][160];

/* Ball mask: [size][position] -> visible (1/0) */
static uint8_t g_bl_mask[4][160];

/* Missile mask: [size][type][position] -> visible (1/0) */
static uint8_t g_mx_mask[4][8][160];

/* Player mask: [suppress][type][position] -> bit mask byte */
static uint8_t g_px_mask[2][8][160];

/* Collision mask: [6-bit object flags] -> 15-bit collision pair flags */
static uint16_t g_collision_mask[64];

static void build_pf_mask(void)
{
    int i, j;
    uint32_t mask;

    memset(g_pf_mask, 0, sizeof(g_pf_mask));

    for (i = 0; i < 20; i++) {
        if (i < 4)
            mask = (uint32_t)(1 << i);
        else if (i < 12)
            mask = (uint32_t)(1 << (15 - i));
        else
            mask = (uint32_t)(1 << i);

        for (j = 0; j < 4; j++) {
            /* Non-reflected: both halves use same pattern */
            g_pf_mask[0][4 * i + j] = mask;
            g_pf_mask[0][80 + 4 * i + j] = mask;

            /* Reflected: left half same, right half mirrored */
            g_pf_mask[1][4 * i + j] = mask;
            g_pf_mask[1][159 - 4 * i - j] = mask;
        }
    }
}

static void build_bl_mask(void)
{
    int size, i;

    memset(g_bl_mask, 0, sizeof(g_bl_mask));

    for (size = 0; size < 4; size++) {
        for (i = 0; i < (1 << size); i++) {
            g_bl_mask[size][i] = 1;
        }
    }
}

static void build_mx_mask(void)
{
    int size, i;

    memset(g_mx_mask, 0, sizeof(g_mx_mask));

    for (size = 0; size < 4; size++) {
        for (i = 0; i < (1 << size); i++) {
            g_mx_mask[size][0][i] = 1;

            g_mx_mask[size][1][i] = 1;
            g_mx_mask[size][1][i + 16] = 1;

            g_mx_mask[size][2][i] = 1;
            g_mx_mask[size][2][i + 32] = 1;

            g_mx_mask[size][3][i] = 1;
            g_mx_mask[size][3][i + 16] = 1;
            g_mx_mask[size][3][i + 32] = 1;

            g_mx_mask[size][4][i] = 1;
            g_mx_mask[size][4][i + 64] = 1;

            g_mx_mask[size][5][i] = 1;

            g_mx_mask[size][6][i] = 1;
            g_mx_mask[size][6][i + 32] = 1;
            g_mx_mask[size][6][i + 64] = 1;

            g_mx_mask[size][7][i] = 1;
        }
    }
}

static void build_px_mask(void)
{
    int nusiz, hpos, shift;

    memset(g_px_mask, 0, sizeof(g_px_mask));

    for (nusiz = 0; nusiz < 8; nusiz++) {
        for (hpos = 0; hpos < 160; hpos++) {
            /* nusiz 0-4 and 6: first copy at hpos 0-7 */
            if ((nusiz >= 0 && nusiz <= 4) || nusiz == 6) {
                if (hpos >= 0 && hpos < 8) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << (7 - hpos));
                }
            }
            /* nusiz 1 or 3: close copy at 16-23 */
            if (nusiz == 1 || nusiz == 3) {
                if (hpos >= 16 && hpos < 24) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << (23 - hpos));
                    g_px_mask[1][nusiz][hpos] = (uint8_t)(1 << (23 - hpos));
                }
            }
            /* nusiz 2, 3, or 6: medium copy at 32-39 */
            if (nusiz == 2 || nusiz == 3 || nusiz == 6) {
                if (hpos >= 32 && hpos < 40) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << (39 - hpos));
                    g_px_mask[1][nusiz][hpos] = (uint8_t)(1 << (39 - hpos));
                }
            }
            /* nusiz 4 or 6: wide copy at 64-71 */
            if (nusiz == 4 || nusiz == 6) {
                if (hpos >= 64 && hpos < 72) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << (71 - hpos));
                    g_px_mask[1][nusiz][hpos] = (uint8_t)(1 << (71 - hpos));
                }
            }
            /* nusiz 5: double size at 0-15 */
            if (nusiz == 5) {
                if (hpos >= 0 && hpos < 16) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << ((15 - hpos) >> 1));
                }
            }
            /* nusiz 7: quad size at 0-31 */
            if (nusiz == 7) {
                if (hpos >= 0 && hpos < 32) {
                    g_px_mask[0][nusiz][hpos] = (uint8_t)(1 << ((31 - hpos) >> 2));
                }
            }
        }

        /* Shift table right by 1 (standard) or 2 (double/quad) positions */
        shift = (nusiz == 5 || nusiz == 7) ? 2 : 1;
        while (shift-- > 0) {
            int i;
            for (i = 159; i > 0; i--) {
                g_px_mask[0][nusiz][i] = g_px_mask[0][nusiz][i - 1];
                g_px_mask[1][nusiz][i] = g_px_mask[1][nusiz][i - 1];
            }
            g_px_mask[0][nusiz][0] = 0;
            g_px_mask[1][nusiz][0] = 0;
        }
    }
}

static void build_collision_mask(void)
{
    int i;
    for (i = 0; i < 64; i++) {
        uint16_t v = 0;
        if ((i & CXF_M0) && (i & CXF_P1)) v |= CXP_M0P1;
        if ((i & CXF_M0) && (i & CXF_P0)) v |= CXP_M0P0;
        if ((i & CXF_M1) && (i & CXF_P0)) v |= CXP_M1P0;
        if ((i & CXF_M1) && (i & CXF_P1)) v |= CXP_M1P1;
        if ((i & CXF_P0) && (i & CXF_PF)) v |= CXP_P0PF;
        if ((i & CXF_P0) && (i & CXF_BL)) v |= CXP_P0BL;
        if ((i & CXF_P1) && (i & CXF_PF)) v |= CXP_P1PF;
        if ((i & CXF_P1) && (i & CXF_BL)) v |= CXP_P1BL;
        if ((i & CXF_M0) && (i & CXF_PF)) v |= CXP_M0PF;
        if ((i & CXF_M0) && (i & CXF_BL)) v |= CXP_M0BL;
        if ((i & CXF_M1) && (i & CXF_PF)) v |= CXP_M1PF;
        if ((i & CXF_M1) && (i & CXF_BL)) v |= CXP_M1BL;
        if ((i & CXF_BL) && (i & CXF_PF)) v |= CXP_BLPF;
        if ((i & CXF_P0) && (i & CXF_P1)) v |= CXP_P0P1;
        if ((i & CXF_M0) && (i & CXF_M1)) v |= CXP_M0M1;
        g_collision_mask[i] = v;
    }
}

static void build_tables(void)
{
    build_pf_mask();
    build_bl_mask();
    build_mx_mask();
    build_px_mask();
    build_collision_mask();
}

/* ========================================================================
 * Helper: HMoveCounter setter (clamp to -1..15)
 * ======================================================================== */

static inline int hmc_set(int v)
{
    return v < 0 ? -1 : v & 0xf;
}

/* ========================================================================
 * Player Graphics Helpers
 * ======================================================================== */

static void set_eff_grp0(TIA *tia)
{
    uint8_t grp0 = tia->regw[VDELP0] ? tia->old_grp0 : tia->regw[GRP0];
    tia->eff_grp0 = tia->regw[REFP0] ? tia_grp_reflect[grp0] : grp0;
}

static void set_eff_grp1(TIA *tia)
{
    uint8_t grp1 = tia->regw[VDELP1] ? tia->old_grp1 : tia->regw[GRP1];
    tia->eff_grp1 = tia->regw[REFP1] ? tia_grp_reflect[grp1] : grp1;
}

static void set_blon(TIA *tia)
{
    tia->blon = tia->regw[VDELBL] ? tia->old_enabl : (tia->regw[ENABL] != 0);
}

/* ========================================================================
 * Core Renderer — port of TIA.cs RenderFromStartClockTo()
 *
 * Renders TIA from start_clock to end_clock, one color clock at a time.
 * Uses lookup tables for object visibility and collisions.
 * Writes pixels using 2D addressing with VBLANK-OFF anchor.
 * ======================================================================== */

static void render_from_start_clock_to(TIA *tia, uint64_t end_clock)
{
    int hblank_edge;

    while (tia->start_clock < end_clock) {
        uint8_t fbyte;
        uint8_t fbyte_colupf;
        int cxflags;

        /* Increment HSync (represents the clock being processed) */
        tia->hsync = (tia->hsync + 1) % 228;

        /* HMOVE startup */
        if (tia->start_clock == tia->start_hmove_clock) {
            tia->hmove_latch = 1;
            tia->hmove_counter = 0xf;
            tia->p0mmr = tia->p1mmr = tia->m0mmr = tia->m1mmr = tia->blmmr = 1;
        } else if (tia->hsync == 0) {
            /* Just wrapped around — clear late HBLANK */
            tia->hmove_latch = 0;
        }

        /* Position counter increment during visible portion */
        hblank_edge = 68 + (tia->hmove_latch ? 8 : 0);
        if (tia->hsync >= hblank_edge) {
            tia->p0 = (tia->p0 + 1) % 160;
            tia->p1 = (tia->p1 + 1) % 160;
            tia->m0 = (tia->m0 + 1) % 160;
            tia->m1 = (tia->m1 + 1) % 160;
            tia->bl = (tia->bl + 1) % 160;
        }

        /* HMOVE compare: once every 1/4 CLK (phase 0) when active */
        if (tia->hmove_counter >= 0 && (tia->hsync & 3) == 0) {
            if (((tia->hmove_counter ^ tia->regw[HMP0]) & 0xf) == 0xf) tia->p0mmr = 0;
            if (((tia->hmove_counter ^ tia->regw[HMP1]) & 0xf) == 0xf) tia->p1mmr = 0;
            if (((tia->hmove_counter ^ tia->regw[HMM0]) & 0xf) == 0xf) tia->m0mmr = 0;
            if (((tia->hmove_counter ^ tia->regw[HMM1]) & 0xf) == 0xf) tia->m1mmr = 0;
            if (((tia->hmove_counter ^ tia->regw[HMBL]) & 0xf) == 0xf) tia->blmmr = 0;
            tia->hmove_counter = hmc_set(tia->hmove_counter - 1);
        }

        /* HMOVE increment: phase 2, during hblank only */
        if (tia->hmove_counter < 0xf && (tia->hsync & 3) == 2) {
            if (tia->hsync < hblank_edge) {
                if (tia->p0mmr) tia->p0 = (tia->p0 + 1) % 160;
                if (tia->p1mmr) tia->p1 = (tia->p1 + 1) % 160;
                if (tia->m0mmr) tia->m0 = (tia->m0 + 1) % 160;
                if (tia->m1mmr) tia->m1 = (tia->m1 + 1) % 160;
                if (tia->blmmr) tia->bl = (tia->bl + 1) % 160;
            }
        }

        /* --- Pixel color determination --- */
        fbyte = 0;
        fbyte_colupf = tia->colupf;
        cxflags = 0;

        if (tia->vblankon || tia->hsync < hblank_edge)
            goto write_pixel;

        fbyte = tia->colubk;

        {
            int colupfon = 0;
            int hpos = tia->hsync - 68;

            /* Playfield */
            if ((tia->pf210 & g_pf_mask[tia->pf_reflection_state][hpos]) != 0) {
                if (tia->scoreon)
                    fbyte_colupf = hpos < 80 ? tia->colup0 : tia->colup1;
                colupfon = 1;
                cxflags |= CXF_PF;
            }

            /* Ball */
            if (tia->blon && tia->bl >= 0 && g_bl_mask[tia->blsize][tia->bl]) {
                colupfon = 1;
                cxflags |= CXF_BL;
            }

            /* PF/BL priority: if not pfpriority, draw PF/BL behind players */
            if (!tia->pfpriority && colupfon) {
                fbyte = fbyte_colupf;
            }

            /* Missile 1 */
            if (tia->m1on && tia->m1 >= 0 && g_mx_mask[tia->m1size][tia->m1type][tia->m1]) {
                fbyte = tia->colup1;
                cxflags |= CXF_M1;
            }

            /* Player 1 */
            if (tia->p1 >= 0 && (g_px_mask[tia->p1suppress][tia->p1type][tia->p1] & tia->eff_grp1) != 0) {
                fbyte = tia->colup1;
                cxflags |= CXF_P1;
            }

            /* Missile 0 */
            if (tia->m0on && tia->m0 >= 0 && g_mx_mask[tia->m0size][tia->m0type][tia->m0]) {
                fbyte = tia->colup0;
                cxflags |= CXF_M0;
            }

            /* Player 0 */
            if (tia->p0 >= 0 && (g_px_mask[tia->p0suppress][tia->p0type][tia->p0] & tia->eff_grp0) != 0) {
                fbyte = tia->colup0;
                cxflags |= CXF_P0;
            }

            /* PF/BL priority: if pfpriority, draw PF/BL on top of players */
            if (tia->pfpriority && colupfon) {
                fbyte = fbyte_colupf;
            }
        }

    write_pixel:
        /* Accumulate collisions */
        tia->collisions |= g_collision_mask[cxflags];

        /* Write pixel to frame buffer */
        if (tia->hsync >= 68) {
            int col = tia->hsync - 68;

            /* 2D pixel addressing with VBLANK-OFF anchor */
            if (tia->vblank_off_scanline >= 0) {
                int row = tia->scanline - tia->vblank_off_scanline;
                if (row >= 0 && row < FB_HEIGHT) {
                    g_frame_buffer[row * FB_WIDTH + col] = fbyte;
                    /* Track last row with visible content (not VBLANK black) */
                    if (!tia->vblankon && row > tia->last_visible_row)
                        tia->last_visible_row = row;
                }
            }

            if (tia->hsync == 227)
                tia->scanline++;
        }

        /* Clear suppress when position counter reaches 156 */
        if (tia->p0 >= 156) tia->p0suppress = 0;
        if (tia->p1 >= 156) tia->p1suppress = 0;

        /* Advance to next clock */
        tia->start_clock++;
    }
}

/* ========================================================================
 * Register Handlers (port of TIA.cs PokeOp methods)
 * ======================================================================== */

/* Diagnostic logging for VSYNC/VBLANK — limited to first N frames */
#define SYNC_LOG_MAX_FRAMES 10
static int g_sync_log_frame = 0;

void tia_reset_sync_log(void)
{
    g_sync_log_frame = 0;
}

static void op_vsync(TIA *tia, uint8_t data, int poke_op_hsync)
{
    /* VSYNC toggle detection: frame ends when VSYNC turns off
     * (was on→off, or was never on and scanline > 258) */
    (void)poke_op_hsync;

    if (g_sync_log_frame < SYNC_LOG_MAX_FRAMES) {
        int old_on = (tia->regw[VSYNC] & 0x02) != 0;
        int new_on = (data & 0x02) != 0;
        if (old_on != new_on || new_on) {
            char msg[128];
            snprintf(msg, sizeof(msg), "VSYNC: data=0x%02X %s->%s sl=%d hsync=%d",
                     data, old_on ? "ON" : "OFF", new_on ? "ON" : "OFF",
                     tia->scanline, tia->hsync);
            log_msg(msg);
        }
    }

    if ((data & 0x02) == 0) {
        if ((tia->regw[VSYNC] & 0x02) != 0
            && (tia->vblank_off_scanline >= 0 || !tia->vblankon)) {
            tia->end_of_frame = 1;

            if (g_sync_log_frame < SYNC_LOG_MAX_FRAMES) {
                char msg[128];
                snprintf(msg, sizeof(msg), "VSYNC: end_of_frame=1 at sl=%d (frame %d)",
                         tia->scanline, g_sync_log_frame);
                log_msg(msg);
                g_sync_log_frame++;
            }
        }
    }
    tia->regw[VSYNC] = data;
}

static void op_vblank(TIA *tia, uint8_t data, uint64_t cpu_clock)
{
    int old_vblankon = tia->vblankon;

    /* Dump port handling */
    if ((tia->regw[VBLANK] & 0x80) == 0) {
        /* Dump was clear, will be set — discharge capacitors */
        if ((data & 0x80) != 0) {
            tia->dump_enabled = 1;
        }
    } else {
        /* Dump was set, will be cleared — start charging */
        if ((data & 0x80) == 0) {
            tia->dump_enabled = 0;
            tia->dump_disabled_cycle = cpu_clock;
        }
    }

    tia->regw[VBLANK] = data;
    tia->vblankon = (data & 0x02) != 0;

    if (g_sync_log_frame < SYNC_LOG_MAX_FRAMES) {
        if (old_vblankon != tia->vblankon) {
            char msg[128];
            snprintf(msg, sizeof(msg), "VBLANK: data=0x%02X %s->%s sl=%d hsync=%d dump=%d",
                     data, old_vblankon ? "ON" : "OFF", tia->vblankon ? "ON" : "OFF",
                     tia->scanline, tia->hsync, (data & 0x80) ? 1 : 0);
            log_msg(msg);
        }
    }

    /* Record first VBLANK OFF transition as vertical anchor */
    if (!tia->vblankon && old_vblankon) {
        if (tia->vblank_off_scanline < 0)
            tia->vblank_off_scanline = tia->scanline;
        /* Remember transitions in the visible area for carry-forward */
        if (tia->scanline < 200)
            tia->last_good_vbo = tia->scanline;
    }
}

static void op_wsync(TIA *tia, int poke_op_hsync)
{
    if (poke_op_hsync > 0) {
        tia->wsync_delay_clocks = 228 - poke_op_hsync;
        /* CPU preempt is set by mem_write_2600 after tia_write returns */
    }
}

static void op_nusiz0(TIA *tia, uint8_t data)
{
    tia->regw[NUSIZ0] = data & 0x37;
    tia->m0size = (tia->regw[NUSIZ0] & 0x30) >> 4;
    tia->m0type = tia->regw[NUSIZ0] & 0x07;
    tia->p0type = tia->m0type;
    tia->p0suppress = 0;
}

static void op_nusiz1(TIA *tia, uint8_t data)
{
    tia->regw[NUSIZ1] = data & 0x37;
    tia->m1size = (tia->regw[NUSIZ1] & 0x30) >> 4;
    tia->m1type = tia->regw[NUSIZ1] & 0x07;
    tia->p1type = tia->m1type;
    tia->p1suppress = 0;
}

static void op_colup0(TIA *tia, uint8_t data)
{
    tia->colup0 = data;
}

static void op_colup1(TIA *tia, uint8_t data)
{
    tia->colup1 = data;
}

static void op_colupf(TIA *tia, uint8_t data)
{
    tia->colupf = data;
}

static void op_colubk(TIA *tia, uint8_t data)
{
    tia->colubk = data;
}

static void op_ctrlpf(TIA *tia, uint8_t data)
{
    tia->regw[CTRLPF] = data;
    tia->pf_reflection_state = data & 1;
    tia->blsize = (data & 0x30) >> 4;
    tia->scoreon = (data & 0x02) != 0;
    tia->pfpriority = (data & 0x04) != 0;
}

static void op_refp0(TIA *tia, uint8_t data)
{
    tia->regw[REFP0] = data & 0x08;
    set_eff_grp0(tia);
}

static void op_refp1(TIA *tia, uint8_t data)
{
    tia->regw[REFP1] = data & 0x08;
    set_eff_grp1(tia);
}

static void op_pf(TIA *tia, uint16_t addr, uint8_t data)
{
    tia->regw[addr] = data;
    tia->pf210 = (uint32_t)((tia->regw[PF2] << 12)
        | (tia->regw[PF1] << 4)
        | ((tia->regw[PF0] >> 4) & 0x0f));
}

static void op_resp0(TIA *tia, int poke_op_hsync, int poke_op_hsync_delta)
{
    if (poke_op_hsync < 68) {
        tia->p0 = 0;
    } else if (tia->hmove_latch && poke_op_hsync >= 68 && poke_op_hsync < 76) {
        tia->p0 = -((poke_op_hsync - 68) >> 1);
    } else {
        tia->p0 = -4;
    }
    tia->p0 -= poke_op_hsync_delta;
    tia->p0 %= 160;
    tia->p0suppress = 1;
}

static void op_resp1(TIA *tia, int poke_op_hsync, int poke_op_hsync_delta)
{
    if (poke_op_hsync < 68) {
        tia->p1 = 0;
    } else if (tia->hmove_latch && poke_op_hsync >= 68 && poke_op_hsync < 76) {
        tia->p1 = -((poke_op_hsync - 68) >> 1);
    } else {
        tia->p1 = -4;
    }
    tia->p1 -= poke_op_hsync_delta;
    tia->p1 %= 160;
    tia->p1suppress = 1;
}

static void op_resm0(TIA *tia, int poke_op_hsync, int poke_op_hsync_delta)
{
    tia->m0 = poke_op_hsync < 68 ? -2 : -4;
    tia->m0 -= poke_op_hsync_delta;
    tia->m0 %= 160;
}

static void op_resm1(TIA *tia, int poke_op_hsync, int poke_op_hsync_delta)
{
    tia->m1 = poke_op_hsync < 68 ? -2 : -4;
    tia->m1 -= poke_op_hsync_delta;
    tia->m1 %= 160;
}

static void op_resbl(TIA *tia, int poke_op_hsync, int poke_op_hsync_delta)
{
    tia->bl = poke_op_hsync < 68 ? -2 : -4;
    tia->bl -= poke_op_hsync_delta;
    tia->bl %= 160;
}

static void op_grp0(TIA *tia, uint8_t data)
{
    tia->regw[GRP0] = data;
    tia->old_grp1 = tia->regw[GRP1];
    set_eff_grp0(tia);
    set_eff_grp1(tia);
}

static void op_grp1(TIA *tia, uint8_t data)
{
    tia->regw[GRP1] = data;
    tia->old_grp0 = tia->regw[GRP0];
    tia->old_enabl = (tia->regw[ENABL] != 0);
    set_eff_grp0(tia);
    set_eff_grp1(tia);
    set_blon(tia);
}

static void op_enam0(TIA *tia, uint8_t data)
{
    tia->regw[ENAM0] = data & 0x02;
    tia->m0on = (tia->regw[ENAM0] != 0) && (tia->regw[RESMP0] == 0);
}

static void op_enam1(TIA *tia, uint8_t data)
{
    tia->regw[ENAM1] = data & 0x02;
    tia->m1on = (tia->regw[ENAM1] != 0) && (tia->regw[RESMP1] == 0);
}

static void op_enabl(TIA *tia, uint8_t data)
{
    tia->regw[ENABL] = data & 0x02;
    set_blon(tia);
}

static void op_hm(TIA *tia, uint16_t addr, uint8_t data)
{
    /* Marshal via >>4 for compare convenience (matches EMU7800 SetHmr) */
    tia->regw[addr] = (uint8_t)((data ^ 0x80) >> 4);
}

static void op_vdelp0(TIA *tia, uint8_t data)
{
    tia->regw[VDELP0] = data & 0x01;
    set_eff_grp0(tia);
}

static void op_vdelp1(TIA *tia, uint8_t data)
{
    tia->regw[VDELP1] = data & 0x01;
    set_eff_grp1(tia);
}

static void op_vdelbl(TIA *tia, uint8_t data)
{
    tia->regw[VDELBL] = data & 0x01;
    set_blon(tia);
}

static void op_resmp0(TIA *tia, uint8_t data)
{
    if (tia->regw[RESMP0] != 0 && (data & 0x02) == 0) {
        int middle = 4;
        switch (tia->regw[NUSIZ0] & 0x07) {
            case 0x05: middle <<= 1; break;
            case 0x07: middle <<= 2; break;
        }
        tia->m0 = (tia->p0 - middle) % 160;
    }
    tia->regw[RESMP0] = data & 0x02;
    tia->m0on = (tia->regw[ENAM0] != 0) && (tia->regw[RESMP0] == 0);
}

static void op_resmp1(TIA *tia, uint8_t data)
{
    if (tia->regw[RESMP1] != 0 && (data & 0x02) == 0) {
        int middle = 4;
        switch (tia->regw[NUSIZ1] & 0x07) {
            case 0x05: middle <<= 1; break;
            case 0x07: middle <<= 2; break;
        }
        tia->m1 = (tia->p1 - middle) % 160;
    }
    tia->regw[RESMP1] = data & 0x02;
    tia->m1on = (tia->regw[ENAM1] != 0) && (tia->regw[RESMP1] == 0);
}

static void op_hmove(TIA *tia, int poke_op_hsync)
{
    uint64_t clock = tia->start_clock;  /* Clock = current TIA clock */

    tia->p0suppress = 0;
    tia->p1suppress = 0;
    tia->start_hmove_clock = clock + 3;

    /* Activision Spiderfighter cheat */
    if (poke_op_hsync == 201)
        tia->start_hmove_clock++;
}

static void op_hmclr(TIA *tia)
{
    op_hm(tia, HMP0, 0);
    op_hm(tia, HMP1, 0);
    op_hm(tia, HMM0, 0);
    op_hm(tia, HMM1, 0);
    op_hm(tia, HMBL, 0);
}

static void op_cxclr(TIA *tia)
{
    tia->collisions = 0;
}

/* ========================================================================
 * TIA Write — Poke with delay-aware rendering
 *
 * Port of TIA.cs Poke(): compute endClock (with optional delay for
 * GRP/PF registers), render up to endClock, then dispatch handler.
 * ======================================================================== */

void tia_write(TIA *tia, uint16_t addr, uint8_t data, uint64_t cpu_clock)
{
    uint64_t clock = cpu_clock * 3;
    uint64_t end_clock = clock;
    int poke_op_hsync_delta;
    int poke_op_hsync;

    addr &= 0x3F;

    /* Compute PokeOpHSync BEFORE rendering (for PF delay calculation).
     * PokeOpHSync = current hsync position = HSync + delta
     * where delta = Clock - LastEndClock = Clock - (StartClock - 1) */
    poke_op_hsync_delta = (int)(clock - (tia->start_clock - 1));
    poke_op_hsync = (tia->hsync + poke_op_hsync_delta) % 228;
    if (poke_op_hsync < 0) poke_op_hsync += 228;

    /* Some writes take extra CLKs to affect TIA state */
    switch (addr) {
        case GRP0:
        case GRP1:
            end_clock += 1;
            break;
        case PF0:
        case PF1:
        case PF2:
            switch (poke_op_hsync & 3) {
                case 0: end_clock += 4; break;
                case 1: end_clock += 3; break;
                case 2: end_clock += 2; break;
                case 3: end_clock += 5; break;
            }
            break;
    }

    /* Render up to the write point */
    render_from_start_clock_to(tia, end_clock);

    /* Recompute PokeOpHSync AFTER rendering (for handler use) */
    poke_op_hsync_delta = (int)(clock - (tia->start_clock - 1));
    poke_op_hsync = (tia->hsync + poke_op_hsync_delta) % 228;
    if (poke_op_hsync < 0) poke_op_hsync += 228;

    /* Dispatch to handler */
    switch (addr) {
        case VSYNC:  op_vsync(tia, data, poke_op_hsync); break;
        case VBLANK: op_vblank(tia, data, cpu_clock); break;
        case WSYNC:  op_wsync(tia, poke_op_hsync); break;
        case RSYNC:  break;  /* RSYNC: not commonly used, skip */
        case NUSIZ0: op_nusiz0(tia, data); break;
        case NUSIZ1: op_nusiz1(tia, data); break;
        case COLUP0: op_colup0(tia, data); break;
        case COLUP1: op_colup1(tia, data); break;
        case COLUPF: op_colupf(tia, data); break;
        case COLUBK: op_colubk(tia, data); break;
        case CTRLPF: op_ctrlpf(tia, data); break;
        case REFP0:  op_refp0(tia, data); break;
        case REFP1:  op_refp1(tia, data); break;
        case PF0:
        case PF1:
        case PF2:    op_pf(tia, addr, data); break;
        case RESP0:  op_resp0(tia, poke_op_hsync, poke_op_hsync_delta); break;
        case RESP1:  op_resp1(tia, poke_op_hsync, poke_op_hsync_delta); break;
        case RESM0:  op_resm0(tia, poke_op_hsync, poke_op_hsync_delta); break;
        case RESM1:  op_resm1(tia, poke_op_hsync, poke_op_hsync_delta); break;
        case RESBL:  op_resbl(tia, poke_op_hsync, poke_op_hsync_delta); break;
        case AUDC0:
        case AUDC1:
        case AUDF0:
        case AUDF1:
        case AUDV0:
        case AUDV1:
            tiasound_render_to_position((int)(tia->start_clock - g_frame_start_tia_clock));
            tia->regw[addr] = data;
            tiasound_update(addr, data);
            break;
        case GRP0:   op_grp0(tia, data); break;
        case GRP1:   op_grp1(tia, data); break;
        case ENAM0:  op_enam0(tia, data); break;
        case ENAM1:  op_enam1(tia, data); break;
        case ENABL:  op_enabl(tia, data); break;
        case HMP0:
        case HMP1:
        case HMM0:
        case HMM1:
        case HMBL:   op_hm(tia, addr, data); break;
        case VDELP0: op_vdelp0(tia, data); break;
        case VDELP1: op_vdelp1(tia, data); break;
        case VDELBL: op_vdelbl(tia, data); break;
        case RESMP0: op_resmp0(tia, data); break;
        case RESMP1: op_resmp1(tia, data); break;
        case HMOVE:  op_hmove(tia, poke_op_hsync); break;
        case HMCLR:  op_hmclr(tia); break;
        case CXCLR:  op_cxclr(tia); break;
        default:     break;
    }
}

/* ========================================================================
 * TIA Read — port of TIA.cs Peek
 * ======================================================================== */

uint8_t tia_read(TIA *tia, uint16_t addr, uint64_t cpu_clock, uint8_t data_bus_state)
{
    int retval = 0;
    uint64_t clock = cpu_clock * 3;

    addr &= 0x0F;

    /* Catch up TIA to current clock before reading */
    render_from_start_clock_to(tia, clock);

    switch (addr) {
        case CXM0P:
            retval |= (tia->collisions & CXP_M0P1) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_M0P0) ? 0x40 : 0;
            break;
        case CXM1P:
            retval |= (tia->collisions & CXP_M1P0) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_M1P1) ? 0x40 : 0;
            break;
        case CXP0FB:
            retval |= (tia->collisions & CXP_P0PF) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_P0BL) ? 0x40 : 0;
            break;
        case CXP1FB:
            retval |= (tia->collisions & CXP_P1PF) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_P1BL) ? 0x40 : 0;
            break;
        case CXM0FB:
            retval |= (tia->collisions & CXP_M0PF) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_M0BL) ? 0x40 : 0;
            break;
        case CXM1FB:
            retval |= (tia->collisions & CXP_M1PF) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_M1BL) ? 0x40 : 0;
            break;
        case CXBLPF:
            retval |= (tia->collisions & CXP_BLPF) ? 0x80 : 0;
            break;
        case CXPPMM:
            retval |= (tia->collisions & CXP_P0P1) ? 0x80 : 0;
            retval |= (tia->collisions & CXP_M0M1) ? 0x40 : 0;
            break;

        case INPT0:
        case INPT1:
        case INPT2:
        case INPT3:
            if (tia->dump_enabled) {
                retval = 0x00;
            } else {
                if (tia->dump_disabled_cycle == 0 ||
                    (cpu_clock - tia->dump_disabled_cycle) > 380) {
                    retval = 0x80;
                } else {
                    retval = 0x00;
                }
            }
            break;
        case INPT4:
            retval = g_frame_trigger[0] ? 0x00 : 0x80;
            if (g_inpt4_log_count < 20) {
                char msg[96];
                snprintf(msg, sizeof(msg), "INPT4 READ: val=$%02X trigger=%d", retval, g_frame_trigger[0]);
                log_msg(msg);
                g_inpt4_log_count++;
            }
            break;
        case INPT5:
            retval = g_frame_trigger[1] ? 0x00 : 0x80;
            break;
        case 0x0F:
            /* Required by Haunted House */
            retval = 0x0F;
            break;
        default:
            break;
    }

    /* TIA reads only drive bits 7-6; lower 6 bits reflect data bus */
    return (uint8_t)(retval | (data_bus_state & 0x3F));
}

/* ========================================================================
 * Render remaining TIA clocks (for final frame catch-up)
 * ======================================================================== */

void tia_render_remaining(TIA *tia, uint64_t target_clock)
{
    render_from_start_clock_to(tia, target_clock);
}

/* ========================================================================
 * Init / Reset / Frame Management
 * ======================================================================== */

static int g_tables_built = 0;
static int g_last_vblank_off_scanline = 40;

/* Active height hysteresis — prevents jitter from +/-1 scanline VBLANK variance */
static int g_last_active_height = 192;
static int g_pending_active_height = 192;
static int g_pending_height_count = 0;
#define HEIGHT_STABLE_FRAMES 3

/* Frame sync stubs — synchronization handled by g_frame_ready in main.c */
int tia_frame_ready(void)
{
    return 0;
}

void tia_frame_consumed(void)
{
}

void tia_init(TIA *tia)
{
    if (!g_tables_built) {
        build_tables();
        g_tables_built = 1;
    }
    memset(tia, 0, sizeof(TIA));
    memset(g_frame_buffers[0], 0, FB_WIDTH * FB_HEIGHT);
    memset(g_frame_buffers[1], 0, FB_WIDTH * FB_HEIGHT);
    g_frame_buffer = g_frame_buffers[0];
    g_display_buffer = g_frame_buffers[1];

    /* Enable frame summary + anomaly detection for first 5 frames */
    tia_dbg_init(
        TIA_DBG_FRAME | TIA_DBG_FRAME_SKIP,
        0, 4, -1);

    tia_reset(tia);
}

void tia_reset(TIA *tia)
{
    int i;
    for (i = 0; i < 0x40; i++)
        tia->regw[i] = 0;

    tia->vblankon = 0;
    tia->scoreon = 0;
    tia->pfpriority = 0;
    tia->m0on = tia->m1on = tia->blon = 0;
    tia->colubk = tia->colupf = tia->colup0 = tia->colup1 = 0;
    tia->pf_reflection_state = 0;
    tia->pf210 = 0;

    /* HSync starts at -1 (first ++HSync in render loop brings it to 0) */
    tia->hsync = -1;
    tia->scanline = 0;

    /* Position counters start at -1 (invisible until first RESPx) */
    tia->p0 = tia->p1 = tia->m0 = tia->m1 = tia->bl = -1;
    tia->p0mmr = tia->p1mmr = tia->m0mmr = tia->m1mmr = tia->blmmr = 0;

    tia->start_hmove_clock = UINT64_MAX;
    tia->hmove_counter = -1;
    tia->hmove_latch = 0;

    tia->eff_grp0 = tia->old_grp0 = 0;
    tia->eff_grp1 = tia->old_grp1 = 0;
    tia->p0type = tia->p1type = 0;
    tia->p0suppress = tia->p1suppress = 0;
    tia->m0type = tia->m0size = 0;
    tia->m1type = tia->m1size = 0;
    tia->blsize = 0;
    tia->old_enabl = 0;

    tia->wsync_delay_clocks = 0;
    tia->end_of_frame = 0;
    tia->vblank_off_scanline = -1;
    tia->last_good_vbo = -1;
    tia->last_visible_row = -1;

    tia->dump_enabled = 0;
    tia->dump_disabled_cycle = 0;

    tia->collisions = 0;

    tiasound_reset();
}

void tia_start_frame(TIA *tia)
{
    tia->wsync_delay_clocks = 0;
    tia->end_of_frame = 0;
    tia->scanline = 0;
    /*
     * VBLANK-OFF anchor logic:
     * - VBLANK ON at frame start: reset to -1, wait for ON→OFF transition
     * - VBLANK OFF at frame start: carry forward last known good anchor,
     *   or default to 0 if no good anchor exists yet.
     *
     * This handles three patterns:
     * 1. Normal games (VBLANK ON at frame end): resets to -1, set by transition
     * 2. Games with inconsistent VBLANK (Nuts): carries forward stable anchor
     * 3. Games that never use VBLANK (Alpha Beam, Name This Game): defaults to 0
     */
    if (tia->vblankon) {
        tia->vblank_off_scanline = -1;
    } else {
        tia->vblank_off_scanline = (tia->last_good_vbo >= 0)
            ? tia->last_good_vbo : 0;
    }
    tia->last_visible_row = -1;

    /* Clear write buffer to prevent stale double-buffer artifacts */
    memset(g_frame_buffer, 0, FB_WIDTH * FB_HEIGHT);

    /* Catch up any pending rendering before frame starts */
    /* (EMU7800: FrameBufferIndex %= 160; RenderFromStartClockTo(Clock);) */
    /* We skip this since we clear the buffer and use 2D addressing */

    g_frame_trigger[0] = machine_sample_trigger(0);
    g_frame_trigger[1] = machine_sample_trigger(1);

    tiasound_start_frame();
}

void tia_end_frame(TIA *tia)
{
    TIA_DBG(TIA_DBG_FRAME,
        "FRAME_END #%d sl=%d vbo_sl=%d",
        g_tia_dbg_frame_num, tia->scanline, tia->vblank_off_scanline);

    /* Swap double buffers */
    {
        uint8_t *temp = g_display_buffer;
        g_display_buffer = g_frame_buffer;
        g_frame_buffer = temp;
    }

    if (tia->vblank_off_scanline >= 0)
        g_last_vblank_off_scanline = tia->vblank_off_scanline;

    /* Update active height with hysteresis */
    if (tia->last_visible_row >= 0) {
        int new_height = tia->last_visible_row + 1;
        if (new_height == g_pending_active_height) {
            if (++g_pending_height_count >= HEIGHT_STABLE_FRAMES)
                g_last_active_height = new_height;
        } else {
            g_pending_active_height = new_height;
            g_pending_height_count = 1;
        }
    }

    tiasound_end_frame();
}

uint8_t *tia_get_frame_buffer(void)
{
    return g_display_buffer;
}

uint8_t *tia_get_write_buffer(void)
{
    return g_frame_buffer;
}

int tia_get_vblank_off_scanline(void)
{
    return g_last_vblank_off_scanline;
}

void tia_set_vblank_off_scanline(int scanline)
{
    g_last_vblank_off_scanline = scanline;
}

int tia_get_active_height(void)
{
    return g_last_active_height;
}

int tia_scan_content_bounds(int *first_row, int *last_row,
                            int *first_col, int *last_col)
{
    int y, x, max_rows;
    const uint8_t *row_ptr;

    *first_row = -1;
    *last_row = -1;
    *first_col = FB_WIDTH;
    *last_col = -1;

    max_rows = g_last_active_height;
    if (max_rows < 1 || max_rows > FB_HEIGHT)
        max_rows = FB_HEIGHT;

    for (y = 0; y < max_rows; y++) {
        row_ptr = g_display_buffer + y * FB_WIDTH;
        for (x = 0; x < FB_WIDTH; x++) {
            if (row_ptr[x] != 0) {
                if (*first_row < 0) *first_row = y;
                *last_row = y;
                if (x < *first_col) *first_col = x;
                if (x > *last_col) *last_col = x;
            }
        }
    }

    if (*first_row < 0) {
        *first_col = -1;
        *last_col = -1;
        return -1;
    }
    return 0;
}

void tia_set_frame_start_clock(uint64_t clock)
{
    g_frame_start_tia_clock = clock;
}

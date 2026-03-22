/*
 * cart.c
 *
 * Cartridge Mapper Implementation
 * Supports 2K, 4K, 8K (F8), 16K (F6), 32K (F4) for 2600
 * and basic 7800 mappers
 *
 * Copyright (c) 2003-2011 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cart.h"

/* External logging function */
extern void log_msg(const char *msg);

/* Diagnostic logging for bank switching — limited to first N events */
#define BANK_LOG_MAX 30
static int g_bank_log_count = 0;

/* SC RAM diagnostic logging — limited to first N events */
#define SC_LOG_MAX 50
static int g_sc_read_log_count = 0;
static int g_sc_write_log_count = 0;

void cart_reset_log_counts(void)
{
    g_bank_log_count = 0;
    g_sc_read_log_count = 0;
    g_sc_write_log_count = 0;
}

/* External machine helpers for Supercharger support */
extern uint32_t machine_get_distinct_accesses(void);
extern uint8_t machine_peek_ram(uint16_t addr);
extern void machine_poke_ram(uint16_t addr, uint8_t data);

/* External CPU clock accessor for DPC music timing */
extern uint64_t machine_get_cpu_clock(void);

/* Supercharger constants */
#define SC_BANK_SIZE  2048
#define SC_RAM_SIZE   (3 * SC_BANK_SIZE)   /* 6144 */
#define SC_LOAD_SIZE  8448

/*
 * Minimal MD5 implementation (RFC 1321) for ROM identification.
 * Used to match headerless ROMs against a known properties database
 * (equivalent to EMU7800's ROMProperties.csv lookup).
 */
#define MD5_LEFTROTATE(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static void md5_compute(const uint8_t *data, size_t len, uint8_t digest[16])
{
    static const uint32_t s[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe, h3 = 0x10325476;
    size_t num_full = len / 64;
    size_t tail = len % 64;
    int pad_blocks = (tail < 56) ? 1 : 2;
    uint8_t last[128];
    size_t bi, j;

    /* Process complete 64-byte blocks from original data */
    for (bi = 0; bi < num_full; bi++) {
        const uint8_t *p = data + bi * 64;
        uint32_t M[16], a = h0, b = h1, c = h2, d = h3;
        for (j = 0; j < 16; j++)
            M[j] = (uint32_t)p[j*4] | ((uint32_t)p[j*4+1]<<8)
                 | ((uint32_t)p[j*4+2]<<16) | ((uint32_t)p[j*4+3]<<24);
        for (j = 0; j < 64; j++) {
            uint32_t f, g;
            if      (j < 16) { f = (b&c)|(~b&d);  g = j; }
            else if (j < 32) { f = (d&b)|(~d&c);  g = (5*j+1)%16; }
            else if (j < 48) { f = b^c^d;          g = (3*j+5)%16; }
            else              { f = c^(b|~d);       g = (7*j)%16; }
            f += a + K[j] + M[g]; a = d; d = c; c = b;
            b += MD5_LEFTROTATE(f, s[j]);
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }

    /* Build last block(s) with padding */
    memset(last, 0, sizeof(last));
    memcpy(last, data + num_full * 64, tail);
    last[tail] = 0x80;
    {
        uint64_t bit_len = (uint64_t)len * 8;
        size_t lo = (size_t)pad_blocks * 64 - 8;
        for (j = 0; j < 8; j++)
            last[lo + j] = (uint8_t)(bit_len >> (j * 8));
    }

    /* Process padding block(s) */
    for (bi = 0; bi < (size_t)pad_blocks; bi++) {
        const uint8_t *p = last + bi * 64;
        uint32_t M[16], a = h0, b = h1, c = h2, d = h3;
        for (j = 0; j < 16; j++)
            M[j] = (uint32_t)p[j*4] | ((uint32_t)p[j*4+1]<<8)
                 | ((uint32_t)p[j*4+2]<<16) | ((uint32_t)p[j*4+3]<<24);
        for (j = 0; j < 64; j++) {
            uint32_t f, g;
            if      (j < 16) { f = (b&c)|(~b&d);  g = j; }
            else if (j < 32) { f = (d&b)|(~d&c);  g = (5*j+1)%16; }
            else if (j < 48) { f = b^c^d;          g = (3*j+5)%16; }
            else              { f = c^(b|~d);       g = (7*j)%16; }
            f += a + K[j] + M[g]; a = d; d = c; c = b;
            b += MD5_LEFTROTATE(f, s[j]);
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }

    for (j = 0; j < 4; j++) {
        digest[j]    = (uint8_t)(h0 >> (j*8));
        digest[4+j]  = (uint8_t)(h1 >> (j*8));
        digest[8+j]  = (uint8_t)(h2 >> (j*8));
        digest[12+j] = (uint8_t)(h3 >> (j*8));
    }
}

/*
 * ROM properties database (from EMU7800 ROMProperties.csv).
 * Maps ROM MD5 → cart type + controller types for headerless ROMs.
 * MD5 is computed over the raw file data (before any header stripping).
 */
typedef struct {
    uint8_t md5[16];
    CartType cart_type;      /* CART_UNKNOWN = don't override */
    int left_controller;
    int right_controller;
} RomDbEntry;

static const RomDbEntry rom_properties_db[] = {
    /* Alien Brigade (NTSC) - S9 + Lightgun */
    {{0x87,0x7d,0xcc,0x97,0xa7,0x75,0xed,0x55,0x08,0x18,0x64,0xb2,0xdb,0xf5,0xf1,0xe2},
     CART_7800_S9, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Alien Brigade (PAL) - S9 + Lightgun */
    {{0xde,0x3e,0x94,0x96,0xcb,0x73,0x41,0xf8,0x65,0xf2,0x7e,0x5a,0x72,0xc7,0xf2,0xf5},
     CART_7800_S9, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Barnyard Blaster (NTSC) - SG + Lightgun */
    {{0x42,0x68,0x24,0x15,0x90,0x6c,0x21,0xc6,0xaf,0x80,0xe4,0x19,0x84,0x03,0xff,0xda},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Barnyard Blaster (PAL) - SG + Lightgun */
    {{0xba,0xbe,0x2b,0xc2,0x97,0x66,0x88,0xba,0xfb,0x8b,0x23,0xc1,0x92,0x65,0x81,0x26},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Crossbow (NTSC) - S9 + Lightgun */
    {{0xa9,0x4e,0x45,0x60,0xb6,0xad,0x05,0x3a,0x1c,0x24,0xe0,0x96,0xf1,0x26,0x2e,0xbf},
     CART_7800_S9, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Crossbow (PAL) - S9 + Lightgun */
    {{0x63,0xdb,0x37,0x1d,0x67,0xa9,0x8d,0xae,0xc5,0x47,0xb2,0xab,0xd5,0xe7,0xaa,0x95},
     CART_7800_S9, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Meltdown (NTSC) - SG + Lightgun */
    {{0xbe,0xdc,0x30,0xec,0x43,0x58,0x7e,0x0c,0x98,0xfc,0x38,0xc3,0x9c,0x1e,0xf9,0xd0},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Meltdown (PAL) - SG + Lightgun */
    {{0xc8,0x01,0x55,0xd7,0xee,0xc9,0xe3,0xdc,0xb7,0x9a,0xa6,0xb8,0x3c,0x9c,0xcd,0x1e},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Sentinel (NTSC) - SG + Lightgun */
    {{0xb6,0x97,0xd9,0xc2,0xd1,0xb9,0xf6,0xcb,0x21,0x04,0x12,0x86,0xd1,0xbb,0xfa,0x7f},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* Sentinel (PAL) - SG + Lightgun */
    {{0x54,0x69,0xb4,0xde,0x06,0x08,0xf2,0x3a,0x5c,0x4f,0x98,0xf3,0x31,0xc9,0xe7,0x5f},
     CART_7800_SG, CTRL_LIGHTGUN, CTRL_LIGHTGUN},
    /* F-18 Hornet (NTSC) - Absolute */
    {{0x22,0x51,0xa6,0xa0,0xf3,0xae,0xc8,0x4c,0xc0,0xaf,0xf6,0x6f,0xc9,0xfa,0x91,0xe8},
     CART_7800_AB, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* F-18 Hornet (PAL) - Absolute */
    {{0xe7,0x70,0x9d,0xa8,0xe4,0x9d,0x37,0x67,0x30,0x19,0x47,0xa0,0xa0,0xb9,0xd2,0xe6},
     CART_7800_AB, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Pit Fighter (NTSC) - S4 */
    {{0x05,0xf4,0x32,0x44,0x46,0x59,0x43,0xce,0x81,0x97,0x80,0xa7,0x1a,0x5b,0x57,0x2a},
     CART_7800_S4, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* RealSports Baseball (NTSC) - S4 */
    {{0x38,0x3e,0xd9,0xbd,0x1e,0xfb,0x9b,0x6c,0xb3,0x38,0x8a,0x77,0x76,0x78,0xc9,0x28},
     CART_7800_S4, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Tank Command (NTSC) - S4 */
    {{0x5c,0x4f,0x75,0x23,0x71,0xa5,0x23,0xf1,0x5e,0x99,0x80,0xfe,0xa7,0x3b,0x87,0x4d},
     CART_7800_S4, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Tower Toppler (NTSC) - S4R (RAM at $6000) */
    {{0x8d,0x64,0x76,0x3d,0xb3,0x10,0x0a,0xad,0xc5,0x52,0xdb,0x5e,0x68,0x68,0x50,0x6a},
     CART_7800_S4R, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Tower Toppler (PAL) - S4R (RAM at $6000) */
    {{0x32,0xa3,0x72,0x44,0xa9,0xc6,0xcc,0x92,0x8d,0xcd,0xf0,0x2b,0x45,0x36,0x5a,0xa8},
     CART_7800_S4R, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Water Ski (NTSC) - S4 */
    {{0x42,0x7c,0xb0,0x5d,0x0a,0x1a,0xbb,0x06,0x89,0x98,0xe2,0x76,0x0d,0x77,0xf4,0xfb},
     CART_7800_S4, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Jinks (NTSC) - SGR (SuperGame + RAM at $4000) */
    {{0x04,0x5f,0xd1,0x20,0x50,0xb7,0xf2,0xb8,0x42,0xd5,0x97,0x0f,0x24,0x14,0xe9,0x12},
     CART_7800_SGR, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
    /* Jinks (PAL) - SGR (SuperGame + RAM at $4000) */
    {{0xdf,0xb8,0x6f,0x4d,0x06,0xf0,0x5a,0xd0,0x0c,0xf4,0x18,0xf0,0xa5,0x9a,0x24,0xf7},
     CART_7800_SGR, CTRL_PROLINE_JOYSTICK, CTRL_PROLINE_JOYSTICK},
};
#define NUM_ROM_DB_ENTRIES (sizeof(rom_properties_db) / sizeof(rom_properties_db[0]))

/* Look up ROM MD5 in properties database. Returns entry or NULL. */
static const RomDbEntry *lookup_rom_db(const uint8_t *md5)
{
    size_t i;
    for (i = 0; i < NUM_ROM_DB_ENTRIES; i++) {
        if (memcmp(md5, rom_properties_db[i].md5, 16) == 0)
            return &rom_properties_db[i];
    }
    return NULL;
}

/*
 * Dummy Supercharger BIOS ROM code (294 bytes)
 * From Stella 7.0 CartAR.cxx - simulates SC loading with progress bars
 */
static uint8_t sc_dummy_rom_code[294] = {
    0xa5, 0xfa, 0x85, 0x80, 0x4c, 0x18, 0xf8, 0xff,
    0xff, 0xff, 0x78, 0xd8, 0xa0, 0x00, 0xa2, 0x00,
    0x94, 0x00, 0xe8, 0xd0, 0xfb, 0x4c, 0x50, 0xf8,
    0xa2, 0x00, 0xbd, 0x06, 0xf0, 0xad, 0xf8, 0xff,
    0xa2, 0x00, 0xad, 0x00, 0xf0, 0xea, 0xbd, 0x00,
    0xf7, 0xca, 0xd0, 0xf6, 0x4c, 0x50, 0xf8, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xa2, 0x03, 0xbc, 0x22, 0xf9, 0x94, 0xfa, 0xca,
    0x10, 0xf8, 0xa0, 0x00, 0xa2, 0x28, 0x94, 0x04,
    0xca, 0x10, 0xfb, 0xa2, 0x1c, 0x94, 0x81, 0xca,
    0x10, 0xfb, 0xa9, 0xff, 0xc9, 0x00, 0xd0, 0x03,
    0x4c, 0x13, 0xf9, 0xa9, 0x00, 0x85, 0x1b, 0x85,
    0x1c, 0x85, 0x1d, 0x85, 0x1e, 0x85, 0x1f, 0x85,
    0x19, 0x85, 0x1a, 0x85, 0x08, 0x85, 0x01, 0xa9,
    0x10, 0x85, 0x21, 0x85, 0x02, 0xa2, 0x07, 0xca,
    0xca, 0xd0, 0xfd, 0xa9, 0x00, 0x85, 0x20, 0x85,
    0x10, 0x85, 0x11, 0x85, 0x02, 0x85, 0x2a, 0xa9,
    0x05, 0x85, 0x0a, 0xa9, 0xff, 0x85, 0x0d, 0x85,
    0x0e, 0x85, 0x0f, 0x85, 0x84, 0x85, 0x85, 0xa9,
    0xf0, 0x85, 0x83, 0xa9, 0x74, 0x85, 0x09, 0xa9,
    0x0c, 0x85, 0x15, 0xa9, 0x1f, 0x85, 0x17, 0x85,
    0x82, 0xa9, 0x07, 0x85, 0x19, 0xa2, 0x08, 0xa0,
    0x00, 0x85, 0x02, 0x88, 0xd0, 0xfb, 0x85, 0x02,
    0x85, 0x02, 0xa9, 0x02, 0x85, 0x02, 0x85, 0x00,
    0x85, 0x02, 0x85, 0x02, 0x85, 0x02, 0xa9, 0x00,
    0x85, 0x00, 0xca, 0x10, 0xe4, 0x06, 0x83, 0x66,
    0x84, 0x26, 0x85, 0xa5, 0x83, 0x85, 0x0d, 0xa5,
    0x84, 0x85, 0x0e, 0xa5, 0x85, 0x85, 0x0f, 0xa6,
    0x82, 0xca, 0x86, 0x82, 0x86, 0x17, 0xe0, 0x0a,
    0xd0, 0xc3, 0xa9, 0x02, 0x85, 0x01, 0xa2, 0x1c,
    0xa0, 0x00, 0x84, 0x19, 0x84, 0x09, 0x94, 0x81,
    0xca, 0x10, 0xfb, 0xa6, 0x80, 0xdd, 0x00, 0xf0,
    0xa9, 0x9a, 0xa2, 0xff, 0xa0, 0x00, 0x9a, 0x4c,
    0xfa, 0x00, 0xcd, 0xf8, 0xff, 0x4c
};

/*
 * Default Supercharger header (256 bytes)
 * Used when ROM is smaller than one full load (8448 bytes)
 */
static const uint8_t sc_default_header[256] = {
    0xac, 0xfa, 0x0f, 0x18, 0x62, 0x00, 0x24, 0x02,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x01, 0x05, 0x09, 0x0d, 0x11, 0x15, 0x19, 0x1d,
    0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a, 0x1e,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

/* Forward declarations for Supercharger internal functions */
static void sc_bank_configuration(SCState *sc, uint8_t config);
static void sc_initialize_rom(SCState *sc);
static void sc_load_into_ram(Cart *cart, uint8_t load);
static uint8_t sc_checksum(const uint8_t *data, int len);

/* SC diagnostic trace counter: logs first N cart reads after game start */
static int sc_trace_count = 0;
/* SC write event counter for logging */
static int sc_write_log_count = 0;


/* External CPU state for kernel trace logging */

/* Initialize cart */
void cart_init(Cart *cart)
{
    memset(cart, 0, sizeof(Cart));
    cart->type = CART_UNKNOWN;
    cart->rom = NULL;
    cart->ram = NULL;
    cart->sc = NULL;
    cart->dpc = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
    cart->bank = 0;
    cart->bank_count = 1;
    cart->left_controller = CTRL_PROLINE_JOYSTICK;
    cart->right_controller = CTRL_PROLINE_JOYSTICK;
}

/* Free cart memory */
void cart_free(Cart *cart)
{
    if (cart->rom) {
        free(cart->rom);
        cart->rom = NULL;
    }
    if (cart->ram) {
        free(cart->ram);
        cart->ram = NULL;
    }
    if (cart->sc) {
        if (cart->sc->loads) {
            free(cart->sc->loads);
        }
        free(cart->sc);
        cart->sc = NULL;
    }
    if (cart->dpc) {
        free(cart->dpc);
        cart->dpc = NULL;
    }
    cart->rom_size = 0;
    cart->ram_size = 0;
}

/*
 * Detect Parker Brothers E0 bankswitching.
 * Scans ROM for 3-byte instruction patterns accessing E0 hotspots ($1FE0-$1FF7).
 * Based on Stella's isProbablyE0().
 */
static int is_probably_e0(const uint8_t *rom, int size)
{
    static const uint8_t e0_sigs[][3] = {
        {0x8D, 0xE0, 0x1F},  /* STA $1FE0 */
        {0x8D, 0xE0, 0x5F},  /* STA $5FE0 (mirror) */
        {0x8D, 0xE9, 0xFF},  /* STA $FFE9 */
        {0x0C, 0xE0, 0x1F},  /* NOP $1FE0 (read) */
        {0xAD, 0xE0, 0x1F},  /* LDA $1FE0 */
        {0xAD, 0xE9, 0xFF},  /* LDA $FFE9 */
        {0xAD, 0xED, 0xFF},  /* LDA $FFED */
        {0xAD, 0xF3, 0xBF},  /* LDA $BFF3 */
    };
    int num_sigs = sizeof(e0_sigs) / sizeof(e0_sigs[0]);
    int s, i;
    for (s = 0; s < num_sigs; s++) {
        for (i = 0; i <= size - 3; i++) {
            if (rom[i] == e0_sigs[s][0] && rom[i+1] == e0_sigs[s][1] && rom[i+2] == e0_sigs[s][2])
                return 1;
        }
    }
    return 0;
}

/*
 * Detect Activision FE bankswitching.
 * Scans ROM for 5-byte signatures unique to known FE games.
 * Based on Stella's isProbablyFE().
 */
static int is_probably_fe(const uint8_t *rom, int size)
{
    static const uint8_t fe_sigs[][5] = {
        {0x20, 0x00, 0xD0, 0xC6, 0xC5},  /* Decathlon */
        {0x20, 0xC3, 0xF8, 0xA5, 0x82},  /* Robot Tank */
        {0xD0, 0xFB, 0x20, 0x73, 0xFE},  /* Space Shuttle */
        {0xD0, 0xFB, 0x20, 0x68, 0xFE},  /* Space Shuttle SECAM */
        {0x20, 0x00, 0xF0, 0x84, 0xD6},  /* Thwocker */
    };
    int num_sigs = sizeof(fe_sigs) / sizeof(fe_sigs[0]);
    int s, i;
    for (s = 0; s < num_sigs; s++) {
        for (i = 0; i <= size - 5; i++) {
            if (rom[i] == fe_sigs[s][0] && rom[i+1] == fe_sigs[s][1] &&
                rom[i+2] == fe_sigs[s][2] && rom[i+3] == fe_sigs[s][3] &&
                rom[i+4] == fe_sigs[s][4])
                return 1;
        }
    }
    return 0;
}

/*
 * Detect Super Chip (SC) RAM - 128 bytes mapped into first 256 bytes of each bank.
 * SC carts have the first 128 bytes equal to the second 128 bytes in every 4K bank,
 * because the RAM area is uninitialized and the ROM dump captures the same pattern.
 */
static int is_probably_sc(const uint8_t *rom, int size)
{
    int offset;
    for (offset = 0; offset < size; offset += 0x1000) {
        if (memcmp(rom + offset, rom + offset + 128, 128) != 0)
            return 0;
    }
    return 1;
}

/* Detect cart type from ROM data and size */
CartType cart_detect_type(const uint8_t *rom, int size, int machine_type)
{
    if (machine_type == 1) {
        /* 7800 */
        if (size <= 8192) return CART_7800_8K;
        if (size <= 16384) return CART_7800_16K;
        if (size <= 32768) return CART_7800_32K;
        if (size <= 49152) return CART_7800_48K;
        if (size <= 65536) return CART_7800_S4;  /* 64KB = SuperGame S4 (most common) */
        /*
         * >64KB: Must be bankswitched.
         * 144KB (9 × 16KB) = SuperGame S9 (Alien Brigade, Crossbow)
         * 128KB (8 × 16KB) = Standard SuperGame
         * Other sizes: default to standard SuperGame
         */
        if (size > 131072) return CART_7800_S9;
        return CART_7800_SG;
    }

    /* 2600: Supercharger - ROM size is exact multiple of 8448 bytes */
    if (size % SC_LOAD_SIZE == 0 && size >= SC_LOAD_SIZE) return CART_AR;

    /* CBS RAM Plus 12K (FA) - exactly 12KB */
    if (size == 12288) return CART_CBS12K;

    /* DPC (Pitfall II) - ~10KB (8K program + 2K display data, some dumps have extra padding) */
    if (size > 8192 && size < 12288) return CART_DPC;

    /* 2600 standard mappers */
    if (size <= 2048) return CART_A2K;
    if (size <= 4096) return CART_A4K;
    if (size <= 8192) {
        if (is_probably_e0(rom, size)) return CART_PB8K;
        if (is_probably_fe(rom, size)) return CART_DC8K;
        return CART_A8K;
    }
    if (size <= 16384) return CART_A16K;
    if (size <= 32768) return CART_A32K;
    return CART_SB;  /* 64KB+ = Superbanking (32/64 x 4K banks) */
}

/* Check for A78 header (128 bytes starting with version + "ATARI7800") */
static int has_a78_header(const uint8_t *data, int size)
{
    /* A78 header is 128 bytes, starts with version byte then "ATARI7800" */
    if (size < 128) return 0;

    /* Check for "ATARI7800" at offset 1 */
    if (data[1] == 'A' && data[2] == 'T' && data[3] == 'A' && data[4] == 'R' &&
        data[5] == 'I' && data[6] == '7' && data[7] == '8' && data[8] == '0' &&
        data[9] == '0') {
        return 1;
    }
    return 0;
}

/* Load ROM into cart */
int cart_load(Cart *cart, const uint8_t *data, int size, int machine_type)
{
    char msg[256];
    uint8_t cart_type2 = 0;
    int has_header = 0;
    int md5_type_set = 0;

    cart_free(cart);

    /* Reset all non-pointer fields that cart_free doesn't clear.
     * Ensures clean state regardless of previous load or stale compilation. */
    cart->type = CART_UNKNOWN;
    cart->bank = 0;
    cart->bank_count = 1;
    memset(cart->banks, 0, sizeof(cart->banks));
    cart->has_sc_ram = 0;
    memset(cart->sc_ram, 0, sizeof(cart->sc_ram));
    cart->left_controller = CTRL_PROLINE_JOYSTICK;
    cart->right_controller = CTRL_PROLINE_JOYSTICK;

    snprintf(msg, sizeof(msg), "cart_load: size=%d, machine_type=%d", size, machine_type);
    log_msg(msg);

    /* Log first 16 raw bytes for header identification */
    if (size >= 16) {
        snprintf(msg, sizeof(msg),
            "cart_load: raw[0..15]=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
            data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
        log_msg(msg);
    }

    /* Check for and skip A78 header on 7800 ROMs */
    if (machine_type == 1 && has_a78_header(data, size)) {
        log_msg("cart_load: A78 header detected, skipping 128 bytes");
        has_header = 1;
        /* Read cart type bytes before skipping header */
        cart_type2 = data[0x36];
        snprintf(msg, sizeof(msg), "cart_load: A78 header cartType1=0x%02X cartType2=0x%02X (SG=%d SGR=%d ROM4K=%d)",
                 data[0x35], cart_type2,
                 (cart_type2 & 0x02) != 0, (cart_type2 & 0x04) != 0, (cart_type2 & 0x08) != 0);
        log_msg(msg);

        /* Parse controller types from A78 header bytes 0x37/0x38 */
        {
            uint8_t lc = data[0x37];
            uint8_t rc = data[0x38];
            cart->left_controller = (lc == 2) ? CTRL_LIGHTGUN :
                                    (lc == 1) ? CTRL_PROLINE_JOYSTICK :
                                    (lc == 0) ? CTRL_NONE : CTRL_PROLINE_JOYSTICK;
            cart->right_controller = (rc == 2) ? CTRL_LIGHTGUN :
                                     (rc == 1) ? CTRL_PROLINE_JOYSTICK :
                                     (rc == 0) ? CTRL_NONE : CTRL_PROLINE_JOYSTICK;
            snprintf(msg, sizeof(msg), "cart_load: A78 controllers left=%d (0x%02X) right=%d (0x%02X)",
                     cart->left_controller, lc, cart->right_controller, rc);
            log_msg(msg);
        }

        data += 128;
        size -= 128;
        snprintf(msg, sizeof(msg), "cart_load: adjusted size=%d", size);
        log_msg(msg);
    } else if (machine_type == 1) {
        snprintf(msg, sizeof(msg), "cart_load: NO A78 header found (bytes[1..4]=%02X%02X%02X%02X)",
                 size >= 5 ? data[1] : 0, size >= 5 ? data[2] : 0,
                 size >= 5 ? data[3] : 0, size >= 5 ? data[4] : 0);
        log_msg(msg);
        if (size > 32768) {
            log_msg("cart_load: WARNING: >32KB 7800 ROM without A78 header, cannot detect SuperGame!");
        }

        /*
         * MD5-based ROM identification for headerless 7800 ROMs.
         * Matches against EMU7800's ROMProperties.csv database to detect
         * cart type and controller types that can't be inferred from ROM data alone.
         */
        {
            uint8_t md5[16];
            const RomDbEntry *db_entry;
            md5_compute(data, (size_t)size, md5);
            snprintf(msg, sizeof(msg),
                "cart_load: ROM MD5=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                md5[0],md5[1],md5[2],md5[3],md5[4],md5[5],md5[6],md5[7],
                md5[8],md5[9],md5[10],md5[11],md5[12],md5[13],md5[14],md5[15]);
            log_msg(msg);

            db_entry = lookup_rom_db(md5);
            if (db_entry) {
                cart->left_controller = db_entry->left_controller;
                cart->right_controller = db_entry->right_controller;
                if (db_entry->cart_type != CART_UNKNOWN) {
                    cart->type = db_entry->cart_type;
                    md5_type_set = 1;
                    snprintf(msg, sizeof(msg), "cart_load: ROM type=%d set from database (MD5 match)", cart->type);
                    log_msg(msg);
                }
                log_msg("cart_load: ROM identified from properties database (MD5 match)");
            }
        }
    }

    /* Detect cart type from A78 header flags or size-based fallback.
     * Skip if MD5 database already set the type. */
    if (md5_type_set) {
        /* Type already set from ROM properties database */
    } else if (machine_type == 1 && has_header) {
        uint8_t cart_type1 = data[-128 + 0x35];  /* data already advanced past header */

        /* Absolute (F-18 Hornet): cartType1 bit 0 */
        if (cart_type1 & 0x01) {
            cart->type = CART_7800_AB;
            log_msg("cart_load: Absolute (A78AB) detected from header");
        }
        /* Activision (Double Dragon, Rampage): cartType1 bit 1 */
        else if (cart_type1 & 0x02) {
            cart->type = CART_7800_AC;
            log_msg("cart_load: Activision (A78AC) detected from header");
        }
        /* SuperGame variants: cartType2 bit 1 */
        else if (cart_type2 & 0x02) {
            /* S9 (9-bank): cartType2 bit 3 (ROM at $4000) */
            if (cart_type2 & 0x08) {
                cart->type = CART_7800_S9;
                log_msg("cart_load: SuperGame S9 (9-bank) detected from header");
            } else if (cart_type2 & 0x04) {
                cart->type = CART_7800_SGR;
                log_msg("cart_load: SuperGame + RAM detected from header");
            } else {
                cart->type = CART_7800_SG;
                log_msg("cart_load: SuperGame detected from header");
            }
        } else {
            cart->type = cart_detect_type(data, size, machine_type);
        }
    } else {
        cart->type = cart_detect_type(data, size, machine_type);
    }

    /*
     * Override SG/SGR to S4/S4R for ROMs <= 64KB.
     * A78 headers may flag SuperGame for 64KB ROMs, but these are actually
     * 4-bank SuperGame (S4/S4R) with bank mask & 3 instead of & 7.
     */
    if (machine_type == 1 && size <= 65536) {
        if (cart->type == CART_7800_SG) {
            cart->type = CART_7800_S4;
            log_msg("cart_load: Overriding SG -> S4 (ROM <= 64KB)");
        } else if (cart->type == CART_7800_SGR) {
            cart->type = CART_7800_S4R;
            log_msg("cart_load: Overriding SGR -> S4R (ROM <= 64KB)");
        }
    }

    /*
     * Heuristic for 128KB headerless 7800 ROMs:
     * Standard SuperGame and Activision are both 128KB but have different
     * bank layouts and bankswitching mechanisms.
     *
     * Method 1 (reliable): Scan for Activision bankswitch instruction pattern.
     * Activision games switch banks by writing to $FF80-$FF8F (STA $FF8x).
     * The 3-byte pattern 0x8D 0x8x 0xFF is highly specific and doesn't appear
     * in SuperGame ROMs, which switch banks via writes to $8000-$BFFF.
     *
     * Method 2 (fallback): Check reset vector validity.
     *   SG: Bank 7 at $C000, reset vector at ROM offset 0x1FFFC
     *   AC: Bank 14 (8KB) at $E000, reset vector at ROM offset 0x1DFFC
     */
    if (cart->type == CART_7800_SG && !has_header && !md5_type_set && size == 131072) {
        int ac_sig = 0;
        int i;
        uint16_t sg_vec, ac_vec;
        int sg_invalid;

        /*
         * Scan for Activision bankswitch instructions targeting $FF80-$FF8F.
         * Activision games switch banks by writing to $FF80-$FF8F.
         * Check all absolute-addressing write opcodes:
         *   8D xx FF = STA $FFxx    (absolute)
         *   8E xx FF = STX $FFxx    (absolute)
         *   8C xx FF = STY $FFxx    (absolute)
         *   9D 80 FF = STA $FF80,X  (absolute,X - base addr $FF80)
         *   99 80 FF = STA $FF80,Y  (absolute,Y - base addr $FF80)
         */
        for (i = 0; i < size - 2; i++) {
            if (data[i+2] == 0xFF && (data[i+1] & 0xF0) == 0x80) {
                uint8_t op = data[i];
                if (op == 0x8D || op == 0x8E || op == 0x8C ||  /* STA/STX/STY abs */
                    op == 0x9D || op == 0x99) {                 /* STA abs,X / abs,Y */
                    ac_sig++;
                }
            }
        }
        /*
         * Check reset vector validity to confirm AC vs SG.
         * Real AC games have invalid SG reset vectors (e.g. $0000, $FFFF)
         * because bank 7 in SG layout doesn't contain valid code.
         * SG games with incidental AC-like byte patterns have valid SG
         * reset vectors (e.g. Crack'ed=$D000, Barnyard Blaster=$D000).
         */
        sg_vec = (uint16_t)(data[0x1FFFC] | (data[0x1FFFD] << 8));
        ac_vec = (uint16_t)(data[0x1DFFC] | (data[0x1DFFD] << 8));
        sg_invalid = (sg_vec < 0x4000) || (sg_vec >= 0xFFF8) ||
                     (data[0x1FFFC] == data[0x1FFFD]);
        snprintf(msg, sizeof(msg), "cart_load: 128KB heuristic: AC_sigs=%d SG_reset=$%04X (invalid=%d) AC_reset=$%04X",
                 ac_sig, sg_vec, sg_invalid, ac_vec);
        log_msg(msg);

        if (sg_invalid && (ac_sig >= 2 || ac_vec >= 0x4000)) {
            cart->type = CART_7800_AC;
            log_msg("cart_load: Activision detected (SG reset invalid + AC signatures or valid AC reset)");
        }
    }

    /*
     * SGR heuristic: if detected as SG and the first 32KB (banks 0-1) are
     * all zeros, upgrade to SGR.  SGR carts have 16KB RAM at $4000-$7FFF,
     * so ROM dumps typically have empty data in the bank slots that overlap
     * the RAM region.  A real SG game wouldn't waste two entire 16KB banks.
     */
    if (cart->type == CART_7800_SG) {
        int all_zero = 1;
        int j;
        for (j = 0; j < 32768 && j < size; j++) {
            if (data[j] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            cart->type = CART_7800_SGR;
            log_msg("cart_load: banks 0-1 empty, upgrading SG -> SGR (RAM at $4000)");
        }
    }

    {
        static const char *type_names[] = {
            "UNKNOWN", "A2K", "A4K", "A8K", "A16K", "A32K", "DC8K", "PB8K", "AR",
            "7800_8K", "7800_16K", "7800_32K", "7800_48K",
            "7800_SG", "7800_SGR", "7800_S9", "7800_AB", "7800_AC",
            "CBS12K", "DPC", "7800_S4", "7800_S4R", "SB"
        };
        const char *name = (cart->type >= 0 && cart->type <= CART_7800_S4R) ?
                           type_names[cart->type] : "INVALID";
        snprintf(msg, sizeof(msg), "cart_load: detected type=%d (%s)", cart->type, name);
        log_msg(msg);
    }

    /* Supercharger: allocate SC state and load images */
    if (cart->type == CART_AR) {
        int loads_size;
        SCState *sc = (SCState *)malloc(sizeof(SCState));
        if (!sc) {
            log_msg("cart_load: malloc failed for SCState!");
            return -1;
        }
        memset(sc, 0, sizeof(SCState));
        cart->sc = sc;

        /* Allocate load images buffer (at least one full load) */
        loads_size = size < SC_LOAD_SIZE ? SC_LOAD_SIZE : size;
        sc->loads = (uint8_t *)malloc(loads_size);
        if (!sc->loads) {
            log_msg("cart_load: malloc failed for SC loads!");
            free(sc);
            cart->sc = NULL;
            return -1;
        }
        memset(sc->loads, 0, loads_size);
        memcpy(sc->loads, data, size);

        /* If ROM is smaller than one load, append default header */
        if (size < SC_LOAD_SIZE) {
            memcpy(sc->loads + 8192, sc_default_header, 256);
            sc->num_loads = 1;
            snprintf(msg, sizeof(msg), "cart_load: SC undersized ROM (%d bytes), using default header", size);
            log_msg(msg);
        } else {
            sc->num_loads = size / SC_LOAD_SIZE;
        }

        snprintf(msg, sizeof(msg), "cart_load: Supercharger with %d load(s)", sc->num_loads);
        log_msg(msg);

        /* Initialize dummy BIOS ROM in image[6144..8191] */
        sc_initialize_rom(sc);

        /* Initial bank configuration: ROM in upper segment */
        sc_bank_configuration(sc, 0);

        log_msg("cart_load: Supercharger loaded successfully");
        return 0;
    }

    /* SuperGame family: SG (8-bank), SGR (8-bank+RAM), S9 (9-bank) */
    if (cart->type == CART_7800_SG || cart->type == CART_7800_SGR || cart->type == CART_7800_S9) {
        int num_banks = (cart->type == CART_7800_S9) ? 9 : 8;
        int buf_size = num_banks * 16384;
        int last_bank = num_banks - 1;
        int b;

        cart->rom = (uint8_t *)malloc(buf_size);
        if (!cart->rom) {
            log_msg("cart_load: malloc failed for SuperGame ROM!");
            return -1;
        }
        memset(cart->rom, 0, buf_size);
        if (size > buf_size) size = buf_size;
        memcpy(cart->rom, data, size);
        cart->rom_size = buf_size;

        snprintf(msg, sizeof(msg), "cart_load: SuperGame(%d-bank) ROM loaded %d bytes into %d byte buffer",
                 num_banks, size, buf_size);
        log_msg(msg);

        /* Log first 4 bytes of each bank */
        for (b = 0; b < num_banks; b++) {
            int off = b * 16384;
            snprintf(msg, sizeof(msg), "cart_load: Bank%d[0x%05X]: %02X %02X %02X %02X%s",
                     b, off, cart->rom[off], cart->rom[off+1], cart->rom[off+2], cart->rom[off+3],
                     (cart->rom[off] == 0 && cart->rom[off+1] == 0 && cart->rom[off+2] == 0 && cart->rom[off+3] == 0) ? " (empty)" : "");
            log_msg(msg);
        }

        /* Log reset vector from last bank (Bank7 for SG, Bank8 for S9) */
        {
            int vec_off = last_bank * 16384 + 0x3FFC;
            snprintf(msg, sizeof(msg), "cart_load: Bank%d reset vector: $%02X%02X (at ROM offset 0x%05X)",
                     last_bank, cart->rom[vec_off+1], cart->rom[vec_off], vec_off);
            log_msg(msg);
        }

        if (cart->type == CART_7800_S9) {
            /*
             * S9 bank mapping (matching Cart78S9.cs):
             * $4000: Bank 0 (fixed ROM)
             * $8000: Banks 0-8, switchable (default Bank 1)
             * $C000: Bank 8 (fixed, contains reset vector)
             */
            cart->banks[0] = 0;
            cart->banks[1] = 0;  /* $4000: Bank 0 */
            cart->banks[2] = 1;  /* $8000: Bank 1 (switchable, default) */
            cart->banks[3] = 8;  /* $C000: Bank 8 (fixed) */
            cart->bank_count = 9;
        } else {
            /* Standard SG/SGR bank mapping (matching Cart78SG.cs) */
            cart->banks[0] = 0;
            cart->banks[1] = 6;  /* $4000: Bank 6 */
            cart->banks[2] = 0;  /* $8000: Bank 0 (switchable) */
            cart->banks[3] = 7;  /* $C000: Bank 7 (fixed) */
            cart->bank_count = 8;
        }

        /* Allocate RAM for SGR variant */
        if (cart->type == CART_7800_SGR) {
            cart->ram_size = 16384;
            cart->ram = (uint8_t *)malloc(cart->ram_size);
            if (!cart->ram) {
                log_msg("cart_load: malloc failed for SuperGame RAM!");
                free(cart->rom);
                cart->rom = NULL;
                return -1;
            }
            memset(cart->ram, 0, cart->ram_size);
            log_msg("cart_load: SuperGame RAM allocated (16KB)");
        }
    }
    /* SuperGame S4/S4R: 64KB, 4 x 16KB banks (like SG but bank mask & 3) */
    else if (cart->type == CART_7800_S4 || cart->type == CART_7800_S4R) {
        int s4_size = 128 * 1024;  /* Pad to 128KB (8 banks) for uniform addressing */
        cart->rom = (uint8_t *)malloc(s4_size);
        if (!cart->rom) {
            log_msg("cart_load: malloc failed for S4 ROM!");
            return -1;
        }
        memset(cart->rom, 0, s4_size);
        if (size > s4_size) size = s4_size;
        memcpy(cart->rom, data, size);
        cart->rom_size = s4_size;

        /*
         * S4 bank mapping (matching Cart78S4.cs):
         * $4000: Bank 2 (fixed)
         * $8000: Bank 0 (switchable, value & 3)
         * $C000: Bank 3 (fixed, contains reset vector)
         */
        cart->banks[0] = 0;
        cart->banks[1] = 2;  /* $4000: Bank 2 (fixed) */
        cart->banks[2] = 0;  /* $8000: Bank 0 (switchable) */
        cart->banks[3] = 3;  /* $C000: Bank 3 (fixed) */
        cart->bank_count = 4;

        /* S4R: 8KB RAM at $6000-$7FFF */
        if (cart->type == CART_7800_S4R) {
            cart->ram_size = 8192;
            cart->ram = (uint8_t *)malloc(cart->ram_size);
            if (!cart->ram) {
                log_msg("cart_load: malloc failed for S4R RAM!");
                free(cart->rom);
                cart->rom = NULL;
                return -1;
            }
            memset(cart->ram, 0, cart->ram_size);
            log_msg("cart_load: S4R RAM allocated (8KB at $6000)");
        }

        {
            int vec_off = 3 * 16384 + 0x3FFC;
            snprintf(msg, sizeof(msg), "cart_load: S4%s ROM %d bytes, reset=$%02X%02X",
                     cart->type == CART_7800_S4R ? "R" : "",
                     size, cart->rom[vec_off+1], cart->rom[vec_off]);
            log_msg(msg);
        }
    }
    /* Absolute mapper (F-18 Hornet): 64KB, 4 x 16KB banks */
    else if (cart->type == CART_7800_AB) {
        int ab_size = 64 * 1024;  /* 64KB */
        cart->rom = (uint8_t *)malloc(ab_size);
        if (!cart->rom) {
            log_msg("cart_load: malloc failed for Absolute ROM!");
            return -1;
        }
        memset(cart->rom, 0, ab_size);
        if (size > ab_size) size = ab_size;
        memcpy(cart->rom, data, size);
        cart->rom_size = ab_size;

        /*
         * Absolute bank mapping (matching Cart78AB.cs):
         * $4000: Bank 0-1, switchable (default Bank 0)
         * $8000: Bank 2 (fixed)
         * $C000: Bank 3 (fixed, contains reset vector)
         */
        cart->banks[0] = 0;
        cart->banks[1] = 0;  /* $4000: switchable, default 0 */
        cart->banks[2] = 2;  /* $8000: fixed */
        cart->banks[3] = 3;  /* $C000: fixed */
        cart->bank_count = 4;

        {
            int vec_off = 3 * 16384 + 0x3FFC;
            snprintf(msg, sizeof(msg), "cart_load: Absolute ROM %d bytes, reset=$%02X%02X",
                     size, cart->rom[vec_off+1], cart->rom[vec_off]);
            log_msg(msg);
        }
    }
    /* Activision mapper (Double Dragon, Rampage): 128KB, 16 x 8KB banks */
    else if (cart->type == CART_7800_AC) {
        int ac_size = 128 * 1024;  /* 128KB = 16 x 8KB */
        cart->rom = (uint8_t *)malloc(ac_size);
        if (!cart->rom) {
            log_msg("cart_load: malloc failed for Activision ROM!");
            return -1;
        }
        memset(cart->rom, 0, ac_size);
        if (size > ac_size) size = ac_size;
        memcpy(cart->rom, data, size);
        cart->rom_size = ac_size;

        /*
         * Activision bank mapping (matching Cart78AC.cs):
         * Uses 8KB banks (not 16KB), addressed by addr >> 13
         * $4000: Bank 13 (fixed)
         * $6000: Bank 12 (fixed)
         * $8000: Bank 15 (fixed)
         * $A000: Bank 2*n (switchable, default n=0 -> Bank 0)
         * $C000: Bank 2*n+1 (switchable, default n=0 -> Bank 1)
         * $E000: Bank 14 (fixed)
         */
        cart->banks[0] = 0;   /* unused (below $4000) */
        cart->banks[1] = 0;   /* unused */
        cart->banks[2] = 13;  /* $4000-$5FFF */
        cart->banks[3] = 12;  /* $6000-$7FFF */
        cart->banks[4] = 15;  /* $8000-$9FFF */
        cart->banks[5] = 0;   /* $A000-$BFFF: switchable (2*n, n=0) */
        cart->banks[6] = 1;   /* $C000-$DFFF: switchable (2*n+1, n=0) */
        cart->banks[7] = 14;  /* $E000-$FFFF */

        {
            int vec_off = 14 * 8192 + 0x1FFC;  /* Bank 14 reset vector */
            snprintf(msg, sizeof(msg), "cart_load: Activision ROM %d bytes, reset=$%02X%02X",
                     size, cart->rom[vec_off+1], cart->rom[vec_off]);
            log_msg(msg);
        }
    } else {
        /* Standard ROM allocation (pad DPC to 0x2800 for display data access) */
        int alloc_size = size;
        if (cart->type == CART_DPC && alloc_size < 0x2800)
            alloc_size = 0x2800;
        cart->rom = (uint8_t *)malloc(alloc_size);
        if (!cart->rom) {
            log_msg("cart_load: malloc failed!");
            return -1;
        }
        memset(cart->rom, 0, alloc_size);
        memcpy(cart->rom, data, size);
        cart->rom_size = alloc_size;

        /* Set up banking for non-SuperGame types */
        switch (cart->type) {
            case CART_A2K:
                cart->bank_count = 1;
                break;
            case CART_A4K:
                cart->bank_count = 1;
                break;
            case CART_A8K:
                cart->bank_count = 2;
                cart->bank = 1;  /* Start at bank 1 */
                break;
            case CART_DC8K:
                cart->bank_count = 2;
                cart->bank = 0;
                break;
            case CART_PB8K:
                cart->bank_count = 8;
                cart->banks[0] = 4;  /* Slice 0 default */
                cart->banks[1] = 5;  /* Slice 1 default */
                cart->banks[2] = 6;  /* Slice 2 default */
                cart->banks[3] = 7;  /* Slice 3 fixed */
                break;
            case CART_CBS12K:
                cart->bank_count = 3;
                cart->bank = 2;  /* Default bank 2 */
                /* Allocate 256 bytes of RAM */
                cart->ram_size = 256;
                cart->ram = (uint8_t *)malloc(256);
                if (!cart->ram) {
                    log_msg("cart_load: malloc failed for CBS12K RAM!");
                    free(cart->rom);
                    cart->rom = NULL;
                    return -1;
                }
                memset(cart->ram, 0, 256);
                log_msg("cart_load: CBS12K loaded (3 banks + 256B RAM)");
                break;
            case CART_DPC:
                cart->bank_count = 2;
                cart->bank = 1;  /* Default bank 1 */
                /* Allocate DPC chip state */
                cart->dpc = (DPCState *)malloc(sizeof(DPCState));
                if (!cart->dpc) {
                    log_msg("cart_load: malloc failed for DPCState!");
                    free(cart->rom);
                    cart->rom = NULL;
                    return -1;
                }
                memset(cart->dpc, 0, sizeof(DPCState));
                cart->dpc->shift_register = 1;
                log_msg("cart_load: DPC loaded (2 banks + 2K display + DPC chip)");
                break;
            case CART_A16K:
                cart->bank_count = 4;
                cart->bank = 0;
                break;
            case CART_A32K:
                cart->bank_count = 8;
                cart->bank = 7;  /* EMU7800: last bank selected at power-on */
                break;
            case CART_SB:
                cart->bank_count = size / 4096;  /* 32 banks (128KB) or 64 banks (256KB) */
                cart->bank = cart->bank_count - 1;  /* Start at last bank */
                break;
            case CART_7800_8K:
            case CART_7800_16K:
            case CART_7800_32K:
            case CART_7800_48K:
                cart->bank_count = 1;
                break;
            default:
                cart->bank_count = 1;
                break;
        }
    }

    /* Detect Super Chip RAM for F8/F6/F4 carts */
    if (cart->type == CART_A8K || cart->type == CART_A16K || cart->type == CART_A32K) {
        if (is_probably_sc(cart->rom, cart->rom_size)) {
            cart->has_sc_ram = 1;
            memset(cart->sc_ram, 0, 128);
            {
                char scmsg[128];
                snprintf(scmsg, sizeof(scmsg), "cart_load: Super Chip RAM detected (%dK + 128B SC RAM)",
                         cart->rom_size / 1024);
                log_msg(scmsg);
            }
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "cart_load: ROM loaded, bank=%d, has_sc_ram=%d",
                 cart->bank, cart->has_sc_ram);
        log_msg(msg);
    }
    return 0;
}

/* Reset cart */
void cart_reset(Cart *cart)
{
    switch (cart->type) {
        case CART_A8K:
            cart->bank = 1;
            break;
        case CART_A16K:
            cart->bank = 0;
            break;
        case CART_A32K:
            cart->bank = 7;  /* EMU7800: last bank selected at power-on */
            break;
        case CART_SB:
            cart->bank = cart->bank_count - 1;  /* Last bank */
            break;
        case CART_DC8K:
            cart->bank = 0;
            break;
        case CART_PB8K:
            cart->banks[0] = 4;
            cart->banks[1] = 5;
            cart->banks[2] = 6;
            /* banks[3] = 7 always, set in cart_load */
            break;
        case CART_AR:
            if (cart->sc) {
                memset(cart->sc->image, 0, sizeof(cart->sc->image));
                sc_initialize_rom(cart->sc);
                cart->sc->write_enabled = 0;
                cart->sc->power = 1;
                cart->sc->data_hold = 0;
                cart->sc->distinct_set = 0;
                cart->sc->write_pending = 0;
                sc_bank_configuration(cart->sc, 0);
                sc_trace_count = 0;
                sc_write_log_count = 0;
            }
            break;
        case CART_7800_SG:
        case CART_7800_SGR:
            cart->banks[0] = 0;
            cart->banks[1] = 6;
            cart->banks[2] = 0;
            cart->banks[3] = 7;
            if (cart->ram) {
                memset(cart->ram, 0, cart->ram_size);
            }
            break;
        case CART_7800_S9:
            cart->banks[0] = 0;
            cart->banks[1] = 0;  /* $4000: Bank 0 */
            cart->banks[2] = 1;  /* $8000: Bank 1 (default) */
            cart->banks[3] = 8;  /* $C000: Bank 8 (fixed) */
            break;
        case CART_CBS12K:
            cart->bank = 2;
            if (cart->ram) memset(cart->ram, 0, cart->ram_size);
            break;
        case CART_DPC:
            cart->bank = 1;
            if (cart->dpc) {
                memset(cart->dpc->counters, 0, sizeof(cart->dpc->counters));
                memset(cart->dpc->tops, 0, sizeof(cart->dpc->tops));
                memset(cart->dpc->bots, 0, sizeof(cart->dpc->bots));
                memset(cart->dpc->flags, 0, sizeof(cart->dpc->flags));
                memset(cart->dpc->music_mode, 0, sizeof(cart->dpc->music_mode));
                cart->dpc->shift_register = 1;
                cart->dpc->last_system_clock = 3 * machine_get_cpu_clock();
                cart->dpc->fractional_clocks = 0.0;
            }
            break;
        case CART_7800_S4:
        case CART_7800_S4R:
            cart->banks[0] = 0;
            cart->banks[1] = 2;  /* $4000: Bank 2 (fixed) */
            cart->banks[2] = 0;  /* $8000: Bank 0 (switchable) */
            cart->banks[3] = 3;  /* $C000: Bank 3 (fixed) */
            if (cart->ram) memset(cart->ram, 0, cart->ram_size);
            break;
        case CART_7800_AB:
            cart->banks[0] = 0;
            cart->banks[1] = 0;  /* $4000: Bank 0 (default) */
            cart->banks[2] = 2;  /* $8000: Bank 2 (fixed) */
            cart->banks[3] = 3;  /* $C000: Bank 3 (fixed) */
            break;
        case CART_7800_AC:
            cart->banks[0] = 0;
            cart->banks[1] = 0;
            cart->banks[2] = 13; /* $4000: Bank 13 */
            cart->banks[3] = 12; /* $6000: Bank 12 */
            cart->banks[4] = 15; /* $8000: Bank 15 */
            cart->banks[5] = 0;  /* $A000: Bank 0 (switchable) */
            cart->banks[6] = 1;  /* $C000: Bank 1 (switchable) */
            cart->banks[7] = 14; /* $E000: Bank 14 */
            break;
        default:
            cart->bank = 0;
            break;
    }
    /* Reset Super Chip RAM */
    if (cart->has_sc_ram)
        memset(cart->sc_ram, 0, 128);
}

/* --- DPC (Pitfall II) helper functions --- */

/* Music amplitude lookup table */
static const uint8_t dpc_music_amplitudes[8] = {
    0x00, 0x04, 0x05, 0x09, 0x06, 0x0a, 0x0b, 0x0f
};

/*
 * Clock the DPC shift register (8-bit LFSR)
 * Taps at bits 0, 2, 3, 4; generates 255-length pseudo-random sequence
 */
static uint8_t dpc_clock_shift_register(DPCState *dpc)
{
    uint8_t a, x;
    a = dpc->shift_register & 0x01;
    x = (dpc->shift_register >> 2) & 0x01; a ^= x;
    x = (dpc->shift_register >> 3) & 0x01; a ^= x;
    x = (dpc->shift_register >> 4) & 0x01; a ^= x;
    a <<= 7;
    dpc->shift_register >>= 1;
    dpc->shift_register |= a;
    return dpc->shift_register;
}

/*
 * Update DPC music mode data fetchers (5, 6, 7)
 * DPC oscillator runs at 15750 Hz, CPU at ~1193191.67 Hz (NTSC 2600 TIA clock / 3)
 */
static void dpc_update_music(DPCState *dpc)
{
    uint64_t sys_clock = 3 * machine_get_cpu_clock();
    uint64_t delta = sys_clock - dpc->last_system_clock;
    double osc_clocks;
    int whole_clocks, i;
    dpc->last_system_clock = sys_clock;

    osc_clocks = 15750.0 * (double)delta / 1193191.66666667 + dpc->fractional_clocks;
    whole_clocks = (int)osc_clocks;
    dpc->fractional_clocks = osc_clocks - (double)whole_clocks;

    if (whole_clocks <= 0) return;

    for (i = 0; i < 3; i++) {
        int r = i + 5;
        int top, new_low;
        if (!dpc->music_mode[i]) continue;

        top = dpc->tops[r] + 1;
        new_low = dpc->counters[r] & 0x00FF;

        if (dpc->tops[r] != 0) {
            new_low -= whole_clocks % top;
            if (new_low < 0) {
                new_low += top;
            }
        } else {
            new_low = 0;
        }

        if (new_low <= dpc->bots[r]) {
            dpc->flags[r] = 0x00;
        } else if (new_low <= dpc->tops[r]) {
            dpc->flags[r] = 0xFF;
        }

        dpc->counters[r] = (uint16_t)((dpc->counters[r] & 0x0700) | (uint16_t)new_low);
    }
}

/*
 * Read a DPC register ($000-$03F)
 * fn (bits 5-3): function select
 * i (bits 2-0): data fetcher index
 */
static uint8_t dpc_read_register(Cart *cart, uint16_t addr)
{
    DPCState *dpc = cart->dpc;
    uint8_t result;
    int i = addr & 0x07;
    int fn = (addr >> 3) & 0x07;

    /* Update flag register for selected data fetcher */
    if ((dpc->counters[i] & 0x00FF) == dpc->tops[i]) {
        dpc->flags[i] = 0xFF;
    } else if ((dpc->counters[i] & 0x00FF) == dpc->bots[i]) {
        dpc->flags[i] = 0x00;
    }

    switch (fn) {
        case 0x00:
            if (i < 4) {
                /* Random number read */
                result = dpc_clock_shift_register(dpc);
            } else {
                /* Music amplitude read */
                uint8_t j = 0;
                dpc_update_music(dpc);
                if (dpc->music_mode[0] && dpc->flags[5] != 0) j |= 0x01;
                if (dpc->music_mode[1] && dpc->flags[6] != 0) j |= 0x02;
                if (dpc->music_mode[2] && dpc->flags[7] != 0) j |= 0x04;
                result = dpc_music_amplitudes[j];
            }
            break;
        case 0x01:
            /* Display data read */
            result = cart->rom[0x2000 + 0x7FF - dpc->counters[i]];
            break;
        case 0x02:
            /* Display data AND'd with flag */
            result = cart->rom[0x2000 + 0x7FF - dpc->counters[i]];
            result &= dpc->flags[i];
            break;
        case 0x07:
            /* Flag register */
            result = dpc->flags[i];
            break;
        default:
            result = 0;
            break;
    }

    /* Clock the selected data fetcher's counter (not music mode fetchers) */
    if (i < 5 || !dpc->music_mode[i - 5]) {
        dpc->counters[i] = (dpc->counters[i] - 1) & 0x07FF;
    }

    return result;
}

/*
 * Write a DPC register ($040-$07F)
 */
static void dpc_write_register(DPCState *dpc, uint16_t addr, uint8_t val)
{
    int i = addr & 0x07;
    int fn = (addr >> 3) & 0x07;

    switch (fn) {
        case 0x00:
            /* Set top count */
            dpc->tops[i] = val;
            dpc->flags[i] = 0x00;
            break;
        case 0x01:
            /* Set bottom count */
            dpc->bots[i] = val;
            break;
        case 0x02:
            /* Set counter low byte */
            dpc->counters[i] &= 0x0700;
            if (i >= 5 && dpc->music_mode[i - 5]) {
                /* Music mode: load from top register */
                dpc->counters[i] |= dpc->tops[i];
            } else {
                dpc->counters[i] |= val;
            }
            break;
        case 0x03:
            /* Set counter high byte + music mode */
            dpc->counters[i] &= 0x00FF;
            dpc->counters[i] |= (uint16_t)((val & 0x07) << 8);
            if (i >= 5) {
                dpc->music_mode[i - 5] = (val & 0x10) != 0;
            }
            break;
        case 0x06:
            /* Reset RNG */
            dpc->shift_register = 1;
            break;
    }

}

/* Read from cart - 2600 mappers */
uint8_t cart_read(Cart *cart, uint16_t addr)
{
    int offset;
    uint16_t masked;

    if (!cart->rom && cart->type != CART_AR) return 0;

    switch (cart->type) {
        case CART_A2K:
            /* 2K mirrored to 4K */
            return cart->rom[addr & 0x07FF];

        case CART_A4K:
            /* 4K standard */
            return cart->rom[addr & 0x0FFF];

        case CART_A8K:
            /* 8K F8 bankswitching */
            /* Cart only sees A0-A11; mask to 12 bits like original EMU7800 */
            masked = addr & 0x0FFF;
            if (masked == 0x0FF8) {
                cart->bank = 0;
                if (g_bank_log_count < BANK_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK READ: F8 addr=$%04X -> bank=0", addr);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            } else if (masked == 0x0FF9) {
                cart->bank = 1;
                if (g_bank_log_count < BANK_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK READ: F8 addr=$%04X -> bank=1", addr);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            /* Super Chip RAM: read port $x080-$x0FF */
            if (cart->has_sc_ram && masked >= 0x0080 && masked < 0x0100)
                return cart->sc_ram[masked & 0x7F];
            offset = (cart->bank * 0x1000) + masked;
            if (offset < cart->rom_size) {
                return cart->rom[offset];
            }
            return 0;

        case CART_A16K:
            /* 16K F6 bankswitching */
            masked = addr & 0x0FFF;
            if (masked >= 0x0FF6 && masked <= 0x0FF9) {
                int old_bank = cart->bank;
                cart->bank = masked - 0x0FF6;
                if (g_bank_log_count < BANK_LOG_MAX && cart->bank != old_bank) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK READ: F6 addr=$%04X -> bank=%d (was %d)", addr, cart->bank, old_bank);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            /* Super Chip RAM: read port $x080-$x0FF */
            if (cart->has_sc_ram && masked >= 0x0080 && masked < 0x0100) {
                uint8_t val = cart->sc_ram[masked & 0x7F];
                if (g_sc_read_log_count < SC_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "SC_READ: clk=%llu addr=$%04X ram[%02X]=$%02X bank=%d",
                             (unsigned long long)machine_get_cpu_clock(), addr, masked & 0x7F, val, cart->bank);
                    log_msg(msg);
                    g_sc_read_log_count++;
                }
                return val;
            }
            offset = (cart->bank * 0x1000) + masked;
            if (offset < cart->rom_size) {
                return cart->rom[offset];
            }
            return 0;

        case CART_A32K:
            /* 32K F4 bankswitching */
            masked = addr & 0x0FFF;
            if (masked >= 0x0FF4 && masked <= 0x0FFB) {
                int old_bank = cart->bank;
                cart->bank = masked - 0x0FF4;
                if (g_bank_log_count < BANK_LOG_MAX && cart->bank != old_bank) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK READ: F4 addr=$%04X -> bank=%d (was %d)", addr, cart->bank, old_bank);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            /* Super Chip RAM: read port $x080-$x0FF */
            if (cart->has_sc_ram && masked >= 0x0080 && masked < 0x0100) {
                uint8_t val = cart->sc_ram[masked & 0x7F];
                if (g_sc_read_log_count < SC_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "SC_READ: clk=%llu addr=$%04X ram[%02X]=$%02X bank=%d",
                             (unsigned long long)machine_get_cpu_clock(), addr, masked & 0x7F, val, cart->bank);
                    log_msg(msg);
                    g_sc_read_log_count++;
                }
                return val;
            }
            offset = (cart->bank * 0x1000) + masked;
            if (offset < cart->rom_size) {
                return cart->rom[offset];
            }
            return 0;

        case CART_DC8K:
            /* FE: A13 determines bank. A13=0 -> bank 1, A13=1 -> bank 0 */
            if (addr & 0x2000)
                return cart->rom[addr & 0x0FFF];            /* Bank 0 */
            else
                return cart->rom[(addr & 0x0FFF) + 0x1000]; /* Bank 1 */

        case CART_PB8K: {
            uint16_t pb_masked = addr & 0x0FFF;
            /* Check hotspots before reading */
            if (pb_masked >= 0x0FE0 && pb_masked < 0x0FE8)
                cart->banks[0] = pb_masked & 7;
            else if (pb_masked >= 0x0FE8 && pb_masked < 0x0FF0)
                cart->banks[1] = pb_masked & 7;
            else if (pb_masked >= 0x0FF0 && pb_masked < 0x0FF8)
                cart->banks[2] = pb_masked & 7;
            /* Read from correct segment */
            {
                int segment = pb_masked >> 10;  /* 0-3 */
                return cart->rom[(cart->banks[segment] << 10) | (pb_masked & 0x03FF)];
            }
        }

        case CART_CBS12K: {
            uint16_t cbs_masked = addr & 0x0FFF;
            /* RAM read port: $1100-$11FF */
            if (cbs_masked >= 0x0100 && cbs_masked < 0x0200) {
                return cart->ram[cbs_masked & 0xFF];
            }
            /* Hotspots for bank selection */
            if (cbs_masked >= 0x0FF8 && cbs_masked <= 0x0FFA) {
                cart->bank = cbs_masked - 0x0FF8;
            }
            return cart->rom[(cart->bank << 12) + cbs_masked];
        }

        case CART_DPC: {
            uint16_t dpc_masked = addr & 0x0FFF;
            /* DPC registers: $1000-$103F */
            if (dpc_masked < 0x0040) {
                return dpc_read_register(cart, dpc_masked);
            }
            /* Hotspots for bank selection */
            if (dpc_masked >= 0x0FF8 && dpc_masked <= 0x0FF9) {
                cart->bank = dpc_masked - 0x0FF8;
            }
            return cart->rom[(cart->bank << 12) + dpc_masked];
        }

        case CART_AR:
        {
            SCState *sc = cart->sc;
            uint8_t result;
            if (!sc) return 0;

            /* Load hotspot: $F850 when upper segment is ROM */
            if (((addr & 0x1FFF) == 0x1850) && (sc->image_offset[1] == SC_RAM_SIZE)) {
                uint8_t load = machine_peek_ram(0x0080);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "SC READ: load hotspot $F850 hit, load#=%d, RAM[$80]=0x%02X", load, load);
                    log_msg(msg);
                }
                sc_load_into_ram(cart, load);
            }

            /* Cancel pending write if >5 distinct accesses */
            if (sc->write_pending &&
                (machine_get_distinct_accesses() > sc->distinct_set + 5)) {
                sc->write_pending = 0;
            }

            /* Data hold register: access to $F0xx */
            if (!(addr & 0x0F00) && (!sc->write_enabled || !sc->write_pending)) {
                sc->data_hold = (uint8_t)addr;
                sc->distinct_set = machine_get_distinct_accesses();
                sc->write_pending = 1;
            }
            /* Bank config hotspot: $FFF8 */
            else if ((addr & 0x1FFF) == 0x1FF8) {
                sc->write_pending = 0;
                sc_bank_configuration(sc, sc->data_hold);
            }
            /* RAM write: enabled, pending, exactly 5 distinct accesses */
            else if (sc->write_enabled && sc->write_pending &&
                     (machine_get_distinct_accesses() == sc->distinct_set + 5)) {
                if ((addr & 0x0800) == 0) {
                    sc->image[(addr & 0x07FF) + sc->image_offset[0]] = sc->data_hold;
                    if (sc_write_log_count < 20) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "SC WRITE: data=0x%02X -> img[%u] (lo seg, addr=$%04X)",
                                 sc->data_hold, (unsigned)((addr & 0x07FF) + sc->image_offset[0]), addr);
                        log_msg(msg);
                        sc_write_log_count++;
                    }
                } else if (sc->image_offset[1] != (3 * SC_BANK_SIZE)) {
                    sc->image[(addr & 0x07FF) + sc->image_offset[1]] = sc->data_hold;
                    if (sc_write_log_count < 20) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "SC WRITE: data=0x%02X -> img[%u] (hi seg, addr=$%04X)",
                                 sc->data_hold, (unsigned)((addr & 0x07FF) + sc->image_offset[1]), addr);
                        log_msg(msg);
                        sc_write_log_count++;
                    }
                }
                sc->write_pending = 0;
            }

            result = sc->image[(addr & 0x07FF) + sc->image_offset[(addr & 0x0800) ? 1 : 0]];

            /* Instruction trace after game start */
            if (sc_trace_count > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "SC TRACE[%d]: addr=$%04X -> img[%u] = $%02X",
                         31 - sc_trace_count, addr,
                         (unsigned)((addr & 0x07FF) + sc->image_offset[(addr & 0x0800) ? 1 : 0]),
                         result);
                log_msg(msg);
                sc_trace_count--;
            }

            return result;
        }

        case CART_7800_8K:
            /* 7800 8K at $E000-$FFFF, mirrored down */
            if (addr >= 0x4000) {
                return cart->rom[addr & 0x1FFF];
            }
            return 0;

        case CART_7800_16K:
            /* 7800 16K at $C000-$FFFF, mirrored down to $4000 */
            if (addr >= 0x4000) {
                return cart->rom[addr & 0x3FFF];
            }
            return 0;

        case CART_7800_32K:
            /* 7800 32K at $8000-$FFFF, mirrored to $4000 */
            if (addr >= 0x4000) {
                return cart->rom[addr & 0x7FFF];
            }
            return 0;

        case CART_7800_48K:
            /* 7800 48K at $4000-$FFFF */
            if (addr >= 0x4000) {
                int rom_addr = addr - 0x4000;
                if (rom_addr < cart->rom_size) {
                    return cart->rom[rom_addr];
                }
            }
            return 0;

        case CART_7800_SG:
        case CART_7800_S9:
            /* SuperGame bankswitched: 8 or 9 x 16KB banks */
            if (addr >= 0x4000) {
                int bankNo = addr >> 14;  /* 0=$0-$3FFF, 1=$4000, 2=$8000, 3=$C000 */
                return cart->rom[(cart->banks[bankNo] << 14) | (addr & 0x3FFF)];
            }
            return 0;

        case CART_7800_SGR:
            /* SuperGame + RAM: RAM at $4000-$7FFF, bankswitched ROM at $8000+ */
            if (addr >= 0x4000) {
                int bankNo = addr >> 14;
                if (bankNo == 1 && cart->ram) {
                    return cart->ram[addr & 0x3FFF];
                }
                return cart->rom[(cart->banks[bankNo] << 14) | (addr & 0x3FFF)];
            }
            return 0;

        case CART_7800_AB:
            /* Absolute: 4 x 16KB banks */
            if (addr >= 0x4000) {
                int bankNo = addr >> 14;
                return cart->rom[(cart->banks[bankNo] << 14) | (addr & 0x3FFF)];
            }
            return 0;

        case CART_7800_AC:
            /* Activision: 16 x 8KB banks */
            if (addr >= 0x4000) {
                int bankNo = addr >> 13;  /* 8KB banks: addr >> 13 */
                return cart->rom[(cart->banks[bankNo] << 13) | (addr & 0x1FFF)];
            }
            return 0;

        case CART_7800_S4:
            /* SuperGame S4: 4 x 16KB banks (same layout as SG) */
            if (addr >= 0x4000) {
                int bankNo = addr >> 14;
                return cart->rom[(cart->banks[bankNo] << 14) | (addr & 0x3FFF)];
            }
            return 0;

        case CART_7800_S4R:
            /* SuperGame S4 + RAM: 8KB RAM at $6000-$7FFF */
            if (addr >= 0x4000) {
                if (cart->ram && addr >= 0x6000 && addr < 0x8000) {
                    return cart->ram[addr & 0x1FFF];
                }
                {
                    int bankNo = addr >> 14;
                    return cart->rom[(cart->banks[bankNo] << 14) | (addr & 0x3FFF)];
                }
            }
            return 0;

        case CART_SB: {
            /* SB Superbanking: simple 4K bank at $1000-$1FFF */
            /* Bank switching happens in mem_read_2600/mem_write_2600 via $0800 hotspot */
            int sb_offset = (cart->bank * 0x1000) + (addr & 0x0FFF);
            if (sb_offset < cart->rom_size) {
                return cart->rom[sb_offset];
            }
            return 0;
        }

        default:
            return cart->rom[addr & (cart->rom_size - 1)];
    }
}

/* Write to cart (for bankswitching) */
void cart_write(Cart *cart, uint16_t addr, uint8_t data)
{
    uint16_t masked;

    switch (cart->type) {
        case CART_A8K:
            masked = addr & 0x0FFF;
            /* Super Chip RAM: write port $x000-$x07F */
            if (cart->has_sc_ram && masked < 0x0080) {
                cart->sc_ram[masked & 0x7F] = data;
                return;
            }
            if (masked == 0x0FF8) {
                cart->bank = 0;
                if (g_bank_log_count < BANK_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK WRITE: F8 addr=$%04X data=0x%02X -> bank=0", addr, data);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            } else if (masked == 0x0FF9) {
                cart->bank = 1;
                if (g_bank_log_count < BANK_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK WRITE: F8 addr=$%04X data=0x%02X -> bank=1", addr, data);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            break;

        case CART_A16K:
            masked = addr & 0x0FFF;
            /* Super Chip RAM: write port $x000-$x07F */
            if (cart->has_sc_ram && masked < 0x0080) {
                if (g_sc_write_log_count < SC_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "SC_WRITE: clk=%llu addr=$%04X ram[%02X]=$%02X bank=%d",
                             (unsigned long long)machine_get_cpu_clock(), addr, masked & 0x7F, data, cart->bank);
                    log_msg(msg);
                    g_sc_write_log_count++;
                }
                cart->sc_ram[masked & 0x7F] = data;
                return;
            }
            if (masked >= 0x0FF6 && masked <= 0x0FF9) {
                int old_bank = cart->bank;
                cart->bank = masked - 0x0FF6;
                if (g_bank_log_count < BANK_LOG_MAX && cart->bank != old_bank) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK WRITE: F6 addr=$%04X data=0x%02X -> bank=%d (was %d)",
                             addr, data, cart->bank, old_bank);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            break;

        case CART_A32K:
            masked = addr & 0x0FFF;
            /* Super Chip RAM: write port $x000-$x07F */
            if (cart->has_sc_ram && masked < 0x0080) {
                if (g_sc_write_log_count < SC_LOG_MAX) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "SC_WRITE: clk=%llu addr=$%04X ram[%02X]=$%02X bank=%d",
                             (unsigned long long)machine_get_cpu_clock(), addr, masked & 0x7F, data, cart->bank);
                    log_msg(msg);
                    g_sc_write_log_count++;
                }
                cart->sc_ram[masked & 0x7F] = data;
                return;
            }
            if (masked >= 0x0FF4 && masked <= 0x0FFB) {
                int old_bank = cart->bank;
                cart->bank = masked - 0x0FF4;
                if (g_bank_log_count < BANK_LOG_MAX && cart->bank != old_bank) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "BANK WRITE: F4 addr=$%04X data=0x%02X -> bank=%d (was %d)",
                             addr, data, cart->bank, old_bank);
                    log_msg(msg);
                    g_bank_log_count++;
                }
            }
            break;

        case CART_DC8K:
            /* FE: no writable state */
            break;

        case CART_PB8K: {
            uint16_t pb_masked = addr & 0x0FFF;
            if (pb_masked >= 0x0FE0 && pb_masked < 0x0FE8)
                cart->banks[0] = pb_masked & 7;
            else if (pb_masked >= 0x0FE8 && pb_masked < 0x0FF0)
                cart->banks[1] = pb_masked & 7;
            else if (pb_masked >= 0x0FF0 && pb_masked < 0x0FF8)
                cart->banks[2] = pb_masked & 7;
            break;
        }

        case CART_CBS12K: {
            uint16_t cbs_masked = addr & 0x0FFF;
            /* RAM write port: $1000-$10FF */
            if (cbs_masked < 0x0100) {
                cart->ram[cbs_masked & 0xFF] = data;
                return;
            }
            /* Hotspots for bank selection */
            if (cbs_masked >= 0x0FF8 && cbs_masked <= 0x0FFA) {
                cart->bank = cbs_masked - 0x0FF8;
            }
            break;
        }

        case CART_DPC: {
            uint16_t dpc_masked = addr & 0x0FFF;
            /* DPC register writes: $1040-$107F */
            if (dpc_masked >= 0x0040 && dpc_masked < 0x0080) {
                dpc_write_register(cart->dpc, dpc_masked, data);
            } else {
                /* Hotspots for bank selection */
                if (dpc_masked >= 0x0FF8 && dpc_masked <= 0x0FF9) {
                    cart->bank = dpc_masked - 0x0FF8;
                }
            }
            break;
        }

        case CART_AR:
        {
            SCState *sc = cart->sc;
            if (!sc) break;

            /* Cancel pending write if >5 distinct accesses */
            if (sc->write_pending &&
                (machine_get_distinct_accesses() > sc->distinct_set + 5)) {
                sc->write_pending = 0;
            }

            /* Data hold register: access to $F0xx */
            if (!(addr & 0x0F00) && (!sc->write_enabled || !sc->write_pending)) {
                sc->data_hold = (uint8_t)addr;
                sc->distinct_set = machine_get_distinct_accesses();
                sc->write_pending = 1;
            }
            /* Bank config hotspot: $FFF8 */
            else if ((addr & 0x1FFF) == 0x1FF8) {
                sc->write_pending = 0;
                sc_bank_configuration(sc, sc->data_hold);
            }
            /* RAM write: enabled, pending, exactly 5 distinct accesses */
            else if (sc->write_enabled && sc->write_pending &&
                     (machine_get_distinct_accesses() == sc->distinct_set + 5)) {
                if ((addr & 0x0800) == 0) {
                    sc->image[(addr & 0x07FF) + sc->image_offset[0]] = sc->data_hold;
                    if (sc_write_log_count < 20) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "SC WRITE(w): data=0x%02X -> img[%u] (lo seg, addr=$%04X)",
                                 sc->data_hold, (unsigned)((addr & 0x07FF) + sc->image_offset[0]), addr);
                        log_msg(msg);
                        sc_write_log_count++;
                    }
                } else if (sc->image_offset[1] != (3 * SC_BANK_SIZE)) {
                    sc->image[(addr & 0x07FF) + sc->image_offset[1]] = sc->data_hold;
                    if (sc_write_log_count < 20) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "SC WRITE(w): data=0x%02X -> img[%u] (hi seg, addr=$%04X)",
                                 sc->data_hold, (unsigned)((addr & 0x07FF) + sc->image_offset[1]), addr);
                        log_msg(msg);
                        sc_write_log_count++;
                    }
                }
                sc->write_pending = 0;
            }
            break;
        }

        case CART_7800_SG:
            /* SuperGame: write to $8000-$BFFF selects bank (0-7) */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[2] = data & 7;
            }
            break;

        case CART_7800_S9:
            /*
             * S9: write to $8000-$BFFF selects bank 1-8
             * Reference Cart78S9.cs: Bank[2] = (value & 7) + 1
             */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[2] = (data & 7) + 1;
            }
            break;

        case CART_7800_SGR:
            /* SuperGame + RAM: bank select at $8000, RAM write at $4000 */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[2] = data & 7;
            } else if (addr >= 0x4000 && addr < 0x8000 && cart->ram) {
                cart->ram[addr & 0x3FFF] = data;
            }
            break;

        case CART_7800_AB:
            /*
             * Absolute: write to $8000-$BFFF selects bank at $4000
             * Reference Cart78AB.cs: Bank[1] = (value - 1) & 1
             */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[1] = (data - 1) & 1;
            }
            break;

        case CART_7800_AC:
            /*
             * Activision: write to $FF80-$FF8F selects switchable banks
             * Reference Cart78AC.cs: Bank[5] = (addr & 7) << 1; Bank[6] = Bank[5] + 1
             */
            if ((addr & 0xFFF0) == 0xFF80) {
                cart->banks[5] = (addr & 7) << 1;
                cart->banks[6] = cart->banks[5] + 1;
            }
            break;

        case CART_7800_S4:
            /* S4: write to $8000-$BFFF selects bank (0-3) */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[2] = data & 3;
            }
            break;

        case CART_7800_S4R:
            /* S4R: bank select at $8000, RAM write at $6000-$7FFF */
            if (addr >= 0x8000 && addr < 0xC000) {
                cart->banks[2] = data & 3;
            } else if (addr >= 0x6000 && addr < 0x8000 && cart->ram) {
                cart->ram[addr & 0x1FFF] = data;
            }
            break;

        case CART_SB:
            /* SB: bankswitching via $0800 hotspot (handled in machine.c), no ROM writes */
            break;

        default:
            break;
    }
}

/* --- Supercharger internal functions --- */

/* Compute 8-bit checksum of data block */
static uint8_t sc_checksum(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

/*
 * Set Supercharger bank configuration
 *
 * D4-D2: Bank config (which RAM/ROM banks map to $F000/$F800)
 * D1:    Write enable (1 = writes to $F000-$F0FF go to RAM)
 * D0:    ROM power (0 = powered, 1 = off)
 */
static void sc_bank_configuration(SCState *sc, uint8_t config)
{
    static const uint32_t OFFSET_0[8] = {
        2 * SC_BANK_SIZE, 0 * SC_BANK_SIZE, 2 * SC_BANK_SIZE, 0 * SC_BANK_SIZE,
        2 * SC_BANK_SIZE, 1 * SC_BANK_SIZE, 2 * SC_BANK_SIZE, 1 * SC_BANK_SIZE
    };
    static const uint32_t OFFSET_1[8] = {
        3 * SC_BANK_SIZE, 3 * SC_BANK_SIZE, 0 * SC_BANK_SIZE, 2 * SC_BANK_SIZE,
        3 * SC_BANK_SIZE, 3 * SC_BANK_SIZE, 1 * SC_BANK_SIZE, 2 * SC_BANK_SIZE
    };
    int bank_config = (config >> 2) & 7;
    uint32_t prev_off1 = sc->image_offset[1];
    char msg[256];

    sc->power = !(config & 1);
    sc->write_enabled = (config >> 1) & 1;
    sc->image_offset[0] = OFFSET_0[bank_config];
    sc->image_offset[1] = OFFSET_1[bank_config];

    snprintf(msg, sizeof(msg), "SC bankconfig: cfg=0x%02X banksel=%d wr=%d pwr=%d off0=%u off1=%u",
             config, bank_config, sc->write_enabled, sc->power,
             (unsigned)sc->image_offset[0], (unsigned)sc->image_offset[1]);
    log_msg(msg);

    /* Detect game start: ROM (offset 6144) → non-ROM in upper segment */
    if (prev_off1 == SC_RAM_SIZE && sc->image_offset[1] != SC_RAM_SIZE) {
        uint8_t entry_lo = machine_peek_ram(0x00FE);
        uint8_t entry_hi = machine_peek_ram(0x00FF);
        uint16_t entry = (uint16_t)(entry_lo | (entry_hi << 8));
        uint32_t seg = (entry & 0x0800) ? 1 : 0;
        uint32_t img_off = (entry & 0x07FF) + sc->image_offset[seg];

        snprintf(msg, sizeof(msg), "SC GAME START: entry=$%04X (seg%d img[%u]) PIA[$FA..FF]=%02X %02X %02X %02X %02X %02X",
                 entry, (int)seg, (unsigned)img_off,
                 machine_peek_ram(0x00FA), machine_peek_ram(0x00FB),
                 machine_peek_ram(0x00FC), machine_peek_ram(0x00FD),
                 entry_lo, entry_hi);
        log_msg(msg);

        /* Log first 16 bytes at entry point */
        if (img_off + 16 <= sizeof(sc->image)) {
            snprintf(msg, sizeof(msg), "SC GAME CODE @$%04X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     entry,
                     sc->image[img_off+0], sc->image[img_off+1], sc->image[img_off+2], sc->image[img_off+3],
                     sc->image[img_off+4], sc->image[img_off+5], sc->image[img_off+6], sc->image[img_off+7],
                     sc->image[img_off+8], sc->image[img_off+9], sc->image[img_off+10], sc->image[img_off+11],
                     sc->image[img_off+12], sc->image[img_off+13], sc->image[img_off+14], sc->image[img_off+15]);
            log_msg(msg);
        }

        /* Log reset/IRQ vectors in new mapping */
        {
            uint32_t vec_off0 = 0x07FC + sc->image_offset[1];
            snprintf(msg, sizeof(msg), "SC GAME VECTORS: $FFFC=%02X%02X $FFFE=%02X%02X (upper seg bank @ off%u)",
                     sc->image[vec_off0+1], sc->image[vec_off0],
                     sc->image[vec_off0+3], sc->image[vec_off0+2],
                     (unsigned)sc->image_offset[1]);
            log_msg(msg);
        }

        /* Dump PIA RAM trampoline area ($F0-$FF) */
        {
            snprintf(msg, sizeof(msg), "SC TRAMPOLINE PIA[$F0..$FF]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     machine_peek_ram(0x00F0), machine_peek_ram(0x00F1),
                     machine_peek_ram(0x00F2), machine_peek_ram(0x00F3),
                     machine_peek_ram(0x00F4), machine_peek_ram(0x00F5),
                     machine_peek_ram(0x00F6), machine_peek_ram(0x00F7),
                     machine_peek_ram(0x00F8), machine_peek_ram(0x00F9),
                     machine_peek_ram(0x00FA), machine_peek_ram(0x00FB),
                     machine_peek_ram(0x00FC), machine_peek_ram(0x00FD),
                     machine_peek_ram(0x00FE), machine_peek_ram(0x00FF));
            log_msg(msg);
        }

        sc_trace_count = 30;
        sc_write_log_count = 0;
    }
}

/*
 * Initialize the dummy Supercharger BIOS ROM
 * Fills image[6144..8191] with BIOS code and vectors
 */
static void sc_initialize_rom(SCState *sc)
{
    char msg[256];

    /* Fill ROM area with JAM opcode (0x02) */
    memset(sc->image + SC_RAM_SIZE, 0x02, SC_BANK_SIZE);

    /* Fast BIOS: skip progress bars (offset 109 = 0xFF) */
    sc_dummy_rom_code[109] = 0xFF;

    /* Random accumulator value (offset 281) - use simple pseudo-random */
    sc_dummy_rom_code[281] = (uint8_t)(sc_dummy_rom_code[281] + 0x9A);

    /* Copy dummy BIOS code */
    memcpy(sc->image + SC_RAM_SIZE, sc_dummy_rom_code, sizeof(sc_dummy_rom_code));

    /* Set reset/IRQ vectors to point to $F80A (initial load code) */
    sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 4] = 0x0A;  /* Reset vector low */
    sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 3] = 0xF8;  /* Reset vector high */
    sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 2] = 0x0A;  /* IRQ vector low */
    sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 1] = 0xF8;  /* IRQ vector high */

    /* Log the reset vector and first bytes of BIOS for verification */
    snprintf(msg, sizeof(msg), "SC initROM: BIOS[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X",
             sc->image[SC_RAM_SIZE+0], sc->image[SC_RAM_SIZE+1],
             sc->image[SC_RAM_SIZE+2], sc->image[SC_RAM_SIZE+3],
             sc->image[SC_RAM_SIZE+4], sc->image[SC_RAM_SIZE+5],
             sc->image[SC_RAM_SIZE+6], sc->image[SC_RAM_SIZE+7]);
    log_msg(msg);
    snprintf(msg, sizeof(msg), "SC initROM: vectors FFFC=%02X%02X FFFE=%02X%02X (at img[%d..%d])",
             sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 3],
             sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 4],
             sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 1],
             sc->image[SC_RAM_SIZE + SC_BANK_SIZE - 2],
             SC_RAM_SIZE + SC_BANK_SIZE - 4,
             SC_RAM_SIZE + SC_BANK_SIZE - 1);
    log_msg(msg);
    /* Log the code at $F80A (offset 10 in BIOS) - this is where reset jumps */
    snprintf(msg, sizeof(msg), "SC initROM: code@$F80A (off 10)=%02X %02X %02X %02X %02X %02X",
             sc->image[SC_RAM_SIZE+10], sc->image[SC_RAM_SIZE+11],
             sc->image[SC_RAM_SIZE+12], sc->image[SC_RAM_SIZE+13],
             sc->image[SC_RAM_SIZE+14], sc->image[SC_RAM_SIZE+15]);
    log_msg(msg);
    /* Log fast BIOS flag and fast path disassembly */
    snprintf(msg, sizeof(msg), "SC initROM: fastBIOS @106..114: %02X %02X %02X %02X %02X %02X %02X %02X %02X (LDA CMP BNE JMP)",
             sc->image[SC_RAM_SIZE+106], sc->image[SC_RAM_SIZE+107],
             sc->image[SC_RAM_SIZE+108], sc->image[SC_RAM_SIZE+109],
             sc->image[SC_RAM_SIZE+110], sc->image[SC_RAM_SIZE+111],
             sc->image[SC_RAM_SIZE+112], sc->image[SC_RAM_SIZE+113],
             sc->image[SC_RAM_SIZE+114]);
    log_msg(msg);
    /* Log trampoline source bytes at offset 290-293 */
    snprintf(msg, sizeof(msg), "SC initROM: trampoline src @290..293: %02X %02X %02X %02X (CMP $FFF8; JMP)",
             sc->image[SC_RAM_SIZE+290], sc->image[SC_RAM_SIZE+291],
             sc->image[SC_RAM_SIZE+292], sc->image[SC_RAM_SIZE+293]);
    log_msg(msg);
    /* Log the trampoline setup code at offset 80 */
    snprintf(msg, sizeof(msg), "SC initROM: trampoline copy @80: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             sc->image[SC_RAM_SIZE+80], sc->image[SC_RAM_SIZE+81],
             sc->image[SC_RAM_SIZE+82], sc->image[SC_RAM_SIZE+83],
             sc->image[SC_RAM_SIZE+84], sc->image[SC_RAM_SIZE+85],
             sc->image[SC_RAM_SIZE+86], sc->image[SC_RAM_SIZE+87],
             sc->image[SC_RAM_SIZE+88]);
    log_msg(msg);
    /* Log the jump-to-trampoline code at offset 275-289 */
    snprintf(msg, sizeof(msg), "SC initROM: exit @275: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             sc->image[SC_RAM_SIZE+275], sc->image[SC_RAM_SIZE+276],
             sc->image[SC_RAM_SIZE+277], sc->image[SC_RAM_SIZE+278],
             sc->image[SC_RAM_SIZE+279], sc->image[SC_RAM_SIZE+280],
             sc->image[SC_RAM_SIZE+281], sc->image[SC_RAM_SIZE+282],
             sc->image[SC_RAM_SIZE+283], sc->image[SC_RAM_SIZE+284],
             sc->image[SC_RAM_SIZE+285], sc->image[SC_RAM_SIZE+286],
             sc->image[SC_RAM_SIZE+287], sc->image[SC_RAM_SIZE+288],
             sc->image[SC_RAM_SIZE+289]);
    log_msg(msg);
}

/*
 * Load a Supercharger tape image into RAM
 * Scans all load images for the matching load number, then copies page data
 */
static void sc_load_into_ram(Cart *cart, uint8_t load)
{
    SCState *sc = cart->sc;
    uint16_t image_idx;
    char msg[256];

    snprintf(msg, sizeof(msg), "SC loadIntoRAM: searching for load #%d in %d images", load, sc->num_loads);
    log_msg(msg);

    for (image_idx = 0; image_idx < sc->num_loads; image_idx++) {
        size_t image_off = (size_t)image_idx * SC_LOAD_SIZE;
        uint8_t found_load = sc->loads[image_off + 8192 + 5];

        /* Check if this is the correct load (load number at header offset 5) */
        if (found_load == load) {
            size_t j;
            uint8_t header_sum;

            /* Copy 256-byte header */
            memcpy(sc->header, sc->loads + image_off + 8192, 256);

            snprintf(msg, sizeof(msg), "SC load #%d: hdr[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X",
                     load, sc->header[0], sc->header[1], sc->header[2], sc->header[3],
                     sc->header[4], sc->header[5], sc->header[6], sc->header[7]);
            log_msg(msg);
            snprintf(msg, sizeof(msg), "SC load #%d: config=0x%02X startHi=0x%02X flags=0x%02X pages=%d",
                     load, sc->header[0], sc->header[1], sc->header[2], sc->header[3]);
            log_msg(msg);

            /* Verify header checksum (sum of first 8 bytes should be 0x55) */
            header_sum = sc_checksum(sc->header, 8);
            if (header_sum != 0x55) {
                snprintf(msg, sizeof(msg), "SC: load #%d header checksum error (got 0x%02X, want 0x55)", load, header_sum);
                log_msg(msg);
            }

            /* Load pages into RAM */
            for (j = 0; j < sc->header[3]; j++) {
                size_t bank = sc->header[16 + j] & 0x03;
                size_t page = (sc->header[16 + j] >> 2) & 0x07;
                const uint8_t *src = sc->loads + image_off + j * 256;
                size_t dest_off = bank * SC_BANK_SIZE + page * 256;

                snprintf(msg, sizeof(msg), "SC load #%d: pg%02d: desc=0x%02X b%d p%d -> img[%d] %02X%02X%02X%02X",
                         load, (int)j, sc->header[16+j], (int)bank, (int)page,
                         (int)dest_off, src[0], src[1], src[2], src[3]);
                log_msg(msg);

                /* Copy page to Supercharger RAM (don't write into ROM area) */
                if (bank < 3) {
                    memcpy(sc->image + dest_off, src, 256);
                }
            }

            /* Store config bytes in 2600 RAM for the dummy BIOS to use */
            machine_poke_ram(0xFE, sc->header[0]);
            machine_poke_ram(0xFF, sc->header[1]);
            machine_poke_ram(0x80, sc->header[2]);

            snprintf(msg, sizeof(msg), "SC: load #%d done (%d pages), RAM[$FE]=0x%02X RAM[$FF]=0x%02X RAM[$80]=0x%02X",
                     load, sc->header[3], sc->header[0], sc->header[1], sc->header[2]);
            log_msg(msg);

            /* Log what's at the expected game entry after bank config */
            {
                uint8_t cfg = sc->header[2];
                int bsel = (cfg >> 2) & 7;
                static const uint32_t OFF0[8] = {4096, 0, 4096, 0, 4096, 2048, 4096, 2048};
                uint32_t entry_seg_off = OFF0[bsel];
                uint16_t entry_addr = (uint16_t)(sc->header[0] | (sc->header[1] << 8));
                uint32_t entry_img = (entry_addr & 0x07FF) + entry_seg_off;
                snprintf(msg, sizeof(msg), "SC: predicted entry=$%04X cfg=0x%02X bsel=%d off0=%u -> img[%u]: %02X %02X %02X %02X %02X %02X %02X %02X",
                         entry_addr, cfg, bsel, (unsigned)entry_seg_off, (unsigned)entry_img,
                         sc->image[entry_img+0], sc->image[entry_img+1], sc->image[entry_img+2], sc->image[entry_img+3],
                         sc->image[entry_img+4], sc->image[entry_img+5], sc->image[entry_img+6], sc->image[entry_img+7]);
                log_msg(msg);
            }

            return;
        } else {
            snprintf(msg, sizeof(msg), "SC loadIntoRAM: image %d has load#=%d (not %d)", image_idx, found_load, load);
            log_msg(msg);
        }
    }

    snprintf(msg, sizeof(msg), "SC: load #%d not found in ROM image!", load);
    log_msg(msg);
}

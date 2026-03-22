/*
 * cart.h
 *
 * Cartridge Mapper Header
 *
 * Copyright (c) 2003-2011 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#ifndef CART_H
#define CART_H

#include <stdint.h>

/* Controller types (from A78 header bytes 0x37/0x38) */
#define CTRL_NONE              0
#define CTRL_PROLINE_JOYSTICK  1
#define CTRL_LIGHTGUN          2

/* Cart types */
typedef enum {
    CART_UNKNOWN = 0,
    CART_A2K,       /* 2K standard */
    CART_A4K,       /* 4K standard */
    CART_A8K,       /* 8K bankswitched (F8) */
    CART_A16K,      /* 16K bankswitched (F6) */
    CART_A32K,      /* 32K bankswitched (F4) */
    CART_DC8K,      /* Activision FE bankswitching (Decathlon, Robot Tank) */
    CART_PB8K,      /* Parker Brothers E0 bankswitching (Frogger II, etc.) */
    CART_AR,        /* Starpath Supercharger (6KB RAM + 2KB ROM) */
    CART_7800_8K,   /* 7800 8K */
    CART_7800_16K,  /* 7800 16K */
    CART_7800_32K,  /* 7800 32K */
    CART_7800_48K,  /* 7800 48K */
    CART_7800_SG,   /* SuperGame bankswitched (128KB, 8x16K banks) */
    CART_7800_SGR,  /* SuperGame + RAM at $4000 */
    CART_7800_S9,   /* SuperGame 9-bank (144KB, 9x16K banks) */
    CART_7800_AB,   /* Absolute (F-18 Hornet) - 64KB, 4x16K banks */
    CART_7800_AC,   /* Activision (Double Dragon, Rampage) - 128KB, 16x8K banks */
    CART_CBS12K,    /* CBS RAM Plus 12K (FA) - 3x4K banks + 256B RAM */
    CART_DPC,       /* Pitfall II DPC - 2x4K banks + 2K display + DPC chip */
    CART_7800_S4,   /* SuperGame 4-bank - 64KB, 4x16K banks */
    CART_7800_S4R,  /* SuperGame 4-bank + 8K RAM at $6000 */
    CART_SB,        /* SB Superbanking - 128/256KB, 32/64 x 4K banks, hotspot $0800 */
} CartType;

/* Starpath Supercharger state */
typedef struct {
    uint8_t image[8192];        /* 6KB RAM (banks 0-2) + 2KB ROM (bank 3) */
    uint8_t *loads;             /* All 8448-byte load images (malloc'd) */
    uint8_t header[256];        /* Current load's 256-byte header */
    uint32_t image_offset[2];   /* Byte offsets into image[] for $F000/$F800 segments */
    uint8_t num_loads;          /* Number of 8448-byte loads */
    uint8_t write_enabled;      /* RAM write enable (config bit D1) */
    uint8_t power;              /* ROM power on (config bit D0, inverted) */
    uint8_t data_hold;          /* Data hold register (low byte of $F0xx access) */
    uint32_t distinct_set;      /* Distinct accesses count when hold was set */
    uint8_t write_pending;      /* Write is pending */
} SCState;

/* DPC (Display Processor Chip) state - Pitfall II */
typedef struct {
    uint16_t counters[8];   /* 11-bit counters (0x07FF mask) */
    uint8_t tops[8];        /* Top counter values */
    uint8_t bots[8];        /* Bottom counter values */
    uint8_t flags[8];       /* Flag registers (0x00 or 0xFF) */
    uint8_t music_mode[3];  /* Music mode enable for fetchers 5-7 */
    uint8_t shift_register; /* 8-bit LFSR for random numbers */
    uint64_t last_system_clock;  /* Last CPU clock for music timing */
    double fractional_clocks;    /* Fractional DPC oscillator clocks */
} DPCState;

/* Cart state structure */
typedef struct {
    CartType type;
    uint8_t *rom;
    uint8_t *ram;       /* Extra RAM (SuperGame RAM at $4000) */
    int rom_size;
    int ram_size;
    int bank;
    int bank_count;
    int banks[8];       /* Bank mapping (SG: 4 x 16KB, AC: 8 x 8KB) */
    SCState *sc;        /* Supercharger state (NULL for non-SC carts) */
    DPCState *dpc;      /* DPC chip state (NULL for non-DPC carts) */
    uint8_t sc_ram[128];   /* Super Chip RAM (128 bytes, used when has_sc_ram=1) */
    int has_sc_ram;        /* Non-zero if cart has Super Chip (F8SC/F6SC/F4SC) */
    int left_controller;   /* Controller type for left jack */
    int right_controller;  /* Controller type for right jack */
} Cart;

/* Initialize cart */
void cart_init(Cart *cart);

/* Load ROM into cart */
int cart_load(Cart *cart, const uint8_t *data, int size, int machine_type);

/* Free cart memory */
void cart_free(Cart *cart);

/* Read from cart */
uint8_t cart_read(Cart *cart, uint16_t addr);

/* Write to cart (for bankswitching) */
void cart_write(Cart *cart, uint16_t addr, uint8_t data);

/* Reset cart */
void cart_reset(Cart *cart);

/* Detect cart type from ROM data and size */
CartType cart_detect_type(const uint8_t *rom, int size, int machine_type);

#endif /* CART_H */

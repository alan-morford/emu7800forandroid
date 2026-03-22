/*
 * savestate.h
 *
 * Save/Load State Header
 * Binary save/load of full emulation state.
 *
 * Copyright (c) 2026 EMU7800
 */

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdint.h>

/* Maria internal state (statics in maria.c) */
typedef struct {
    uint8_t line_ram[0x200];
    uint8_t registers[0x40];
    uint16_t dll;
    uint16_t dl;
    int offset;
    int holey;
    int width;
    uint8_t hpos;
    int palette_no;
    int ind_mode;
    int wm;
    int dma_enabled;
    int color_kill;
    int cwidth;
    int bcntl;
    int kangaroo;
    uint8_t rm;
    int ctrl_lock;
    int scanline;
    int dli;
    int dli_pending;
    int dma_clocks;
    int maria_frame_count;
} MariaInternalState;

/* TIA sound internal state (statics in tiasound.c) */
typedef struct {
    uint8_t audc[2];
    uint8_t audf[2];
    uint8_t audv[2];
    int p4[2];
    int p5[2];
    int p9[2];
    int div_n_counter[2];
    int div_n_maximum[2];
    uint8_t output_vol[2];
    uint32_t phase_accum;
    int buffer_index;
} TiaSoundState;

/* Check if a save file exists for the given ROM.
 * Returns 1 if exists, 0 if not. */
int savestate_exists(const char *rom_path);

/* Save emulation state to file.
 * rom_path: path to the currently loaded ROM (used to derive .sav path).
 * Returns 0 on success, negative on error. */
int savestate_save(const char *rom_path);

/* Load emulation state from file.
 * rom_path: path to the currently loaded ROM (used to derive .sav path).
 * Returns 0 on success, negative on error. */
int savestate_load(const char *rom_path);

#endif /* SAVESTATE_H */

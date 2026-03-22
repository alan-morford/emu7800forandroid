/*
 * pokeysound.h
 *
 * POKEY Sound Chip Header (7800)
 * Stub implementation for future 7800 support
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef POKEYSOUND_H
#define POKEYSOUND_H

#include <stdint.h>

/* Initialize POKEY sound */
void pokeysound_init(int sample_rate);

/* Reset POKEY sound */
void pokeysound_reset(void);

/* Update POKEY sound register */
void pokeysound_update(uint16_t addr, uint8_t data);

/* Render audio samples */
void pokeysound_render(int16_t *buffer, int num_samples);

#endif /* POKEYSOUND_H */

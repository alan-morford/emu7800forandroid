/*
 * pokeysound.c
 *
 * POKEY Sound Chip (7800)
 * Stub implementation for future 7800 support
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include "pokeysound.h"

/*
 * Note: This is a stub implementation.
 * Full POKEY emulation requires implementing the
 * polynomial counters and audio generation.
 *
 * For now, 7800 games will have no POKEY sound.
 */

static int g_sample_rate = 44100;

/* Initialize POKEY sound */
void pokeysound_init(int sample_rate)
{
    g_sample_rate = sample_rate;
    pokeysound_reset();
}

/* Reset POKEY sound */
void pokeysound_reset(void)
{
    /* TODO: Reset POKEY state */
}

/* Update POKEY sound register */
void pokeysound_update(uint16_t addr, uint8_t data)
{
    (void)addr;
    (void)data;
    /* TODO: Implement POKEY register writes */
}

/* Render audio samples */
void pokeysound_render(int16_t *buffer, int num_samples)
{
    /* Output silence for now */
    memset(buffer, 0, num_samples * sizeof(int16_t));
}

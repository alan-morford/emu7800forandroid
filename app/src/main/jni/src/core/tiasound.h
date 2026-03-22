/*
 * tiasound.h
 *
 * TIA Sound Emulator Header
 * Based on TIASound by Ron Fries
 *
 * Copyright (c) 1997 Ron Fries
 * C port Copyright (c) 2024 EMU7800
 */

#ifndef TIASOUND_H
#define TIASOUND_H

#include <stdint.h>

/* Initialize TIA sound */
void tiasound_init(int sample_rate);

/* Reset TIA sound */
void tiasound_reset(void);

/* Update TIA sound register */
void tiasound_update(uint16_t addr, uint8_t data);

/* Start a new frame */
void tiasound_start_frame(void);

/* End the current frame */
void tiasound_end_frame(void);

/* Render audio samples up to a given position within the frame.
 * Call BEFORE applying a register change so the old values are rendered
 * up to the correct point. Position is in TIA clocks since frame start. */
void tiasound_render_to_position(int tia_clocks_into_frame);

/* Render audio samples */
void tiasound_render(int16_t *buffer, int num_samples);

/* Get the sound buffer */
int16_t *tiasound_get_buffer(void);

/* Get the number of samples in the buffer */
int tiasound_get_buffer_samples(void);

/* Save state: get/set internal state */
#include "savestate.h"
void tiasound_get_state(TiaSoundState *s);
void tiasound_set_state(const TiaSoundState *s);

#endif /* TIASOUND_H */

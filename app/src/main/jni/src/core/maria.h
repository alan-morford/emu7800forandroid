/*
 * maria.h
 *
 * Maria Graphics Chip Header (Atari 7800)
 * Based on EMU7800 by Mike Murphy
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef MARIA_H
#define MARIA_H

#include <stdint.h>

/* Maria state structure */
typedef struct {
    int scanline;       /* Current scanline */
    int wsync;          /* WSYNC halt flag */
} Maria;

/* Initialize Maria */
void maria_init(Maria *maria);

/* Reset Maria */
void maria_reset(Maria *maria);

/* Read from Maria register */
uint8_t maria_read(Maria *maria, uint16_t addr);

/* Write to Maria register */
void maria_write(Maria *maria, uint16_t addr, uint8_t data);

/* Start a new frame */
void maria_start_frame(Maria *maria);

/* End the current frame */
void maria_end_frame(Maria *maria);

/* Process DMA for current scanline - returns DMA clock cycles used */
int maria_do_dma(Maria *maria);

/* Get Maria frame buffer (320 pixels wide) */
uint8_t *maria_get_frame_buffer(void);

/* Check if frame is ready for display */
int maria_frame_ready(void);

/* Signal that frame has been consumed */
void maria_frame_consumed(void);

/* Set DMA read callback - allows Maria to read from system memory */
void maria_set_dma_read(uint8_t (*read_func)(uint16_t));

/* Set input callbacks for button reading */
void maria_set_input_callbacks(int (*trigger)(int), int (*trigger2)(int));

/* Set CPU preempt callback for WSYNC */
void maria_set_cpu_preempt_callback(void (*preempt_func)(void));

/* Set NMI callback for DLI */
void maria_set_nmi_callback(void (*nmi_func)(void));

/* Set PIA Port B gate callback for ProLine two-button INPT4/5 masking */
void maria_set_portb_gate_callback(int (*gate_func)(int playerNo));

/* Enable diagnostic logging for N frames */
void maria_enable_diagnostics(int frames);

/* 7800 NTSC palette */
extern const uint32_t maria_ntsc_palette[256];

/* Palette selection */
#define MARIA_PALETTE_COOL  0
#define MARIA_PALETTE_WARM  1
#define MARIA_PALETTE_HOT   2
#define MARIA_PALETTE_COUNT 3
const uint32_t *maria_get_palette(int index);

/* Save state: get/set internal statics */
#include "savestate.h"
void maria_get_internal_state(MariaInternalState *s);
void maria_set_internal_state(const MariaInternalState *s);

#endif /* MARIA_H */

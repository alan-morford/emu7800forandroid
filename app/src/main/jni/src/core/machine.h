/*
 * machine.h
 *
 * Machine Integration Header
 * Ties together CPU, TIA, PIA, and Cart
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>

/* Machine types */
#define MACHINE_2600  0
#define MACHINE_7800  1

/* Initialize the machine */
void machine_init(void);

/* Shutdown the machine */
void machine_shutdown(void);

/* Load a ROM file */
int machine_load_rom(const char *path, int machine_type);

/* Check if a ROM is loaded */
int machine_is_loaded(void);

/* Get current machine type */
int machine_get_type(void);

/* Reset the machine */
void machine_reset(void);

/* Run one frame */
void machine_run_frame(void);

/* Get frame buffer (TIA indexed color values) */
uint8_t *machine_get_frame_buffer(void);

/* Get frame dimensions */
int machine_get_frame_width(void);
int machine_get_frame_height(void);

/* Get sound buffer */
int16_t *machine_get_sound_buffer(void);
int machine_get_sound_samples(void);

/* Input sampling — all return frame-sampled copies, stable within one frame */
void machine_sample_input(void);  /* Snapshot all input at frame start */
int machine_sample_joystick(int player, int direction);  /* 0=up,1=down,2=left,3=right */
int machine_sample_trigger(int player);   /* Primary fire button */
int machine_sample_trigger2(int player);  /* Secondary fire button (7800) */
int machine_sample_switch(int sw);  /* 0=reset,1=select,2=ldiff,3=rdiff */

/* Set input state */
void machine_set_joystick(int player, int direction, int pressed);
void machine_set_trigger(int player, int pressed);   /* Primary fire button */
void machine_set_trigger2(int player, int pressed);  /* Secondary fire button (7800) */
void machine_set_switch(int sw, int pressed);

/* Clear all input */
void machine_clear_input(void);

/* Controller type accessors (from A78 header) */
int machine_get_left_controller(void);
int machine_get_right_controller(void);

/* Supercharger support: distinct access tracking */
uint32_t machine_get_distinct_accesses(void);

/* Supercharger support: direct RAM peek/poke (bypasses TIA/PIA decoding) */
uint8_t machine_peek_ram(uint16_t addr);
void machine_poke_ram(uint16_t addr, uint8_t data);

/* --- Save state accessors (emulator must be paused) --- */
#include "m6502.h"
#include "tia.h"
#include "pia.h"
#include "cart.h"
#include "maria.h"

M6502 *machine_get_cpu(void);
TIA *machine_get_tia(void);
PIA *machine_get_pia(void);
Cart *machine_get_cart(void);
Maria *machine_get_maria(void);

/* RAM accessors */
uint8_t *machine_get_ram_2600(void);
uint8_t *machine_get_ram0_7800(void);
uint8_t *machine_get_ram1_7800(void);

/* Machine flags for save state */
void machine_get_state_flags(int *nmi_delay_frames, int *frame_count_7800,
                             int *frame_count_2600, uint32_t *distinct_accesses);
void machine_set_state_flags(int nmi_delay_frames, int frame_count_7800,
                             int frame_count_2600, uint32_t distinct_accesses);

/* Get current ROM path (for save state path derivation) */
const char *machine_get_rom_path(void);

#endif /* MACHINE_H */

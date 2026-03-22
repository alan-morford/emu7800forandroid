/*
 * pia.h
 *
 * Peripheral Interface Adapter (6532) Header
 * a.k.a. RIOT (RAM I/O Timer)
 *
 * Copyright (c) 2003-2012 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#ifndef PIA_H
#define PIA_H

#include <stdint.h>

/* PIA state structure */
typedef struct {
    uint8_t ram[0x80];
    uint64_t timer_target;
    int timer_shift;
    int irq_enabled;
    int irq_triggered;
    uint8_t ddra;
    uint8_t ddrb;
    uint8_t written_port_a;
    uint8_t written_port_b;
} PIA;

/* Initialize PIA */
void pia_init(PIA *pia);

/* Reset PIA */
void pia_reset(PIA *pia, uint64_t cpu_clock);

/* Read from PIA */
uint8_t pia_read(PIA *pia, uint16_t addr, uint64_t cpu_clock);

/* Write to PIA */
void pia_write(PIA *pia, uint16_t addr, uint8_t data, uint64_t cpu_clock);

#endif /* PIA_H */

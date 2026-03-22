/*
 * m6502.h
 *
 * MOS 6502 CPU Emulator Header
 * Ported from EMU7800 C# to C
 *
 * Copyright (c) 2003-2005 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#ifndef M6502_H
#define M6502_H

#include <stdint.h>

/* CPU State Structure */
typedef struct {
    /* Clock counters */
    uint64_t clock;
    int run_clocks;
    int run_clocks_multiple;

    /* Control flags */
    int emulator_preempt_request;
    int jammed;
    int irq_interrupt_request;
    int nmi_interrupt_request;

    /* Registers */
    uint16_t PC;    /* Program Counter */
    uint8_t A;      /* Accumulator */
    uint8_t X;      /* X Index Register */
    uint8_t Y;      /* Y Index Register */
    uint8_t S;      /* Stack Pointer */
    uint8_t P;      /* Processor Status */
} M6502;

/* Memory read/write callbacks */
typedef uint8_t (*mem_read_fn)(uint16_t addr);
typedef void (*mem_write_fn)(uint16_t addr, uint8_t data);

/* Initialize CPU */
void m6502_init(M6502 *cpu, int run_clocks_multiple);

/* Reset CPU */
void m6502_reset(M6502 *cpu, mem_read_fn read);

/* Execute CPU cycles */
void m6502_execute(M6502 *cpu, mem_read_fn read, mem_write_fn write);

/* Processor status flag bits */
#define FLAG_C  (1 << 0)  /* Carry */
#define FLAG_Z  (1 << 1)  /* Zero */
#define FLAG_I  (1 << 2)  /* IRQ Disable */
#define FLAG_D  (1 << 3)  /* Decimal Mode */
#define FLAG_B  (1 << 4)  /* Break */
#define FLAG_V  (1 << 6)  /* Overflow */
#define FLAG_N  (1 << 7)  /* Negative */

/* Interrupt vectors */
#define NMI_VEC  0xFFFA
#define RST_VEC  0xFFFC
#define IRQ_VEC  0xFFFE

#endif /* M6502_H */

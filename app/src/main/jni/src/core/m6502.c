/*
 * m6502.c
 *
 * MOS 6502 CPU Emulator
 * Ported from EMU7800 C# to C
 *
 * Copyright (c) 2003-2005 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdio.h>
#include "m6502.h"

/* Memory access function pointers (set during execute) */
static mem_read_fn g_read;
static mem_write_fn g_write;
static M6502 *g_cpu;

/* Helper macros */
#define MSB(u16) ((uint8_t)((u16) >> 8))
#define LSB(u16) ((uint8_t)(u16))
#define WORD(lsb, msb) ((uint16_t)((lsb) | ((msb) << 8)))

/* Flag operations */
#define SET_FLAG(f, v) do { \
    if (v) g_cpu->P |= (f); else g_cpu->P &= ~(f); \
} while(0)

#define GET_FLAG(f) ((g_cpu->P & (f)) != 0)

#define SET_NZ(val) do { \
    SET_FLAG(FLAG_N, ((val) & 0x80) != 0); \
    SET_FLAG(FLAG_Z, ((val) & 0xFF) == 0); \
} while(0)

/* Stack operations */
static inline uint8_t pull(void)
{
    g_cpu->S++;
    return g_read(0x0100 + g_cpu->S);
}

static inline void push(uint8_t data)
{
    g_write(0x0100 + g_cpu->S, data);
    g_cpu->S--;
}

/* Clock management */
static inline void clk(int ticks)
{
    g_cpu->clock += ticks;
    g_cpu->run_clocks -= ticks * g_cpu->run_clocks_multiple;
}

/* Addressing modes */
static inline uint16_t aREL(void)  /* Relative */
{
    int8_t bo = (int8_t)g_read(g_cpu->PC);
    g_cpu->PC++;
    return (uint16_t)(g_cpu->PC + bo);
}

static inline uint16_t aZPG(void)  /* Zero Page */
{
    return WORD(g_read(g_cpu->PC++), 0x00);
}

static inline uint16_t aZPX(void)  /* Zero Page,X */
{
    uint8_t base = g_read(g_cpu->PC++);
    (void)g_read(base);  /* Phantom: read base ZP before adding X (real 6502 bus cycle) */
    return WORD((uint8_t)(base + g_cpu->X), 0x00);
}

static inline uint16_t aZPY(void)  /* Zero Page,Y */
{
    uint8_t base = g_read(g_cpu->PC++);
    (void)g_read(base);  /* Phantom: read base ZP before adding Y (real 6502 bus cycle) */
    return WORD((uint8_t)(base + g_cpu->Y), 0x00);
}

static inline uint16_t aABS(void)  /* Absolute */
{
    uint8_t lsb = g_read(g_cpu->PC++);
    uint8_t msb = g_read(g_cpu->PC++);
    return WORD(lsb, msb);
}

static inline uint16_t aABX(int eclk)  /* Absolute,X - reads */
{
    uint16_t ea = aABS();
    if (LSB(ea) + g_cpu->X > 0xFF) {
        /* Phantom: read from intermediate (wrong page) address */
        (void)g_read((ea & 0xFF00) | (uint8_t)(LSB(ea) + g_cpu->X));
        clk(eclk);
    }
    return (uint16_t)(ea + g_cpu->X);
}

static inline uint16_t aABY(int eclk)  /* Absolute,Y - reads */
{
    uint16_t ea = aABS();
    if (LSB(ea) + g_cpu->Y > 0xFF) {
        /* Phantom: read from intermediate (wrong page) address */
        (void)g_read((ea & 0xFF00) | (uint8_t)(LSB(ea) + g_cpu->Y));
        clk(eclk);
    }
    return (uint16_t)(ea + g_cpu->Y);
}

static inline uint16_t aABXw(void)  /* Absolute,X - stores (always phantom) */
{
    uint16_t ea = aABS();
    /* Phantom: ALWAYS read intermediate address for stores */
    (void)g_read((ea & 0xFF00) | (uint8_t)(LSB(ea) + g_cpu->X));
    return (uint16_t)(ea + g_cpu->X);
}

static inline uint16_t aABYw(void)  /* Absolute,Y - stores (always phantom) */
{
    uint16_t ea = aABS();
    /* Phantom: ALWAYS read intermediate address for stores */
    (void)g_read((ea & 0xFF00) | (uint8_t)(LSB(ea) + g_cpu->Y));
    return (uint16_t)(ea + g_cpu->Y);
}

static inline uint16_t aIDX(void)  /* Indexed Indirect (X) */
{
    uint8_t base = g_read(g_cpu->PC++);
    (void)g_read(base);  /* Phantom: read base ZP before adding X */
    uint8_t zpa = (uint8_t)(base + g_cpu->X);
    uint8_t lsb = g_read(zpa++);
    uint8_t msb = g_read(zpa);
    return WORD(lsb, msb);
}

static inline uint16_t aIDY(int eclk)  /* Indirect Indexed (Y) - reads */
{
    uint8_t zpa = g_read(g_cpu->PC++);
    uint8_t lsb = g_read(zpa++);
    uint8_t msb = g_read(zpa);
    if (lsb + g_cpu->Y > 0xFF) {
        /* Phantom: read from intermediate (possibly wrong page) address */
        (void)g_read((msb << 8) | (uint8_t)(lsb + g_cpu->Y));
        clk(eclk);
    }
    return (uint16_t)(WORD(lsb, msb) + g_cpu->Y);
}

static inline uint16_t aIDYw(void)  /* Indirect Indexed (Y) - stores (always phantom) */
{
    uint8_t zpa = g_read(g_cpu->PC++);
    uint8_t lsb = g_read(zpa++);
    uint8_t msb = g_read(zpa);
    /* Phantom: ALWAYS read intermediate address for stores */
    (void)g_read((msb << 8) | (uint8_t)(lsb + g_cpu->Y));
    return (uint16_t)(WORD(lsb, msb) + g_cpu->Y);
}

static inline uint16_t aIND(void)  /* Indirect (JMP only) */
{
    uint16_t ea = aABS();
    uint8_t lsb = g_read(ea);
    /* NMOS 6502 quirk: does not fetch across page boundaries */
    ea = WORD((uint8_t)(LSB(ea) + 1), MSB(ea));
    uint8_t msb = g_read(ea);
    return WORD(lsb, msb);
}

/* Branch helper */
static inline void br(int cond, uint16_t ea)
{
    if (cond) {
        clk(MSB(g_cpu->PC) == MSB(ea) ? 1 : 2);
        g_cpu->PC = ea;
    }
}

/* Instructions */
static inline void iADC(uint8_t mem)
{
    int c = GET_FLAG(FLAG_C) ? 1 : 0;
    int sum = g_cpu->A + mem + c;
    SET_FLAG(FLAG_V, (~(g_cpu->A ^ mem) & (g_cpu->A ^ (sum & 0xFF)) & 0x80) != 0);

    if (GET_FLAG(FLAG_D)) {
        int lo = (g_cpu->A & 0x0F) + (mem & 0x0F) + c;
        int hi = (g_cpu->A >> 4) + (mem >> 4);
        if (lo > 9) { lo += 6; hi++; }
        if (hi > 9) { hi += 6; }
        g_cpu->A = (uint8_t)((lo & 0x0F) | (hi << 4));
        SET_FLAG(FLAG_C, (hi & 0x10) != 0);
    } else {
        g_cpu->A = (uint8_t)sum;
        SET_FLAG(FLAG_C, (sum & 0x100) != 0);
    }
    SET_NZ((uint8_t)sum);
}

static inline void iAND(uint8_t mem)
{
    g_cpu->A &= mem;
    SET_NZ(g_cpu->A);
}

static inline uint8_t iASL(uint8_t mem)
{
    SET_FLAG(FLAG_C, (mem & 0x80) != 0);
    mem <<= 1;
    SET_NZ(mem);
    return mem;
}

static inline void iBIT(uint8_t mem)
{
    SET_FLAG(FLAG_N, (mem & 0x80) != 0);
    SET_FLAG(FLAG_V, (mem & 0x40) != 0);
    SET_FLAG(FLAG_Z, (mem & g_cpu->A) == 0);
}

static inline void iBRK(void)
{
    (void)g_read(g_cpu->PC);  /* Phantom: read byte after BRK (discarded) */
    g_cpu->PC++;
    SET_FLAG(FLAG_B, 1);
    push(MSB(g_cpu->PC));
    push(LSB(g_cpu->PC));
    push(g_cpu->P);
    SET_FLAG(FLAG_I, 1);
    g_cpu->PC = WORD(g_read(IRQ_VEC), g_read(IRQ_VEC + 1));
}

static inline void iCMP(uint8_t mem)
{
    SET_FLAG(FLAG_C, g_cpu->A >= mem);
    SET_NZ((uint8_t)(g_cpu->A - mem));
}

static inline void iCPX(uint8_t mem)
{
    SET_FLAG(FLAG_C, g_cpu->X >= mem);
    SET_NZ((uint8_t)(g_cpu->X - mem));
}

static inline void iCPY(uint8_t mem)
{
    SET_FLAG(FLAG_C, g_cpu->Y >= mem);
    SET_NZ((uint8_t)(g_cpu->Y - mem));
}

static inline uint8_t iDEC(uint8_t mem)
{
    mem--;
    SET_NZ(mem);
    return mem;
}

static inline void iEOR(uint8_t mem)
{
    g_cpu->A ^= mem;
    SET_NZ(g_cpu->A);
}

static inline uint8_t iINC(uint8_t mem)
{
    mem++;
    SET_NZ(mem);
    return mem;
}

static inline void iLDA(uint8_t mem)
{
    g_cpu->A = mem;
    SET_NZ(g_cpu->A);
}

static inline void iLDX(uint8_t mem)
{
    g_cpu->X = mem;
    SET_NZ(g_cpu->X);
}

static inline void iLDY(uint8_t mem)
{
    g_cpu->Y = mem;
    SET_NZ(g_cpu->Y);
}

static inline uint8_t iLSR(uint8_t mem)
{
    SET_FLAG(FLAG_C, (mem & 0x01) != 0);
    mem >>= 1;
    SET_NZ(mem);
    return mem;
}

static inline void iORA(uint8_t mem)
{
    g_cpu->A |= mem;
    SET_NZ(g_cpu->A);
}

static inline uint8_t iROL(uint8_t mem)
{
    uint8_t d0 = GET_FLAG(FLAG_C) ? 0x01 : 0x00;
    SET_FLAG(FLAG_C, (mem & 0x80) != 0);
    mem = (mem << 1) | d0;
    SET_NZ(mem);
    return mem;
}

static inline uint8_t iROR(uint8_t mem)
{
    uint8_t d7 = GET_FLAG(FLAG_C) ? 0x80 : 0x00;
    SET_FLAG(FLAG_C, (mem & 0x01) != 0);
    mem = (mem >> 1) | d7;
    SET_NZ(mem);
    return mem;
}

static inline void iSBC(uint8_t mem)
{
    int c = GET_FLAG(FLAG_C) ? 0 : 1;
    int sum = g_cpu->A - mem - c;
    SET_FLAG(FLAG_V, ((g_cpu->A ^ mem) & (g_cpu->A ^ (sum & 0xFF)) & 0x80) != 0);

    if (GET_FLAG(FLAG_D)) {
        int lo = (g_cpu->A & 0x0F) - (mem & 0x0F) - c;
        int hi = (g_cpu->A >> 4) - (mem >> 4);
        if ((lo & 0x10) != 0) { lo -= 6; hi--; }
        if ((hi & 0x10) != 0) { hi -= 6; }
        g_cpu->A = (uint8_t)((lo & 0x0F) | (hi << 4));
    } else {
        g_cpu->A = (uint8_t)sum;
    }
    SET_FLAG(FLAG_C, (sum & 0x100) == 0);
    SET_NZ((uint8_t)sum);
}

/* Illegal opcodes */
static inline void iLAX(uint8_t mem)
{
    g_cpu->A = g_cpu->X = mem;
    SET_NZ(g_cpu->A);
}

static inline uint8_t iISC(uint8_t mem)
{
    mem++;
    iSBC(mem);
    return mem;
}

/* DCP: DEC + CMP (read-modify-write) */
static inline uint8_t iDCP(uint8_t mem)
{
    mem--;
    SET_FLAG(FLAG_C, g_cpu->A >= mem);
    SET_NZ((uint8_t)(g_cpu->A - mem));
    return mem;
}

/* SLO: ASL + ORA (read-modify-write) */
static inline uint8_t iSLO(uint8_t mem)
{
    SET_FLAG(FLAG_C, (mem & 0x80) != 0);
    mem <<= 1;
    g_cpu->A |= mem;
    SET_NZ(g_cpu->A);
    return mem;
}

/* SRE: LSR + EOR (read-modify-write) */
static inline uint8_t iSRE(uint8_t mem)
{
    SET_FLAG(FLAG_C, (mem & 0x01) != 0);
    mem >>= 1;
    g_cpu->A ^= mem;
    SET_NZ(g_cpu->A);
    return mem;
}

/* RRA: ROR + ADC (read-modify-write) */
static inline uint8_t iRRA(uint8_t mem)
{
    uint8_t d7 = GET_FLAG(FLAG_C) ? 0x80 : 0x00;
    SET_FLAG(FLAG_C, (mem & 0x01) != 0);
    mem = (mem >> 1) | d7;
    iADC(mem);
    return mem;
}

static inline uint8_t iRLA(uint8_t mem)
{
    uint8_t d0 = GET_FLAG(FLAG_C) ? 0x01 : 0x00;
    SET_FLAG(FLAG_C, (mem & 0x80) != 0);
    mem = (mem << 1) | d0;
    g_cpu->A &= mem;
    SET_NZ(g_cpu->A);
    return mem;
}

static inline uint8_t iSAX(void)
{
    return (uint8_t)(g_cpu->A & g_cpu->X);
}

static inline void iALR(uint8_t mem)
{
    iAND(mem);
    g_cpu->A = iLSR(g_cpu->A);
}

static inline void iANC(uint8_t mem)
{
    iAND(mem);
    SET_FLAG(FLAG_C, (g_cpu->A & 0x80) != 0);
}

/* Interrupt handling */
static void interrupt_nmi(void)
{
    (void)g_read(g_cpu->PC);  /* Phantom: read current PC (discarded) */
    (void)g_read(g_cpu->PC);  /* Phantom: re-read current PC (discarded) */
    push(MSB(g_cpu->PC));
    push(LSB(g_cpu->PC));
    SET_FLAG(FLAG_B, 0);
    push(g_cpu->P);
    SET_FLAG(FLAG_I, 1);
    g_cpu->PC = WORD(g_read(NMI_VEC), g_read(NMI_VEC + 1));
    clk(7);
}

static void interrupt_irq(void)
{
    if (!GET_FLAG(FLAG_I)) {
        (void)g_read(g_cpu->PC);  /* Phantom: read current PC (discarded) */
        (void)g_read(g_cpu->PC);  /* Phantom: re-read current PC (discarded) */
        push(MSB(g_cpu->PC));
        push(LSB(g_cpu->PC));
        SET_FLAG(FLAG_B, 0);
        push(g_cpu->P);
        SET_FLAG(FLAG_I, 1);
        g_cpu->PC = WORD(g_read(IRQ_VEC), g_read(IRQ_VEC + 1));
    }
    clk(7);
}

/* External logging function */
extern void log_msg(const char *msg);

static int g_cpu_log_count = 0;

/* Initialize CPU */
void m6502_init(M6502 *cpu, int run_clocks_multiple)
{
    memset(cpu, 0, sizeof(M6502));
    cpu->run_clocks_multiple = run_clocks_multiple;
    cpu->P = (1 << 5);  /* Bit 5 always set */
    g_cpu_log_count = 0;  /* Reset log count on init */
}

/* Reset CPU */
void m6502_reset(M6502 *cpu, mem_read_fn read)
{
    char msg[128];
    uint8_t lo, hi;

    cpu->jammed = 0;
    cpu->S = 0xFF;
    cpu->P |= FLAG_I | FLAG_Z;

    lo = read(RST_VEC);
    hi = read(RST_VEC + 1);
    cpu->PC = WORD(lo, hi);

    /* Match reference M6502.cs Reset: clk(6) advances clock AND adjusts RunClocks */
    cpu->clock += 6;
    cpu->run_clocks -= 6 * cpu->run_clocks_multiple;

    snprintf(msg, sizeof(msg), "CPU RESET: PC=$%04X (from $FFFC: lo=$%02X hi=$%02X)",
             cpu->PC, lo, hi);
    log_msg(msg);
}

/* Execute CPU cycles */
void m6502_execute(M6502 *cpu, mem_read_fn read, mem_write_fn write)
{
    uint16_t ea;
    uint8_t opcode;

    g_cpu = cpu;
    g_read = read;
    g_write = write;

    cpu->emulator_preempt_request = 0;

    while (cpu->run_clocks > 0 && !cpu->emulator_preempt_request && !cpu->jammed) {
        if (cpu->nmi_interrupt_request) {
            interrupt_nmi();
            cpu->nmi_interrupt_request = 0;
        } else if (cpu->irq_interrupt_request) {
            interrupt_irq();
            cpu->irq_interrupt_request = 0;
        } else {
            opcode = read(cpu->PC++);

            switch (opcode) {
                /* ADC */
                case 0x65: ea = aZPG();  clk(3); iADC(read(ea)); break;
                case 0x75: ea = aZPX();  clk(4); iADC(read(ea)); break;
                case 0x61: ea = aIDX();  clk(6); iADC(read(ea)); break;
                case 0x71: ea = aIDY(1); clk(5); iADC(read(ea)); break;
                case 0x79: ea = aABY(1); clk(4); iADC(read(ea)); break;
                case 0x6D: ea = aABS();  clk(4); iADC(read(ea)); break;
                case 0x7D: ea = aABX(1); clk(4); iADC(read(ea)); break;
                case 0x69: clk(2); iADC(read(cpu->PC++)); break;

                /* AND */
                case 0x25: ea = aZPG();  clk(3); iAND(read(ea)); break;
                case 0x35: ea = aZPX();  clk(4); iAND(read(ea)); break;
                case 0x21: ea = aIDX();  clk(6); iAND(read(ea)); break;
                case 0x31: ea = aIDY(1); clk(5); iAND(read(ea)); break;
                case 0x2D: ea = aABS();  clk(4); iAND(read(ea)); break;
                case 0x39: ea = aABY(1); clk(4); iAND(read(ea)); break;
                case 0x3D: ea = aABX(1); clk(4); iAND(read(ea)); break;
                case 0x29: clk(2); iAND(read(cpu->PC++)); break;

                /* ASL */
                case 0x06: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iASL(v)); } break;
                case 0x16: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iASL(v)); } break;
                case 0x0E: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iASL(v)); } break;
                case 0x1E: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iASL(v)); } break;
                case 0x0A: clk(2); cpu->A = iASL(cpu->A); break;

                /* BIT */
                case 0x24: ea = aZPG();  clk(3); iBIT(read(ea)); break;
                case 0x2C: ea = aABS();  clk(4); iBIT(read(ea)); break;

                /* Branches */
                case 0x10: ea = aREL(); clk(2); br(!GET_FLAG(FLAG_N), ea); break; /* BPL */
                case 0x30: ea = aREL(); clk(2); br( GET_FLAG(FLAG_N), ea); break; /* BMI */
                case 0x50: ea = aREL(); clk(2); br(!GET_FLAG(FLAG_V), ea); break; /* BVC */
                case 0x70: ea = aREL(); clk(2); br( GET_FLAG(FLAG_V), ea); break; /* BVS */
                case 0x90: ea = aREL(); clk(2); br(!GET_FLAG(FLAG_C), ea); break; /* BCC */
                case 0xB0: ea = aREL(); clk(2); br( GET_FLAG(FLAG_C), ea); break; /* BCS */
                case 0xD0: ea = aREL(); clk(2); br(!GET_FLAG(FLAG_Z), ea); break; /* BNE */
                case 0xF0: ea = aREL(); clk(2); br( GET_FLAG(FLAG_Z), ea); break; /* BEQ */

                /* BRK */
                case 0x00: clk(7); iBRK(); break;

                /* Flag operations */
                case 0x18: clk(2); SET_FLAG(FLAG_C, 0); break; /* CLC */
                case 0xD8: clk(2); SET_FLAG(FLAG_D, 0); break; /* CLD */
                case 0x58: clk(2); SET_FLAG(FLAG_I, 0); break; /* CLI */
                case 0xB8: clk(2); SET_FLAG(FLAG_V, 0); break; /* CLV */
                case 0x38: clk(2); SET_FLAG(FLAG_C, 1); break; /* SEC */
                case 0xF8: clk(2); SET_FLAG(FLAG_D, 1); break; /* SED */
                case 0x78: clk(2); SET_FLAG(FLAG_I, 1); break; /* SEI */

                /* CMP */
                case 0xC5: ea = aZPG();  clk(3); iCMP(read(ea)); break;
                case 0xD5: ea = aZPX();  clk(4); iCMP(read(ea)); break;
                case 0xC1: ea = aIDX();  clk(6); iCMP(read(ea)); break;
                case 0xD1: ea = aIDY(1); clk(5); iCMP(read(ea)); break;
                case 0xCD: ea = aABS();  clk(4); iCMP(read(ea)); break;
                case 0xDD: ea = aABX(1); clk(4); iCMP(read(ea)); break;
                case 0xD9: ea = aABY(1); clk(4); iCMP(read(ea)); break;
                case 0xC9: clk(2); iCMP(read(cpu->PC++)); break;

                /* CPX */
                case 0xE4: ea = aZPG();  clk(3); iCPX(read(ea)); break;
                case 0xEC: ea = aABS();  clk(4); iCPX(read(ea)); break;
                case 0xE0: clk(2); iCPX(read(cpu->PC++)); break;

                /* CPY */
                case 0xC4: ea = aZPG();  clk(3); iCPY(read(ea)); break;
                case 0xCC: ea = aABS();  clk(4); iCPY(read(ea)); break;
                case 0xC0: clk(2); iCPY(read(cpu->PC++)); break;

                /* DEC */
                case 0xC6: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iDEC(v)); } break;
                case 0xD6: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iDEC(v)); } break;
                case 0xCE: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iDEC(v)); } break;
                case 0xDE: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iDEC(v)); } break;

                /* DEX, DEY */
                case 0xCA: clk(2); cpu->X--; SET_NZ(cpu->X); break;
                case 0x88: clk(2); cpu->Y--; SET_NZ(cpu->Y); break;

                /* EOR */
                case 0x45: ea = aZPG();  clk(3); iEOR(read(ea)); break;
                case 0x55: ea = aZPX();  clk(4); iEOR(read(ea)); break;
                case 0x41: ea = aIDX();  clk(6); iEOR(read(ea)); break;
                case 0x51: ea = aIDY(1); clk(5); iEOR(read(ea)); break;
                case 0x4D: ea = aABS();  clk(4); iEOR(read(ea)); break;
                case 0x5D: ea = aABX(1); clk(4); iEOR(read(ea)); break;
                case 0x59: ea = aABY(1); clk(4); iEOR(read(ea)); break;
                case 0x49: clk(2); iEOR(read(cpu->PC++)); break;

                /* INC */
                case 0xE6: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iINC(v)); } break;
                case 0xF6: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iINC(v)); } break;
                case 0xEE: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iINC(v)); } break;
                case 0xFE: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iINC(v)); } break;

                /* INX, INY */
                case 0xE8: clk(2); cpu->X++; SET_NZ(cpu->X); break;
                case 0xC8: clk(2); cpu->Y++; SET_NZ(cpu->Y); break;

                /* JMP */
                case 0x4C: ea = aABS(); clk(3); cpu->PC = ea; break;
                case 0x6C: ea = aIND(); clk(5); cpu->PC = ea; break;

                /* JSR */
                case 0x20: ea = aABS(); clk(6);
                    cpu->PC--;
                    (void)g_read(0x0100 + cpu->S);  /* Phantom: internal stack read */
                    push(MSB(cpu->PC));
                    push(LSB(cpu->PC));
                    cpu->PC = ea;
                    break;

                /* LDA */
                case 0xA5: ea = aZPG();  clk(3); iLDA(read(ea)); break;
                case 0xB5: ea = aZPX();  clk(4); iLDA(read(ea)); break;
                case 0xA1: ea = aIDX();  clk(6); iLDA(read(ea)); break;
                case 0xB1: ea = aIDY(1); clk(5); iLDA(read(ea)); break;
                case 0xAD: ea = aABS();  clk(4); iLDA(read(ea)); break;
                case 0xBD: ea = aABX(1); clk(4); iLDA(read(ea)); break;
                case 0xB9: ea = aABY(1); clk(4); iLDA(read(ea)); break;
                case 0xA9: clk(2); iLDA(read(cpu->PC++)); break;

                /* LDX */
                case 0xA6: ea = aZPG();  clk(3); iLDX(read(ea)); break;
                case 0xB6: ea = aZPY();  clk(4); iLDX(read(ea)); break;
                case 0xAE: ea = aABS();  clk(4); iLDX(read(ea)); break;
                case 0xBE: ea = aABY(1); clk(4); iLDX(read(ea)); break;
                case 0xA2: clk(2); iLDX(read(cpu->PC++)); break;

                /* LDY */
                case 0xA4: ea = aZPG();  clk(3); iLDY(read(ea)); break;
                case 0xB4: ea = aZPX();  clk(4); iLDY(read(ea)); break;
                case 0xAC: ea = aABS();  clk(4); iLDY(read(ea)); break;
                case 0xBC: ea = aABX(1); clk(4); iLDY(read(ea)); break;
                case 0xA0: clk(2); iLDY(read(cpu->PC++)); break;

                /* LSR */
                case 0x46: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iLSR(v)); } break;
                case 0x56: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iLSR(v)); } break;
                case 0x4E: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iLSR(v)); } break;
                case 0x5E: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iLSR(v)); } break;
                case 0x4A: clk(2); cpu->A = iLSR(cpu->A); break;

                /* NOP */
                case 0xEA: clk(2); break;

                /* ORA */
                case 0x05: ea = aZPG();  clk(3); iORA(read(ea)); break;
                case 0x15: ea = aZPX();  clk(4); iORA(read(ea)); break;
                case 0x01: ea = aIDX();  clk(6); iORA(read(ea)); break;
                case 0x11: ea = aIDY(1); clk(5); iORA(read(ea)); break;
                case 0x0D: ea = aABS();  clk(4); iORA(read(ea)); break;
                case 0x1D: ea = aABX(1); clk(4); iORA(read(ea)); break;
                case 0x19: ea = aABY(1); clk(4); iORA(read(ea)); break;
                case 0x09: clk(2); iORA(read(cpu->PC++)); break;

                /* Stack operations */
                case 0x48: clk(3); (void)g_read(cpu->PC); push(cpu->A); break;         /* PHA */
                case 0x68: clk(4); (void)g_read(cpu->PC); (void)g_read(0x0100 + cpu->S); cpu->A = pull(); SET_NZ(cpu->A); break; /* PLA */
                case 0x08: clk(3); (void)g_read(cpu->PC); push(cpu->P); break;         /* PHP */
                case 0x28: clk(4); (void)g_read(cpu->PC); (void)g_read(0x0100 + cpu->S); cpu->P = pull(); SET_FLAG(FLAG_B, 1); break; /* PLP */

                /* ROL */
                case 0x26: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iROL(v)); } break;
                case 0x36: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iROL(v)); } break;
                case 0x2E: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iROL(v)); } break;
                case 0x3E: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iROL(v)); } break;
                case 0x2A: clk(2); cpu->A = iROL(cpu->A); break;

                /* ROR */
                case 0x66: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iROR(v)); } break;
                case 0x76: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iROR(v)); } break;
                case 0x6E: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iROR(v)); } break;
                case 0x7E: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iROR(v)); } break;
                case 0x6A: clk(2); cpu->A = iROR(cpu->A); break;

                /* RTI */
                case 0x40: clk(6);
                    (void)g_read(cpu->PC);           /* Phantom: read next byte (discarded) */
                    (void)g_read(0x0100 + cpu->S);   /* Phantom: internal stack read before increment */
                    cpu->P = pull();
                    cpu->PC = WORD(pull(), pull());
                    SET_FLAG(FLAG_B, 1);
                    break;

                /* RTS */
                case 0x60: clk(6);
                    (void)g_read(cpu->PC);           /* Phantom: read next byte (discarded) */
                    (void)g_read(0x0100 + cpu->S);   /* Phantom: internal stack read before increment */
                    cpu->PC = WORD(pull(), pull());
                    cpu->PC++;
                    (void)g_read(cpu->PC);           /* Phantom: read new PC (discarded) */
                    break;

                /* SBC */
                case 0xE5: ea = aZPG();  clk(3); iSBC(read(ea)); break;
                case 0xF5: ea = aZPX();  clk(4); iSBC(read(ea)); break;
                case 0xE1: ea = aIDX();  clk(6); iSBC(read(ea)); break;
                case 0xF1: ea = aIDY(1); clk(5); iSBC(read(ea)); break;
                case 0xED: ea = aABS();  clk(4); iSBC(read(ea)); break;
                case 0xFD: ea = aABX(1); clk(4); iSBC(read(ea)); break;
                case 0xF9: ea = aABY(1); clk(4); iSBC(read(ea)); break;
                case 0xE9: clk(2); iSBC(read(cpu->PC++)); break;

                /* STA */
                case 0x85: ea = aZPG();  clk(3); write(ea, cpu->A); break;
                case 0x95: ea = aZPX();  clk(4); write(ea, cpu->A); break;
                case 0x81: ea = aIDX();  clk(6); write(ea, cpu->A); break;
                case 0x91: ea = aIDYw(); clk(6); write(ea, cpu->A); break;
                case 0x8D: ea = aABS();  clk(4); write(ea, cpu->A); break;
                case 0x99: ea = aABYw(); clk(5); write(ea, cpu->A); break;
                case 0x9D: ea = aABXw(); clk(5); write(ea, cpu->A); break;

                /* STX */
                case 0x86: ea = aZPG();  clk(3); write(ea, cpu->X); break;
                case 0x96: ea = aZPY();  clk(4); write(ea, cpu->X); break;
                case 0x8E: ea = aABS();  clk(4); write(ea, cpu->X); break;

                /* STY */
                case 0x84: ea = aZPG();  clk(3); write(ea, cpu->Y); break;
                case 0x94: ea = aZPX();  clk(4); write(ea, cpu->Y); break;
                case 0x8C: ea = aABS();  clk(4); write(ea, cpu->Y); break;

                /* Transfer operations */
                case 0xAA: clk(2); cpu->X = cpu->A; SET_NZ(cpu->X); break; /* TAX */
                case 0xA8: clk(2); cpu->Y = cpu->A; SET_NZ(cpu->Y); break; /* TAY */
                case 0xBA: clk(2); cpu->X = cpu->S; SET_NZ(cpu->X); break; /* TSX */
                case 0x8A: clk(2); cpu->A = cpu->X; SET_NZ(cpu->A); break; /* TXA */
                case 0x9A: clk(2); cpu->S = cpu->X; break;                 /* TXS */
                case 0x98: clk(2); cpu->A = cpu->Y; SET_NZ(cpu->A); break; /* TYA */

                /* ---- Illegal/Undocumented Opcodes ---- */

                /* KIL/JAM - Jam the processor */
                case 0x02: case 0x12: case 0x22: case 0x32:
                case 0x42: case 0x52: case 0x62: case 0x72:
                case 0x92: case 0xB2: case 0xD2: case 0xF2: {
                    char jmsg[128];
                    snprintf(jmsg, sizeof(jmsg), "CPU JAMMED at PC=$%04X opcode=$%02X A=$%02X X=$%02X Y=$%02X S=$%02X",
                             cpu->PC - 1, opcode, cpu->A, cpu->X, cpu->Y, cpu->S);
                    log_msg(jmsg);
                    snprintf(jmsg, sizeof(jmsg), "  Bytes at JAM-4: %02X %02X %02X %02X [%02X] %02X %02X %02X",
                             read(cpu->PC - 5), read(cpu->PC - 4), read(cpu->PC - 3), read(cpu->PC - 2),
                             opcode, read(cpu->PC), read(cpu->PC + 1), read(cpu->PC + 2));
                    log_msg(jmsg);
                    clk(2); cpu->jammed = 1; break;
                }

                /* NOP (implied, single byte) */
                case 0x1A: case 0x3A: case 0x5A: case 0x7A:
                case 0xDA: case 0xFA:
                    clk(2); break;

                /* DOP/SKB (skip byte - zero page) */
                case 0x04: case 0x44: case 0x64:
                    clk(3); cpu->PC++; break;

                /* DOP/SKB (skip byte - zero page,X) */
                case 0x14: case 0x34: case 0x54: case 0x74:
                case 0xD4: case 0xF4:
                    clk(4); cpu->PC++; break;

                /* DOP/SKB (skip byte - immediate) */
                case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
                    clk(2); cpu->PC++; break;

                /* TOP/SKW (skip word - absolute) */
                case 0x0C: ea = aABS();  clk(4); break;

                /* TOP/SKW (skip word - absolute,X) */
                case 0x1C: case 0x3C: case 0x5C: case 0x7C:
                case 0x9C: case 0xDC: case 0xFC:
                    ea = aABX(1); clk(4); break;

                /* ALR: AND + LSR */
                case 0x4B: clk(2); iALR(read(cpu->PC++)); break;

                /* ANC: AND + set C from N */
                case 0x0B: case 0x2B: clk(2); iANC(read(cpu->PC++)); break;

                /* SBC (unofficial mirror of $E9) */
                case 0xEB: clk(2); iSBC(read(cpu->PC++)); break;

                /* LAX: LDA + LDX */
                case 0xA7: ea = aZPG();  clk(3); iLAX(read(ea)); break;
                case 0xB7: ea = aZPY();  clk(4); iLAX(read(ea)); break;
                case 0xAF: ea = aABS();  clk(4); iLAX(read(ea)); break;
                case 0xBF: ea = aABY(1); clk(4); iLAX(read(ea)); break;
                case 0xA3: ea = aIDX();  clk(6); iLAX(read(ea)); break;
                case 0xB3: ea = aIDY(1); clk(5); iLAX(read(ea)); break;

                /* SAX: store A & X */
                case 0x87: ea = aZPG(); clk(3); write(ea, iSAX()); break;
                case 0x97: ea = aZPY(); clk(4); write(ea, iSAX()); break;
                case 0x8F: ea = aABS(); clk(4); write(ea, iSAX()); break;
                case 0x83: ea = aIDX(); clk(6); write(ea, iSAX()); break;

                /* DCP: DEC + CMP */
                case 0xC7: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xD7: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xCF: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xDF: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xDB: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xC3: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;
                case 0xD3: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iDCP(v)); } break;

                /* ISC/ISB: INC + SBC */
                case 0xE7: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xF7: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xEF: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xFF: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xFB: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xE3: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;
                case 0xF3: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iISC(v)); } break;

                /* SLO: ASL + ORA */
                case 0x07: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x17: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x0F: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x1F: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x1B: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x03: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;
                case 0x13: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iSLO(v)); } break;

                /* RLA: ROL + AND */
                case 0x27: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x37: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x2F: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x3F: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x3B: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x23: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;
                case 0x33: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iRLA(v)); } break;

                /* SRE: LSR + EOR */
                case 0x47: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x57: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x4F: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x5F: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x5B: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x43: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;
                case 0x53: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iSRE(v)); } break;

                /* RRA: ROR + ADC */
                case 0x67: ea = aZPG();  clk(5); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x77: ea = aZPX();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x6F: ea = aABS();  clk(6); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x7F: ea = aABXw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x7B: ea = aABYw(); clk(7); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x63: ea = aIDX();  clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;
                case 0x73: ea = aIDYw(); clk(8); { uint8_t v = read(ea); write(ea, v); write(ea, iRRA(v)); } break;

                default:
                    /* Unknown opcode - treat as 2-cycle NOP */
                    clk(2);
                    break;
            }
        }
    }
}

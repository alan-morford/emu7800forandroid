/*
 * pia.c
 *
 * Peripheral Interface Adapter (6532)
 * a.k.a. RIOT (RAM I/O Timer)
 *
 * Copyright (c) 2003-2012 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdio.h>
#include "pia.h"
#include "machine.h"

extern void log_msg(const char *msg);

/* Diagnostic logging for RIOT timer — limited to first N events per game */
#define RIOT_LOG_MAX 5
static int g_riot_timer_set_count = 0;
static int g_riot_read_count = 0;
static int g_swcha_read_count = 0;
static int g_ddra_write_count = 0;
static const char *timer_name[4] = { "TIM1T", "TIM8T", "TIM64T", "T1024T" };

void pia_reset_log_counts(void)
{
    g_riot_timer_set_count = 0;
    g_riot_read_count = 0;
    g_swcha_read_count = 0;
    g_ddra_write_count = 0;
}

/* Initialize PIA */
void pia_init(PIA *pia)
{
    memset(pia, 0, sizeof(PIA));
}

/* Reset PIA */
void pia_reset(PIA *pia, uint64_t cpu_clock)
{
    /* RAM is NOT cleared on reset — preserved across warm resets.
     * Reference PIA.Reset() only resets timer/IRQ/DDR state, not RAM.
     * pia_init() zeros everything at construction time. */

    /* Some games hang if timer is initialized to zero */
    pia->timer_shift = 10;
    pia->timer_target = cpu_clock + (0xFF << pia->timer_shift);

    pia->irq_enabled = 0;
    pia->irq_triggered = 0;
    pia->ddra = 0;
    pia->ddrb = 0;
    pia->written_port_a = 0;
    pia->written_port_b = 0;
}

/* Read Port A (Controllers) */
static uint8_t read_port_a(PIA *pia)
{
    uint8_t porta = 0;
    int left_ctrl = machine_get_left_controller();
    int right_ctrl = machine_get_right_controller();

    /* Left controller jack - bits 4-7 */
    switch (left_ctrl) {
        case CTRL_PROLINE_JOYSTICK:
        case CTRL_LIGHTGUN:
            /* Light gun games still use joystick for menu navigation */
            if (!machine_sample_joystick(0, 0)) porta |= 0x10;  /* Up */
            if (!machine_sample_joystick(0, 1)) porta |= 0x20;  /* Down */
            if (!machine_sample_joystick(0, 2)) porta |= 0x40;  /* Left */
            if (!machine_sample_joystick(0, 3)) porta |= 0x80;  /* Right */
            break;
        case CTRL_NONE:
        default:
            /* No controller: bits 4-7 = 0 */
            break;
    }

    /* Right controller jack - bits 0-3 */
    switch (right_ctrl) {
        case CTRL_PROLINE_JOYSTICK:
        case CTRL_LIGHTGUN:
            if (!machine_sample_joystick(1, 0)) porta |= 0x01;  /* Up */
            if (!machine_sample_joystick(1, 1)) porta |= 0x02;  /* Down */
            if (!machine_sample_joystick(1, 2)) porta |= 0x04;  /* Left */
            if (!machine_sample_joystick(1, 3)) porta |= 0x08;  /* Right */
            break;
        case CTRL_NONE:
        default:
            break;
    }

    /* Apply DDRA masking per EMU7800 reference:
     * Bits set in DDRA are output (return written value),
     * bits clear in DDRA are input (return joystick state). */
    return (porta & ~pia->ddra) | (pia->written_port_a & pia->ddra);
}

/* Read Port B (Console Switches) */
static uint8_t read_port_b(void)
{
    uint8_t portb = 0;

    /* D0 = Game Reset (active low) */
    if (!machine_sample_switch(0)) portb |= 0x01;

    /* D1 = Game Select (active low) */
    if (!machine_sample_switch(1)) portb |= 0x02;

    /* D3 = Color/BW (1=color) */
    portb |= 0x08;

    /* D6 = Left difficulty (1=A/pro) */
    if (machine_sample_switch(2)) portb |= 0x40;

    /* D7 = Right difficulty (1=A/pro) */
    if (machine_sample_switch(3)) portb |= 0x80;

    return portb;
}

/* Read timer register */
static uint8_t read_timer(PIA *pia, uint64_t cpu_clock)
{
    int delta;
    uint8_t result;

    pia->irq_triggered = 0;
    delta = (int)(pia->timer_target - cpu_clock);

    if (delta >= 0) {
        result = (uint8_t)(delta >> pia->timer_shift);
    } else {
        if (delta != -1) {
            pia->irq_triggered = 1;
        }
        result = (uint8_t)((delta >= -256) ? delta : 0);
    }

    if (g_riot_read_count < RIOT_LOG_MAX) {
        char msg[128];
        snprintf(msg, sizeof(msg), "RIOT READ: INTIM=%d(0x%02X) delta=%d shift=%d cpu=%llu expired=%d",
                 result, result, delta, pia->timer_shift,
                 (unsigned long long)cpu_clock, (delta < 0) ? 1 : 0);
        log_msg(msg);
        g_riot_read_count++;
    }

    return result;
}

/* Read interrupt flag */
static uint8_t read_interrupt_flag(PIA *pia, uint64_t cpu_clock)
{
    int delta = (int)(pia->timer_target - cpu_clock);
    return (uint8_t)((delta >= 0 || (pia->irq_enabled && pia->irq_triggered)) ? 0x00 : 0x80);
}

/* Set timer register */
static void set_timer(PIA *pia, uint8_t data, int interval, uint64_t cpu_clock)
{
    pia->irq_triggered = 0;

    switch (interval & 3) {
        case 0: pia->timer_shift = 0; break;   /* TIM1T */
        case 1: pia->timer_shift = 3; break;   /* TIM8T */
        case 2: pia->timer_shift = 6; break;   /* TIM64T */
        case 3: pia->timer_shift = 10; break;  /* T1024T */
    }

    pia->timer_target = cpu_clock + ((uint64_t)data << pia->timer_shift);

    if (g_riot_timer_set_count < RIOT_LOG_MAX) {
        char msg[128];
        snprintf(msg, sizeof(msg), "RIOT SET: %s val=%d(0x%02X) shift=%d target=%llu cpu=%llu irq_en=%d",
                 timer_name[interval & 3], data, data, pia->timer_shift,
                 (unsigned long long)pia->timer_target,
                 (unsigned long long)cpu_clock, pia->irq_enabled);
        log_msg(msg);
        g_riot_timer_set_count++;
    }
}

/* Read from PIA */
uint8_t pia_read(PIA *pia, uint16_t addr, uint64_t cpu_clock)
{
    /* RAM at $80-$FF */
    if ((addr & 0x200) == 0) {
        return pia->ram[addr & 0x7F];
    }

    switch (addr & 7) {
        case 0:  /* SWCHA: Controllers */
        {
            uint8_t val = read_port_a(pia);
            if (g_swcha_read_count < 20) {
                char msg[160];
                snprintf(msg, sizeof(msg), "SWCHA READ: addr=$%04X val=$%02X ddra=$%02X written=$%02X left_ctrl=%d right_ctrl=%d joy0=$%02X",
                         addr, val, pia->ddra, pia->written_port_a,
                         machine_get_left_controller(), machine_get_right_controller(),
                         (uint8_t)((machine_sample_joystick(0,3) ? 0 : 0x80) |
                                   (machine_sample_joystick(0,2) ? 0 : 0x40) |
                                   (machine_sample_joystick(0,1) ? 0 : 0x20) |
                                   (machine_sample_joystick(0,0) ? 0 : 0x10)));
                log_msg(msg);
                g_swcha_read_count++;
            }
            return val;
        }
        case 1:  /* SWCHA DDR */
            return pia->ddra;
        case 2:  /* SWCHB: Console switches */
            return read_port_b();
        case 3:  /* SWCHB DDR */
            return 0;
        case 4:  /* INTIM */
        case 6:
            return read_timer(pia, cpu_clock);
        case 5:  /* INTFLG */
        case 7:
            return read_interrupt_flag(pia, cpu_clock);
        default:
            return 0;
    }
}

/* Write to PIA */
void pia_write(PIA *pia, uint16_t addr, uint8_t data, uint64_t cpu_clock)
{
    /* RAM at $80-$FF */
    if ((addr & 0x200) == 0) {
        pia->ram[addr & 0x7F] = data;
        return;
    }

    /* A2 distinguishes I/O registers from Timer */
    if ((addr & 0x04) != 0) {
        if ((addr & 0x10) != 0) {
            pia->irq_enabled = (addr & 0x08) != 0;
            set_timer(pia, data, addr & 3, cpu_clock);
        }
    } else {
        switch (addr & 3) {
            case 0:  /* SWCHA: Port A */
                pia->written_port_a = (data & pia->ddra) | (pia->written_port_a & ~pia->ddra);
                break;
            case 1:  /* SWACNT: Port A DDR */
                if (g_ddra_write_count < 10) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "DDRA WRITE: addr=$%04X data=$%02X (was $%02X)",
                             addr, data, pia->ddra);
                    log_msg(msg);
                    g_ddra_write_count++;
                }
                pia->ddra = data;
                break;
            case 2:  /* SWCHB: Port B */
                pia->written_port_b = (data & pia->ddrb) | (pia->written_port_b & ~pia->ddrb);
                break;
            case 3:  /* SWBCNT: Port B DDR */
                pia->ddrb = data;
                break;
        }
    }
}

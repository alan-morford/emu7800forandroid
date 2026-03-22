/*
 * machine.c
 *
 * Machine Integration
 * Ties together CPU, TIA, PIA, Maria, and Cart for 2600/7800
 *
 * Copyright (c) 2024 EMU7800
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "m6502.h"
#include "tia.h"
#include "tiasound.h"
#include "pia.h"
#include "cart.h"
#include "maria.h"

/* External logging function */
extern void log_msg(const char *msg);

/* Diagnostic log reset functions */
extern void pia_reset_log_counts(void);
extern void cart_reset_log_counts(void);
extern void tia_reset_sync_log(void);

/* 2600 Constants */
#define CLOCKS_PER_SCANLINE_CPU     76      /* CPU cycles per scanline */
#define TIA_CLOCKS_PER_SCANLINE     228     /* TIA color clocks per scanline */
#define SCANLINES_PER_FRAME         262     /* Canonical NTSC scanlines */
/*
 * Frame budget: generous upper bound so VSYNC naturally ends frames.
 * Games vary from ~262 (NTSC) to ~312 (PAL) scanlines. A budget of
 * 320 accommodates all, with VSYNC ending the frame at the right point.
 * No performance cost — VSYNC breaks the loop early for normal games.
 */
#define FRAME_BUDGET_SCANLINES      320

/* 7800 Constants */
#define CLOCKS_PER_SCANLINE_7800    114
#define SCANLINES_PER_FRAME_7800    262

/* System RAM */
static uint8_t g_ram_2600[0x80];       /* 2600: 128 bytes PIA RAM */

/*
 * 7800 has TWO separate 2KB RAMs (RAM6116 chips):
 * - RAM0: Primary mapped at $1800-$1FFF
 * - RAM1: Primary mapped at $2000-$27FF
 *         Also aliased to: $0040-$00FF (zero page high)
 *                          $0140-$01FF (stack)
 *                          $2040-$20FF, $2140-$21FF (mirrors)
 *                          $2800-$3FFF (mirrors)
 */
static uint8_t g_ram0_7800[0x800];     /* 7800 RAM0: 2KB at $1800-$1FFF */
static uint8_t g_ram1_7800[0x800];     /* 7800 RAM1: 2KB at $2000+ and aliases */

static int g_nmi_delay_frames = 0;  /* Delay NMI for a few frames after init */

/* Machine state */
static M6502 g_cpu;
static TIA g_tia;
static PIA g_pia;
static Cart g_cart;
static Maria g_maria;

static int g_machine_type = MACHINE_2600;
static int g_rom_loaded = 0;
static int g_7800_frame_count = 0;
static int g_2600_frame_count = 0;

/* Supercharger distinct access tracking (mirrors Stella M6502 distinctAccesses) */
static uint32_t g_distinct_accesses = 0;
static uint16_t g_last_access_addr = 0xFFFF;

/* Data bus state: last value read/written on the bus.
 * TIA reads OR this into lower 6 bits (open-bus behavior per EMU7800 reference). */
static uint8_t g_data_bus_state = 0;


/* Current scanline for 7800 mid-frame audio position */
static int g_7800_current_scanline = 0;

/*
 * Input state: two-layer design for thread safety.
 *
 * Layer 1 (volatile): Written by SDL/touch thread, read only at frame start.
 * Layer 2 (frame-sampled): Snapshot taken at frame start, used by CPU emulation.
 *
 * All TIA/PIA register reads during emulation use the frame-sampled copies,
 * ensuring input state is stable throughout the entire frame.  This prevents
 * mid-scanline input changes from causing horizontal shifts or other artifacts.
 */
static volatile uint8_t g_joystick[2];  /* Bit 0=up, 1=down, 2=left, 3=right */
static volatile uint8_t g_trigger[2];   /* Primary fire button */
static volatile uint8_t g_trigger2[2];  /* Secondary fire button (7800) */
static volatile uint8_t g_switches;     /* Bit 0=reset, 1=select, 2=ldiff, 3=rdiff */

/* Frame-sampled copies — stable for the duration of one frame */
static uint8_t g_frame_joystick[2];
static uint8_t g_frame_trigger_m[2];   /* _m suffix to avoid clash with tia.c g_frame_trigger */
static uint8_t g_frame_trigger2_m[2];
static uint8_t g_frame_switches;

/* Forward declarations */
static uint8_t mem_read_2600(uint16_t addr);
static void mem_write_2600(uint16_t addr, uint8_t data);
static uint8_t mem_read_7800(uint16_t addr);
static void mem_write_7800(uint16_t addr, uint8_t data);

/* DMA read callback for Maria */
static uint8_t maria_dma_read(uint16_t addr)
{
    return mem_read_7800(addr);
}

/* Input callbacks for Maria */
static int maria_trigger_callback(int player)
{
    if (player < 0 || player > 1) return 0;
    return g_trigger[player] != 0;
}

static int maria_trigger2_callback(int player)
{
    if (player < 0 || player > 1) return 0;
    return g_trigger2[player] != 0;
}

/* CPU preempt callback - called by Maria when WSYNC is written */
static void cpu_preempt_callback(void)
{
    g_cpu.emulator_preempt_request = 1;
}

/* NMI callback - called by Maria for DLI */
static void nmi_callback(void)
{
    /* Suppress DLI NMI during startup delay (same guard as VBLANK NMI) */
    if (g_nmi_delay_frames == 0) {
        g_cpu.nmi_interrupt_request = 1;
    }
}

/*
 * PIA Port B gate callback for ProLine two-button controller.
 *
 * Reference (Maria.cs SampleINPTLatched):
 *   var portbline = 4 << (playerNo << 1);
 *   if ((M.PIA.DDRB & portbline) != 0 && (M.PIA.WrittenPortB & portbline) == 0)
 *       return false;  // masked - INPT4/5 returns "not pressed"
 *
 * When a game wants to read buttons individually (two-button mode), it:
 * 1. Sets PIA DDRB bit as output for the player's portbline
 * 2. Drives WrittenPortB low for that bit
 * 3. This masks INPT4/5, so the game can read INPT0/INPT1 separately
 *
 * Player 0: portbline = 4 << 0 = bit 2 (0x04)
 * Player 1: portbline = 4 << 2 = bit 4 (0x10)
 *
 * Returns 1 if INPT4/5 should be masked (return "not pressed"), 0 otherwise.
 */
static int portb_gate_callback(int playerNo)
{
    int portbline = 4 << (playerNo << 1);
    return (g_pia.ddrb & portbline) != 0 && (g_pia.written_port_b & portbline) == 0;
}

/* Memory read callback for 2600 */
static uint8_t mem_read_2600(uint16_t addr)
{
    /* Distinct access tracking for Supercharger */
    if (addr != g_last_access_addr) {
        g_distinct_accesses++;
        g_last_access_addr = addr;
    }

    /* 2600 memory map:
     * $00-$7F: TIA (active on A12=0, A7=0)
     * $80-$FF: PIA RAM (active on A12=0, A7=1, A9=0)
     * $280-$2FF: PIA I/O and timer (active on A12=0, A7=1, A9=1)
     * $1000-$1FFF: Cartridge ROM
     */
    /* SB Superbanking: cart watches address bus for $0800-$0FFF hotspot */
    if (g_cart.type == CART_SB && (addr & 0x1800) == 0x0800) {
        g_cart.bank = addr & (g_cart.bank_count - 1);
    }

    if ((addr & 0x1000) == 0) {
        if ((addr & 0x0080) == 0) {
            /* TIA read - pass clock and data bus state for open-bus behavior */
            g_data_bus_state = tia_read(&g_tia, addr, g_cpu.clock, g_data_bus_state);
            return g_data_bus_state;
        } else {
            /* PIA read */
            g_data_bus_state = pia_read(&g_pia, addr, g_cpu.clock);
            return g_data_bus_state;
        }
    } else {
        /* Cartridge */
        g_data_bus_state = cart_read(&g_cart, addr);
        return g_data_bus_state;
    }
}

/* Memory write callback for 2600 */
static void mem_write_2600(uint16_t addr, uint8_t data)
{
    /* Distinct access tracking for Supercharger */
    if (addr != g_last_access_addr) {
        g_distinct_accesses++;
        g_last_access_addr = addr;
    }

    g_data_bus_state = data;

    /* SB Superbanking: cart watches address bus for $0800-$0FFF hotspot */
    if (g_cart.type == CART_SB && (addr & 0x1800) == 0x0800) {
        g_cart.bank = addr & (g_cart.bank_count - 1);
    }

    if ((addr & 0x1000) == 0) {
        if ((addr & 0x0080) == 0) {
            /* TIA write - pass clock for cycle-accurate rendering */
            tia_write(&g_tia, addr, data, g_cpu.clock);

            /* WSYNC or VSYNC frame-end: preempt CPU so frame loop handles it */
            if (g_tia.wsync_delay_clocks > 0 || g_tia.end_of_frame) {
                g_cpu.emulator_preempt_request = 1;
            }
        } else {
            /* PIA / RIOT I/O */
            pia_write(&g_pia, addr, data, g_cpu.clock);
        }
    } else {
        /* Cart (for bankswitching) */
        cart_write(&g_cart, addr, data);
    }
}

/* Memory read callback for 7800 */
static uint8_t mem_read_7800(uint16_t addr)
{
    /*
     * 7800 memory map - matches original EMU7800 Machine7800.cs
     * Uses 64-byte pages with specific device mappings:
     *
     * Maria: $0000-$003F, $0100-$013F, $0200-$023F, $0300-$033F
     * PIA:   $0280-$02FF, $0480-$04FF, $0580-$05FF
     * RAM0:  $1800-$1FFF
     * RAM1:  $2000-$27FF (primary)
     *        $0040-$00FF, $0140-$01FF (zero page/stack aliases)
     *        $2040-$20FF, $2140-$21FF (mirrors)
     *        $2800-$3FFF (mirrors)
     * Cart:  $4000-$FFFF
     */

    /* Cartridge $4000-$FFFF - check first for speed */
    if (addr >= 0x4000) {
        return cart_read(&g_cart, addr);
    }

    /* RAM mirrors $2800-$3FFF -> RAM1 */
    if (addr >= 0x2800 && addr < 0x4000) {
        return g_ram1_7800[addr & 0x7FF];
    }

    /* RAM1 primary $2000-$27FF */
    if (addr >= 0x2000 && addr < 0x2800) {
        return g_ram1_7800[addr & 0x7FF];
    }

    /* RAM0 primary $1800-$1FFF */
    if (addr >= 0x1800 && addr < 0x2000) {
        return g_ram0_7800[addr & 0x7FF];
    }

    /* Low memory $0000-$07FF - use page-based mapping */
    if (addr < 0x0800) {
        uint16_t page = addr >> 6;     /* 64-byte pages */
        uint8_t offset = addr & 0x3F;  /* offset within page */

        switch (page) {
            /* Maria pages: $0000-$003F, $0100-$013F, $0200-$023F, $0300-$033F */
            case 0:   /* $0000-$003F */
            case 4:   /* $0100-$013F */
            case 8:   /* $0200-$023F */
            case 12:  /* $0300-$033F */
                if (offset >= 0x08 && offset <= 0x0D) {
                    /* INPT0-5 ($08-$0D): Input registers handled by Maria on 7800 */
                    return maria_read(&g_maria, offset);
                } else if (offset < 0x20) {
                    /* Other TIA reads - limited on 7800, return 0 */
                    return 0;
                } else {
                    /* Maria registers $20-$3F */
                    return maria_read(&g_maria, offset);
                }

            /* RAM1 pages: $0040-$00FF (zero page), $0140-$01FF (stack) */
            /* Uses addr & 0x7FF to alias with $2000-$27FF */
            case 1:   /* $0040-$007F */
            case 2:   /* $0080-$00BF */
            case 3:   /* $00C0-$00FF */
                return g_ram1_7800[addr & 0x7FF];
            case 5:   /* $0140-$017F */
            case 6:   /* $0180-$01BF */
            case 7:   /* $01C0-$01FF */
                return g_ram1_7800[addr & 0x7FF];

            /* PIA pages: $0280-$02FF, $0480-$04FF, $0580-$05FF */
            case 10:  /* $0280-$02BF */
            case 11:  /* $02C0-$02FF */
            case 18:  /* $0480-$04BF */
            case 19:  /* $04C0-$04FF */
            case 22:  /* $0580-$05BF */
            case 23:  /* $05C0-$05FF */
                return pia_read(&g_pia, addr, g_cpu.clock);

            default:
                /* Unmapped areas return open bus / 0 */
                return 0;
        }
    }

    return 0;
}

/* Memory write callback for 7800 */
static void mem_write_7800(uint16_t addr, uint8_t data)
{
    /* Cartridge $4000-$FFFF - for bankswitching */
    if (addr >= 0x4000) {
        cart_write(&g_cart, addr, data);
        return;
    }

    /* RAM mirrors $2800-$3FFF -> RAM1 */
    if (addr >= 0x2800 && addr < 0x4000) {
        g_ram1_7800[addr & 0x7FF] = data;
        return;
    }

    /* RAM1 primary $2000-$27FF */
    if (addr >= 0x2000 && addr < 0x2800) {
        g_ram1_7800[addr & 0x7FF] = data;
        return;
    }

    /* RAM0 primary $1800-$1FFF */
    if (addr >= 0x1800 && addr < 0x2000) {
        g_ram0_7800[addr & 0x7FF] = data;
        return;
    }

    /* Low memory $0000-$07FF - use page-based mapping */
    if (addr < 0x0800) {
        uint16_t page = addr >> 6;     /* 64-byte pages */
        uint8_t offset = addr & 0x3F;  /* offset within page */

        switch (page) {
            /* Maria pages: $0000-$003F, $0100-$013F, $0200-$023F, $0300-$033F */
            case 0:   /* $0000-$003F */
            case 4:   /* $0100-$013F */
            case 8:   /* $0200-$023F */
            case 12:  /* $0300-$033F */
                if (offset < 0x20) {
                    /* TIA write - mainly for audio on 7800 */
                    if (offset >= 0x15 && offset <= 0x1a) {
                        /* Render audio up to current position before applying change */
                        tiasound_render_to_position(g_7800_current_scanline * 228);
                        tiasound_update(offset, data);
                    }
                } else {
                    /* Maria registers $20-$3F */
                    maria_write(&g_maria, offset, data);
                }
                return;

            /* RAM1 pages: $0040-$00FF (zero page), $0140-$01FF (stack) */
            /* Uses addr & 0x7FF to alias with $2000-$27FF */
            case 1:   /* $0040-$007F */
            case 2:   /* $0080-$00BF */
            case 3:   /* $00C0-$00FF */
                g_ram1_7800[addr & 0x7FF] = data;
                return;
            case 5:   /* $0140-$017F */
            case 6:   /* $0180-$01BF */
            case 7:   /* $01C0-$01FF */
                g_ram1_7800[addr & 0x7FF] = data;
                return;

            /* PIA pages: $0280-$02FF, $0480-$04FF, $0580-$05FF */
            case 10:  /* $0280-$02BF */
            case 11:  /* $02C0-$02FF */
            case 18:  /* $0480-$04BF */
            case 19:  /* $04C0-$04FF */
            case 22:  /* $0580-$05BF */
            case 23:  /* $05C0-$05FF */
                pia_write(&g_pia, addr, data, g_cpu.clock);
                return;

            default:
                /* Unmapped areas - writes are ignored */
                return;
        }
    }
}

/* Initialize the machine */
void machine_init(void)
{
    memset(g_ram_2600, 0, sizeof(g_ram_2600));
    memset(g_ram0_7800, 0, sizeof(g_ram0_7800));
    memset(g_ram1_7800, 0, sizeof(g_ram1_7800));
    /* Clear volatile input state without memset */
    g_joystick[0] = 0;
    g_joystick[1] = 0;
    g_trigger[0] = 0;
    g_trigger[1] = 0;
    g_trigger2[0] = 0;
    g_trigger2[1] = 0;
    g_switches = 0;

    m6502_init(&g_cpu, 1);
    tia_init(&g_tia);
    pia_init(&g_pia);
    cart_init(&g_cart);
    maria_init(&g_maria);
    tiasound_init(31440);  /* NTSC TIA audio rate: 3.58MHz / 114 */

    /* Set up Maria callbacks */
    maria_set_dma_read(maria_dma_read);
    maria_set_input_callbacks(maria_trigger_callback, maria_trigger2_callback);
    maria_set_cpu_preempt_callback(cpu_preempt_callback);
    maria_set_nmi_callback(nmi_callback);
    maria_set_portb_gate_callback(portb_gate_callback);

    g_rom_loaded = 0;
}

/* Shutdown the machine */
void machine_shutdown(void)
{
    cart_free(&g_cart);
    g_rom_loaded = 0;
}

/* Load a ROM file */
int machine_load_rom(const char *path, int machine_type)
{
    FILE *f;
    uint8_t *data;
    long size;

    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 512 * 1024) {
        fclose(f);
        return -2;
    }

    data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        return -3;
    }

    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -4;
    }
    fclose(f);

    /* Set machine type */
    g_machine_type = machine_type;

    /* Load cart */
    if (cart_load(&g_cart, data, (int)size, machine_type) != 0) {
        free(data);
        return -5;
    }

    free(data);

    /* Reset everything */
    machine_reset();
    g_rom_loaded = 1;

    /* Reset diagnostic log counters for the new ROM */
    pia_reset_log_counts();
    cart_reset_log_counts();
    tia_reset_sync_log();

    {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "===== ROM LOADED: %s (%s) =====",
            path, machine_type == MACHINE_2600 ? "2600" : "7800");
        log_msg(msg);
    }

    return 0;
}

/* Check if a ROM is loaded */
int machine_is_loaded(void)
{
    return g_rom_loaded;
}

/* Get current machine type */
int machine_get_type(void)
{
    return g_machine_type;
}

/* Reset the machine */
void machine_reset(void)
{
    if (g_machine_type == MACHINE_2600) {
        /* RAM is NOT cleared on reset — preserved across warm resets.
         * Reference Machine2600.Reset() does not clear address-space RAM.
         * machine_init() zeros RAM at construction time. */
        g_distinct_accesses = 0;
        g_last_access_addr = 0xFFFF;
        g_2600_frame_count = 0;  /* Reset so debug logging re-arms for frames [0,N] */

        tia_reset(&g_tia);
        pia_reset(&g_pia, 0);
        cart_reset(&g_cart);
        tiasound_reset();

        m6502_init(&g_cpu, 1);
        m6502_reset(&g_cpu, mem_read_2600);

        /* Sync TIA clock with CPU clock (TIA runs at 3x CPU) */
        g_tia.start_clock = g_cpu.clock * 3;
    } else {
        /* 7800 reset */
        memset(g_ram0_7800, 0, sizeof(g_ram0_7800));
        memset(g_ram1_7800, 0, sizeof(g_ram1_7800));

        maria_reset(&g_maria);
        pia_reset(&g_pia, 1);  /* 7800 mode */
        cart_reset(&g_cart);
        tiasound_reset();

        /* 7800 CPU runs at 1.79 MHz (vs 2600's 1.19 MHz) */
        /* Use run clocks multiple of 4 for proper DMA timing */
        m6502_init(&g_cpu, 4);
        g_nmi_delay_frames = 5;  /* Delay NMI to let game initialize */

        log_msg("7800 init: unlocking Maria registers");

        /* Enable Maria and lock INPTCTRL */
        maria_write(&g_maria, 0x01, 0x03);  /* INPTCTRL: lock=1, maria=1 */

        /* Enable MARIA diagnostic logging for first 5 frames */
        maria_enable_diagnostics(5);

        m6502_reset(&g_cpu, mem_read_7800);

        {
            char msg[256];
            uint8_t lo = mem_read_7800(0xFFFC);
            uint8_t hi = mem_read_7800(0xFFFD);
            snprintf(msg, sizeof(msg),
                "7800 init done: PC=$%04X (vector @FFFC=%02X%02X) cartType=%d",
                g_cpu.PC, hi, lo, g_cart.type);
            log_msg(msg);
        }
    }
}

/*
 * Run one frame - 2600 EMULATION
 *
 * EMU7800 model (Machine2600.ComputeNextFrame):
 * - CPU gets a full frame budget of (262 + 3) * 76 CPU cycles
 * - CPU runs freely; TIA catches up lazily on read/write
 * - WSYNC: TIA sets wsync_delay_clocks, frame loop advances CPU clock
 * - VSYNC toggle: TIA sets end_of_frame, frame loop breaks
 * - After loop: final TIA catch-up via tia_render_remaining
 */
static void machine_run_frame_2600(void)
{
    uint64_t frame_start_cpu = g_cpu.clock;

    /* Set debug frame counter BEFORE increment (0-based) */
    tia_dbg_set_frame(g_2600_frame_count);
    g_2600_frame_count++;

    /* Snapshot all input from the touch/SDL thread into frame-stable copies.
     * Must happen before tia_start_frame and any CPU emulation. */
    machine_sample_input();

    tia_start_frame(&g_tia);

    /* Force CPU-TIA synchronization at frame boundary */
    g_tia.start_clock = g_cpu.clock * 3;

    /* Set frame-start clock for audio positioning AFTER the force-sync */
    tia_set_frame_start_clock(g_tia.start_clock);

    TIA_DBG(TIA_DBG_FRAME,
        "FRAME_START #%d PC=$%04X cpu=%llu tia=%llu hsync=%d",
        g_2600_frame_count - 1, g_cpu.PC,
        (unsigned long long)g_cpu.clock,
        (unsigned long long)g_tia.start_clock, g_tia.hsync);

    /* Frame budget: generous so VSYNC naturally ends each frame */
    g_cpu.run_clocks = (FRAME_BUDGET_SCANLINES + 3) * CLOCKS_PER_SCANLINE_CPU;

    while (g_cpu.run_clocks > 0 && !g_cpu.jammed) {
        /* WSYNC: advance CPU clock by delay (in CPU cycles) */
        if (g_tia.wsync_delay_clocks > 0) {
            int delay_cpu = g_tia.wsync_delay_clocks / 3;
            g_cpu.clock += delay_cpu;
            g_cpu.run_clocks -= delay_cpu;
            g_tia.wsync_delay_clocks = 0;
        }

        /* VSYNC-driven frame end */
        if (g_tia.end_of_frame)
            break;

        m6502_execute(&g_cpu, mem_read_2600, mem_write_2600);
    }

    /* Final TIA catch-up — render any remaining clocks */
    tia_render_remaining(&g_tia, g_cpu.clock * 3);

    /* Frame-end summary */
    {
        uint64_t cpu_cycles = g_cpu.clock - frame_start_cpu;
        uint64_t tia_clocks = g_tia.start_clock - (frame_start_cpu * 3);
        int64_t drift = (int64_t)g_tia.start_clock - (int64_t)(g_cpu.clock * 3);
        TIA_DBG(TIA_DBG_FRAME,
            "FRAME_END #%d cpu_cyc=%llu tia_clk=%llu sl=%d hsync=%d PC=$%04X drift=%lld vbo=%d",
            g_2600_frame_count - 1, (unsigned long long)cpu_cycles,
            (unsigned long long)tia_clocks, g_tia.scanline,
            g_tia.hsync, g_cpu.PC, (long long)drift,
            g_tia.vblank_off_scanline);
    }

    tia_end_frame(&g_tia);
}

/*
 * Run one frame - 7800 MARIA-DRIVEN EMULATION
 * Based on EMU7800's Machine7800.ComputeNextFrame()
 *
 * Key changes vs previous version:
 * - RunClocks uses += (ADD) not = (SET) to preserve carry-over from over-execution
 * - WSYNC handled via emulator_preempt_request callback (CPU exits execute immediately)
 * - DMA overrun protection prevents negative run_clocks on heavy DMA scanlines
 * - DLI fires NMI directly from Maria's consume_next_dll_entry via callback
 */
static void machine_run_frame_7800(void)
{
    int scanline;
    int dma_clocks;
    int remaining_run_clocks;
    uint64_t start_of_scanline_clock;
    uint64_t remaining_cpu_clocks;

    /* Decrement NMI delay counter if active */
    if (g_nmi_delay_frames > 0) {
        g_nmi_delay_frames--;
    }

    /* Snapshot all input from the touch/SDL thread into frame-stable copies */
    machine_sample_input();

    maria_start_frame(&g_maria);
    tiasound_start_frame();

    for (scanline = 0; scanline < SCANLINES_PER_FRAME_7800; scanline++) {
        g_7800_current_scanline = scanline;
        start_of_scanline_clock = g_cpu.clock + (g_cpu.run_clocks / g_cpu.run_clocks_multiple);

        /* Phase 1: 7 initial CPU cycles (ADD to preserve carry) */
        g_cpu.run_clocks += 7 * g_cpu.run_clocks_multiple;
        remaining_run_clocks = (CLOCKS_PER_SCANLINE_7800 - 7) * g_cpu.run_clocks_multiple;
        m6502_execute(&g_cpu, mem_read_7800, mem_write_7800);

        /* Check WSYNC (set by callback during Execute) */
        if (g_cpu.emulator_preempt_request) {
            dma_clocks = maria_do_dma(&g_maria);
            remaining_cpu_clocks = CLOCKS_PER_SCANLINE_7800 - (g_cpu.clock - start_of_scanline_clock);
            g_cpu.clock += remaining_cpu_clocks;
            g_cpu.run_clocks = 0;
            g_maria.wsync = 0;
            continue;
        }

        /* Phase 2: DMA */
        dma_clocks = maria_do_dma(&g_maria);

        /* DMA overrun protection */
        while (g_cpu.run_clocks + remaining_run_clocks < dma_clocks) {
            dma_clocks >>= 1;
        }

        /* Align DMA clocks to multiple of 4 */
        if ((dma_clocks & 3) != 0) {
            dma_clocks += 4;
            dma_clocks -= dma_clocks & 3;
        }

        /* Account for DMA clocks */
        g_cpu.clock += dma_clocks / g_cpu.run_clocks_multiple;
        g_cpu.run_clocks -= dma_clocks;

        /* Phase 3: Rest of scanline (ADD to preserve carry) */
        g_cpu.run_clocks += remaining_run_clocks;
        m6502_execute(&g_cpu, mem_read_7800, mem_write_7800);

        /* Late WSYNC */
        if (g_cpu.emulator_preempt_request) {
            remaining_cpu_clocks = CLOCKS_PER_SCANLINE_7800 - (g_cpu.clock - start_of_scanline_clock);
            g_cpu.clock += remaining_cpu_clocks;
            g_cpu.run_clocks = 0;
            g_maria.wsync = 0;
        }
    }

    maria_end_frame(&g_maria);
    tiasound_end_frame();
    g_7800_frame_count++;
}

/* Run one frame - dispatch to appropriate handler */
void machine_run_frame(void)
{
    if (!g_rom_loaded) return;

    if (g_machine_type == MACHINE_2600) {
        machine_run_frame_2600();
    } else {
        machine_run_frame_7800();
    }
}

/* Get frame buffer - dispatch based on machine type */
uint8_t *machine_get_frame_buffer(void)
{
    if (g_machine_type == MACHINE_7800) {
        return maria_get_frame_buffer();
    }
    return tia_get_frame_buffer();
}

/* Get frame dimensions */
int machine_get_frame_width(void)
{
    if (g_machine_type == MACHINE_7800) {
        return 320;
    }
    return 160;
}

int machine_get_frame_height(void)
{
    if (g_machine_type == MACHINE_7800) {
        return 242;  /* Visible scanlines for 7800 */
    }
    return 192;  /* Visible scanlines for 2600 */
}

/* Get sound buffer */
int16_t *machine_get_sound_buffer(void)
{
    return tiasound_get_buffer();
}

int machine_get_sound_samples(void)
{
    return tiasound_get_buffer_samples();
}

/* Input sampling */
/*
 * Snapshot all input from the volatile layer into frame-sampled copies.
 * Called once at the start of each frame, before any CPU emulation.
 */
void machine_sample_input(void)
{
    g_frame_joystick[0] = g_joystick[0];
    g_frame_joystick[1] = g_joystick[1];
    g_frame_trigger_m[0] = g_trigger[0];
    g_frame_trigger_m[1] = g_trigger[1];
    g_frame_trigger2_m[0] = g_trigger2[0];
    g_frame_trigger2_m[1] = g_trigger2[1];
    g_frame_switches = g_switches;
}

/* All sampling functions return frame-sampled copies (stable within a frame) */

int machine_sample_joystick(int player, int direction)
{
    if (player < 0 || player > 1 || direction < 0 || direction > 3) {
        return 0;
    }
    return (g_frame_joystick[player] & (1 << direction)) != 0;
}

int machine_sample_trigger(int player)
{
    if (player < 0 || player > 1) return 0;
    return g_frame_trigger_m[player] != 0;
}

int machine_sample_trigger2(int player)
{
    if (player < 0 || player > 1) return 0;
    return g_frame_trigger2_m[player] != 0;
}

int machine_sample_switch(int sw)
{
    if (sw < 0 || sw > 3) return 0;
    return (g_frame_switches & (1 << sw)) != 0;
}

/* Set input state (called from SDL/input thread) */
void machine_set_joystick(int player, int direction, int pressed)
{
    if (player < 0 || player > 1 || direction < 0 || direction > 3) {
        return;
    }
    if (pressed) {
        g_joystick[player] |= (1 << direction);
    } else {
        g_joystick[player] &= ~(1 << direction);
    }
}

void machine_set_trigger(int player, int pressed)
{
    if (player < 0 || player > 1) return;
    g_trigger[player] = pressed ? 1 : 0;
}

void machine_set_trigger2(int player, int pressed)
{
    if (player < 0 || player > 1) return;
    g_trigger2[player] = pressed ? 1 : 0;
}

void machine_set_switch(int sw, int pressed)
{
    if (sw < 0 || sw > 3) return;
    if (pressed) {
        g_switches |= (1 << sw);
    } else {
        g_switches &= ~(1 << sw);
    }
}

/* Clear all input */
void machine_clear_input(void)
{
    g_joystick[0] = 0;
    g_joystick[1] = 0;
    g_trigger[0] = 0;
    g_trigger[1] = 0;
    g_trigger2[0] = 0;
    g_trigger2[1] = 0;
    g_switches = 0;
}

/* Supercharger support: get distinct access count */
uint32_t machine_get_distinct_accesses(void)
{
    return g_distinct_accesses;
}

/* Supercharger support: direct 2600 RAM peek (reads PIA RAM, same as CPU) */
uint8_t machine_peek_ram(uint16_t addr)
{
    return g_pia.ram[addr & 0x7F];
}

/* Supercharger support: direct 2600 RAM poke (writes PIA RAM, same as CPU) */
void machine_poke_ram(uint16_t addr, uint8_t data)
{
    g_pia.ram[addr & 0x7F] = data;
}

/* CPU clock accessor (for DPC music timing) */
uint64_t machine_get_cpu_clock(void) { return g_cpu.clock; }

/* Controller type accessors */
int machine_get_left_controller(void) { return g_cart.left_controller; }
int machine_get_right_controller(void) { return g_cart.right_controller; }

/* --- Save state accessors --- */

M6502 *machine_get_cpu(void) { return &g_cpu; }
TIA *machine_get_tia(void) { return &g_tia; }
PIA *machine_get_pia(void) { return &g_pia; }
Cart *machine_get_cart(void) { return &g_cart; }
Maria *machine_get_maria(void) { return &g_maria; }

uint8_t *machine_get_ram_2600(void) { return g_ram_2600; }
uint8_t *machine_get_ram0_7800(void) { return g_ram0_7800; }
uint8_t *machine_get_ram1_7800(void) { return g_ram1_7800; }

void machine_get_state_flags(int *nmi_delay_frames, int *frame_count_7800,
                             int *frame_count_2600, uint32_t *distinct_accesses)
{
    *nmi_delay_frames = g_nmi_delay_frames;
    *frame_count_7800 = g_7800_frame_count;
    *frame_count_2600 = g_2600_frame_count;
    *distinct_accesses = g_distinct_accesses;
}

void machine_set_state_flags(int nmi_delay_frames, int frame_count_7800,
                             int frame_count_2600, uint32_t distinct_accesses)
{
    g_nmi_delay_frames = nmi_delay_frames;
    g_7800_frame_count = frame_count_7800;
    g_2600_frame_count = frame_count_2600;
    g_distinct_accesses = distinct_accesses;
}

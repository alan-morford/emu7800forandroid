/*
 * maria.c
 *
 * Maria Graphics Chip (Atari 7800)
 * Based on EMU7800 by Mike Murphy
 * Derived from Dan Boris' work with 7800/MESS emulation
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdio.h>
#include "maria.h"
#include "cart.h"
#include "savestate.h"

/* External logging function */
extern void log_msg(const char *msg);

/* External controller type accessors (from machine.c) */
extern int machine_get_left_controller(void);
extern int machine_get_right_controller(void);

/* Maria register addresses */
#define INPTCTRL    0x01    /* Input port control (VBLANK in TIA) */
#define INPT0       0x08    /* Read pot port */
#define INPT1       0x09
#define INPT2       0x0a
#define INPT3       0x0b
#define INPT4       0x0c    /* Read P1 joystick trigger */
#define INPT5       0x0d    /* Read P2 joystick trigger */
#define AUDC0       0x15    /* TIA audio registers */
#define AUDC1       0x16
#define AUDF0       0x17
#define AUDF1       0x18
#define AUDV0       0x19
#define AUDV1       0x1a
#define BACKGRND    0x20    /* Background color */
#define P0C1        0x21    /* Palette 0, color 1 */
#define P0C2        0x22
#define P0C3        0x23
#define WSYNC       0x24    /* Wait for sync */
#define P1C1        0x25
#define P1C2        0x26
#define P1C3        0x27
#define MSTAT       0x28    /* Maria status */
#define P2C1        0x29
#define P2C2        0x2a
#define P2C3        0x2b
#define DPPH        0x2c    /* Display list list pointer high */
#define P3C1        0x2d
#define P3C2        0x2e
#define P3C3        0x2f
#define DPPL        0x30    /* Display list list pointer low */
#define P4C1        0x31
#define P4C2        0x32
#define P4C3        0x33
#define CHARBASE    0x34    /* Character base address */
#define P5C1        0x35
#define P5C2        0x36
#define P5C3        0x37
#define OFFSET      0x38    /* Reserved for future expansion */
#define P6C1        0x39
#define P6C2        0x3a
#define P6C3        0x3b
#define CTRL        0x3c    /* Maria control register */
#define P7C1        0x3d
#define P7C2        0x3e
#define P7C3        0x3f

/* Frame buffer for Maria output (320 pixels wide) */
#define MARIA_FB_WIDTH  320
#define MARIA_FB_HEIGHT 262
static uint8_t g_maria_frame_buffer[MARIA_FB_WIDTH * MARIA_FB_HEIGHT];
static uint8_t g_maria_display_buffer[MARIA_FB_WIDTH * MARIA_FB_HEIGHT];

/* Maria state */
static uint8_t g_line_ram[0x200];  /* Line RAM for building scanlines */
static uint8_t g_registers[0x40]; /* Maria registers */

/* Sampled input state - captured once at frame start for consistency */
static int g_frame_trigger[2];    /* Fire button state for this frame */
static int g_frame_trigger2[2];   /* Fire2 button state for this frame */

/* Input logging state - track changes to avoid spam */
static int g_last_logged_trigger[2] = {-1, -1};
static int g_last_logged_trigger2[2] = {-1, -1};
static int g_inpt_read_count = 0;

/* Display list processing state */
static uint16_t g_dll;        /* Display List List pointer */
static uint16_t g_dl;         /* Current Display List pointer */
static int g_offset;          /* Zone offset counter */
static int g_holey;           /* Holey DMA mode */
static int g_width;           /* Object width */
static uint8_t g_hpos;        /* Horizontal position */
static int g_palette_no;      /* Current palette number */
static int g_ind_mode;        /* Indirect mode flag */
static int g_wm;              /* Write mode flag */

/* Maria control register flags */
static int g_dma_enabled;
static int g_color_kill;
static int g_cwidth;          /* Character width (indirect mode) */
static int g_bcntl;           /* Border control */
static int g_kangaroo;        /* Kangaroo mode */
static uint8_t g_rm;          /* Read mode (0-3) */

static int g_ctrl_lock;       /* Control register lock */
static int g_scanline;        /* Current scanline */
static int g_dli;             /* DLI flag for current zone */
static int g_dli_pending;     /* DLI pending signal (zone with DLI completed) */

/* Visible scanline range */
#define FIRST_VISIBLE_SCANLINE  11
#define LAST_VISIBLE_SCANLINE   253  /* 11 + 242 */

/* External memory read function (to be set by machine.c) */
static uint8_t (*g_dma_read)(uint16_t addr) = NULL;

/* External input sampling functions */
static int (*g_sample_trigger)(int player) = NULL;
static int (*g_sample_trigger2)(int player) = NULL;

/* CPU preempt callback (for WSYNC) */
static void (*g_cpu_preempt)(void) = NULL;

/* NMI callback (for DLI) */
static void (*g_nmi_callback)(void) = NULL;

/* PIA Port B gate callback for ProLine two-button INPT4/5 masking */
static int (*g_portb_gate)(int) = NULL;

/* Module-level DMA clock counter (so consume_next_dll_entry can add to it) */
static int g_dma_clocks = 0;

/* Frame counter for Maria */
static int g_maria_frame_count = 0;

/* Diagnostic: log DMA activity for first N frames after DMA is first enabled */
static int g_diag_frames_left = 0;
#define DIAG_FRAMES 5

/* Enable diagnostic logging for a specified number of frames */
void maria_enable_diagnostics(int frames)
{
    g_diag_frames_left = frames;
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "MARIA DIAG: enabled for %d frames", frames);
        log_msg(msg);
    }
}

/* Forward declarations */
static void build_line_ram(void);
static void output_line_ram(int scanline);
static void consume_next_dll_entry(void);
static void build_line_ram_160a(uint16_t graphaddr);
static void build_line_ram_160b(uint16_t graphaddr);
static void build_line_ram_320a(uint16_t graphaddr);
static void build_line_ram_320b(uint16_t graphaddr);
static void build_line_ram_320c(uint16_t graphaddr);
static void build_line_ram_320d(uint16_t graphaddr);

/* Set DMA read callback */
void maria_set_dma_read(uint8_t (*read_func)(uint16_t))
{
    g_dma_read = read_func;
}

/* Set input callbacks */
void maria_set_input_callbacks(int (*trigger)(int), int (*trigger2)(int))
{
    g_sample_trigger = trigger;
    g_sample_trigger2 = trigger2;
}


/* Set CPU preempt callback for WSYNC */
void maria_set_cpu_preempt_callback(void (*preempt_func)(void))
{
    g_cpu_preempt = preempt_func;
}

/* Set NMI callback for DLI */
void maria_set_nmi_callback(void (*nmi_func)(void))
{
    g_nmi_callback = nmi_func;
}

/* Set PIA Port B gate callback for ProLine two-button INPT4/5 masking */
void maria_set_portb_gate_callback(int (*gate_func)(int playerNo))
{
    g_portb_gate = gate_func;
}

/* Initialize Maria */
void maria_init(Maria *maria)
{
    memset(maria, 0, sizeof(Maria));
    memset(g_line_ram, 0, sizeof(g_line_ram));
    memset(g_registers, 0, sizeof(g_registers));
    memset(g_maria_frame_buffer, 0, sizeof(g_maria_frame_buffer));
    memset(g_maria_display_buffer, 0, sizeof(g_maria_display_buffer));

    g_dma_enabled = 0;
    g_color_kill = 0;
    g_cwidth = 0;
    g_bcntl = 0;
    g_kangaroo = 0;
    g_rm = 0;
    g_ctrl_lock = 0;
    g_scanline = 0;
    g_dli = 0;
    g_dli_pending = 0;
}

/* Reset Maria */
void maria_reset(Maria *maria)
{
    memset(g_registers, 0, sizeof(g_registers));
    memset(g_line_ram, 0, sizeof(g_line_ram));

    g_dma_enabled = 0;
    g_color_kill = 0;
    g_cwidth = 0;
    g_bcntl = 0;
    g_kangaroo = 0;
    g_rm = 0;
    g_ctrl_lock = 0;
    g_scanline = 0;
    g_dli = 0;
    g_dli_pending = 0;

    maria->scanline = 0;
    maria->wsync = 0;
}

/* Read from Maria register */
uint8_t maria_read(Maria *maria, uint16_t addr)
{
    char msg[128];
    uint8_t result;

    addr &= 0x3f;

    switch (addr) {
        case MSTAT:
            /* Return VBLANK status in bit 7 */
            if (g_scanline < FIRST_VISIBLE_SCANLINE ||
                g_scanline >= LAST_VISIBLE_SCANLINE) {
                return 0x80;  /* VBLANK ON */
            }
            return 0x00;  /* VBLANK OFF */

        case INPT0:
            /* Player 1 right button (active high - bit 7 = 1 when pressed) */
            /* Use sampled frame state for consistent reads within a frame */
            return g_frame_trigger[0] ? 0x80 : 0x00;

        case INPT1:
            /* Player 1 left button (active high - bit 7 = 1 when pressed) */
            return g_frame_trigger2[0] ? 0x80 : 0x00;

        case INPT2:
            /* Player 2 right button (active high - bit 7 = 1 when pressed) */
            return g_frame_trigger[1] ? 0x80 : 0x00;

        case INPT3:
            /* Player 2 left button (active high - bit 7 = 1 when pressed) */
            return g_frame_trigger2[1] ? 0x80 : 0x00;

        case INPT4:
            /*
             * Player 1 fire (active low for 7800 ProLine).
             * Either button press = fire, UNLESS PIA Port B gates it off.
             *
             * Reference (Maria.cs SampleINPTLatched):
             * When PIA DDRB has the player's bit set as output AND
             * WrittenPortB has that bit driven low, INPT4/5 returns
             * "not pressed" (0x80). This is how two-button games
             * differentiate buttons: they mask INPT4 via PIA, then
             * read INPT0/INPT1 individually.
             *
             * For light gun: returns light-detection result.
             * 0x80 = no light detected (idle state with no gun).
             */
            if (machine_get_left_controller() == CTRL_LIGHTGUN)
                return 0x80;  /* No light detected */
            if (g_portb_gate && g_portb_gate(0))
                return 0x80;
            return (g_frame_trigger[0] || g_frame_trigger2[0]) ? 0x00 : 0x80;

        case INPT5:
            /* Player 2 fire - same PIA Port B gating as INPT4 */
            if (machine_get_right_controller() == CTRL_LIGHTGUN)
                return 0x80;  /* No light detected */
            if (g_portb_gate && g_portb_gate(1))
                return 0x80;
            return (g_frame_trigger[1] || g_frame_trigger2[1]) ? 0x00 : 0x80;

        default:
            return g_registers[addr];
    }
}

/* Write to Maria register */
void maria_write(Maria *maria, uint16_t addr, uint8_t data)
{
    addr &= 0x3f;

    switch (addr) {
        case INPTCTRL:
            if (g_ctrl_lock) {
                /* Ignore writes when locked */
                break;
            }
            g_ctrl_lock = (data & 0x01) != 0;
            /* Bit 1: Maria enable */
            /* Bit 3: TIA output enable */
            break;

        case WSYNC:
            /* Request WSYNC - CPU will halt until end of scanline */
            maria->wsync = 1;
            if (g_cpu_preempt) {
                g_cpu_preempt();
            }
            if (g_diag_frames_left > 0 && g_scanline < 20) {
                char msg[64];
                snprintf(msg, sizeof(msg), "MARIA WSYNC sl=%d frm=%d", g_scanline, g_maria_frame_count);
                log_msg(msg);
            }
            break;

        case CTRL:
            {
                int was_dma = g_dma_enabled;
                g_registers[CTRL] = data;
                g_color_kill = (data & 0x80) != 0;
                g_dma_enabled = (data & 0x60) == 0x40;
                g_cwidth = (data & 0x10) != 0;
                g_bcntl = (data & 0x08) != 0;
                g_kangaroo = (data & 0x04) != 0;
                g_rm = data & 0x03;
                /* Log CTRL writes for first few frames, or any DMA enable transition */
                if (g_diag_frames_left > 0 || (!was_dma && g_dma_enabled)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "MARIA CTRL=$%02X dma=%d ck=%d cw=%d bc=%d kang=%d rm=%d sl=%d frm=%d",
                        data, g_dma_enabled, g_color_kill, g_cwidth, g_bcntl, g_kangaroo, g_rm,
                        g_scanline, g_maria_frame_count);
                    log_msg(msg);
                    if (!was_dma && g_dma_enabled && g_diag_frames_left == 0) {
                        g_diag_frames_left = DIAG_FRAMES;
                        log_msg("MARIA DIAG: DMA first enabled, starting diagnostic logging");
                    }
                }
            }
            break;

        case MSTAT:
            /* Read-only register */
            break;

        case CHARBASE:
        case DPPH:
        case DPPL:
            g_registers[addr] = data;
            if (g_diag_frames_left > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "MARIA %s=$%02X (DPP=$%04X CHARBASE=$%02X) sl=%d frm=%d",
                    addr == DPPH ? "DPPH" : addr == DPPL ? "DPPL" : "CHARBASE",
                    data,
                    (g_registers[DPPL] | (g_registers[DPPH] << 8)),
                    g_registers[CHARBASE],
                    g_scanline, g_maria_frame_count);
                log_msg(msg);
            }
            break;

        case BACKGRND:
        case P0C1: case P0C2: case P0C3:
        case P1C1: case P1C2: case P1C3:
        case P2C1: case P2C2: case P2C3:
        case P3C1: case P3C2: case P3C3:
        case P4C1: case P4C2: case P4C3:
        case P5C1: case P5C2: case P5C3:
        case P6C1: case P6C2: case P6C3:
        case P7C1: case P7C2: case P7C3:
            g_registers[addr] = data;
            if (g_diag_frames_left > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "MARIA PAL reg$%02X=$%02X sl=%d frm=%d",
                    addr, data, g_scanline, g_maria_frame_count);
                log_msg(msg);
            }
            break;

        case AUDC0:
        case AUDC1:
        case AUDF0:
        case AUDF1:
        case AUDV0:
        case AUDV1:
            /* Pass to TIA sound */
            g_registers[addr] = data;
            /* tiasound_update(addr, data); - called from machine.c */
            break;

        case OFFSET:
            /* Reserved for future expansion - ignore */
            break;

        default:
            g_registers[addr] = data;
            break;
    }
}

/* Start a new frame */
void maria_start_frame(Maria *maria)
{
    g_scanline = 0;
    maria->scanline = 0;
    memset(g_maria_frame_buffer, 0, sizeof(g_maria_frame_buffer));

    /*
     * Sample input state at frame start for consistency.
     * This ensures INPT reads return the same value throughout the frame,
     * preventing mid-frame input changes from confusing game logic.
     */
    g_frame_trigger[0] = g_sample_trigger ? g_sample_trigger(0) : 0;
    g_frame_trigger[1] = g_sample_trigger ? g_sample_trigger(1) : 0;
    g_frame_trigger2[0] = g_sample_trigger2 ? g_sample_trigger2(0) : 0;
    g_frame_trigger2[1] = g_sample_trigger2 ? g_sample_trigger2(1) : 0;
}

/* Process DMA for current scanline */
int maria_do_dma(Maria *maria)
{
    g_dma_clocks = 0;

    /* Output previous line's LineRAM */
    output_line_ram(g_scanline);

    /* Skip if DMA disabled or outside visible area */
    if (!g_dma_enabled || g_scanline < FIRST_VISIBLE_SCANLINE ||
        g_scanline >= LAST_VISIBLE_SCANLINE) {
        g_scanline++;
        maria->scanline = g_scanline;
        return 0;
    }

    /* First visible scanline - initialize DLL */
    if (g_scanline == FIRST_VISIBLE_SCANLINE) {
        /* DMA Startup + long shutdown */
        g_dma_clocks += 15;

        g_dll = (uint16_t)(g_registers[DPPL] | (g_registers[DPPH] << 8));

        if (g_diag_frames_left > 0) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                "MARIA DMA START frm=%d DPP=$%04X CTRL=$%02X dma=%d rm=%d",
                g_maria_frame_count, g_dll, g_registers[CTRL], g_dma_enabled, g_rm);
            log_msg(msg);
        }

        consume_next_dll_entry();
    }

    /* DMA Startup */
    g_dma_clocks += 5;

    /* Build this scanline */
    build_line_ram();

    /* Check if zone is complete */
    if (--g_offset < 0) {
        /* Zone complete - get next DLL entry (DLI fires in consume_next_dll_entry) */
        consume_next_dll_entry();
        /* DMA Shutdown: Last line of zone */
        g_dma_clocks += 10;
    } else {
        /* DMA Shutdown: Other line of zone */
        g_dma_clocks += 4;
    }

    /* Log per-scanline DMA clocks for diagnostic frames */
    if (g_diag_frames_left > 0 && g_scanline >= FIRST_VISIBLE_SCANLINE &&
        g_scanline < FIRST_VISIBLE_SCANLINE + 20) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "MARIA DMA sl=%d clks=%d dll=$%04X dl=$%04X off=%d",
            g_scanline, g_dma_clocks, g_dll, g_dl, g_offset);
        log_msg(msg);
    }

    g_scanline++;
    maria->scanline = g_scanline;

    return g_dma_clocks;
}

/* End the current frame */
void maria_end_frame(Maria *maria)
{
    memcpy(g_maria_display_buffer, g_maria_frame_buffer, sizeof(g_maria_frame_buffer));

    if (g_diag_frames_left > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "MARIA END frm=%d dma_en=%d CTRL=$%02X DPP=$%04X",
            g_maria_frame_count, g_dma_enabled, g_registers[CTRL],
            (g_registers[DPPL] | (g_registers[DPPH] << 8)));
        log_msg(msg);
        g_diag_frames_left--;
    }

    g_maria_frame_count++;
    (void)maria;
}

/* Get Maria frame buffer */
uint8_t *maria_get_frame_buffer(void)
{
    return g_maria_display_buffer;
}

/* Stubs — kept for API compat; synchronization now handled by
 * g_frame_ready + memory barriers in main.c */
int maria_frame_ready(void)
{
    return 0;
}

void maria_frame_consumed(void)
{
}

/* (maria_check_and_clear_dli removed - DLI now fires NMI directly via callback) */

/* Consume next Display List List entry */
static void consume_next_dll_entry(void)
{
    uint8_t dll0, dll1, dll2;
    int dli;

    if (!g_dma_read) return;

    /* Read DLL entry (3 bytes) */
    dll0 = g_dma_read(g_dll++);  /* DLI, Holey, Offset */
    dll1 = g_dma_read(g_dll++);  /* High DL address */
    dll2 = g_dma_read(g_dll++);  /* Low DL address */

    dli = (dll0 & 0x80) != 0;
    g_holey = (dll0 & 0x60) >> 5;
    g_offset = dll0 & 0x0f;

    /* Update current Display List pointer */
    g_dl = (uint16_t)(dll2 | (dll1 << 8));

    /* Log DLL entries during diagnostic frames */
    if (g_diag_frames_left > 0) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "MARIA DLL @$%04X: [%02X %02X %02X] -> DL=$%04X off=%d holey=%d dli=%d sl=%d",
            (uint16_t)(g_dll - 3), dll0, dll1, dll2, g_dl, g_offset, g_holey, dli, g_scanline);
        log_msg(msg);
    }

    if (dli) {
        g_dma_clocks += 1;
        if (g_nmi_callback) {
            g_nmi_callback();
        }
    }
}

/* Build line RAM from display list */
static void build_line_ram(void)
{
    uint16_t dl = g_dl;
    uint8_t mode_byte;
    uint16_t graphaddr;
    uint8_t dl0, dl1, dl2, dl3, dl4;

    if (!g_dma_read) return;

    /* Clear line RAM to background */
    memset(g_line_ram, 0, sizeof(g_line_ram));

    /* Iterate through Display List */
    while (1) {
        mode_byte = g_dma_read(dl + 1);
        if ((mode_byte & 0x5f) == 0) {
            break;  /* End of display list */
        }

        g_ind_mode = 0;

        if ((mode_byte & 0x1f) == 0) {
            /* Extended DL header (5 bytes) */
            dl0 = g_dma_read(dl++);  /* Low address */
            dl1 = g_dma_read(dl++);  /* Mode */
            dl2 = g_dma_read(dl++);  /* High address */
            dl3 = g_dma_read(dl++);  /* Palette/width */
            dl4 = g_dma_read(dl++);  /* Horizontal position */

            graphaddr = (uint16_t)(dl0 | (dl2 << 8));
            g_wm = (dl1 & 0x80) != 0;
            g_ind_mode = (dl1 & 0x20) != 0;
            g_palette_no = (dl3 & 0xe0) >> 3;
            g_width = (~dl3 & 0x1f) + 1;
            g_hpos = dl4;

            /* DMA TIMING: DL 5-byte header */
            g_dma_clocks += 10;
        } else {
            /* Normal DL header (4 bytes) */
            dl0 = g_dma_read(dl++);  /* Low address */
            dl1 = g_dma_read(dl++);  /* Palette/width */
            dl2 = g_dma_read(dl++);  /* High address */
            dl3 = g_dma_read(dl++);  /* Horizontal position */

            graphaddr = (uint16_t)(dl0 | (dl2 << 8));
            g_palette_no = (dl1 & 0xe0) >> 3;
            g_width = (~dl1 & 0x1f) + 1;
            g_hpos = dl3;

            /* DMA TIMING: DL 4-byte header */
            g_dma_clocks += 8;
        }

        /* DMA TIMING: Graphic reads (matching reference Maria.cs) */
        if (g_rm != 1) {
            g_dma_clocks += g_width * (g_ind_mode ? (g_cwidth ? 9 : 6) : 3);
        }

        /* Render based on mode */
        switch (g_rm) {
            case 0:
                if (g_wm)
                    build_line_ram_160b(graphaddr);
                else
                    build_line_ram_160a(graphaddr);
                break;
            case 1:
                /* Mode 1 = blank/skip */
                continue;
            case 2:
                if (g_wm)
                    build_line_ram_320b(graphaddr);
                else
                    build_line_ram_320d(graphaddr);
                break;
            case 3:
                if (g_wm)
                    build_line_ram_320c(graphaddr);
                else
                    build_line_ram_320a(graphaddr);
                break;
        }
    }
}

/* Output line RAM to frame buffer */
static void output_line_ram(int scanline)
{
    int i;
    int fbi;
    uint8_t color_index;
    uint8_t final_color;

    if (scanline < 0 || scanline >= MARIA_FB_HEIGHT) return;

    fbi = scanline * MARIA_FB_WIDTH;

    for (i = 0; i < MARIA_FB_WIDTH; i++) {
        color_index = g_line_ram[i];

        if ((color_index & 3) == 0) {
            final_color = g_registers[BACKGRND];
        } else {
            final_color = g_registers[BACKGRND + color_index];
        }

        g_maria_frame_buffer[fbi++] = final_color;

        /* Clear line RAM as we go */
        g_line_ram[i] = 0;
    }
}

/* 160A mode - 4 pixels per byte, 2 bits per pixel */
static void build_line_ram_160a(uint16_t graphaddr)
{
    int indbytes = (g_ind_mode && g_cwidth) ? 2 : 1;
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, j, c, d;
    uint8_t val;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        for (j = 0; j < indbytes; j++) {
            /* Check holey DMA */
            if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
                (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
                hpos += 8;
                dataaddr++;
                continue;
            }

            d = g_dma_read(dataaddr++);

            /* Pixel 0 (bits 7-6) */
            c = (d & 0xc0) >> 6;
            if (c != 0) {
                val = (uint8_t)(g_palette_no | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            }
            hpos += 2;

            /* Pixel 1 (bits 5-4) */
            c = (d & 0x30) >> 4;
            if (c != 0) {
                val = (uint8_t)(g_palette_no | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            }
            hpos += 2;

            /* Pixel 2 (bits 3-2) */
            c = (d & 0x0c) >> 2;
            if (c != 0) {
                val = (uint8_t)(g_palette_no | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            }
            hpos += 2;

            /* Pixel 3 (bits 1-0) */
            c = d & 0x03;
            if (c != 0) {
                val = (uint8_t)(g_palette_no | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            }
            hpos += 2;
        }
    }
}

/* 160B mode - write mode, 2 pixels per byte with embedded palette */
static void build_line_ram_160b(uint16_t graphaddr)
{
    int indbytes = (g_ind_mode && g_cwidth) ? 2 : 1;
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, j, c, d, p;
    uint8_t val;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        for (j = 0; j < indbytes; j++) {
            /* Check holey DMA */
            if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
                (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
                hpos += 4;
                dataaddr++;
                continue;
            }

            d = g_dma_read(dataaddr++);

            /* Pixel 0 */
            c = (d & 0xc0) >> 6;
            if (c != 0) {
                p = ((g_palette_no >> 2) & 0x04) | ((d & 0x0c) >> 2);
                val = (uint8_t)((p << 2) | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
                g_line_ram[(hpos + 1) & 0x1ff] = 0;
            }
            hpos += 2;

            /* Pixel 1 */
            c = (d & 0x30) >> 4;
            if (c != 0) {
                p = ((g_palette_no >> 2) & 0x04) | (d & 0x03);
                val = (uint8_t)((p << 2) | c);
                g_line_ram[hpos & 0x1ff] = val;
                g_line_ram[(hpos + 1) & 0x1ff] = val;
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
                g_line_ram[(hpos + 1) & 0x1ff] = 0;
            }
            hpos += 2;
        }
    }
}

/* 320A mode - 8 pixels per byte, 1 bit per pixel */
static void build_line_ram_320a(uint16_t graphaddr)
{
    uint8_t color = (uint8_t)(g_palette_no | 2);
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, d;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        /* Check holey DMA */
        if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
            (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
            hpos += 8;
            dataaddr++;
            continue;
        }

        d = g_dma_read(dataaddr++);

        if (d & 0x80) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x40) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x20) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x10) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x08) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x04) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x02) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x01) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;
    }
}

/* 320B mode - write mode, 4 pixels per byte with 2 bits per pixel */
static void build_line_ram_320b(uint16_t graphaddr)
{
    int indbytes = (g_ind_mode && g_cwidth) ? 2 : 1;
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, j, c, d;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        for (j = 0; j < indbytes; j++) {
            /* Check holey DMA */
            if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
                (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
                hpos += 4;
                dataaddr++;
                continue;
            }

            d = g_dma_read(dataaddr++);

            c = ((d & 0x80) >> 6) | ((d & 0x08) >> 3);
            if (c != 0) {
                if ((d & 0xc0) != 0 || g_kangaroo)
                    g_line_ram[hpos & 0x1ff] = (uint8_t)(g_palette_no | c);
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
            } else if ((d & 0xcc) != 0) {
                g_line_ram[hpos & 0x1ff] = 0;
            }
            hpos++;

            c = ((d & 0x40) >> 5) | ((d & 0x04) >> 2);
            if (c != 0) {
                if ((d & 0xc0) != 0 || g_kangaroo)
                    g_line_ram[hpos & 0x1ff] = (uint8_t)(g_palette_no | c);
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
            } else if ((d & 0xcc) != 0) {
                g_line_ram[hpos & 0x1ff] = 0;
            }
            hpos++;

            c = ((d & 0x20) >> 4) | ((d & 0x02) >> 1);
            if (c != 0) {
                if ((d & 0x30) != 0 || g_kangaroo)
                    g_line_ram[hpos & 0x1ff] = (uint8_t)(g_palette_no | c);
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
            } else if ((d & 0x33) != 0) {
                g_line_ram[hpos & 0x1ff] = 0;
            }
            hpos++;

            c = ((d & 0x10) >> 3) | (d & 0x01);
            if (c != 0) {
                if ((d & 0x30) != 0 || g_kangaroo)
                    g_line_ram[hpos & 0x1ff] = (uint8_t)(g_palette_no | c);
            } else if (g_kangaroo) {
                g_line_ram[hpos & 0x1ff] = 0;
            } else if ((d & 0x33) != 0) {
                g_line_ram[hpos & 0x1ff] = 0;
            }
            hpos++;
        }
    }
}

/* 320C mode - write mode with embedded palette */
static void build_line_ram_320c(uint16_t graphaddr)
{
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, d;
    uint8_t color;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        /* Check holey DMA */
        if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
            (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
            hpos += 4;
            dataaddr++;
            continue;
        }

        d = g_dma_read(dataaddr++);

        color = (uint8_t)(((((d & 0x0c) >> 2) | ((g_palette_no >> 2) & 0x04)) << 2) | 2);

        if (d & 0x80) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x40) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        color = (uint8_t)((((d & 0x03) | ((g_palette_no >> 2) & 0x04)) << 2) | 2);

        if (d & 0x20) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;

        if (d & 0x10) g_line_ram[hpos & 0x1ff] = color;
        else if (g_kangaroo) g_line_ram[hpos & 0x1ff] = 0;
        hpos++;
    }
}

/* 320D mode */
static void build_line_ram_320d(uint16_t graphaddr)
{
    int indbytes = (g_ind_mode && g_cwidth) ? 2 : 1;
    int hpos = g_hpos << 1;
    uint16_t dataaddr = graphaddr + (g_offset << 8);
    int i, j, c, d;

    for (i = 0; i < g_width; i++) {
        if (g_ind_mode) {
            dataaddr = g_dma_read(graphaddr + i) |
                      ((g_registers[CHARBASE] + g_offset) << 8);
        }

        for (j = 0; j < indbytes; j++) {
            /* Check holey DMA */
            if ((g_holey == 0x02 && (dataaddr & 0x9000) == 0x9000) ||
                (g_holey == 0x01 && (dataaddr & 0x8800) == 0x8800)) {
                hpos += 8;
                dataaddr++;
                continue;
            }

            d = g_dma_read(dataaddr++);

            c = ((d & 0x80) >> 6) | (((g_palette_no >> 2) & 2) >> 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x40) >> 5) | ((g_palette_no >> 2) & 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x20) >> 4) | (((g_palette_no >> 2) & 2) >> 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x10) >> 3) | ((g_palette_no >> 2) & 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x08) >> 2) | (((g_palette_no >> 2) & 2) >> 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x04) >> 1) | ((g_palette_no >> 2) & 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = (d & 0x02) | (((g_palette_no >> 2) & 2) >> 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;

            c = ((d & 0x01) << 1) | ((g_palette_no >> 2) & 1);
            if (c != 0)
                g_line_ram[hpos & 0x1ff] = (uint8_t)((g_palette_no & 0x10) | c);
            else if (g_kangaroo)
                g_line_ram[hpos & 0x1ff] = 0;
            hpos++;
        }
    }
}

/* Save state: get all internal statics */
void maria_get_internal_state(MariaInternalState *s)
{
    memcpy(s->line_ram, g_line_ram, sizeof(g_line_ram));
    memcpy(s->registers, g_registers, sizeof(g_registers));
    s->dll = g_dll;
    s->dl = g_dl;
    s->offset = g_offset;
    s->holey = g_holey;
    s->width = g_width;
    s->hpos = g_hpos;
    s->palette_no = g_palette_no;
    s->ind_mode = g_ind_mode;
    s->wm = g_wm;
    s->dma_enabled = g_dma_enabled;
    s->color_kill = g_color_kill;
    s->cwidth = g_cwidth;
    s->bcntl = g_bcntl;
    s->kangaroo = g_kangaroo;
    s->rm = g_rm;
    s->ctrl_lock = g_ctrl_lock;
    s->scanline = g_scanline;
    s->dli = g_dli;
    s->dli_pending = g_dli_pending;
    s->dma_clocks = g_dma_clocks;
    s->maria_frame_count = g_maria_frame_count;
}

/* Save state: set all internal statics */
void maria_set_internal_state(const MariaInternalState *s)
{
    memcpy(g_line_ram, s->line_ram, sizeof(g_line_ram));
    memcpy(g_registers, s->registers, sizeof(g_registers));
    g_dll = s->dll;
    g_dl = s->dl;
    g_offset = s->offset;
    g_holey = s->holey;
    g_width = s->width;
    g_hpos = s->hpos;
    g_palette_no = s->palette_no;
    g_ind_mode = s->ind_mode;
    g_wm = s->wm;
    g_dma_enabled = s->dma_enabled;
    g_color_kill = s->color_kill;
    g_cwidth = s->cwidth;
    g_bcntl = s->bcntl;
    g_kangaroo = s->kangaroo;
    g_rm = s->rm;
    g_ctrl_lock = s->ctrl_lock;
    g_scanline = s->scanline;
    g_dli = s->dli;
    g_dli_pending = s->dli_pending;
    g_dma_clocks = s->dma_clocks;
    g_maria_frame_count = s->maria_frame_count;
}

/* 7800 NTSC palette - A7800 NTSC LCD V1 STD-WARM (Trebor Standard) */
const uint32_t maria_ntsc_palette[256] = {
    0x000000, 0x050505, 0x171717, 0x292929, 0x3b3b3b, 0x4c4c4c, 0x5e5e5e, 0x707070,
    0x828282, 0x949494, 0xa6a6a6, 0xb8b8b8, 0xc9c9c9, 0xdbdbdb, 0xededed, 0xffffff,
    0x000d00, 0x0e1e00, 0x203000, 0x324200, 0x445400, 0x566600, 0x677800, 0x798a00,
    0x8b9b00, 0x9dad00, 0xafbf0e, 0xc1d120, 0xd3e331, 0xe4f543, 0xf6ff55, 0xffff67,
    0x220000, 0x340600, 0x461800, 0x582a00, 0x6a3c00, 0x7c4e00, 0x8d6000, 0x9f7200,
    0xb18309, 0xc3951b, 0xd5a72c, 0xe7b93e, 0xf9cb50, 0xffdd62, 0xffef74, 0xffff86,
    0x3e0000, 0x500000, 0x620000, 0x741200, 0x862400, 0x98360c, 0xa9471e, 0xbb5930,
    0xcd6b41, 0xdf7d53, 0xf18f65, 0xffa177, 0xffb389, 0xffc49b, 0xffd6ad, 0xffe8be,
    0x4a0000, 0x5c000b, 0x6e001d, 0x80002f, 0x921041, 0xa42252, 0xb53464, 0xc74676,
    0xd95888, 0xeb6a9a, 0xfd7bac, 0xff8dbe, 0xff9fcf, 0xffb1e1, 0xffc3f3, 0xffd5ff,
    0x44003e, 0x550050, 0x670062, 0x790074, 0x8b0686, 0x9d1898, 0xaf2aaa, 0xc13cbb,
    0xd24dcd, 0xe45fdf, 0xf671f1, 0xff83ff, 0xff95ff, 0xffa7ff, 0xffb8ff, 0xffcaff,
    0x2c0074, 0x3e0085, 0x4f0097, 0x6100a9, 0x7307bb, 0x8519cd, 0x972bdf, 0xa93cf1,
    0xbb4eff, 0xcc60ff, 0xde72ff, 0xf084ff, 0xff96ff, 0xffa8ff, 0xffb9ff, 0xffcbff,
    0x08008d, 0x1a009f, 0x2b00b1, 0x3d01c3, 0x4f13d5, 0x6125e7, 0x7337f8, 0x8548ff,
    0x975aff, 0xa86cff, 0xba7eff, 0xcc90ff, 0xdea2ff, 0xf0b4ff, 0xffc5ff, 0xffd7ff,
    0x000086, 0x000098, 0x0304aa, 0x1516bc, 0x2727ce, 0x3939df, 0x4b4bf1, 0x5c5dff,
    0x6e6fff, 0x8081ff, 0x9292ff, 0xa4a4ff, 0xb6b6ff, 0xc7c8ff, 0xd9daff, 0xebecff,
    0x000060, 0x000a71, 0x001c83, 0x002e95, 0x0340a7, 0x1552b9, 0x2664cb, 0x3876dc,
    0x4a87ee, 0x5c99ff, 0x6eabff, 0x80bdff, 0x92cfff, 0xa3e1ff, 0xb5f2ff, 0xc7ffff,
    0x001022, 0x002234, 0x003445, 0x004657, 0x005769, 0x00697b, 0x0e7b8d, 0x208d9f,
    0x329fb1, 0x44b1c2, 0x56c3d4, 0x68d4e6, 0x79e6f8, 0x8bf8ff, 0x9dffff, 0xafffff,
    0x002100, 0x003300, 0x004500, 0x005710, 0x006921, 0x007b33, 0x078c45, 0x199e57,
    0x2bb069, 0x3dc27b, 0x4fd48d, 0x61e69e, 0x72f8b0, 0x84ffc2, 0x96ffd4, 0xa8ffe6,
    0x002900, 0x003b00, 0x004d00, 0x005e00, 0x007000, 0x018200, 0x139403, 0x25a615,
    0x37b827, 0x48c938, 0x5adb4a, 0x6ced5c, 0x7eff6e, 0x90ff80, 0xa2ff92, 0xb4ffa4,
    0x002500, 0x003700, 0x004900, 0x005a00, 0x0b6c00, 0x1d7e00, 0x2f9000, 0x40a200,
    0x52b400, 0x64c50a, 0x76d71c, 0x88e92d, 0x9afb3f, 0xacff51, 0xbdff63, 0xcfff75,
    0x001600, 0x002800, 0x0d3a00, 0x1f4c00, 0x315e00, 0x436f00, 0x558100, 0x669300,
    0x78a500, 0x8ab700, 0x9cc90a, 0xaedb1c, 0xc0ec2e, 0xd1fe40, 0xe3ff52, 0xf5ff64,
    0x110000, 0x231200, 0x352400, 0x473600, 0x594800, 0x6b5900, 0x7d6b00, 0x8e7d00,
    0xa08f00, 0xb2a108, 0xc4b31a, 0xd6c52c, 0xe8d63e, 0xf9e850, 0xfffa62, 0xffff74
};

/* 7800 NTSC palette - A7800 NTSC LCD V1 STD-COOL (Trebor Standard) */
static const uint32_t maria_palette_cool[256] = {
    0x000000, 0x050505, 0x171717, 0x292929, 0x3b3b3b, 0x4c4c4c, 0x5e5e5e, 0x707070,
    0x828282, 0x949494, 0xa6a6a6, 0xb8b8b8, 0xc9c9c9, 0xdbdbdb, 0xededed, 0xffffff,
    0x000d00, 0x0e1e00, 0x203000, 0x324200, 0x445400, 0x566600, 0x677800, 0x798a00,
    0x8b9b00, 0x9dad00, 0xafbf0e, 0xc1d120, 0xd3e331, 0xe4f543, 0xf6ff55, 0xffff67,
    0x210000, 0x330700, 0x451900, 0x572b00, 0x683d00, 0x7a4f00, 0x8c6100, 0x9e7300,
    0xb08407, 0xc29619, 0xd4a82b, 0xe5ba3d, 0xf7cc4e, 0xffde60, 0xfff072, 0xffff84,
    0x3d0000, 0x4f0000, 0x600200, 0x721400, 0x842600, 0x963707, 0xa84919, 0xba5b2a,
    0xcb6d3c, 0xdd7f4e, 0xef9160, 0xffa372, 0xffb484, 0xffc696, 0xffd8a7, 0xffeab9,
    0x4a0000, 0x5c0002, 0x6e0014, 0x7f0026, 0x911238, 0xa3244a, 0xb5365c, 0xc7486d,
    0xd95a7f, 0xea6c91, 0xfc7da3, 0xff8fb5, 0xffa1c7, 0xffb3d9, 0xffc5ea, 0xffd7fc,
    0x460034, 0x580046, 0x6a0058, 0x7b006a, 0x8d077c, 0x9f198d, 0xb12b9f, 0xc33cb1,
    0xd54ec3, 0xe760d5, 0xf872e7, 0xff84f8, 0xff96ff, 0xffa8ff, 0xffb9ff, 0xffcbff,
    0x32006b, 0x43007d, 0x55008f, 0x6700a0, 0x7906b2, 0x8b18c4, 0x9d29d6, 0xaf3be8,
    0xc04dfa, 0xd25fff, 0xe471ff, 0xf683ff, 0xff95ff, 0xffa6ff, 0xffb8ff, 0xffcaff,
    0x11008a, 0x23009c, 0x3500ae, 0x4700c0, 0x580fd1, 0x6a21e3, 0x7c33f5, 0x8e45ff,
    0xa057ff, 0xb268ff, 0xc47aff, 0xd58cff, 0xe79eff, 0xf9b0ff, 0xffc2ff, 0xffd3ff,
    0x00008b, 0x00009d, 0x0e00af, 0x200fc1, 0x3221d3, 0x4433e5, 0x5645f7, 0x6857ff,
    0x7968ff, 0x8b7aff, 0x9d8cff, 0xaf9eff, 0xc1b0ff, 0xd3c2ff, 0xe5d4ff, 0xf6e5ff,
    0x00006f, 0x000281, 0x001493, 0x0026a4, 0x0d38b6, 0x1f4ac8, 0x315cda, 0x436eec,
    0x557ffe, 0x6791ff, 0x78a3ff, 0x8ab5ff, 0x9cc7ff, 0xaed9ff, 0xc0eaff, 0xd2fcff,
    0x00083a, 0x001a4c, 0x002c5e, 0x003e6f, 0x004f81, 0x036193, 0x1573a5, 0x2785b7,
    0x3997c9, 0x4ba9da, 0x5dbbec, 0x6eccfe, 0x80deff, 0x92f0ff, 0xa4ffff, 0xb6ffff,
    0x001b00, 0x002d09, 0x003f1b, 0x00512c, 0x00633e, 0x007550, 0x088762, 0x1a9874,
    0x2caa86, 0x3dbc98, 0x4fcea9, 0x61e0bb, 0x73f2cd, 0x85ffdf, 0x97fff1, 0xa9ffff,
    0x002700, 0x003900, 0x004b00, 0x005c00, 0x006e00, 0x00800c, 0x0c921e, 0x1da430,
    0x2fb642, 0x41c854, 0x53d966, 0x65eb78, 0x77fd89, 0x89ff9b, 0x9affad, 0xacffbf,
    0x002800, 0x003a00, 0x004c00, 0x005e00, 0x007000, 0x0e8100, 0x209300, 0x31a500,
    0x43b70b, 0x55c91d, 0x67db2f, 0x79ed41, 0x8bfe52, 0x9dff64, 0xaeff76, 0xc0ff88,
    0x001f00, 0x003100, 0x004300, 0x0a5500, 0x1c6600, 0x2e7800, 0x408a00, 0x529c00,
    0x64ae00, 0x76c000, 0x87d10f, 0x99e321, 0xabf533, 0xbdff45, 0xcfff57, 0xe1ff68,
    0x000d00, 0x0d1f00, 0x1f3100, 0x314300, 0x435500, 0x556600, 0x667800, 0x788a00,
    0x8a9c00, 0x9cae00, 0xaec00d, 0xc0d21f, 0xd2e331, 0xe3f543, 0xf5ff55, 0xffff67
};

/* 7800 NTSC palette - A7800 NTSC LCD V1 STD-HOT (Trebor Standard) */
static const uint32_t maria_palette_hot[256] = {
    0x000000, 0x050505, 0x171717, 0x292929, 0x3b3b3b, 0x4c4c4c, 0x5e5e5e, 0x707070,
    0x828282, 0x949494, 0xa6a6a6, 0xb8b8b8, 0xc9c9c9, 0xdbdbdb, 0xededed, 0xffffff,
    0x000d00, 0x0e1e00, 0x203000, 0x324200, 0x445400, 0x566600, 0x677800, 0x798a00,
    0x8b9b00, 0x9dad00, 0xafbf0e, 0xc1d120, 0xd3e331, 0xe4f543, 0xf6ff55, 0xffff67,
    0x240000, 0x350600, 0x471700, 0x592900, 0x6b3b00, 0x7d4d00, 0x8f5f00, 0xa17100,
    0xb2830a, 0xc4941c, 0xd6a62e, 0xe8b840, 0xfaca52, 0xffdc64, 0xffee75, 0xffff87,
    0x400000, 0x520000, 0x630000, 0x751000, 0x872200, 0x993411, 0xab4622, 0xbd5834,
    0xcf6a46, 0xe07b58, 0xf28d6a, 0xff9f7c, 0xffb18e, 0xffc39f, 0xffd5b1, 0xffe7c3,
    0x4a0001, 0x5c0013, 0x6e0024, 0x800036, 0x920f48, 0xa4215a, 0xb6326c, 0xc7447e,
    0xd95690, 0xeb68a1, 0xfd7ab3, 0xff8cc5, 0xff9ed7, 0xffafe9, 0xffc1fb, 0xffd3ff,
    0x410047, 0x530059, 0x65006b, 0x77007d, 0x89058f, 0x9a17a0, 0xac29b2, 0xbe3bc4,
    0xd04dd6, 0xe25fe8, 0xf471fa, 0xff82ff, 0xff94ff, 0xffa6ff, 0xffb8ff, 0xffcaff,
    0x26007a, 0x38008c, 0x4a009e, 0x5c00b0, 0x6e08c2, 0x7f1ad4, 0x912ce5, 0xa33ef7,
    0xb550ff, 0xc761ff, 0xd973ff, 0xea85ff, 0xfc97ff, 0xffa9ff, 0xffbbff, 0xffcdff,
    0x00008f, 0x1100a0, 0x2300b2, 0x3505c4, 0x4717d6, 0x5928e8, 0x6a3afa, 0x7c4cff,
    0x8e5eff, 0xa070ff, 0xb282ff, 0xc494ff, 0xd6a5ff, 0xe7b7ff, 0xf9c9ff, 0xffdbff,
    0x00007f, 0x000091, 0x0009a3, 0x0b1bb5, 0x1d2dc7, 0x2f3fd9, 0x4151ea, 0x5363fc,
    0x6575ff, 0x7786ff, 0x8898ff, 0x9aaaff, 0xacbcff, 0xbeceff, 0xd0e0ff, 0xe2f2ff,
    0x000050, 0x001162, 0x002374, 0x003586, 0x004797, 0x0c59a9, 0x1e6bbb, 0x307ccd,
    0x428edf, 0x54a0f1, 0x66b2ff, 0x77c4ff, 0x89d6ff, 0x9be8ff, 0xadf9ff, 0xbfffff,
    0x00160c, 0x00281d, 0x003a2f, 0x004c41, 0x005e53, 0x007065, 0x0a8177, 0x1c9389,
    0x2ea59a, 0x40b7ac, 0x52c9be, 0x63dbd0, 0x75ede2, 0x87fef4, 0x99ffff, 0xabffff,
    0x002500, 0x003700, 0x004900, 0x005b00, 0x006d09, 0x007e1b, 0x09902d, 0x1ba23e,
    0x2db450, 0x3fc662, 0x51d874, 0x63ea86, 0x74fb98, 0x86ffaa, 0x98ffbb, 0xaaffcd,
    0x002900, 0x003a00, 0x004c00, 0x005e00, 0x007000, 0x0a8200, 0x1c9400, 0x2ea600,
    0x40b712, 0x51c923, 0x63db35, 0x75ed47, 0x87ff59, 0x99ff6b, 0xabff7d, 0xbdff8f,
    0x002000, 0x003200, 0x004400, 0x085500, 0x1a6700, 0x2c7900, 0x3e8b00, 0x509d00,
    0x61af00, 0x73c100, 0x85d210, 0x97e422, 0xa9f634, 0xbbff46, 0xccff58, 0xdeff6a,
    0x000d00, 0x0e1f00, 0x203100, 0x314200, 0x435400, 0x556600, 0x677800, 0x798a00,
    0x8b9c00, 0x9dae00, 0xaebf0e, 0xc0d11f, 0xd2e331, 0xe4f543, 0xf6ff55, 0xffff67,
    0x230000, 0x350600, 0x471800, 0x592a00, 0x6b3b00, 0x7c4d00, 0x8e5f00, 0xa07100,
    0xb2830a, 0xc4951c, 0xd6a72d, 0xe8b83f, 0xf9ca51, 0xffdc63, 0xffee75, 0xffff87
};

/* Get palette by index */
const uint32_t *maria_get_palette(int index)
{
    switch (index) {
        case MARIA_PALETTE_COOL: return maria_palette_cool;
        case MARIA_PALETTE_HOT:  return maria_palette_hot;
        default:                 return maria_ntsc_palette;  /* WARM */
    }
}

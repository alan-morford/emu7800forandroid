/*
 * tia.h
 *
 * Television Interface Adaptor (TIA)
 * EMU7800-style batch renderer with lookup tables
 *
 * Based on EMU7800 by Mike Murphy (TIA.cs + TIATables.cs)
 * C port Copyright (c) 2024-2026 EMU7800
 */

#ifndef TIA_H
#define TIA_H

#include <stdint.h>

/* ========================================================================
 * Main TIA Struct — flat EMU7800-style
 * ======================================================================== */

typedef struct {
    uint8_t regw[0x40];           /* Register file */

    int hsync;                     /* Horizontal sync counter (mod 228) */
    int scanline;                  /* Current scanline */

    /* Position counters (mod 160, can be negative) */
    int p0, p1, m0, m1, bl;

    /* HMOVE */
    uint64_t start_hmove_clock;    /* When to start HMOVE (UINT64_MAX = off) */
    int hmove_counter;             /* -1 to 0xf */
    int hmove_latch;               /* Extended hblank active */
    int p0mmr, p1mmr, m0mmr, m1mmr, blmmr;  /* "more motion required" */

    /* Player graphics */
    uint8_t eff_grp0, old_grp0;
    uint8_t eff_grp1, old_grp1;
    int p0type, p1type;
    int p0suppress, p1suppress;

    /* Missile */
    int m0type, m0size;
    int m1type, m1size;
    int m0on, m1on;

    /* Ball */
    int blsize;
    int blon;
    int old_enabl;

    /* Playfield */
    uint32_t pf210;
    int pf_reflection_state;

    /* Colors */
    uint8_t colubk, colupf, colup0, colup1;

    /* Control flags */
    int vblankon, scoreon, pfpriority;

    /* Clock tracking */
    uint64_t start_clock;          /* First unrendered TIA clock */

    /* WSYNC + frame control */
    int wsync_delay_clocks;
    int end_of_frame;

    /* VBLANK-OFF anchor (our improvement over EMU7800) */
    int vblank_off_scanline;

    /* Last known good VBLANK-OFF scanline (persists across frames) */
    int last_good_vbo;

    /* Last row with visible (non-VBLANK) content this frame */
    int last_visible_row;

    /* Input */
    int dump_enabled;
    uint64_t dump_disabled_cycle;

    /* Collisions (15-bit pair flags, matches TIACxPairFlags) */
    uint16_t collisions;

    /* Audio position tracking */
    uint64_t frame_start_tia_clock;
} TIA;

/* ========================================================================
 * Public API
 * ======================================================================== */

/* Initialize TIA */
void tia_init(TIA *tia);

/* Reset TIA */
void tia_reset(TIA *tia);

/* Read from TIA register - catches up TIA before reading
 * data_bus_state: last value on the data bus (for open-bus lower 6 bits) */
uint8_t tia_read(TIA *tia, uint16_t addr, uint64_t cpu_clock, uint8_t data_bus_state);

/* Write to TIA register - catches up TIA before applying change */
void tia_write(TIA *tia, uint16_t addr, uint8_t data, uint64_t cpu_clock);

/* Render TIA up to a target clock (for final frame catch-up) */
void tia_render_remaining(TIA *tia, uint64_t target_clock);

/* Start a new frame */
void tia_start_frame(TIA *tia);

/* End the current frame */
void tia_end_frame(TIA *tia);

/* Get frame buffer pointer */
uint8_t *tia_get_frame_buffer(void);
uint8_t *tia_get_write_buffer(void);

/* Get/set the scanline where VBLANK first turned OFF (visible content start) */
int tia_get_vblank_off_scanline(void);
void tia_set_vblank_off_scanline(int scanline);

/* Get the active display height (rows with visible content, hysteresis-filtered) */
int tia_get_active_height(void);

/* Scan display buffer for non-zero pixel bounding box (for diagnostics).
 * Returns 0 if content found, -1 if buffer is all zero.
 * first/last row and col are inclusive. */
int tia_scan_content_bounds(int *first_row, int *last_row,
                            int *first_col, int *last_col);

/* Set the frame-start TIA clock for audio position tracking.
 * Must be called AFTER any force-sync on start_clock. */
void tia_set_frame_start_clock(uint64_t clock);

/* Frame synchronization for multi-threaded rendering */
int tia_frame_ready(void);      /* Check if new frame is available */
void tia_frame_consumed(void);  /* Signal that frame has been rendered */

/* Reset VSYNC/VBLANK diagnostic log counter */
void tia_reset_sync_log(void);

/* TIA lookup tables */
extern const uint32_t tia_ntsc_palette[256];
extern const uint8_t tia_grp_reflect[256];

/* ========================================================================
 * Diagnostic Logging
 *
 * Cycle-level instrumentation for diagnosing TIA timing issues.
 * Categories are OR'd into a bitfield; logging only fires when the
 * category is enabled AND the current frame is in [start, end].
 * ======================================================================== */

/* Debug categories (bitfield) */
#define TIA_DBG_CPU         0x0001  /* CPU/TIA interleave + WSYNC stalls */
#define TIA_DBG_HTIMING     0x0002  /* Scanline starts, hblank transitions */
#define TIA_DBG_REG_WRITE   0x0004  /* Selected TIA register writes */
#define TIA_DBG_LATCH       0x0008  /* Register handler events */
#define TIA_DBG_FRAME       0x0010  /* Frame boundary summary */
#define TIA_DBG_FRAME_SKIP  0x0020  /* Frame anomalies (bad sl count, drift) */
#define TIA_DBG_PIXEL       0x0040  /* Per-pixel object/priority state */
#define TIA_DBG_ALL         0x007F

/* Initialize: flags=categories, only log frames [start,end], pixel_sl=-1=off */
void tia_dbg_init(uint32_t flags, int frame_start, int frame_end, int pixel_scanline);

/* Call at start of each frame with 0-based frame number */
void tia_dbg_set_frame(int frame_num);

/* Formatted output to log file (internal — use TIA_DBG macro) */
void tia_dbg_printf(const char *fmt, ...);

/* Globals for fast macro checks */
extern uint32_t g_tia_dbg_flags;
extern int g_tia_dbg_frame_ok;
extern int g_tia_dbg_pixel_sl;

/* Zero-overhead macros: only format+log when category is active */
#define TIA_DBG(cat, ...) do { \
    if ((g_tia_dbg_flags & (cat)) && g_tia_dbg_frame_ok) \
        tia_dbg_printf(__VA_ARGS__); \
} while(0)

#define TIA_DBG_PX(sl, ...) do { \
    if ((g_tia_dbg_flags & TIA_DBG_PIXEL) && g_tia_dbg_frame_ok && \
        (g_tia_dbg_pixel_sl < 0 || (sl) == g_tia_dbg_pixel_sl)) \
        tia_dbg_printf(__VA_ARGS__); \
} while(0)

#endif /* TIA_H */

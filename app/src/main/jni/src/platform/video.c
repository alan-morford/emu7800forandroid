/*
 * video.c
 *
 * SDL2 Video Output — Android port
 * 8-bit indexed → RGB565 conversion via pre-computed palette LUT.
 *
 * Scaling pipeline:
 *   ORIGINAL: 4:3 aspect ratio at full screen height (pillarboxed if narrower than screen).
 *   FULLSCREEN: stretch to fill the full screen.
 *
 * Overlay (virtual gamepad) is drawn in screen pixel coordinates so its
 * size is independent of the emulator resolution.
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <SDL.h>
#include "video.h"
#include "machine.h"
#include "tia.h"
#include "maria.h"
#include "input.h"
#include "font.h"

extern void log_msg(const char *msg);

/* Framebuffer dimensions per machine type. */
#define FB_W_7800   320
#define FB_H_7800   242    /* visible scanlines (skip first 11 vblank lines) */
#define TEX_W_7800  512    /* next power-of-2 ≥ 320 */
#define TEX_H_7800  256    /* next power-of-2 ≥ 242 */
#define SKIP_7800    11

#define FB_W_2600   160
#define FB_H_2600   210    /* typical visible NTSC scanlines */
#define TEX_W_2600  256
#define TEX_H_2600  256
#define SKIP_2600     0

/* CRT display window: scanlines 0–30 are hidden by the bezel on real hardware.
 * Games that turn VBLANK off before scanline 31 write overscan rows to the top
 * of the frame buffer — we skip those so the image is positioned correctly. */
#define CRT_START_SL  31
#define CRT_MAX_H    210

/*
 * Pixel aspect ratios (PAR): how many display pixels wide each emulator
 * pixel should be, relative to its height.
 */
#define PAR_2600  1.2f
#define PAR_7800  ((4.0f / 3.0f) * (float)FB_H_7800 / (float)FB_W_7800)

static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture  = NULL;
static int   g_tex_w, g_tex_h;
static int   g_fb_w,  g_fb_h;
static int   g_skip;
static float g_par;           /* pixel aspect ratio for current machine */

/* Pre-computed RGB565 palette LUTs. */
static uint16_t g_tia_pal[256];
static uint16_t g_maria_pal[256];

/* Settings */
static int g_scanlines        = 0;
static int g_scanline_bright  = 1;   /* 0=dark, 1=medium, 2=bright */
static int g_maria_palette    = 0;   /* 0=warm, 1=cool, 2=original */
static int g_zoom_level       = 0;   /* 0=MAX HEIGHT, 1=STRETCH */

static const char *g_zoom_labels[] = { "ORIGINAL", "FULLSCREEN" };

/* Build RGB565 LUT from a uint32_t 0xRRGGBB palette, with optional tint. */
static void build_pal565_tinted(const uint32_t *src, uint16_t *dst, int palette)
{
    for (int i = 0; i < 256; i++) {
        uint32_t c = src[i];
        uint8_t r = (c >> 16) & 0xFF;
        uint8_t g = (c >>  8) & 0xFF;
        uint8_t b = (c      ) & 0xFF;

        if (palette == 1) {
            /* Cool: shift toward blue */
            int rr = (int)r - 20;  if (rr < 0) rr = 0;
            int gg = (int)g;
            int bb = (int)b + 20;  if (bb > 255) bb = 255;
            r = (uint8_t)rr;
            b = (uint8_t)bb;
            (void)gg;
        } else if (palette == 2) {
            /* Original: slightly desaturated (approx. older NTSC phosphor look) */
            int lum = ((int)r * 30 + (int)g * 59 + (int)b * 11) / 100;
            r = (uint8_t)((r * 3 + lum) / 4);
            g = (uint8_t)((g * 3 + lum) / 4);
            b = (uint8_t)((b * 3 + lum) / 4);
        }
        /* palette == 0 (warm): use as-is */

        dst[i] = (uint16_t)(
            ((r >> 3) << 11) |   /* R5 */
            ((g >> 2) <<  5) |   /* G6 */
            ((b >> 3)      )     /* B5 */
        );
    }
}

static void build_pal565(const uint32_t *src, uint16_t *dst)
{
    build_pal565_tinted(src, dst, 0);
}

static int create_texture(void)
{
    if (g_texture) { SDL_DestroyTexture(g_texture); g_texture = NULL; }
    g_texture = SDL_CreateTexture(g_renderer,
                                  SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  g_tex_w, g_tex_h);
    return g_texture ? 0 : -1;
}

/*
 * Compute the destination rect for SDL_RenderCopy.
 */
static void get_screen_size(int *w, int *h)
{
    SDL_GL_GetDrawableSize(g_window, w, h);
    if (*w <= 0 || *h <= 0)
        SDL_GetWindowSize(g_window, w, h);  /* fallback */
}

static SDL_Rect compute_dest_rect(void)
{
    int sw, sh;
    get_screen_size(&sw, &sh);

    int final_h = sh;
    int final_w;

    if (g_zoom_level == 1) {
        /* FULLSCREEN: fill both dimensions. */
        final_w = sw;
    } else {
        /* ORIGINAL: 4:3 at full screen height. */
        final_w = 4 * sh / 3;
        if (final_w > sw) final_w = sw;
    }

    static int logged = 0;
    if (!logged) {
        char buf[128];
        SDL_snprintf(buf, sizeof(buf),
            "video: sw=%d sh=%d zoom=%d fw=%d fh=%d", sw, sh, g_zoom_level, final_w, final_h);
        log_msg(buf);
        logged = 1;
    }

    SDL_Rect dst;
    dst.x = (sw - final_w) / 2;
    dst.y = 0;
    dst.w = final_w;
    dst.h = final_h;
    return dst;
}

int video_init(SDL_Window *window)
{
    g_window = window;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  /* nearest-neighbour */

    g_renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer)
        g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        log_msg("video_init: SDL_CreateRenderer failed");
        return -1;
    }

    build_pal565(tia_ntsc_palette,   g_tia_pal);
    build_pal565(maria_ntsc_palette, g_maria_pal);

    /* Default to 2600 until a ROM is loaded */
    g_fb_w  = FB_W_2600;  g_fb_h  = FB_H_2600;
    g_tex_w = TEX_W_2600; g_tex_h = TEX_H_2600;
    g_skip  = SKIP_2600;
    g_par   = PAR_2600;
    create_texture();

    log_msg("video_init: OK");
    return 0;
}

void video_set_machine_type(int type)
{
    if (type == MACHINE_7800) {
        g_fb_w  = FB_W_7800;  g_fb_h  = FB_H_7800;
        g_tex_w = TEX_W_7800; g_tex_h = TEX_H_7800;
        g_skip  = SKIP_7800;
        g_par   = PAR_7800;
    } else {
        g_fb_w  = FB_W_2600;  g_fb_h  = FB_H_2600;
        g_tex_w = TEX_W_2600; g_tex_h = TEX_H_2600;
        g_skip  = SKIP_2600;
        g_par   = PAR_2600;
    }
    create_texture();
}

/* Draw scanline overlay over the rendered frame destination rect. */
static void draw_scanlines(SDL_Renderer *r, SDL_Rect *dst, int src_h)
{
    if (!g_scanlines) return;

    /* Use evenly-spaced lines (one per rounded scale factor) so the pattern is
     * uniform regardless of screen size. Per-source-scanline float positions
     * cause irregular spacing when scale_y is non-integer. */
    int bright_idx = g_scanline_bright < 0 ? 0 : g_scanline_bright > 2 ? 2 : g_scanline_bright;
    static const Uint8 alphas[]    = {100, 200, 255};
    static const int   line_heights[] = {1,   1,   2};  /* Dark uses 2px for 25% more coverage */
    Uint8 a        = alphas[bright_idx];
    int   line_h   = line_heights[bright_idx];

    float scale_y = (float)dst->h / (float)src_h;
    int step = (int)(scale_y + 0.5f);  /* round to nearest integer */
    if (step < 2) return;              /* too small to show distinct lines */

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, a);

    /* One dark line per step, starting at the bottom of the first game row */
    for (int y = dst->y + step - 1; y < dst->y + dst->h; y += step) {
        SDL_Rect line = {dst->x, y - (line_h - 1), dst->w, line_h};
        SDL_RenderFillRect(r, &line);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void video_render(void)
{
    if (!g_texture || !g_renderer) return;

    const uint8_t  *src    = machine_get_frame_buffer();
    const int       mtype  = machine_get_type();
    const uint16_t *pal    = (mtype == MACHINE_7800) ? g_maria_pal : g_tia_pal;
    const int       stride = machine_get_frame_width();

    if (!src) return;

    /* For 2600 games, apply CRT display window correction:
     * games that turn VBLANK off before scanline 31 store overscan rows at
     * the top of the frame buffer that a real CRT hides behind the bezel.
     * Skip those rows so the image is positioned as it would be on original hardware. */
    int skip = g_skip;
    int fb_h = g_fb_h;
    if (mtype != MACHINE_7800) {
        int vbo_sl   = tia_get_vblank_off_scanline();
        int active_h = tia_get_active_height();
        if (active_h < 1) active_h = 192;
        if (vbo_sl >= 0 && vbo_sl < CRT_START_SL) {
            skip = CRT_START_SL - vbo_sl;
            if (skip >= active_h) skip = 0;
        }
        fb_h = active_h - skip;
        if (skip > 0 && fb_h > CRT_MAX_H) fb_h = CRT_MAX_H;
        if (fb_h < 1)          fb_h = 192;
        if (fb_h > TEX_H_2600) fb_h = TEX_H_2600;
    }

    /* Upload frame to texture */
    void *pixels;
    int   pitch;
    if (SDL_LockTexture(g_texture, NULL, &pixels, &pitch) < 0) return;

    uint16_t *dst     = (uint16_t *)pixels;
    int       pitch16 = pitch / 2;

    for (int y = 0; y < fb_h; y++) {
        const uint8_t *row  = src + (skip + y) * stride;
        uint16_t      *drow = dst + y * pitch16;
        for (int x = 0; x < g_fb_w; x++)
            drow[x] = pal[row[x]];
    }

    SDL_UnlockTexture(g_texture);

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);

    SDL_Rect src_rect = {0, 0, g_fb_w, fb_h};
    SDL_Rect dst_rect = compute_dest_rect();
    SDL_RenderCopy(g_renderer, g_texture, &src_rect, &dst_rect);

    draw_scanlines(g_renderer, &dst_rect, fb_h);

    /* Virtual gamepad drawn in screen pixel coordinates */
    input_draw_overlay(g_renderer);

    SDL_RenderPresent(g_renderer);
}

void video_draw_waiting_screen(void)
{
    if (!g_renderer) return;

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);

    int sw, sh;
    get_screen_size(&sw, &sh);

    static const char *msg = "Tap to Load ROM";
    int scale = (sw > 1280) ? 4 : 2;
    int tw = font_string_width(msg, scale);
    font_draw_string(g_renderer, msg,
                     (sw - tw) / 2, (sh - 8 * scale) / 2,
                     scale, 255, 255, 255);

    SDL_RenderPresent(g_renderer);
}

void video_shutdown(void)
{
    if (g_texture)  { SDL_DestroyTexture(g_texture);   g_texture  = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
}

SDL_Renderer *video_get_renderer(void)
{
    return g_renderer;
}

/* ---- Scanlines ---- */

void video_set_scanlines(int enabled)       { g_scanlines = enabled ? 1 : 0; }
int  video_get_scanlines(void)              { return g_scanlines; }

void video_set_scanline_brightness(int lv)
{
    if (lv < 0) lv = 0;
    if (lv > 2) lv = 2;
    g_scanline_bright = lv;
}
int video_get_scanline_brightness(void)     { return g_scanline_bright; }

/* ---- Maria palette ---- */

void video_set_maria_palette(int palette)
{
    if (palette < 0) palette = 0;
    if (palette > 2) palette = 2;
    g_maria_palette = palette;
    build_pal565_tinted(maria_ntsc_palette, g_maria_pal, palette);
}
int video_get_maria_palette(void)           { return g_maria_palette; }

/* ---- Zoom ---- */

void video_cycle_zoom(void)
{
    g_zoom_level = (g_zoom_level + 1) % 2;
}

const char *video_get_zoom_label(void)
{
    return g_zoom_labels[g_zoom_level];
}

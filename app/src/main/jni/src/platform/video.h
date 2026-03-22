/*
 * video.h
 *
 * SDL2 Video Output Header — Android port
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <SDL.h>

/* Initialize video subsystem. Creates renderer from the given window. */
int video_init(SDL_Window *window);

/* Shutdown video subsystem. */
void video_shutdown(void);

/* Render the current emulator frame to screen. */
void video_render(void);

/* Draw the "waiting for ROM" screen. */
void video_draw_waiting_screen(void);

/* Configure texture dimensions for the given machine type (MACHINE_2600 or MACHINE_7800). */
void video_set_machine_type(int type);

/* Return the SDL_Renderer (for font_init and overlay drawing). */
SDL_Renderer *video_get_renderer(void);

/* Scanline overlay — drawn over the emulator image when enabled. */
void video_set_scanlines(int enabled);
int  video_get_scanlines(void);
void video_set_scanline_brightness(int level);  /* 0=dark, 1=medium, 2=bright */
int  video_get_scanline_brightness(void);

/* Maria palette selection for 7800 games. */
void video_set_maria_palette(int palette);      /* 0=warm, 1=cool, 2=original */
int  video_get_maria_palette(void);

/* Zoom level cycling. */
void        video_cycle_zoom(void);
const char *video_get_zoom_label(void);

#endif /* VIDEO_H */

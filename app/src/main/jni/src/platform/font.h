/*
 * font.h
 *
 * SDL2 Bitmap Font Renderer Header — Android port
 * 8x8 monospace font rendered via SDL_Renderer texture blits.
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef FONT_H
#define FONT_H

#include <SDL.h>

/* Initialize font system. Creates a texture atlas on the given renderer. */
int font_init(SDL_Renderer *renderer);

/* Shutdown font system. Destroys the atlas texture. */
void font_shutdown(void);

/* Draw a string at pixel coordinates using the given renderer.
 * x, y: top-left in logical pixels.
 * scale: pixel multiplier (1 = 8px tall, 2 = 16px tall).
 * r, g, b: text colour (0-255). */
void font_draw_string(SDL_Renderer *renderer, const char *text,
                      int x, int y, int scale,
                      Uint8 r, Uint8 g, Uint8 b);

/* Return string width in pixels at the given scale. */
int font_string_width(const char *text, int scale);

#endif /* FONT_H */

/*
 * filepicker.h
 *
 * File Picker UI — Android SDL2 port
 * Provides a scrollable ROM browser with recent list, settings, and save-state popup.
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef FILEPICKER_H
#define FILEPICKER_H

#include <SDL.h>

/* Set the data directory for persistence files (settings, recent list, lastrom).
 * Must be called before filepicker_init(). */
void filepicker_set_data_dir(const char *dir);

/* Set the display density (Android DisplayMetrics.density) so that the
 * filepicker can size its bitmap font to match the system's 16sp body text.
 * Call once from initNativeDirs() before the first draw. */
void filepicker_set_display_density(float density);

/* Inform the filepicker whether the current display has a cutout (notch).
 * When has_cutout is 0 the "Use Notch Area" setting row is greyed out and
 * non-interactive.  Re-call whenever the display changes (e.g. foldable open/close). */
void filepicker_set_has_cutout(int has_cutout);

/* Initialize: load persistence files, create textures from embedded image headers. */
void filepicker_init(void);

/* Release all resources. */
void filepicker_shutdown(void);

/* Scan the given directory and populate the file list. */
void filepicker_scan(const char *dir);

/* Re-scan the current directory (e.g. after returning from emulator). */
void filepicker_rescan(void);

/* Draw the complete filepicker UI to the renderer. */
void filepicker_draw(SDL_Renderer *r);

/* Touch input handlers. */
void filepicker_touch_down(int x, int y);
void filepicker_touch_move(int x, int y);
int  filepicker_touch_up(int x, int y);   /* returns 1 if a ROM was selected */

/* Query selected ROM (valid after filepicker_touch_up returns 1). */
const char *filepicker_get_selected_path(void);
int         filepicker_get_selected_type(void);

/* Whether to auto-load the save state for the selected ROM. */
int filepicker_should_load_save(void);

/* Update last ROM (called from main.c after emulator launches). */
void        filepicker_set_last_rom(const char *path, int type);
int         filepicker_has_last_rom(void);
const char *filepicker_get_last_rom_path(void);
int         filepicker_get_last_rom_type(void);

/* Show the "file not found" popup (e.g. if RESUME ROM is missing). */
void filepicker_show_notfound(void);

/* Save all settings to the persistence file. */
void filepicker_save_settings(void);

/* Get the current default ROM directory. */
const char *filepicker_get_default_romdir(void);

/* Handle Android back key / back-swipe:
 * - If any popup is open, close it.
 * - Otherwise navigate up one directory. */
void filepicker_back(void);

#endif /* FILEPICKER_H */

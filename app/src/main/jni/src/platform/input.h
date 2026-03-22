/*
 * input.h
 *
 * SDL2 Input Handler Header — Android port
 * Handles multitouch virtual buttons and SDL_GameController.
 *
 * Virtual layout (landscape):
 *
 *   +------+------+---------+---------+------+------+
 *   | BACK | PAUS |         | SELECT  | RST  |      |  <- top 7%
 *   +------+------+---------+---------+------+------+
 *   |           |                   |           |
 *   |  D-PAD    |   (game area)     |  FIRE1    |
 *   | (left 28%)|                   | (right 28%|
 *   |           |                   |  FIRE2    |
 *   +-----------+-------------------+-----------+
 *   | SAVE | LOAD | ZOOM | OPTIONS             |  <- bottom 8%
 *   +------+------+------+---------------------+
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef INPUT_H
#define INPUT_H

#include <SDL.h>

/* Set display density (dp → px scale factor).
 * Must be called once at startup and again on every configuration change
 * (fold/unfold, display switch) so dp-based button sizes remain physically
 * consistent regardless of screen pixel dimensions. */
void input_set_display_density(float d);

/* Initialize input subsystem. */
void input_init(void);

/* Dispatch one SDL event into the input layer. */
void input_handle_event(SDL_Event *e);

/* Draw the on-screen virtual gamepad overlay. */
void input_draw_overlay(SDL_Renderer *renderer);

/* Draw the in-game options popup (when visible). */
void input_draw_options_popup(SDL_Renderer *r);

/* Handle touch events for the in-game options popup. */
void input_touch_options_popup(int x, int y, int up);

/* One-shot button queries — return 1 once then clear. */
int input_pause_pressed(void);
int input_save_pressed(void);
int input_load_pressed(void);
int input_zoom_pressed(void);
int input_back_pressed(void);
int input_options_pressed(void);

/* Level queries (held state). */
int input_reset_pressed(void);
int input_select_pressed(void);

/* Popup visibility queries. */
int input_options_popup_visible(void);
int input_confirm_visible(void);
int input_autosave_warn_visible(void);
int input_any_popup_visible(void);

/* Confirm popup (shown when BACK is tapped with autosave+ask). */
void input_show_confirm(void);
int  input_confirm_result(void);  /* -1=pending, 0=No, 1=Yes */

/* Close the topmost visible popup. */
void input_close_popup(void);

/* Open the in-game options popup (called from main loop). */
void input_open_options_popup(void);

/* Brief notification text (shown after SAVE/LOAD). */
void input_show_notification(const char *msg);
/* Brief zoom mode label: white text at top-center, no background (shown after ZOOM). */
void input_show_zoom_label(const char *msg);
void input_tick(void);            /* advance notification timer; call each frame */

/* Save-exists hint (drives options popup display). */
void input_set_save_exists(int exists);

/* Settings accessors — used by filepicker settings popup and save/load. */
int  input_get_autosave(void);
void input_set_autosave(int v);
int  input_get_autosave_ask(void);
void input_set_autosave_ask(int v);
int  input_get_control_dim(void);
void input_set_control_dim(int v);
int  input_get_btn_size(void);
void input_set_btn_size(int v);
int  input_get_dpad_size(void);
void input_set_dpad_size(int v);

#endif /* INPUT_H */

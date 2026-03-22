/*
 * input.c
 *
 * SDL2 Input Handler — Android port
 * Multitouch virtual gamepad + SDL_GameController support.
 * Calls machine_set_* directly to feed input state to the emulator core.
 *
 * Virtual layout (landscape):
 *
 *   +------+------+---------+---------+------+------+
 *   | BACK | PAUS |         | SELECT  | RST  |      |  <- top 7%
 *   +------+------+---------+---------+------+------+
 *   |           |                   |           |
 *   |  D-PAD    |   (game area)     |  FIRE1    |
 *   | (left 28%)|                   | (top 50%) |
 *   |           |                   |  FIRE2    |
 *   |           |                   | (bot 50%) |
 *   +-----------+-------------------+-----------+
 *   | SAVE | LOAD | ZOOM |       OPTIONS        |  <- bottom 8%
 *   +------+------+------+----------------------+
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "input.h"
#include "machine.h"
#include "savestate.h"
#include "video.h"
#include "font.h"

extern void log_msg(const char *msg);
extern void filepicker_save_settings(void);  /* filepicker.c */
extern void jni_send_bug_report_email(void); /* jni_bridge.c */

/* Forward declarations (defined later in this file) */
static void input_handle_confirm_touch(int x, int y, int lw, int lh);
static void input_motion_options_popup(int y);
static void input_draw_autosave_warn(SDL_Renderer *r, int lw, int lh);
static void input_draw_btmap_popup(SDL_Renderer *r, int lw, int lh);
static int  input_touch_autosave_warn(int x, int y, int lw, int lh);
static int  dpad_compute_r(int lh);  /* compute dpad radius from current settings */

/* ---- Virtual button IDs ---- */
typedef enum {
    VBTN_NONE = 0,
    VBTN_UP,
    VBTN_DOWN,
    VBTN_LEFT,
    VBTN_RIGHT,
    VBTN_FIRE1,
    VBTN_FIRE2,
    VBTN_PAUSE,
    VBTN_RESET,
    VBTN_SELECT,
    VBTN_SAVE,
    VBTN_LOAD,
    VBTN_ZOOM,
    VBTN_BACK,
    VBTN_OPTIONS
} VBtn;

/* ---- Per-finger slot tracking ---- */
#define MAX_FINGERS 10
typedef struct {
    SDL_FingerID id;
    VBtn         btn;
    int          active;
} TouchSlot;
static TouchSlot g_slots[MAX_FINGERS];

/* ---- One-shot button flags (consumed by main loop) ---- */
static int g_pause_flag   = 0;
static int g_save_flag    = 0;
static int g_load_flag    = 0;
static int g_zoom_flag    = 0;
static int g_back_flag    = 0;
static int g_options_flag = 0;

/* ---- Level state (held) ---- */
static int g_reset_held  = 0;
static int g_select_held = 0;

/* ---- Popups ---- */
static int g_options_popup_visible  = 0;
static int g_options_just_opened    = 0;   /* absorb the FINGERUP that opened the popup */
static int g_confirm_visible        = 0;
static int g_btmap_visible          = 0;   /* Bluetooth controller map popup */
static int g_confirm_result         = -1;  /* -1=pending, 0=No, 1=Yes */
static int g_opt_autosave_warn      = 0;   /* warn before disabling Ask Before Saving */
static int g_save_exists            = 0;

/* ---- DPad finger tracking ---- */
static int          g_dpad_touch_active = 0;
static int          g_dpad_touch_x      = 0;
static int          g_dpad_touch_y      = 0;
static SDL_FingerID g_dpad_finger_id    = 0;

/* ---- Options popup scroll state ---- */
static int g_opt_scroll_px      = 0;  /* pixel scroll offset into row list */
static int g_opt_touch_start_y  = 0;  /* y at finger-down (tap vs scroll) */
static int g_opt_last_touch_y   = 0;  /* y at last motion event */
static int g_opt_did_scroll     = 0;  /* 1 once movement exceeds tap threshold */

/* ---- Notification ---- */
#define NOTIFY_DURATION_MS 1500
static char    g_notify_msg[64];
static Uint32  g_notify_until = 0;

/* ---- Zoom mode label (white, top-center, no background) ---- */
static char    g_zoom_label_msg[32];
static Uint32  g_zoom_label_until = 0;

/* ---- Settings ---- */
static int g_autosave       = 0;
static int g_autosave_ask   = 1;
static int g_control_dim    = 2;   /* 0=Off, 1=Low, 2=Med, 3=High */
static int g_btn_size       = 1;   /* 0=Small, 1=Medium, 2=Large */
static int g_dpad_size      = 1;

/* ---- GameController ---- */
static SDL_GameController *g_controller = NULL;

/* ---- Display density (dp → px) ---- *
 * All button SIZES are defined here in dp and converted once via px_dp().
 * Screen pixel dimensions are only used for POSITIONING (anchoring to edges).
 * This keeps controls physically consistent across foldables and display changes. */
static float g_density = 2.0f;   /* updated by input_set_display_density() */

/* Fixed dp values — never derived from screenWidth/screenHeight */
#define TOP_BAR_DP      48    /* top control bar height */
#define BOT_BAR_DP      48    /* bottom control bar height */
#define BTN_TOP_W_DP    64    /* each top-bar button width (BACK/PAUSE/SEL/RST) */
#define DPAD_R_DP       80    /* d-pad circle base radius */
#define FIRE_R_DP       60    /* fire button base radius */
#define OPT_ROW_DP      44    /* options popup row height */
#define CONF_BTN_DP     40    /* confirm popup button height */
#define OPT_POPUP_W_DP 440    /* options popup width — fixed dp, never from screen width */
#define CONF_POPUP_W_DP 380   /* confirm popup width — fixed dp, never from screen width */

static int px_dp(int dp_val) { return (int)(dp_val * g_density + 0.5f); }

/* Button/dpad size scale factors: 0=Small, 1=Medium, 2=Large */
static const float size_scales[]      = {0.6f, 0.8f, 1.1f};
static const float fire_btn_scales[]  = {0.6f, 0.8f, 0.99f}; /* Large reduced 10% */

/* Font scale matching filepicker.c: round(2 * density), clamped [2,8] */
static int input_font_scale(void) {
    int fs = (int)(g_density * 2.0f + 0.5f);
    if (fs < 2) fs = 2;
    if (fs > 8) fs = 8;
    return fs;
}

void input_set_display_density(float d) { if (d > 0.0f) g_density = d; }

/* ---- Helpers ---- */

#define DIR_UP    0
#define DIR_DOWN  1
#define DIR_LEFT  2
#define DIR_RIGHT 3

static void set_dpad(int dir, int pressed)
{
    machine_set_joystick(0, dir, pressed);
}

static void apply_btn(VBtn btn, int pressed)
{
    switch (btn) {
    case VBTN_UP:     set_dpad(DIR_UP,    pressed); break;
    case VBTN_DOWN:   set_dpad(DIR_DOWN,  pressed); break;
    case VBTN_LEFT:   set_dpad(DIR_LEFT,  pressed); break;
    case VBTN_RIGHT:  set_dpad(DIR_RIGHT, pressed); break;
    case VBTN_FIRE1:  machine_set_trigger(0, pressed);  break;
    case VBTN_FIRE2:  machine_set_trigger2(0, pressed); break;
    case VBTN_RESET:
        g_reset_held = pressed;
        machine_set_switch(0, pressed);
        break;
    case VBTN_SELECT:
        g_select_held = pressed;
        machine_set_switch(1, pressed);
        break;
    case VBTN_PAUSE:
        if (pressed) g_pause_flag = 1;
        break;
    case VBTN_SAVE:
        if (pressed) g_save_flag = 1;
        break;
    case VBTN_LOAD:
        if (pressed && g_save_exists) g_load_flag = 1;
        break;
    case VBTN_ZOOM:
        if (pressed) g_zoom_flag = 1;
        break;
    case VBTN_BACK:
        if (pressed) g_back_flag = 1;
        break;
    case VBTN_OPTIONS:
        if (pressed) g_options_flag = 1;
        break;
    default: break;
    }
}

/*
 * Returns 1 if the leftmost visible fire button overlaps the OPTIONS button
 * horizontally, meaning we should use the shorter "OPTS" label.
 */
static int fire_overlaps_options(int lw, int lh, int fs)
{
    float btn_sc = fire_btn_scales[g_btn_size < 0 ? 0 : g_btn_size > 2 ? 2 : g_btn_size];
    int fire_r = (int)(px_dp(FIRE_R_DP) * btn_sc);
    if (fire_r < px_dp(16)) fire_r = px_dp(16);
    /* Mirror the exact ctrl_h calculation from input_draw_overlay */
    int main_top = px_dp(TOP_BAR_DP);
    int main_h   = lh - main_top;
    int ctrl_top = main_top + main_h * 3 / 10;
    int ctrl_h   = lh - ctrl_top;
    int fire_r_max = ctrl_h / 2 - px_dp(12);
    if (fire_r_max > px_dp(16) && fire_r > fire_r_max) fire_r = fire_r_max;

    int fire_margin = px_dp(12);
    int fire_gap    = px_dp(12);
    int fire2_cx    = lw - fire_r - fire_margin;
    int fire1_cx    = fire2_cx - fire_gap - 2 * fire_r;
    /* In 7800 mode two buttons exist; fire1 is the leftmost one */
    int leftmost_cx = (machine_get_type() == MACHINE_7800) ? fire1_cx : fire2_cx;
    int fire_left   = leftmost_cx - fire_r;

    int w_save        = font_string_width("SAVE", fs)    + px_dp(16);
    int w_load        = font_string_width("LOAD", fs)    + px_dp(16);
    int w_zoom        = font_string_width("ZOOM", fs)    + px_dp(16);
    int w_opts_layout = font_string_width("OPTIONS", fs) + px_dp(16);
    int gap           = px_dp(4);
    int total_act_w   = w_save + w_load + w_zoom + w_opts_layout + gap * 3;
    int bx_start      = (lw - total_act_w) / 2;
    int opts_left     = bx_start + w_save + gap + w_load + gap + w_zoom + gap;
    int opts_right    = opts_left + w_opts_layout;

    /* Overlap: any part of the fire circle is over any part of the OPTIONS button */
    return fire_left < opts_right;
}

/*
 * Map pixel touch coordinates to a virtual button zone.
 * Bar heights use fixed dp values (never screen %).
 * D-pad / fire zones are screen-fraction anchors (position only, not size).
 */
static VBtn get_virtual_btn_px(int x, int y, int lw, int lh)
{
    int fs    = input_font_scale();
    int top_h = px_dp(TOP_BAR_DP);
    int btn_w = font_string_width("SELECT", fs) + px_dp(16);

    /* Top bar — gaps between BACK/PAUSE and SELECT/RESET match action-bar gap */
    int bar_gap = px_dp(4);
    if (y < top_h) {
        if      (x < btn_w)                                               return VBTN_BACK;
        else if (x >= btn_w + bar_gap && x < btn_w * 2 + bar_gap)        return VBTN_PAUSE;
        else if (x >= lw - btn_w * 2 - bar_gap && x < lw - btn_w)       return VBTN_SELECT;
        else if (x >= lw - btn_w)                                         return VBTN_RESET;
        else                                                              return VBTN_NONE;
    }

    /* Main area (full height below top bar) */
    int main_top = top_h;
    int main_bot = lh;
    int main_h   = main_bot - main_top;
    if (main_h <= 0) return VBTN_NONE;

    /* Center action buttons: SAVE / LOAD / ZOOM / OPTIONS (or OPTS when fire overlaps).
     * Always use OPTIONS width for the centering calculation so SAVE/LOAD/ZOOM
     * don't shift position when the label switches to OPTS. */
    {
        const char *opt_lbl = fire_overlaps_options(lw, lh, fs) ? "OPTS" : "OPTIONS";
        int w_save = font_string_width("SAVE", fs) + px_dp(16);
        int w_load = font_string_width("LOAD", fs) + px_dp(16);
        int w_zoom = font_string_width("ZOOM", fs) + px_dp(16);
        int w_opts = font_string_width(opt_lbl, fs) + px_dp(16);
        int w_opts_layout = font_string_width("OPTIONS", fs) + px_dp(16);
        int bah = px_dp(BOT_BAR_DP) * 4 / 5;
        int gap = px_dp(4);
        int total_act_w = w_save + w_load + w_zoom + w_opts_layout + gap * 3;
        int ax0 = (lw - total_act_w) / 2;
        int ay0 = lh - bah;
        if (y >= ay0 && y < lh) {
            static const VBtn act[] = {VBTN_SAVE, VBTN_LOAD, VBTN_ZOOM, VBTN_OPTIONS};
            const int act_ws[] = {w_save, w_load, w_zoom, w_opts};
            int bx = ax0;
            for (int i = 0; i < 4; i++) {
                if (x >= bx && x < bx + act_ws[i]) return act[i];
                bx += act_ws[i] + gap;
            }
            return VBTN_NONE;
        }
    }

    /* Control zone: bottom 70% of main area */
    int ctrl_top = main_top + main_h * 3 / 10;

    /* D-pad zone: anchored bottom-left, matches draw code */
    {
        float ds  = size_scales[g_dpad_size < 0 ? 0 : g_dpad_size > 2 ? 2 : g_dpad_size];
        int   dr  = (int)(px_dp(DPAD_R_DP) * ds);
        if (dr < px_dp(20)) dr = px_dp(20);
        int dr_max = (lh - ctrl_top) / 2 - px_dp(12);
        if (dr_max > px_dp(20) && dr > dr_max) dr = dr_max;
        int dcx = dr + px_dp(12);
        int dcy = lh - dr - px_dp(12);
        /* Hit box: full bounding square of the dpad */
        if (x <= dcx + dr && y >= dcy - dr) {
            float rel_x = (float)(x - (dcx - dr)) / (float)(dr * 2);
            float rel_y = (float)(y - (dcy - dr)) / (float)(dr * 2);
            int col = (int)(rel_x * 3.0f); if (col > 2) col = 2; if (col < 0) col = 0;
            int row = (int)(rel_y * 3.0f); if (row > 2) row = 2; if (row < 0) row = 0;
            if (row == 0) return VBTN_UP;
            if (row == 2) return VBTN_DOWN;
            if (col == 0) return VBTN_LEFT;
            if (col == 2) return VBTN_RIGHT;
            return VBTN_NONE;
        }
    }

    /* Right zone: fire buttons (bottom 70% only).
     * In 2600 mode only one button (far right = FIRE1). */
    if (x > lw * 72 / 100 && y >= ctrl_top) {
        if (machine_get_type() != MACHINE_7800) return VBTN_FIRE1;
        float btn_sc   = fire_btn_scales[g_btn_size < 0 ? 0 : g_btn_size > 2 ? 2 : g_btn_size];
        int   fr       = (int)(px_dp(FIRE_R_DP) * btn_sc);
        if (fr < px_dp(16)) fr = px_dp(16);
        int   fire2_cx = lw - fr - px_dp(12);
        int   fire1_cx = fire2_cx - px_dp(12) - 2 * fr;
        int   split    = (fire1_cx + fire2_cx) / 2;
        return (x < split) ? VBTN_FIRE1 : VBTN_FIRE2;
    }

    return VBTN_NONE;
}

/* Return the dpad radius for the current settings and screen height. */
static int dpad_compute_r(int lh)
{
    int top_h    = px_dp(TOP_BAR_DP);
    int ctrl_top = top_h + (lh - top_h) * 3 / 10;
    float ds = size_scales[g_dpad_size < 0 ? 0 : g_dpad_size > 2 ? 2 : g_dpad_size];
    int dr = (int)(px_dp(DPAD_R_DP) * ds);
    if (dr < px_dp(20)) dr = px_dp(20);
    int dr_max = (lh - ctrl_top) / 2 - px_dp(12);
    if (dr_max > px_dp(20) && dr > dr_max) dr = dr_max;
    return dr;
}

/* Return 1 if (x,y) is within the dpad bounding box for screen height lh. */
static int is_in_dpad_zone(int x, int y, int lh)
{
    int dr  = dpad_compute_r(lh);
    int dcx = dr + px_dp(12);
    int dcy = lh - dr - px_dp(12);
    return (x <= dcx + dr && y >= dcy - dr);
}

/* ---- Input init ---- */

void input_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_pause_flag          = 0;
    g_save_flag           = 0;
    g_load_flag           = 0;
    g_zoom_flag           = 0;
    g_back_flag           = 0;
    g_options_flag        = 0;
    g_reset_held          = 0;
    g_select_held         = 0;
    g_options_popup_visible = 0;
    g_btmap_visible         = 0;
    g_confirm_visible       = 0;
    g_confirm_result        = -1;
    g_dpad_touch_active     = 0;
    g_notify_until          = 0;
    g_notify_msg[0]         = '\0';
    SDL_GameControllerEventState(SDL_ENABLE);
}

/* ---- Event dispatch ---- */

void input_handle_event(SDL_Event *e)
{
    switch (e->type) {

    /* ---- Multitouch ---- */
    case SDL_FINGERDOWN: {
        if (g_opt_autosave_warn) return;  /* warn popup absorbs all input */
        if (g_btmap_visible)     return;  /* btmap popup absorbs all input */
        /* If options popup visible, route touch to popup handler */
        if (g_options_popup_visible) {
            int sw, sh;
            SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
            if (win) SDL_GL_GetDrawableSize(win, &sw, &sh);
            else { sw = 1024; sh = 600; }
            input_touch_options_popup(
                (int)(e->tfinger.x * sw),
                (int)(e->tfinger.y * sh), 0);
            return;
        }
        if (g_confirm_visible) return;  /* block game input while confirm up */

        int sw = 1024, sh = 600;
        { SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
          if (win) SDL_GL_GetDrawableSize(win, &sw, &sh);
          if (sw <= 0 || sh <= 0) { sw = 1024; sh = 600; } }
        int touch_x = (int)(e->tfinger.x * sw);
        int touch_y = (int)(e->tfinger.y * sh);
        VBtn btn = get_virtual_btn_px(touch_x, touch_y, sw, sh);
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!g_slots[i].active) {
                g_slots[i].active = 1;
                g_slots[i].id     = e->tfinger.fingerId;
                g_slots[i].btn    = btn;
                apply_btn(btn, 1);
                break;
            }
        }
        /* Start dpad touch tracking */
        if (!g_dpad_touch_active && is_in_dpad_zone(touch_x, touch_y, sh)) {
            g_dpad_touch_active = 1;
            g_dpad_touch_x      = touch_x;
            g_dpad_touch_y      = touch_y;
            g_dpad_finger_id    = e->tfinger.fingerId;
        }
        break;
    }

    case SDL_FINGERUP: {
        int sw, sh;
        {
            SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
            if (win) SDL_GL_GetDrawableSize(win, &sw, &sh);
            else { sw = 1024; sh = 600; }
            if (sw <= 0 || sh <= 0) { sw = 1024; sh = 600; }
        }
        int px = (int)(e->tfinger.x * sw);
        int py_coord = (int)(e->tfinger.y * sh);

        if (g_opt_autosave_warn) {
            input_touch_autosave_warn(px, py_coord, sw, sh);
            return;
        }
        if (g_btmap_visible) {
            g_btmap_visible = 0;  /* tap anywhere closes the map */
            return;
        }
        if (g_options_popup_visible) {
            input_touch_options_popup(px, py_coord, 1);
            return;
        }
        if (g_confirm_visible) {
            input_handle_confirm_touch(px, py_coord, sw, sh);
            return;
        }
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (g_slots[i].active && g_slots[i].id == e->tfinger.fingerId) {
                apply_btn(g_slots[i].btn, 0);
                g_slots[i].active = 0;
                break;
            }
        }
        if (g_dpad_touch_active && e->tfinger.fingerId == g_dpad_finger_id)
            g_dpad_touch_active = 0;
        break;
    }

    case SDL_FINGERMOTION: {
        if (g_opt_autosave_warn) return;  /* warn popup absorbs all input */
        if (g_btmap_visible)     return;
        if (g_options_popup_visible) {
            int sh = 600;
            SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
            if (win) { int sw; SDL_GL_GetDrawableSize(win, &sw, &sh); }
            input_motion_options_popup((int)(e->tfinger.y * sh));
            return;
        }
        if (g_confirm_visible) return;
        {
            int sw = 1024, sh = 600;
            { SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
              if (win) SDL_GL_GetDrawableSize(win, &sw, &sh);
              if (sw <= 0 || sh <= 0) { sw = 1024; sh = 600; } }
            int mx = (int)(e->tfinger.x * sw);
            int my = (int)(e->tfinger.y * sh);
            for (int i = 0; i < MAX_FINGERS; i++) {
                if (g_slots[i].active && g_slots[i].id == e->tfinger.fingerId) {
                    VBtn new_btn = get_virtual_btn_px(mx, my, sw, sh);
                    if (new_btn != g_slots[i].btn) {
                        apply_btn(g_slots[i].btn, 0);
                        g_slots[i].btn = new_btn;
                        apply_btn(new_btn, 1);
                    }
                    break;
                }
            }
            if (g_dpad_touch_active && e->tfinger.fingerId == g_dpad_finger_id) {
                g_dpad_touch_x = mx;
                g_dpad_touch_y = my;
            }
        }
        break;
    }

    /* ---- Mouse (for emulator testing on desktop) ---- */
    case SDL_MOUSEBUTTONDOWN: {
        if (g_options_popup_visible || g_confirm_visible) return;
        int sw, sh;
        SDL_Window *win = SDL_RenderGetWindow(video_get_renderer());
        if (win) SDL_GL_GetDrawableSize(win, &sw, &sh);
        else { sw = 1024; sh = 600; }
        if (sw <= 0 || sh <= 0) break;
        VBtn btn = get_virtual_btn_px(e->button.x, e->button.y, sw, sh);
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!g_slots[i].active) {
                g_slots[i].active = 1;
                g_slots[i].id     = (SDL_FingerID)(-1 - e->button.which);
                g_slots[i].btn    = btn;
                apply_btn(btn, 1);
                break;
            }
        }
        break;
    }

    case SDL_MOUSEBUTTONUP: {
        SDL_FingerID fake_id = (SDL_FingerID)(-1 - e->button.which);
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (g_slots[i].active && g_slots[i].id == fake_id) {
                apply_btn(g_slots[i].btn, 0);
                g_slots[i].active = 0;
                break;
            }
        }
        break;
    }

    /* ---- GameController device events ---- */
    case SDL_CONTROLLERDEVICEADDED:
        if (!g_controller) {
            g_controller = SDL_GameControllerOpen(e->cdevice.which);
            if (g_controller) log_msg("input: controller connected");
        }
        break;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_controller) {
            SDL_Joystick *js = SDL_GameControllerGetJoystick(g_controller);
            SDL_JoystickID id = SDL_JoystickInstanceID(js);
            if (id == e->cdevice.which) {
                SDL_GameControllerClose(g_controller);
                g_controller = NULL;
                log_msg("input: controller disconnected");
            }
        }
        break;

    /* ---- GameController buttons ---- */
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int pressed = (e->type == SDL_CONTROLLERBUTTONDOWN);
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    set_dpad(DIR_UP,    pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  set_dpad(DIR_DOWN,  pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  set_dpad(DIR_LEFT,  pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: set_dpad(DIR_RIGHT, pressed); break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_Y:
            machine_set_trigger(0, pressed);  break;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_X:
            machine_set_trigger2(0, pressed); break;
        case SDL_CONTROLLER_BUTTON_START:
            if (pressed) g_pause_flag = 1;   break;
        case SDL_CONTROLLER_BUTTON_BACK:       /* View/Select → return to filepicker */
            if (pressed) g_back_flag = 1;    break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (pressed) g_save_flag = 1;    break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (pressed) g_load_flag = 1;    break;
        default: break;
        }
        break;
    }

    /* ---- GameController axes ---- */
    case SDL_CONTROLLERAXISMOTION: {
        const int DEAD = 8000;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            set_dpad(DIR_LEFT,  e->caxis.value < -DEAD);
            set_dpad(DIR_RIGHT, e->caxis.value >  DEAD);
        } else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            set_dpad(DIR_UP,   e->caxis.value < -DEAD);
            set_dpad(DIR_DOWN, e->caxis.value >  DEAD);
        }
        break;
    }

    /* ---- Keyboard ---- */
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int pressed = (e->type == SDL_KEYDOWN);
        switch (e->key.keysym.sym) {
        case SDLK_UP:       set_dpad(DIR_UP,    pressed); break;
        case SDLK_DOWN:     set_dpad(DIR_DOWN,  pressed); break;
        case SDLK_LEFT:     set_dpad(DIR_LEFT,  pressed); break;
        case SDLK_RIGHT:    set_dpad(DIR_RIGHT, pressed); break;
        case SDLK_z:
        case SDLK_SPACE:    machine_set_trigger(0,  pressed); break;
        case SDLK_x:        machine_set_trigger2(0, pressed); break;
        case SDLK_RETURN:   if (pressed) g_pause_flag  = 1;  break;
        case SDLK_F2:       if (pressed) g_save_flag   = 1;  break;
        case SDLK_F3:       if (pressed) g_load_flag   = 1;  break;
        case SDLK_ESCAPE:   if (pressed) g_back_flag   = 1;  break;
        case SDLK_r:
            g_reset_held = pressed;
            machine_set_switch(0, pressed); break;
        case SDLK_s:
            g_select_held = pressed;
            machine_set_switch(1, pressed); break;
        default: break;
        }
        break;
    }

    default: break;
    }
}

/* ---- Drawing helpers ---- */

static void draw_filled_rect(SDL_Renderer *r, int x, int y, int w, int h,
                             Uint8 rv, Uint8 gv, Uint8 bv, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, rv, gv, bv, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void draw_rect_outline(SDL_Renderer *r, int x, int y, int w, int h,
                               Uint8 rv, Uint8 gv, Uint8 bv, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, rv, gv, bv, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

/* Draw a filled circle (approximate) */
static void draw_circle_filled(SDL_Renderer *r, int cx, int cy, int radius,
                                Uint8 rv, Uint8 gv, Uint8 bv, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, rv, gv, bv, a);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)SDL_sqrt((double)(radius * radius - dy * dy));
        SDL_Rect line = {cx - dx, cy + dy, dx * 2, 1};
        SDL_RenderFillRect(r, &line);
    }
}

/* Draw centered label text in a rect */
static void draw_btn_label(SDL_Renderer *r, const char *label,
                           int x, int y, int w, int h, int scale,
                           Uint8 rv, Uint8 gv, Uint8 bv)
{
    int tw = font_string_width(label, scale);
    int th = 8 * scale;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - th) / 2;
    font_draw_string(r, label, tx, ty, scale, rv, gv, bv);
}

/* Get control alpha based on g_control_dim setting.
 * Off (0): UI bars show at full alpha; gamepad hidden separately.
 * Low/Med/High (1-3): dims all overlay elements. */
static Uint8 ctrl_alpha(Uint8 base)
{
    static const float factors[] = {0.35f, 0.35f, 0.65f, 1.0f}; /* Off→Low dim, Low, Med, High */
    int d = g_control_dim;
    if (d < 0) d = 0;
    if (d > 3) d = 3;
    return (Uint8)((float)base * factors[d]);
}

/* ---- Main overlay draw ---- */

void input_draw_overlay(SDL_Renderer *renderer)
{
    if (!renderer) return;

    int lw, lh;
    SDL_Window *win = SDL_RenderGetWindow(renderer);
    if (win) {
        SDL_GL_GetDrawableSize(win, &lw, &lh);
        if (lw <= 0 || lh <= 0) SDL_GetWindowSize(win, &lw, &lh);
    } else {
        SDL_GetWindowSize(SDL_GL_GetCurrentWindow(), &lw, &lh);
    }
    if (lw <= 0 || lh <= 0) return;

    int top_h    = px_dp(TOP_BAR_DP);
    int main_top = top_h;
    int main_bot = lh;
    int main_h   = main_bot - main_top;

    int   fs     = input_font_scale();
    /* Top bar button width = widest label (SELECT) + 8dp each side */
    int   btn_w  = font_string_width("SELECT", fs) + px_dp(16);
    Uint8 a_fill = ctrl_alpha(70);

    /* ---- Top bar — all buttons same width, grey with orange text ---- */
    /* gap between BACK/PAUSE and SELECT/RESET matches action-bar button gap */
    int bar_gap = px_dp(4);

    /* BACK — anchored left, orange background with white text */
    draw_filled_rect(renderer, 0, 0, btn_w, top_h, 200, 100, 0, a_fill);
    draw_btn_label(renderer, "BACK", 0, 0, btn_w, top_h, fs,
                   ctrl_alpha(255), ctrl_alpha(255), ctrl_alpha(255));

    /* PAUSE */
    int pau_x = btn_w + bar_gap;
    draw_filled_rect(renderer, pau_x, 0, btn_w, top_h, 60, 60, 60, a_fill);
    draw_btn_label(renderer, "PAUSE", pau_x, 0, btn_w, top_h, fs,
                   ctrl_alpha(200), ctrl_alpha(100), 0);

    /* SELECT — anchored right, with gap before RESET */
    int sel_x = lw - btn_w * 2 - bar_gap;
    draw_filled_rect(renderer, sel_x, 0, btn_w, top_h, 60, 60, 60, a_fill);
    draw_btn_label(renderer, "SELECT", sel_x, 0, btn_w, top_h, fs,
                   ctrl_alpha(200), ctrl_alpha(100), 0);

    /* RESET */
    int rst_x = lw - btn_w;
    draw_filled_rect(renderer, rst_x, 0, btn_w, top_h, 60, 60, 60, a_fill);
    draw_btn_label(renderer, "RESET", rst_x, 0, btn_w, top_h, fs,
                   ctrl_alpha(200), ctrl_alpha(100), 0);

    /* ---- Control zone: bottom 70% of main area ---- */
    int ctrl_top = main_top + main_h * 3 / 10;
    int ctrl_h   = main_bot - ctrl_top;

    /* ---- D-pad and fire buttons — hidden when Controls Visibility = Off ---- */
    if (g_control_dim > 0) {

    /* ---- D-pad (bottom-left corner) ---- */
    float dpad_scale = size_scales[g_dpad_size < 0 ? 0 : g_dpad_size > 2 ? 2 : g_dpad_size];
    int dpad_r = (int)(px_dp(DPAD_R_DP) * dpad_scale);
    if (dpad_r < px_dp(20)) dpad_r = px_dp(20);
    int dpad_r_max = ctrl_h / 2 - px_dp(12);
    if (dpad_r_max > px_dp(20) && dpad_r > dpad_r_max) dpad_r = dpad_r_max;

    /* Anchor dpad to bottom-left; larger sizes grow right automatically */
    int dpad_cx = dpad_r + px_dp(12);
    int dpad_cy = main_bot - dpad_r - px_dp(12);

    /* Outer circle (gray) */
    draw_circle_filled(renderer, dpad_cx, dpad_cy, dpad_r, 120, 120, 120, a_fill);

    /* Cross bars — thin, stop short of circle edge, orange to match button text */
    int arm_w = dpad_r / 10;
    if (arm_w < 2) arm_w = 2;
    int inset = dpad_r / 4;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, ctrl_alpha(200), ctrl_alpha(100), 0, ctrl_alpha(180));
    SDL_Rect hbar = {dpad_cx - (dpad_r - inset), dpad_cy - arm_w / 2, (dpad_r - inset) * 2, arm_w};
    SDL_Rect vbar = {dpad_cx - arm_w / 2, dpad_cy - (dpad_r - inset), arm_w, (dpad_r - inset) * 2};
    SDL_RenderFillRect(renderer, &hbar);
    SDL_RenderFillRect(renderer, &vbar);

    /* Finger touch indicator — orange circle tracking current touch position */
    if (g_dpad_touch_active) {
        draw_circle_filled(renderer, g_dpad_touch_x, g_dpad_touch_y,
                           dpad_r / 4, 220, 120, 0, ctrl_alpha(200));
    }

    /* ---- Fire buttons (bottom-right) ---- */
    float btn_scale = fire_btn_scales[g_btn_size < 0 ? 0 : g_btn_size > 2 ? 2 : g_btn_size];
    int fire_r = (int)(px_dp(FIRE_R_DP) * btn_scale);
    if (fire_r < px_dp(16)) fire_r = px_dp(16);
    int fire_r_max = ctrl_h / 2 - px_dp(12);
    if (fire_r_max > px_dp(16) && fire_r > fire_r_max) fire_r = fire_r_max;

    int fire_margin = px_dp(12);
    int fire_gap    = px_dp(12);
    int fire_cy  = main_bot - fire_r - fire_margin;
    int fire2_cx = lw - fire_r - fire_margin;
    int fire1_cx = fire2_cx - fire_gap - 2 * fire_r;

    int is_7800 = (machine_get_type() == MACHINE_7800);

    if (is_7800) {
        /* 7800: two fire buttons */
        draw_circle_filled(renderer, fire1_cx, fire_cy, fire_r, 220, 120, 0, a_fill);
        draw_circle_filled(renderer, fire2_cx, fire_cy, fire_r, 220, 120, 0, a_fill);
    } else {
        /* 2600: single fire button at far-right position */
        draw_circle_filled(renderer, fire2_cx, fire_cy, fire_r, 220, 120, 0, a_fill);
    }

    } /* end if (g_control_dim > 0) */

    /* ---- Center action buttons: SAVE / LOAD / ZOOM / OPTIONS (OPTS when fire overlaps) ---- */
    {
        const char *opt_lbl = fire_overlaps_options(lw, lh, fs) ? "OPTS" : "OPTIONS";
        /* SAVE/LOAD/ZOOM sized to their own label; OPTIONS keeps the wider size.
         * Use OPTIONS width for centering so SAVE/LOAD/ZOOM don't shift when OPTS appears. */
        int w_save = font_string_width("SAVE", fs) + px_dp(16);
        int w_load = font_string_width("LOAD", fs) + px_dp(16);
        int w_zoom = font_string_width("ZOOM", fs) + px_dp(16);
        int w_opts = font_string_width(opt_lbl, fs) + px_dp(16);
        int w_opts_layout = font_string_width("OPTIONS", fs) + px_dp(16);
        int bah = px_dp(BOT_BAR_DP) * 4 / 5;   /* 20% shorter than bar height */
        int gap = px_dp(4);
        int total_act_w = w_save + w_load + w_zoom + w_opts_layout + gap * 3;
        int bx_start = (lw - total_act_w) / 2;  /* centered on full screen */
        int ay0 = lh - bah;
        const char *act_labels[] = {"SAVE", "LOAD", "ZOOM", opt_lbl};
        const int   act_widths[] = {w_save, w_load, w_zoom, w_opts};
        int bx = bx_start;
        for (int i = 0; i < 4; i++) {
            int bw_i = act_widths[i];
            int greyed = (i == 1 && !g_save_exists);
            draw_filled_rect(renderer, bx, ay0, bw_i, bah, 60, 60, 60,
                             greyed ? ctrl_alpha(25) : a_fill);
            if (greyed)
                draw_btn_label(renderer, act_labels[i], bx, ay0, bw_i, bah,
                               fs, ctrl_alpha(70), ctrl_alpha(70), ctrl_alpha(70));
            else
                draw_btn_label(renderer, act_labels[i], bx, ay0, bw_i, bah,
                               fs, ctrl_alpha(200), ctrl_alpha(100), 0);
            bx += bw_i + gap;
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    /* ---- Notification — top center, white ---- */
    if (g_notify_until > 0 && SDL_GetTicks() < g_notify_until && g_notify_msg[0]) {
        int tw = font_string_width(g_notify_msg, fs);
        int th = 8 * fs;
        int nx = (lw - tw) / 2;
        int ny = top_h + px_dp(6);
        draw_filled_rect(renderer, nx - 8, ny - 4, tw + 16, th + 8, 0, 0, 0, 160);
        font_draw_string(renderer, g_notify_msg, nx, ny, fs, 255, 255, 255);
    }

    /* ---- Zoom mode label (white, centered in top bar between PAUSE and SELECT) ---- */
    if (g_zoom_label_until > 0 && SDL_GetTicks() < g_zoom_label_until && g_zoom_label_msg[0]) {
        int tw = font_string_width(g_zoom_label_msg, fs);
        int ty = (top_h - 8 * fs) / 2;
        if (ty < 0) ty = 0;
        font_draw_string(renderer, g_zoom_label_msg,
                         (lw - tw) / 2, ty, fs, 255, 255, 255);
    }

    /* ---- Confirm popup (filepicker style) ---- */
    if (g_confirm_visible) {
        int ts  = fs + 1;
        /* Button size matches top-bar buttons (BACK/PAUSE/SELECT/RESET) */
        int bh2 = top_h;
        int bw2 = btn_w;   /* = font_string_width("SELECT", fs) + px_dp(16) */
        int pw  = px_dp(CONF_POPUP_W_DP);
        int min_pw = font_string_width("Save state before exit?", ts) + px_dp(24);
        int min_pw2 = bw2 * 2 + px_dp(8) + px_dp(24);
        if (pw < min_pw)  pw = min_pw;
        if (pw < min_pw2) pw = min_pw2;
        if (pw > lw * 9 / 10) pw = lw * 9 / 10;
        int title_h = 8 * ts + px_dp(16);
        int ph  = title_h + bh2 + px_dp(16);
        int px  = (lw - pw) / 2;
        int py  = (lh - ph) / 2;
        /* Background + border + inner shadow */
        draw_filled_rect(renderer, px, py, pw, ph, 20, 20, 20, 235);
        draw_rect_outline(renderer, px, py, pw, ph, 220, 140, 0, 255);
        draw_rect_outline(renderer, px + 1, py + 1, pw - 2, ph - 2, 150, 80, 0, 120);
        /* Title centered */
        int tx = px + (pw - font_string_width("Save state before exit?", ts)) / 2;
        font_draw_string(renderer, "Save state before exit?", tx, py + px_dp(8), ts, 200, 100, 0);
        /* YES / NO buttons centered, same size as top-bar buttons */
        int btn_gap = px_dp(8);
        int by    = py + ph - bh2 - px_dp(8);
        int yes_x = px + (pw - bw2 * 2 - btn_gap) / 2;
        int no_x  = yes_x + bw2 + btn_gap;
        draw_filled_rect(renderer, yes_x, by, bw2, bh2, 60, 60, 60, 220);
        draw_btn_label(renderer, "YES", yes_x, by, bw2, bh2, fs, 200, 100, 0);
        draw_filled_rect(renderer, no_x, by, bw2, bh2, 60, 60, 60, 220);
        draw_btn_label(renderer, "NO", no_x, by, bw2, bh2, fs, 200, 100, 0);
    }

    /* ---- Options popup ---- */
    if (g_options_popup_visible) {
        input_draw_options_popup(renderer);
    }
    if (g_btmap_visible) {
        input_draw_btmap_popup(renderer, lw, lh);
    }
    if (g_opt_autosave_warn) {
        input_draw_autosave_warn(renderer, lw, lh);
    }
}

/* ---- Options popup ---- */

#define OPT_ROWS 9
static const char *opt_labels[OPT_ROWS] = {
    "Auto-Save",
    "Ask Before Saving",
    "Controls Visibility",
    "DPad Size",
    "Button Size",
    "Scanlines",
    "Palette (7800)",
    "Bluetooth Controls",
    "Bug Report"
};

static void get_opt_value_str(int row, char *buf, int bufsz)
{
    static const char *onoff[]       = {"OFF", "ON"};
    static const char *visibility[]  = {"OFF", "DIMMER", "DIM", "BRIGHT"};
    static const char *sizes[]       = {"SMALL", "MEDIUM", "LARGE"};
    static const char *palettes[]    = {"WARM", "COOL", "ORIG"};
    static const char *scanbright[]  = {"OFF", "LIGHT", "MEDIUM", "DARK"};
    switch (row) {
    case 0: snprintf(buf, bufsz, "%s", onoff[g_autosave & 1]); break;
    case 1: snprintf(buf, bufsz, "%s", onoff[g_autosave_ask & 1]); break;
    case 2: snprintf(buf, bufsz, "%s", visibility[g_control_dim > 3 ? 3 : g_control_dim]); break;
    case 3: snprintf(buf, bufsz, "%s", sizes[g_dpad_size > 2 ? 2 : g_dpad_size]); break;
    case 4: snprintf(buf, bufsz, "%s", sizes[g_btn_size > 2 ? 2 : g_btn_size]); break;
    case 5: {
        int sl = !video_get_scanlines() ? 0 : (video_get_scanline_brightness() > 2 ? 2 : video_get_scanline_brightness()) + 1;
        snprintf(buf, bufsz, "%s", scanbright[sl]);
        break;
    }
    case 6: snprintf(buf, bufsz, "%s", palettes[video_get_maria_palette()]); break;
    case 7: snprintf(buf, bufsz, "MAP"); break;
    case 8: snprintf(buf, bufsz, "EMAIL"); break;
    default: buf[0] = '\0'; break;
    }
}

static void cycle_opt_value(int row)
{
    switch (row) {
    case 0:
        g_autosave = !g_autosave;
        if (!g_autosave) g_autosave_ask = 1;   /* reset to ON when auto-save disabled */
        break;
    case 1:
        if (!g_autosave) return;               /* greyed out — no-op */
        if (g_autosave_ask) {
            g_opt_autosave_warn = 1;           /* confirm before disabling */
            return;
        } else {
            g_autosave_ask = 1;                /* turn back ON immediately */
        }
        break;
    case 2: g_control_dim = (g_control_dim + 1) % 4; break;
    case 3: g_dpad_size   = (g_dpad_size   + 1) % 3; break;
    case 4: g_btn_size    = (g_btn_size    + 1) % 3; break;
    case 5:
        if (!video_get_scanlines()) {
            video_set_scanlines(1);
            video_set_scanline_brightness(0);  /* OFF → LIGHT */
        } else {
            int b = video_get_scanline_brightness();
            if (b < 2) video_set_scanline_brightness(b + 1);  /* LIGHT → MEDIUM → DARK */
            else       video_set_scanlines(0);                 /* DARK → OFF */
        }
        break;
    case 6: video_set_maria_palette((video_get_maria_palette() + 1) % 3); break;
    case 7: g_btmap_visible = 1; g_options_popup_visible = 0; return;
    case 8: jni_send_bug_report_email(); g_options_popup_visible = 0; return;
    default: break;
    }
    filepicker_save_settings();
}

/* ---- Bluetooth controller map popup ---- */

#define BTMAP_ROWS 9
static const char *btmap_buttons[BTMAP_ROWS] = {
    "B / Y", "A / X", "D-Pad",
    "LT", "RT", "Start", "Select", "LB", "RB"
};
static const char *btmap_actions[BTMAP_ROWS] = {
    "Fire 1", "Fire 2", "Joystick",
    "Select", "Reset", "Pause", "Back",
    "Save State", "Load State"
};

static void input_draw_btmap_popup(SDL_Renderer *r, int lw, int lh)
{
    int fs = input_font_scale();
    int ts = fs + 1;
    int row_h   = 8 * fs + px_dp(10);
    int title_h = 8 * ts + px_dp(16);

    /* Width: fit both columns + gap */
    int col1_w = 0, col2_w = 0;
    for (int i = 0; i < BTMAP_ROWS; i++) {
        int w = font_string_width(btmap_buttons[i], fs);
        if (w > col1_w) col1_w = w;
        w = font_string_width(btmap_actions[i], fs);
        if (w > col2_w) col2_w = w;
    }
    int col_gap = px_dp(32);
    int pw      = col1_w + col_gap + col2_w + px_dp(32);
    int title_pw = font_string_width("CONTROLS MAP", ts) + px_dp(24);
    if (pw < title_pw) pw = title_pw;
    if (pw > lw * 9 / 10) pw = lw * 9 / 10;

    int ph    = title_h + BTMAP_ROWS * row_h + px_dp(8);
    /* Shrink row_h to fit if screen is short */
    if (ph > lh) {
        row_h = (lh - title_h - px_dp(8)) / BTMAP_ROWS;
        ph    = title_h + BTMAP_ROWS * row_h + px_dp(8);
    }
    int pop_x = (lw - pw) / 2;
    int pop_y = (lh - ph) / 2;
    if (pop_y < 0) pop_y = 0;

    /* Background + border + inner shadow */
    draw_filled_rect(r, pop_x, pop_y, pw, ph, 20, 20, 20, 235);
    draw_rect_outline(r, pop_x, pop_y, pw, ph, 220, 140, 0, 255);
    draw_rect_outline(r, pop_x + 1, pop_y + 1, pw - 2, ph - 2, 150, 80, 0, 120);

    /* Title */
    int tx = pop_x + (pw - font_string_width("CONTROLS MAP", ts)) / 2;
    font_draw_string(r, "CONTROLS MAP", tx, pop_y + px_dp(8), ts, 200, 100, 0);

    /* Separator */
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, pop_x + 4, pop_y + title_h, pop_x + pw - 4, pop_y + title_h);

    /* Column positions: button labels left-aligned, actions right-aligned */
    int x1 = pop_x + px_dp(16);
    int x2 = pop_x + pw - px_dp(16) - col2_w;

    for (int i = 0; i < BTMAP_ROWS; i++) {
        int ry = pop_y + title_h + i * row_h + (row_h - 8 * fs) / 2;
        font_draw_string(r, btmap_buttons[i], x1, ry, fs, 255, 255, 255);
        font_draw_string(r, btmap_actions[i], x2, ry, fs, 200, 100, 0);
    }
}

/* Options popup geometry — computed once, passed to draw/touch/motion handlers.
 * Matches filepicker settings popup: btn_h = px_dp(48)-4, row_h = btn_h+16. */
typedef struct {
    int fs, ts;
    int pw, ph, pop_x, pop_y;
    int title_h, btn_h, row_h;
    int scroll_area_h, max_scroll_px;
    int vbw;   /* value button width = pw/4 */
} OptGeom;

static void opt_compute_geom(int lw, int lh, OptGeom *g)
{
    g->fs      = input_font_scale();
    g->ts      = g->fs + 1;
    g->pw      = px_dp(OPT_POPUP_W_DP);
    if (g->pw > lw * 9 / 10) g->pw = lw * 9 / 10;
    g->title_h = 8 * g->ts + px_dp(16);
    g->btn_h   = px_dp(TOP_BAR_DP) - 4;      /* matches filepicker resume_h */
    g->row_h   = g->btn_h + 16;               /* 8px padding top + bottom */
    int ideal_ph = g->title_h + 8 + OPT_ROWS * g->row_h + 8;
    if (ideal_ph <= lh - px_dp(24)) {
        /* Fits comfortably — center the popup */
        g->ph    = ideal_ph;
        g->pop_y = (lh - g->ph) / 2;
    } else {
        /* Won't fit — expand to full screen height to minimize scrolling */
        g->ph    = lh;
        g->pop_y = 0;
    }
    g->pop_x   = (lw - g->pw) / 2;
    g->scroll_area_h = g->ph - g->title_h;
    g->max_scroll_px = 8 + OPT_ROWS * g->row_h + 8 - g->scroll_area_h;
    if (g->max_scroll_px < 0) g->max_scroll_px = 0;
    g->vbw = g->pw / 4;
}

void input_draw_options_popup(SDL_Renderer *r)
{
    if (!r) return;

    int lw, lh;
    SDL_Window *win = SDL_RenderGetWindow(r);
    if (win) {
        SDL_GL_GetDrawableSize(win, &lw, &lh);
        if (lw <= 0 || lh <= 0) SDL_GetWindowSize(win, &lw, &lh);
    } else {
        lw = 1024; lh = 600;
    }

    OptGeom g;
    opt_compute_geom(lw, lh, &g);

    /* Clamp scroll */
    if (g_opt_scroll_px < 0)                 g_opt_scroll_px = 0;
    if (g_opt_scroll_px > g.max_scroll_px)   g_opt_scroll_px = g.max_scroll_px;

    /* Background + border + inner shadow */
    draw_filled_rect(r, g.pop_x, g.pop_y, g.pw, g.ph, 20, 20, 20, 235);
    draw_rect_outline(r, g.pop_x, g.pop_y, g.pw, g.ph, 220, 140, 0, 255);
    draw_rect_outline(r, g.pop_x + 1, g.pop_y + 1, g.pw - 2, g.ph - 2, 150, 80, 0, 120);

    /* Title — always visible */
    int tx = g.pop_x + (g.pw - font_string_width("OPTIONS", g.ts)) / 2;
    font_draw_string(r, "OPTIONS", tx, g.pop_y + px_dp(8), g.ts, 200, 100, 0);

    /* Separator below title */
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, g.pop_x + 4, g.pop_y + g.title_h,
                          g.pop_x + g.pw - 4, g.pop_y + g.title_h);

    /* Scrollbar track + thumb (right edge, inside border) */
    int sb_w = px_dp(6);
    int sb_x = g.pop_x + g.pw - sb_w - 2;
    if (g.max_scroll_px > 0) {
        draw_filled_rect(r, sb_x, g.pop_y + g.title_h, sb_w, g.scroll_area_h, 40, 40, 40, 180);
        int thumb_h = g.scroll_area_h * g.scroll_area_h / (OPT_ROWS * g.row_h);
        if (thumb_h < px_dp(20)) thumb_h = px_dp(20);
        int thumb_range = g.scroll_area_h - thumb_h;
        int thumb_y = g.pop_y + g.title_h + g_opt_scroll_px * thumb_range / g.max_scroll_px;
        draw_filled_rect(r, sb_x, thumb_y, sb_w, thumb_h, 200, 120, 0, 220);
    }

    /* Scrollable rows — clipped */
    SDL_Rect scroll_clip = {g.pop_x + 1, g.pop_y + g.title_h, g.pw - 2, g.scroll_area_h};
    SDL_RenderSetClipRect(r, &scroll_clip);

    int   start_row   = g_opt_scroll_px / g.row_h;
    int   draw_rows   = (g.scroll_area_h + g.row_h - 1) / g.row_h + 1;
    int   ty_off      = (g.row_h - 8 * g.fs) / 2;
    int   btn_y_off   = (g.row_h - g.btn_h) / 2;
    int   bx          = g.pop_x + g.pw - g.vbw - sb_w - 8;
    char  val[32];

    for (int i = start_row; i < start_row + draw_rows && i < OPT_ROWS; i++) {
        int ry = g.pop_y + g.title_h + 8 + i * g.row_h - g_opt_scroll_px;

        /* Determine active state: greyed rows can't be toggled */
        int active = 1;
        if (i == 1 && !g_autosave)    active = 0;  /* Ask Before Saving */

        Uint8 lc  = active ? 255 :  100;   /* label colour */
        Uint8 ba  = active ? 220 :   80;   /* button bg alpha */
        Uint8 ta  = active ? 200 :   80;   /* button text alpha */

        font_draw_string(r, opt_labels[i], g.pop_x + 12, ry + ty_off, g.fs, lc, lc, lc);

        get_opt_value_str(i, val, sizeof(val));
        int vby = ry + btn_y_off;
        draw_filled_rect(r, bx, vby, g.vbw, g.btn_h, 60, 60, 60, ba);
        font_draw_string(r, val,
                         bx + (g.vbw - font_string_width(val, g.fs)) / 2,
                         vby + (g.btn_h - 8 * g.fs) / 2,
                         g.fs, ta, ta / 2, 0);
    }

    SDL_RenderSetClipRect(r, NULL);
}

/* Draw the "disable Ask Before Saving?" warning popup. */
static void input_draw_autosave_warn(SDL_Renderer *r, int lw, int lh)
{
    int fs       = input_font_scale();
    int ts       = fs + 1;
    int btn_h    = px_dp(TOP_BAR_DP) - 4;
    int btn_w    = px_dp(120);
    int line_h   = 8 * fs + 6;
    int title_h  = 8 * ts + px_dp(16);

    /* Size popup to fit the widest text line */
    int pw = font_string_width("This will automatically overwrite", fs) + px_dp(24);
    { int min2 = font_string_width("your save. Are you sure?", fs) + px_dp(24);
      if (min2 > pw) pw = min2; }
    if (pw > lw * 9 / 10) pw = lw * 9 / 10;

    int ph = title_h + px_dp(8) + 2 * line_h + px_dp(16) + btn_h + px_dp(8);
    int pop_x = (lw - pw) / 2;
    int pop_y = (lh - ph) / 2;

    draw_filled_rect(r, pop_x, pop_y, pw, ph, 20, 20, 20, 235);
    draw_rect_outline(r, pop_x, pop_y, pw, ph, 220, 140, 0, 255);
    draw_rect_outline(r, pop_x + 1, pop_y + 1, pw - 2, ph - 2, 150, 80, 0, 120);

    font_draw_string(r, "WARNING", pop_x + px_dp(12), pop_y + px_dp(8), ts, 200, 100, 0);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, pop_x + 4, pop_y + title_h, pop_x + pw - 4, pop_y + title_h);

    int cy = pop_y + title_h + px_dp(10);
    font_draw_string(r, "This will automatically overwrite", pop_x + px_dp(12), cy, fs, 255, 255, 255);
    cy += line_h;
    font_draw_string(r, "your save. Are you sure?", pop_x + px_dp(12), cy, fs, 255, 255, 255);

    int by      = pop_y + ph - btn_h - px_dp(8);
    int gap     = px_dp(16);
    int total_w = btn_w * 2 + gap;
    int bx_yes  = pop_x + (pw - total_w) / 2;
    int bx_no   = bx_yes + btn_w + gap;
    draw_filled_rect(r, bx_yes, by, btn_w, btn_h, 60, 60, 60, 220);
    draw_btn_label(r, "YES", bx_yes, by, btn_w, btn_h, fs, 200, 100, 0);
    draw_filled_rect(r, bx_no, by, btn_w, btn_h, 60, 60, 60, 220);
    draw_btn_label(r, "NO", bx_no, by, btn_w, btn_h, fs, 200, 100, 0);
}

/* Handle touch on the autosave-warn popup. Returns 1 if consumed. */
static int input_touch_autosave_warn(int x, int y, int lw, int lh)
{
    if (!g_opt_autosave_warn) return 0;

    int fs       = input_font_scale();
    int ts       = fs + 1;
    int btn_h    = px_dp(TOP_BAR_DP) - 4;
    int btn_w    = px_dp(120);
    int line_h   = 8 * fs + 6;
    int title_h  = 8 * ts + px_dp(16);

    int pw = font_string_width("This will automatically overwrite", fs) + px_dp(24);
    { int min2 = font_string_width("your save. Are you sure?", fs) + px_dp(24);
      if (min2 > pw) pw = min2; }
    if (pw > lw * 9 / 10) pw = lw * 9 / 10;

    int ph      = title_h + px_dp(8) + 2 * line_h + px_dp(16) + btn_h + px_dp(8);
    int pop_x   = (lw - pw) / 2;
    int pop_y   = (lh - ph) / 2;
    int by      = pop_y + ph - btn_h - px_dp(8);
    int gap     = px_dp(16);
    int total_w = btn_w * 2 + gap;
    int bx_yes  = pop_x + (pw - total_w) / 2;
    int bx_no   = bx_yes + btn_w + gap;

    if (x >= bx_yes && x <= bx_yes + btn_w && y >= by && y <= by + btn_h) {
        /* Yes — disable Ask Before Saving */
        g_autosave_ask      = 0;
        g_opt_autosave_warn = 0;
        filepicker_save_settings();
    } else if (x >= bx_no && x <= bx_no + btn_w && y >= by && y <= by + btn_h) {
        /* No — keep ON */
        g_autosave_ask      = 1;
        g_opt_autosave_warn = 0;
    } else if (x < pop_x || x > pop_x + pw || y < pop_y || y > pop_y + ph) {
        /* Tap outside — keep ON */
        g_autosave_ask      = 1;
        g_opt_autosave_warn = 0;
    }
    return 1;
}

/* Apply a scroll delta (pixels, positive = scroll down) with clamping. */
static void input_motion_options_popup(int y)
{
    if (!g_options_popup_visible) return;

    int dy = g_opt_last_touch_y - y;   /* finger moved up → positive dy → scroll down */
    g_opt_last_touch_y = y;

    if (abs(y - g_opt_touch_start_y) > px_dp(8))
        g_opt_did_scroll = 1;

    SDL_Renderer *r = video_get_renderer();
    int lw = 1024, lh = 600;
    SDL_Window *win = r ? SDL_RenderGetWindow(r) : NULL;
    if (win) {
        SDL_GL_GetDrawableSize(win, &lw, &lh);
        if (lw <= 0 || lh <= 0) SDL_GetWindowSize(win, &lw, &lh);
    }

    OptGeom g;
    opt_compute_geom(lw, lh, &g);

    g_opt_scroll_px += dy;
    if (g_opt_scroll_px < 0)               g_opt_scroll_px = 0;
    if (g_opt_scroll_px > g.max_scroll_px) g_opt_scroll_px = g.max_scroll_px;
}

/* Handle touch on the options popup.
 * x, y are in screen pixels; up=0 for finger-down, up=1 for finger-up. */
void input_touch_options_popup(int x, int y, int up)
{
    if (!g_options_popup_visible) return;

    SDL_Renderer *r = video_get_renderer();
    int lw, lh;
    SDL_Window *win = r ? SDL_RenderGetWindow(r) : NULL;
    if (win) {
        SDL_GL_GetDrawableSize(win, &lw, &lh);
        if (lw <= 0 || lh <= 0) SDL_GetWindowSize(win, &lw, &lh);
    } else {
        lw = 1024; lh = 600;
    }

    OptGeom g;
    opt_compute_geom(lw, lh, &g);

    if (!up) {
        g_opt_touch_start_y   = y;
        g_opt_last_touch_y    = y;
        g_opt_did_scroll      = 0;
        g_options_just_opened = 0;  /* finger-down inside popup: clear flag */
        return;
    }

    /* Ignore the FINGERUP from the touch that opened the popup */
    if (g_options_just_opened) {
        g_options_just_opened = 0;
        return;
    }

    /* Tap outside popup → close */
    if (x < g.pop_x || x > g.pop_x + g.pw || y < g.pop_y || y > g.pop_y + g.ph) {
        g_options_popup_visible = 0;
        return;
    }

    /* If finger moved enough, it was a scroll — don't cycle a value */
    if (g_opt_did_scroll) return;

    /* Tap: hit-test within scroll area */
    int rel_y = y - (g.pop_y + g.title_h);
    if (rel_y < 0 || rel_y >= g.scroll_area_h) return;
    int row = (rel_y + g_opt_scroll_px) / g.row_h;
    if (row >= OPT_ROWS) return;

    cycle_opt_value(row);
}

/* ---- One-shot and level queries ---- */

int input_pause_pressed(void)   { int v = g_pause_flag;   g_pause_flag   = 0; return v; }
int input_save_pressed(void)    { int v = g_save_flag;    g_save_flag    = 0; return v; }
int input_load_pressed(void)    { int v = g_load_flag;    g_load_flag    = 0; return v; }
int input_zoom_pressed(void)    { int v = g_zoom_flag;    g_zoom_flag    = 0; return v; }
int input_back_pressed(void)    { int v = g_back_flag;    g_back_flag    = 0; return v; }
int input_options_pressed(void) { int v = g_options_flag; g_options_flag = 0; return v; }
int input_reset_pressed(void)   { return g_reset_held;  }
int input_select_pressed(void)  { return g_select_held; }

int input_options_popup_visible(void)  { return g_options_popup_visible; }
int input_confirm_visible(void)        { return g_confirm_visible; }
int input_any_popup_visible(void)
{
    return g_options_popup_visible || g_confirm_visible || g_opt_autosave_warn || g_btmap_visible;
}

int input_autosave_warn_visible(void) { return g_opt_autosave_warn; }

void input_show_confirm(void)
{
    g_confirm_visible = 1;
    g_confirm_result  = -1;
}

int input_confirm_result(void)
{
    return g_confirm_result;
}

void input_close_popup(void)
{
    if (g_btmap_visible)         { g_btmap_visible = 0; return; }
    if (g_options_popup_visible) { g_options_popup_visible = 0; return; }
    if (g_confirm_visible)       { g_confirm_visible = 0; g_confirm_result = 0; return; }
}

/* Handle confirm popup touch. */
static void input_handle_confirm_touch(int x, int y, int lw, int lh)
{
    if (!g_confirm_visible) return;
    int fs  = input_font_scale();
    int ts  = fs + 1;
    int top_h = px_dp(TOP_BAR_DP);
    int bh2 = top_h;
    int bw2 = font_string_width("SELECT", fs) + px_dp(16);
    int pw  = px_dp(CONF_POPUP_W_DP);
    int min_pw  = font_string_width("Save state before exit?", ts) + px_dp(24);
    int min_pw2 = bw2 * 2 + px_dp(8) + px_dp(24);
    if (pw < min_pw)  pw = min_pw;
    if (pw < min_pw2) pw = min_pw2;
    if (pw > lw * 9 / 10) pw = lw * 9 / 10;
    int title_h = 8 * ts + px_dp(16);
    int ph  = title_h + bh2 + px_dp(16);
    int px  = (lw - pw) / 2;
    int py  = (lh - ph) / 2;
    int btn_gap = px_dp(8);
    int by  = py + ph - bh2 - px_dp(8);

    /* YES button */
    int yes_x = px + (pw - bw2 * 2 - btn_gap) / 2;
    if (x >= yes_x && x <= yes_x + bw2 && y >= by && y <= by + bh2) {
        g_confirm_result  = 1;
        g_confirm_visible = 0;
        return;
    }
    /* NO button */
    int no_x = yes_x + bw2 + btn_gap;
    if (x >= no_x && x <= no_x + bw2 && y >= by && y <= by + bh2) {
        g_confirm_result  = 0;
        g_confirm_visible = 0;
        return;
    }
}

void input_show_notification(const char *msg)
{
    if (!msg) return;
    strncpy(g_notify_msg, msg, sizeof(g_notify_msg) - 1);
    g_notify_msg[sizeof(g_notify_msg) - 1] = '\0';
    g_notify_until = SDL_GetTicks() + NOTIFY_DURATION_MS;
}

void input_show_zoom_label(const char *msg)
{
    if (!msg) return;
    strncpy(g_zoom_label_msg, msg, sizeof(g_zoom_label_msg) - 1);
    g_zoom_label_msg[sizeof(g_zoom_label_msg) - 1] = '\0';
    g_zoom_label_until = SDL_GetTicks() + NOTIFY_DURATION_MS;
}

void input_tick(void)
{
    Uint32 now = SDL_GetTicks();
    if (g_notify_until > 0 && now >= g_notify_until) {
        g_notify_until  = 0;
        g_notify_msg[0] = '\0';
    }
    if (g_zoom_label_until > 0 && now >= g_zoom_label_until) {
        g_zoom_label_until  = 0;
        g_zoom_label_msg[0] = '\0';
    }
}

void input_set_save_exists(int exists) { g_save_exists = exists; }

/* ---- Settings accessors ---- */

int  input_get_autosave(void)        { return g_autosave; }
void input_set_autosave(int v)       { g_autosave = v ? 1 : 0; }
int  input_get_autosave_ask(void)    { return g_autosave_ask; }
void input_set_autosave_ask(int v)   { g_autosave_ask = v ? 1 : 0; }
int  input_get_control_dim(void)     { return g_control_dim; }
void input_set_control_dim(int v)    { g_control_dim = (v < 0 ? 0 : v > 3 ? 3 : v); }
int  input_get_btn_size(void)        { return g_btn_size; }
void input_set_btn_size(int v)       { g_btn_size  = (v < 0 ? 0 : v > 2 ? 2 : v); }
int  input_get_dpad_size(void)       { return g_dpad_size; }
void input_set_dpad_size(int v)      { g_dpad_size = (v < 0 ? 0 : v > 2 ? 2 : v); }

/* ---- Options popup open (called by main loop when OPTIONS tapped) ---- */
void input_open_options_popup(void)
{
    g_options_popup_visible = 1;
    g_options_just_opened   = 1;
    g_opt_autosave_warn     = 0;
    g_opt_scroll_px         = 0;
    g_opt_touch_start_y     = 0;
    g_opt_last_touch_y      = 0;
    g_opt_did_scroll        = 0;
}

/* ---- JNI entry points: called from Java when Android intercepts gamepad events ----
 *
 * Android intercepts gamepad button events (B → KEYCODE_BACK, etc.) before SDL's
 * game-controller subsystem sees them.  EMU7800Activity.dispatchKeyEvent /
 * dispatchGenericMotionEvent capture those events and call these functions directly,
 * bypassing the SDL event queue entirely.
 *
 * Button indices match SDL_GameControllerButton:
 *   A=0  B=1  X=2  Y=3  BACK=4  START=6  LSTICK=7  RSTICK=8
 *   LSHOULDER=9  RSHOULDER=10  DPAD_UP=11  DPAD_DOWN=12  DPAD_LEFT=13  DPAD_RIGHT=14
 *
 * Axis indices (custom — not SDL enums):
 *   LEFT_X=0  LEFT_Y=1  HAT_X=6  HAT_Y=7
 */
#include <jni.h>

JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeControllerButton(
    JNIEnv *env, jclass cls, jint button, jboolean pressed)
{
    int p = pressed ? 1 : 0;
    switch ((int)button) {
    case 11: set_dpad(DIR_UP,    p); break;  /* DPAD_UP    */
    case 12: set_dpad(DIR_DOWN,  p); break;  /* DPAD_DOWN  */
    case 13: set_dpad(DIR_LEFT,  p); break;  /* DPAD_LEFT  */
    case 14: set_dpad(DIR_RIGHT, p); break;  /* DPAD_RIGHT */
    case  1:                                 /* B           */
    case  3: machine_set_trigger(0,  p); break; /* Y        */
    case  0:                                 /* A           */
    case  2: machine_set_trigger2(0, p); break; /* X        */
    case  6: if (p) g_pause_flag = 1;   break;  /* START   */
    case  4: if (p) g_back_flag  = 1;   break;  /* BACK/View */
    case  9: if (p) g_save_flag  = 1;   break;  /* LSHOULDER */
    case 10: if (p) g_load_flag  = 1;   break;  /* RSHOULDER */
    default: break;
    }
}

JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeControllerAxis(
    JNIEnv *env, jclass cls, jint axis, jfloat value)
{
    const float DEAD = 0.25f;
    switch ((int)axis) {
    case 0:  /* LEFT_X → D-pad left/right */
        set_dpad(DIR_LEFT,  value < -DEAD);
        set_dpad(DIR_RIGHT, value >  DEAD);
        break;
    case 1:  /* LEFT_Y → D-pad up/down */
        set_dpad(DIR_UP,   value < -DEAD);
        set_dpad(DIR_DOWN, value >  DEAD);
        break;
    case 4:  /* TRIGGER_L → Select */
        machine_set_switch(1, value > 0.5f);
        break;
    case 5:  /* TRIGGER_R → Reset */
        machine_set_switch(0, value > 0.5f);
        break;
    case 6:  /* HAT_X → D-pad left/right (discrete: -1, 0, 1) */
        set_dpad(DIR_LEFT,  value < -0.5f);
        set_dpad(DIR_RIGHT, value >  0.5f);
        break;
    case 7:  /* HAT_Y → D-pad up/down (discrete: -1, 0, 1) */
        set_dpad(DIR_UP,   value < -0.5f);
        set_dpad(DIR_DOWN, value >  0.5f);
        break;
    default: break;
    }
}

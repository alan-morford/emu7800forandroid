/*
 * main.c
 *
 * EMU7800 Android/SDL2 Entry Point
 *
 * Threading model:
 *   Main thread  : SDL event loop + rendering (filepicker or emulator)
 *   Emulator thread: machine_run_frame() + audio_update() at 59.94 FPS
 *   g_frame_ready: volatile flag, lock-free handoff between threads
 *
 * Lifecycle:
 *   1. SDL_main() starts, enters FILEPICKER state
 *   2. User browses and taps a ROM in the filepicker
 *   3. app_load_rom() starts the emulator thread
 *   4. BACK button → return_to_filepicker() stops emu, returns to FILEPICKER
 *
 * Copyright (c) 2024 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <SDL.h>
#include "machine.h"
#include "savestate.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "font.h"
#include "filepicker.h"
#include "zip_load.h"
#include "updater.h"

extern void log_msg(const char *msg);

/* App states */
#define APP_STATE_FILEPICKER 0
#define APP_STATE_EMULATOR   1

/* Current ROM path (used for savestate_save/load) */
static char g_current_rom_path[512] = "";

/* Shared state — volatile for cross-thread visibility */
static volatile int g_running             = 0;
static volatile int g_emulator_running    = 0;
static volatile int g_emulator_paused     = 0;
static          int g_paused_by_options   = 0;  /* auto-unpause when OPTIONS popup closes */
static volatile int g_app_state          = APP_STATE_FILEPICKER;
static volatile int g_pending_mtype      = -1;

volatile int g_frame_ready = 0;

static pthread_t g_emu_thread;
static int       g_emu_thread_created = 0;

/* Monotonic clock helper */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Forward declarations */
static void return_to_filepicker(void);
static void launch_selected_rom(void);

/*
 * Emulator thread — runs CPU + TIA/Maria + audio at 59.94 FPS.
 */
static void *emulator_thread_func(void *arg)
{
    const uint64_t FRAME_US = 16683ULL;
    uint64_t next = get_time_us() + FRAME_US;
    (void)arg;

    while (g_running && g_emulator_running) {
        if (g_emulator_paused) {
            usleep(50000);
            next = get_time_us() + FRAME_US;
            continue;
        }

        uint64_t now = get_time_us();
        if (now < next) {
            uint64_t wait = next - now;
            if (wait > 1000) usleep((unsigned int)(wait - 500));
            while (get_time_us() < next) { /* spin */ }
        }

        now = get_time_us();
        if (now > next + FRAME_US)
            next = now + FRAME_US;
        else
            next += FRAME_US;

        if (machine_is_loaded()) {
            machine_run_frame();
            audio_update();
            __sync_synchronize();
            g_frame_ready = 1;
        }
    }

    return NULL;
}

/*
 * Stop the emulator thread and return to the filepicker.
 */
static void return_to_filepicker(void)
{
    g_emulator_running  = 0;
    g_paused_by_options = 0;
    if (g_emu_thread_created) {
        pthread_join(g_emu_thread, NULL);
        g_emu_thread_created = 0;
    }

    audio_pause(1);

    machine_shutdown();
    machine_init();

    input_init();
    filepicker_rescan();

    g_app_state  = APP_STATE_FILEPICKER;
    g_frame_ready = 0;

    log_msg("return_to_filepicker: done");
}

/*
 * Launch the ROM selected by the filepicker.
 */
static void launch_selected_rom(void)
{
    const char *path = filepicker_get_selected_path();
    int mtype        = filepicker_get_selected_type();
    int load_save    = filepicker_should_load_save();

    if (!path || path[0] == '\0') {
        log_msg("launch_selected_rom: no path");
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "launch_selected_rom: %s type=%d save=%d",
             path, mtype, load_save);
    log_msg(msg);

    /* Stop any running emulator */
    if (g_emu_thread_created) {
        g_emulator_running = 0;
        pthread_join(g_emu_thread, NULL);
        g_emu_thread_created = 0;
    }

    machine_shutdown();
    machine_init();

    /* ZIP path: extract ROM data from the archive, then load from memory. */
    int rc;
    size_t plen = strlen(path);
    if (plen >= 4 && strcasecmp(path + plen - 4, ".zip") == 0) {
        unsigned char *rom_data = NULL;
        unsigned long  rom_size = 0;
        int actual_mtype        = mtype;
        if (zip_extract_rom(path, &rom_data, &rom_size, &actual_mtype) != 0) {
            log_msg("launch_selected_rom: zip_extract_rom failed");
            filepicker_show_notfound();
            return;
        }
        rc = machine_load_rom_data(rom_data, (long)rom_size, actual_mtype);
        free(rom_data);
        if (rc == 0) {
            snprintf(msg, sizeof(msg),
                "===== ROM LOADED (ZIP): %s (%s) =====",
                path, actual_mtype == MACHINE_2600 ? "2600" : "7800");
            log_msg(msg);
        }
        mtype = actual_mtype;
    } else {
        rc = machine_load_rom(path, mtype);
    }

    if (rc != 0) {
        log_msg("launch_selected_rom: machine_load_rom failed");
        filepicker_show_notfound();
        return;
    }

    /* Store current ROM path for savestate_save/load */
    strncpy(g_current_rom_path, path, sizeof(g_current_rom_path) - 1);
    g_current_rom_path[sizeof(g_current_rom_path) - 1] = '\0';

    /* Update last ROM in filepicker persistence */
    filepicker_set_last_rom(path, mtype);

    g_pending_mtype = mtype;

    audio_pause(1);   /* ensure paused while flushing stale samples */
    audio_flush();

    input_init();
    input_set_save_exists(savestate_exists(g_current_rom_path));

    if (load_save) {
        savestate_load(g_current_rom_path);
    }

    g_frame_ready      = 0;
    g_emulator_paused  = 0;
    g_emulator_running = 1;
    g_app_state        = APP_STATE_EMULATOR;

    if (pthread_create(&g_emu_thread, NULL, emulator_thread_func, NULL) == 0) {
        g_emu_thread_created = 1;
        log_msg("launch_selected_rom: emulator thread started");
        /* Wait for the first frame before unpausing audio so the ring buffer
         * has valid samples — prevents the startup pop/click. */
        {
            uint64_t deadline = get_time_us() + 200000ULL;
            while (!g_frame_ready && get_time_us() < deadline)
                usleep(2000);
        }
        audio_pause(0);
    } else {
        log_msg("launch_selected_rom: pthread_create failed");
        g_emulator_running = 0;
        g_app_state = APP_STATE_FILEPICKER;
    }
}

/*
 * Handle BACK button logic in emulator state.
 */
static void handle_emu_back(void)
{
    if (input_any_popup_visible()) {
        input_close_popup();
        return;
    }

    int autosave = input_get_autosave();
    int ask      = input_get_autosave_ask();

    if (autosave && ask) {
        /* Pause emulator while the save-before-exit popup is visible */
        if (!g_emulator_paused) {
            g_paused_by_options = 1;
            g_emulator_paused   = 1;
            audio_pause(1);
        }
        input_show_confirm();
    } else {
        if (autosave) {
            savestate_save(g_current_rom_path);
            input_show_notification("SAVED");
        }
        return_to_filepicker();
    }
}

/*
 * SDL_main — called by SDL2's Android JNI shim.
 */
int SDL_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    log_msg("EMU7800 starting");

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
             SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);

    SDL_Window *window = SDL_CreateWindow(
        "EMU7800",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        log_msg("SDL_CreateWindow failed");
        SDL_Quit();
        return 1;
    }

    machine_init();

    if (video_init(window) != 0) {
        log_msg("video_init failed");
        machine_shutdown();
        SDL_Quit();
        return 1;
    }

    font_init(video_get_renderer());

    if (audio_init() != 0) {
        log_msg("audio_init failed — continuing without audio");
    }

    input_init();

    /* filepicker_set_data_dir() was called earlier by jni_bridge.c via
     * nativeSetDataDir(). filepicker_init() loads persistence from that dir. */
    filepicker_init();

    updater_check_start();

    g_running   = 1;
    g_app_state = APP_STATE_FILEPICKER;

    log_msg("Entering main loop");

    SDL_Event e;
    while (g_running) {

        /* Advance notification timer */
        input_tick();

        /* Process SDL events */
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                g_running = 0;
                break;

            case SDL_APP_WILLENTERBACKGROUND:
                g_emulator_paused = 1;
                audio_pause(1);
                break;

            case SDL_APP_DIDENTERFOREGROUND:
                g_emulator_paused = 0;
                if (g_app_state == APP_STATE_EMULATOR)
                    audio_pause(0);
                break;

            /* Android hardware back key */
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_AC_BACK) {
                    if (g_app_state == APP_STATE_FILEPICKER) {
                        filepicker_back();
                    } else {
                        handle_emu_back();
                    }
                } else {
                    if (g_app_state == APP_STATE_EMULATOR)
                        input_handle_event(&e);
                }
                break;

            case SDL_KEYUP:
                if (g_app_state == APP_STATE_EMULATOR)
                    input_handle_event(&e);
                break;

            case SDL_FINGERDOWN: {
                if (g_app_state == APP_STATE_FILEPICKER) {
                    int sw, sh;
                    SDL_GL_GetDrawableSize(window, &sw, &sh);
                    if (sw <= 0 || sh <= 0) SDL_GetWindowSize(window, &sw, &sh);
                    int px = (int)(e.tfinger.x * sw);
                    int py = (int)(e.tfinger.y * sh);
                    filepicker_touch_down(px, py);
                } else {
                    input_handle_event(&e);
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                if (g_app_state != APP_STATE_FILEPICKER)
                    input_handle_event(&e);
                break;
            }

            case SDL_FINGERMOTION: {
                if (g_app_state == APP_STATE_FILEPICKER) {
                    int sw, sh;
                    SDL_GL_GetDrawableSize(window, &sw, &sh);
                    if (sw <= 0 || sh <= 0) SDL_GetWindowSize(window, &sw, &sh);
                    int px = (int)(e.tfinger.x * sw);
                    int py = (int)(e.tfinger.y * sh);
                    filepicker_touch_move(px, py);
                } else {
                    input_handle_event(&e);
                }
                break;
            }

            case SDL_MOUSEMOTION: {
                if (g_app_state != APP_STATE_FILEPICKER)
                    input_handle_event(&e);
                break;
            }

            case SDL_FINGERUP: {
                if (g_app_state == APP_STATE_FILEPICKER) {
                    int sw, sh;
                    SDL_GL_GetDrawableSize(window, &sw, &sh);
                    if (sw <= 0 || sh <= 0) SDL_GetWindowSize(window, &sw, &sh);
                    int px = (int)(e.tfinger.x * sw);
                    int py = (int)(e.tfinger.y * sh);
                    if (filepicker_touch_up(px, py)) {
                        launch_selected_rom();
                    }
                } else {
                    input_handle_event(&e);
                }
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (g_app_state != APP_STATE_FILEPICKER)
                    input_handle_event(&e);
                break;
            }

            default:
                if (g_app_state == APP_STATE_EMULATOR)
                    input_handle_event(&e);
                break;
            }
        }

        /* Handle confirm popup result (autosave + ask before exit) */
        if (g_app_state == APP_STATE_EMULATOR && input_confirm_visible() == 0) {
            int cr = input_confirm_result();
            if (cr == 1) {
                /* YES — save and return */
                savestate_save(g_current_rom_path);
                return_to_filepicker();
            } else if (cr == 0) {
                /* NO — just return */
                return_to_filepicker();
            }
            /* If cr == -1, pending — handled when confirm popup opens */
        }

        /* Render */
        if (g_app_state == APP_STATE_EMULATOR) {
            /* Apply pending machine type change on main thread */
            if (g_pending_mtype >= 0) {
                video_set_machine_type(g_pending_mtype);
                g_pending_mtype = -1;
            }

            /* Handle one-shot button presses from input overlay */
            if (input_pause_pressed()) {
                g_emulator_paused   = !g_emulator_paused;
                g_paused_by_options = 0;
                if (g_emulator_paused) audio_pause(1);
                else                   audio_pause(0);
            }

            if (input_save_pressed()) {
                if (savestate_save(g_current_rom_path) == 0)
                    input_show_notification("SAVED");
                else
                    input_show_notification("SAVE FAILED");
                input_set_save_exists(1);
            }

            if (input_load_pressed()) {
                if (savestate_load(g_current_rom_path) == 0)
                    input_show_notification("LOADED");
                else
                    input_show_notification("NO SAVE FOUND");
            }

            if (input_zoom_pressed()) {
                video_cycle_zoom();
                input_show_zoom_label(video_get_zoom_label());
            }

            if (input_options_pressed()) {
                if (!g_emulator_paused) {
                    g_paused_by_options = 1;
                    g_emulator_paused   = 1;
                    audio_pause(1);
                }
                input_open_options_popup();
            }

            /* Auto-unpause when all popups are gone (options, btmap, confirm) */
            if (!input_any_popup_visible() && g_paused_by_options) {
                g_paused_by_options = 0;
                g_emulator_paused   = 0;
                audio_pause(0);
            }

            if (input_back_pressed()) {
                handle_emu_back();
            }

            if (g_frame_ready) {
                __sync_synchronize();
                g_frame_ready = 0;
                video_render();
            } else if (input_any_popup_visible()) {
                video_render();
                usleep(16000);
            } else {
                usleep(500);
            }

        } else {
            /* FILEPICKER state */
            /* g_back_flag may be set by the controller View button OR by the
             * hardware back key when Android routes it through the gamepad
             * device (controller connected).  Handle it the same way as the
             * SDL SDLK_AC_BACK key event. */
            if (input_back_pressed()) {
                filepicker_back();
            }
            SDL_Renderer *r = video_get_renderer();
            if (r) {
                filepicker_draw(r);
                SDL_RenderPresent(r);
            }
            usleep(16000);
        }
    }

    log_msg("Main loop exited — shutting down");

    g_emulator_running = 0;
    if (g_emu_thread_created) {
        pthread_join(g_emu_thread, NULL);
        g_emu_thread_created = 0;
    }

    filepicker_shutdown();
    audio_shutdown();
    font_shutdown();
    video_shutdown();
    machine_shutdown();
    SDL_Quit();

    return 0;
}

/*
 * Called from jni_bridge.c when the SAF picker delivers a ROM file.
 * Kept for compatibility — with the new filepicker this is not the primary path,
 * but SAF fallback can still use it.
 */
void app_load_rom(const char *path, int machine_type)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "app_load_rom: %s type=%d", path, machine_type);
    log_msg(msg);

    if (g_emu_thread_created) {
        g_emulator_running = 0;
        pthread_join(g_emu_thread, NULL);
        g_emu_thread_created = 0;
    }

    machine_shutdown();
    machine_init();

    int rc = machine_load_rom(path, machine_type);
    if (rc != 0) {
        log_msg("app_load_rom: machine_load_rom failed");
        return;
    }

    strncpy(g_current_rom_path, path, sizeof(g_current_rom_path) - 1);
    g_current_rom_path[sizeof(g_current_rom_path) - 1] = '\0';

    filepicker_set_last_rom(path, machine_type);
    g_pending_mtype = machine_type;

    audio_pause(1);
    audio_flush();

    input_init();

    g_frame_ready      = 0;
    g_emulator_paused  = 0;
    g_emulator_running = 1;
    g_app_state        = APP_STATE_EMULATOR;

    if (pthread_create(&g_emu_thread, NULL, emulator_thread_func, NULL) == 0) {
        g_emu_thread_created = 1;
        log_msg("app_load_rom: emulator thread started");
        {
            uint64_t deadline = get_time_us() + 200000ULL;
            while (!g_frame_ready && get_time_us() < deadline)
                usleep(2000);
        }
        audio_pause(0);
    } else {
        log_msg("app_load_rom: pthread_create failed");
        g_emulator_running = 0;
        g_app_state = APP_STATE_FILEPICKER;
    }
}

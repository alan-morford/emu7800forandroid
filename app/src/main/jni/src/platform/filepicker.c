/*
 * filepicker.c
 *
 * File Picker UI — Android SDL2 port
 *
 * Scans a directory and displays a scrollable list of all files and
 * subdirectories. Only recognized ROM extensions (.a26, .a78, .bin)
 * launch the emulator; other files are shown dimmed and are not tappable.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE

#ifndef EMU7800_VERSION
#define EMU7800_VERSION "?"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>    /* strcasecmp */
#include <unistd.h>
#include <SDL.h>
#include "filepicker.h"
#include "font.h"
#include "machine.h"
#include "video.h"
#include "input.h"
#include "asteroids_img.h"
#include "logo_img.h"
#include "gear_img.h"
#include "up_img.h"

extern void log_msg(const char *msg);
extern void jni_send_bug_report_email(void);
extern void jni_filepicker_at_root(void);
extern void jni_set_cutout_mode(int enabled);

/* ---- Constants ---- */

#define MAX_FILES    512
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
#define RECENT_MAX    32
#define DIRPICKER_MAX 128

/* Entry type constants.
 * Note: MACHINE_2600=0, MACHINE_7800=1 (defined in machine.h) */
#define ENTRY_DIR    -2   /* directory */
#define ENTRY_FILE   -1   /* unrecognized file — shown dimmed, not launchable */
#define ENTRY_SAV    -3   /* .sav save-state — shown in purple, not launchable */

/* ---- File entry ---- */
typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int  type;    /* ENTRY_DIR / ENTRY_FILE / MACHINE_2600 / MACHINE_7800 */
    int  is_dir;
} FileEntry;

typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int  is_dir;
} DirEntry;

/* ---- State ---- */

static FileEntry g_files[MAX_FILES];
static int       g_file_count    = 0;
static float     g_scroll_offset = 0.0f;
static char      g_current_dir[MAX_PATH_LEN] = "/storage/emulated/0/";

/* Selected ROM */
static char g_selected_path[MAX_PATH_LEN];
static int  g_selected_type    = 0;
static int  g_should_load_save = 0;

/* Last played ROM */
static char g_last_rom_path[MAX_PATH_LEN];
static int  g_last_rom_type  = 0;
static int  g_has_last_rom   = 0;

/* Data directory (persistence files).
 * Starts empty; set by filepicker_set_data_dir() (called from nativeSetDataDir
 * in jni_bridge.c) before filepicker_init() reads settings. */
static char g_data_dir[MAX_PATH_LEN] = "";

/* Path to extracted bundled Asteroids ROM */
static char g_asteroids_path[MAX_PATH_LEN] = "";

/* Recently played list */
static char g_recent_paths[RECENT_MAX][MAX_PATH_LEN];
static int  g_recent_types[RECENT_MAX];
static int  g_recent_count = 0;

/* SDL textures for images */
static SDL_Texture *g_ast_tex  = NULL;
static SDL_Texture *g_gear_tex = NULL;
static SDL_Texture *g_logo_tex = NULL;
static SDL_Texture *g_up_tex   = NULL;

/* Logo animation */
static int    g_logo_frame     = 0;
static Uint32 g_logo_last_tick = 0;

/* Settings */
static char  g_default_romdir[MAX_PATH_LEN] = "";
static int   g_use_cutout  = 0;
static int   g_has_cutout  = 0;  /* set by Java; 0 = no notch, row greyed out */

/* Display density (Android DisplayMetrics.density).
 * Used to size the bitmap font to match the system's 16sp body text size.
 * 2.0 is a safe default (xhdpi, 320dpi). */
static float g_display_density = 2.0f;

/* Popup visibility */
static int  g_first_launch_popup      = 0;   /* shown once until settings file exists */
static int  g_recent_popup_visible    = 0;
static int  g_settings_popup_visible  = 0;
static int  g_save_popup_visible      = 0;
static int  g_delete_confirm_visible  = 0;
static char g_save_popup_path[MAX_PATH_LEN];
static int  g_save_popup_type         = 0;
static int  g_notfound_visible        = 0;
static int  g_notfound_recent_idx     = -1;
static int  g_dirpicker_popup_visible   = 0;
static int  g_about_popup_visible       = 0;
static int  g_autosave_warn_visible     = 0;

/* Directory picker */
static DirEntry g_dirpicker_dirs[DIRPICKER_MAX];
static int      g_dirpicker_count   = 0;
static char     g_dirpicker_current[MAX_PATH_LEN];
static float    g_dirpicker_scroll  = 0.0f;

/* Recent popup scroll */
static float g_recent_scroll = 0.0f;

/* Touch tracking */
static int   g_touch_active    = 0;
static int   g_touch_start_x  = 0;
static int   g_touch_start_y  = 0;
static int   g_touch_last_y   = 0;
static int   g_touch_moved    = 0;
static float g_scroll_at_touch = 0.0f;
/* 0=main list, 1=dirpicker popup, 2=recent popup */
static int   g_scroll_mode    = 0;
#define TAP_THRESHOLD 12

/* ---- Persistence paths ---- */

static void get_settings_path(char *buf, int bufsz)
{ snprintf(buf, bufsz, "%s/.emu7800_settings", g_data_dir); }

static void get_lastrom_path(char *buf, int bufsz)
{ snprintf(buf, bufsz, "%s/.emu7800_lastrom", g_data_dir); }

static void get_recent_path(char *buf, int bufsz)
{ snprintf(buf, bufsz, "%s/.emu7800_recent", g_data_dir); }

/* ---- ROM type detection ---- */

static int get_rom_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return ENTRY_FILE;
    if (strcasecmp(dot, ".a26") == 0) return MACHINE_2600;
    if (strcasecmp(dot, ".a78") == 0) return MACHINE_7800;
    if (strcasecmp(dot, ".bin") == 0) return MACHINE_2600;
    if (strcasecmp(dot, ".rom") == 0) return MACHINE_2600;
    if (strcasecmp(dot, ".sav") == 0) return ENTRY_SAV;
    return ENTRY_FILE;
}

static int is_rom_type(int type)
{
    return type == MACHINE_2600 || type == MACHINE_7800;
}

/* ---- Sort: directories first, then alpha ---- */

static int compare_files(const void *a, const void *b)
{
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    /* Directories before files */
    if (fa->is_dir != fb->is_dir)
        return fb->is_dir - fa->is_dir;
    /* ROM files before non-ROM files */
    int a_rom = is_rom_type(fa->type);
    int b_rom = is_rom_type(fb->type);
    if (a_rom != b_rom) return b_rom - a_rom;
    return strcasecmp(fa->name, fb->name);
}

static int compare_dirs(const void *a, const void *b)
{
    const DirEntry *da = (const DirEntry *)a;
    const DirEntry *db = (const DirEntry *)b;
    if (strcmp(da->name, "..") == 0) return -1;
    if (strcmp(db->name, "..") == 0) return 1;
    return strcasecmp(da->name, db->name);
}

/* ---- Persistence: last ROM ---- */

static void load_last_rom(void)
{
    char path[MAX_PATH_LEN + 32];
    get_lastrom_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    char  line[MAX_PATH_LEN];
    if (!f) return;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    {
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    }
    strncpy(g_last_rom_path, line, MAX_PATH_LEN - 1);
    g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    g_last_rom_type = atoi(line);
    g_has_last_rom  = 1;
    fclose(f);
}

static void save_last_rom(void)
{
    char path[MAX_PATH_LEN + 32];
    get_lastrom_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n", g_last_rom_path, g_last_rom_type);
    fclose(f);
}

/* ---- Persistence: recent list ---- */

static void save_recent_list(void)
{
    char path[MAX_PATH_LEN + 32];
    get_recent_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_recent_count; i++)
        fprintf(f, "%s\n%d\n", g_recent_paths[i], g_recent_types[i]);
    fclose(f);
}

static void load_recent_list(void)
{
    char path[MAX_PATH_LEN + 32];
    get_recent_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    char  line[MAX_PATH_LEN];
    if (!f) return;
    g_recent_count = 0;
    while (g_recent_count < RECENT_MAX && fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;
        strncpy(g_recent_paths[g_recent_count], line, MAX_PATH_LEN - 1);
        g_recent_paths[g_recent_count][MAX_PATH_LEN - 1] = '\0';
        if (!fgets(line, sizeof(line), f)) break;
        g_recent_types[g_recent_count] = atoi(line);
        g_recent_count++;
    }
    fclose(f);
}

static void add_to_recent(const char *path, int type)
{
    int found = -1;
    for (int i = 0; i < g_recent_count; i++) {
        if (strcmp(g_recent_paths[i], path) == 0) { found = i; break; }
    }
    if (found > 0) {
        char tmp[MAX_PATH_LEN];
        int  tmp_type = g_recent_types[found];
        strncpy(tmp, g_recent_paths[found], MAX_PATH_LEN);
        for (int i = found; i > 0; i--) {
            strncpy(g_recent_paths[i], g_recent_paths[i - 1], MAX_PATH_LEN);
            g_recent_types[i] = g_recent_types[i - 1];
        }
        strncpy(g_recent_paths[0], tmp, MAX_PATH_LEN);
        g_recent_types[0] = tmp_type;
    } else if (found != 0) {
        int cap = g_recent_count < RECENT_MAX ? g_recent_count : RECENT_MAX - 1;
        for (int i = cap; i > 0; i--) {
            strncpy(g_recent_paths[i], g_recent_paths[i - 1], MAX_PATH_LEN);
            g_recent_types[i] = g_recent_types[i - 1];
        }
        strncpy(g_recent_paths[0], path, MAX_PATH_LEN - 1);
        g_recent_paths[0][MAX_PATH_LEN - 1] = '\0';
        g_recent_types[0] = type;
        if (g_recent_count < RECENT_MAX) g_recent_count++;
    }
    g_recent_types[0] = type;
    save_recent_list();
}

static void remove_recent_entry(int idx)
{
    if (idx < 0 || idx >= g_recent_count) return;
    for (int i = idx; i < g_recent_count - 1; i++) {
        strncpy(g_recent_paths[i], g_recent_paths[i + 1], MAX_PATH_LEN);
        g_recent_types[i] = g_recent_types[i + 1];
    }
    g_recent_count--;
    save_recent_list();
}

/* ---- Persistence: settings ---- */

static void load_settings(void)
{
    char path[MAX_PATH_LEN + 32];
    get_settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    char  line[MAX_PATH_LEN];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if      (strncmp(line, "romdir=",       7)  == 0) { strncpy(g_default_romdir, line + 7,  MAX_PATH_LEN - 1); }
        else if (strncmp(line, "scanlines=",   10)  == 0) { video_set_scanlines(atoi(line + 10)); }
        else if (strncmp(line, "brightness=",  11)  == 0) { video_set_scanline_brightness(atoi(line + 11)); }
        else if (strncmp(line, "palette=",      8)  == 0) { video_set_maria_palette(atoi(line + 8)); }
        else if (strncmp(line, "autosave=",     9)  == 0) { input_set_autosave(atoi(line + 9)); }
        else if (strncmp(line, "autosave_ask=",13)  == 0) { input_set_autosave_ask(atoi(line + 13)); }
        else if (strncmp(line, "control_dim=", 12)  == 0) { input_set_control_dim(atoi(line + 12)); }
        else if (strncmp(line, "btn_size=",     9)  == 0) { input_set_btn_size(atoi(line + 9)); }
        else if (strncmp(line, "dpad_size=",   10)  == 0) { input_set_dpad_size(atoi(line + 10)); }
        else if (strncmp(line, "use_cutout=",  11)  == 0) { g_use_cutout = atoi(line + 11) ? 1 : 0; }
    }
    fclose(f);
    if (g_use_cutout) jni_set_cutout_mode(1);
}

void filepicker_save_settings(void)
{
    char path[MAX_PATH_LEN + 32];
    get_settings_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (g_default_romdir[0] != '\0') fprintf(f, "romdir=%s\n",       g_default_romdir);
    fprintf(f, "scanlines=%d\n",    video_get_scanlines());
    fprintf(f, "brightness=%d\n",   video_get_scanline_brightness());
    fprintf(f, "palette=%d\n",      video_get_maria_palette());
    fprintf(f, "autosave=%d\n",     input_get_autosave());
    fprintf(f, "autosave_ask=%d\n", input_get_autosave_ask());
    fprintf(f, "control_dim=%d\n",  input_get_control_dim());
    fprintf(f, "btn_size=%d\n",     input_get_btn_size());
    fprintf(f, "dpad_size=%d\n",    input_get_dpad_size());
    fprintf(f, "use_cutout=%d\n",   g_use_cutout);
    fclose(f);
}

/* ---- Logo frame pointer lookup ---- */

static const unsigned short *logo_frame_ptr(int frame)
{
    static const unsigned short *frames[LOGO_FRAME_COUNT] = {NULL};
    static int built = 0;
    if (!built) {
        frames[0] = logo_frame_0; frames[1] = logo_frame_1;
        frames[2] = logo_frame_2; frames[3] = logo_frame_3;
        frames[4] = logo_frame_4; frames[5] = logo_frame_5;
        frames[6] = logo_frame_6; frames[7] = logo_frame_7;
        built = 1;
    }
    if (frame < 0 || frame >= LOGO_FRAME_COUNT) frame = 0;
    return frames[frame];
}

/* ---- Texture creation ---- */

static SDL_Texture *create_tex_rgb565(SDL_Renderer *r, const unsigned short *data,
                                      int tex_w, int tex_h)
{
    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (!tex) return NULL;
    SDL_UpdateTexture(tex, NULL, data, tex_w * 2);
    return tex;
}

static SDL_Texture *create_tex_rgba8888(SDL_Renderer *r, const unsigned char *data,
                                         int tex_w, int tex_h)
{
    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, NULL, data, tex_w * 4);
    return tex;
}

/* ---- Asset extraction ---- */

/*
 * Extract the bundled Asteroids.a78 from the APK assets to internal storage.
 * SDL2 on Android redirects SDL_RWFromFile to look inside the APK asset bundle.
 * Returns 1 on success, 0 if already extracted or failed.
 */
static int extract_asteroids_asset(void)
{
    snprintf(g_asteroids_path, MAX_PATH_LEN, "%s/Asteroids.a78", g_data_dir);

    /* If already extracted, nothing to do */
    if (access(g_asteroids_path, F_OK) == 0) {
        log_msg("filepicker: Asteroids.a78 already extracted");
        return 1;
    }

    /* Open from APK asset bundle via SDL2 */
    SDL_RWops *src = SDL_RWFromFile("Asteroids.a78", "rb");
    if (!src) {
        char msg[256];
        snprintf(msg, sizeof(msg), "filepicker: SDL_RWFromFile Asteroids.a78 failed: %s",
                 SDL_GetError());
        log_msg(msg);
        g_asteroids_path[0] = '\0';
        return 0;
    }

    FILE *dst = fopen(g_asteroids_path, "wb");
    if (!dst) {
        log_msg("filepicker: cannot write Asteroids.a78 to data dir");
        SDL_RWclose(src);
        g_asteroids_path[0] = '\0';
        return 0;
    }

    char    buf[8192];
    size_t  n;
    int     ok = 1;
    while ((n = SDL_RWread(src, buf, 1, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; }
    }
    SDL_RWclose(src);
    fclose(dst);

    if (!ok) {
        remove(g_asteroids_path);
        g_asteroids_path[0] = '\0';
        log_msg("filepicker: failed to write Asteroids.a78");
        return 0;
    }

    log_msg("filepicker: Asteroids.a78 extracted OK");
    return 1;
}

/* ---- Public API ---- */

void filepicker_set_data_dir(const char *dir)
{
    if (!dir) return;
    strncpy(g_data_dir, dir, MAX_PATH_LEN - 1);
    g_data_dir[MAX_PATH_LEN - 1] = '\0';
}

void filepicker_set_display_density(float density)
{
    if (density > 0.5f)
        g_display_density = density;
}

void filepicker_set_has_cutout(int has_cutout)
{
    g_has_cutout = has_cutout ? 1 : 0;
}

void filepicker_init(void)
{
    SDL_Renderer *r = video_get_renderer();
    if (!r) return;

    /* Check for first launch BEFORE load_settings so we can detect a missing file */
    {
        char sp[MAX_PATH_LEN + 32];
        get_settings_path(sp, sizeof(sp));
        if (access(sp, F_OK) != 0)
            g_first_launch_popup = 1;
    }

    load_settings();
    load_last_rom();
    load_recent_list();

    /* Extract bundled Asteroids ROM from APK assets */
    extract_asteroids_asset();

    /* Asteroids thumbnail (256×256 RGB565) */
    g_ast_tex = create_tex_rgb565(r, asteroids_img_data, 256, 256);

    /* Gear icon (RGBA 64×64) */
    g_gear_tex = create_tex_rgba8888(r, gear_img_data, GEAR_TEX_SIZE, GEAR_TEX_SIZE);

    /* Up arrow icon (RGBA 64×57) */
    g_up_tex = create_tex_rgba8888(r, up_img_data, UP_IMG_W, UP_IMG_H);

    /* Logo animation texture */
    g_logo_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565,
                                   SDL_TEXTUREACCESS_STATIC,
                                   LOGO_TEX_SIZE, LOGO_TEX_SIZE);
    if (g_logo_tex)
        SDL_UpdateTexture(g_logo_tex, NULL, logo_frame_ptr(0), LOGO_TEX_SIZE * 2);

    g_logo_frame     = 0;
    g_logo_last_tick = SDL_GetTicks();

    /* Choose starting directory */
    if (g_default_romdir[0] != '\0')
        filepicker_scan(g_default_romdir);
    else
        filepicker_scan("/storage/emulated/0/");

    log_msg("filepicker_init: OK");
}

void filepicker_shutdown(void)
{
    if (g_ast_tex)  { SDL_DestroyTexture(g_ast_tex);  g_ast_tex  = NULL; }
    if (g_gear_tex) { SDL_DestroyTexture(g_gear_tex); g_gear_tex = NULL; }
    if (g_logo_tex) { SDL_DestroyTexture(g_logo_tex); g_logo_tex = NULL; }
    if (g_up_tex)   { SDL_DestroyTexture(g_up_tex);   g_up_tex   = NULL; }
}

/* ---- Directory scan ---- */

void filepicker_scan(const char *directory)
{
    if (!directory) return;

    g_file_count    = 0;
    g_scroll_offset = 0.0f;

    strncpy(g_current_dir, directory, MAX_PATH_LEN - 1);
    g_current_dir[MAX_PATH_LEN - 1] = '\0';

    /* Ensure trailing slash */
    int dlen = strlen(g_current_dir);
    if (dlen > 0 && g_current_dir[dlen - 1] != '/' && dlen < MAX_PATH_LEN - 1) {
        g_current_dir[dlen]     = '/';
        g_current_dir[dlen + 1] = '\0';
    }

    /* Add ".." entry unless we're already at a root we treat as top-level */
    if (strcmp(g_current_dir, "/") != 0 &&
        strcmp(g_current_dir, "/storage/emulated/0/") != 0) {
        FileEntry *e = &g_files[g_file_count++];
        strncpy(e->name, "..", MAX_NAME_LEN - 1);
        e->name[MAX_NAME_LEN - 1] = '\0';
        /* Derive parent path */
        char parent[MAX_PATH_LEN];
        strncpy(parent, g_current_dir, MAX_PATH_LEN - 1);
        int plen = strlen(parent);
        if (plen > 1 && parent[plen - 1] == '/') parent[--plen] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash) { *(slash + 1) = '\0'; }
        strncpy(e->path, parent, MAX_PATH_LEN - 1);
        e->path[MAX_PATH_LEN - 1] = '\0';
        e->type   = ENTRY_DIR;
        e->is_dir = 1;
    }

    DIR           *dir = opendir(g_current_dir);
    struct dirent *ent;
    struct stat    st;
    char           fullpath[MAX_PATH_LEN];

    if (!dir) {
        char msg[256];
        snprintf(msg, sizeof(msg), "filepicker_scan: opendir failed: %s", g_current_dir);
        log_msg(msg);

        /* Fall back to the default root rather than showing an empty list.
         * Also discard any ".." entry that was added above for the failed path. */
        if (strcmp(g_current_dir, "/storage/emulated/0/") != 0 &&
            strcmp(g_current_dir, "/") != 0) {
            log_msg("filepicker_scan: falling back to /storage/emulated/0/");
            g_file_count = 0;
            strncpy(g_current_dir, "/storage/emulated/0/", MAX_PATH_LEN - 1);
            g_current_dir[MAX_PATH_LEN - 1] = '\0';
            dir = opendir(g_current_dir);
            if (!dir) {
                log_msg("filepicker_scan: fallback opendir also failed");
                return;
            }
            /* No ".." entry needed — we're now at the top-level root */
        } else {
            return;
        }
    }

    while ((ent = readdir(dir)) != NULL && g_file_count < MAX_FILES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_name[0] == '.') continue;  /* hidden files */

        snprintf(fullpath, MAX_PATH_LEN, "%s%s", g_current_dir, ent->d_name);
        if (stat(fullpath, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);
        int type   = is_dir ? ENTRY_DIR : get_rom_type(ent->d_name);

        /* Hide unrecognized files; show .sav save-state files in purple */
        if (type == ENTRY_FILE) continue;

        FileEntry *entry = &g_files[g_file_count++];
        strncpy(entry->path, fullpath, MAX_PATH_LEN - 1);
        entry->path[MAX_PATH_LEN - 1] = '\0';
        strncpy(entry->name, ent->d_name, MAX_NAME_LEN - 1);
        entry->name[MAX_NAME_LEN - 1] = '\0';
        entry->type   = type;
        entry->is_dir = is_dir;
    }

    closedir(dir);

    /* Sort: ".." pinned at [0], then dirs first, then ROM files, then others */
    if (g_file_count > 1) {
        int sort_start = (strcmp(g_files[0].name, "..") == 0) ? 1 : 0;
        if (g_file_count - sort_start > 1)
            qsort(g_files + sort_start, g_file_count - sort_start,
                  sizeof(FileEntry), compare_files);
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "filepicker_scan: %d entries in %s",
                 g_file_count, g_current_dir);
        log_msg(msg);
    }
}

void filepicker_rescan(void)
{
    /* Re-try asset extraction in case it failed at init because g_data_dir
     * wasn't set yet — happens when MANAGE_EXTERNAL_STORAGE is granted after
     * the first launch and SDL is already running. */
    if (g_asteroids_path[0] == '\0')
        extract_asteroids_asset();
    filepicker_scan(g_current_dir);
}

/* ---- Directory picker scan ---- */

static void dirpicker_scan(const char *directory)
{
    g_dirpicker_count  = 0;
    g_dirpicker_scroll = 0.0f;
    strncpy(g_dirpicker_current, directory, MAX_PATH_LEN - 1);
    g_dirpicker_current[MAX_PATH_LEN - 1] = '\0';

    int dlen = strlen(g_dirpicker_current);
    if (dlen > 0 && g_dirpicker_current[dlen - 1] != '/' && dlen < MAX_PATH_LEN - 1) {
        g_dirpicker_current[dlen]     = '/';
        g_dirpicker_current[dlen + 1] = '\0';
    }

    if (strcmp(g_dirpicker_current, "/storage/emulated/0/") != 0 &&
        strcmp(g_dirpicker_current, "/") != 0) {
        DirEntry *d = &g_dirpicker_dirs[g_dirpicker_count++];
        strncpy(d->name, "..", MAX_NAME_LEN - 1);
        char parent[MAX_PATH_LEN];
        strncpy(parent, g_dirpicker_current, MAX_PATH_LEN - 1);
        int plen = strlen(parent);
        if (plen > 1 && parent[plen - 1] == '/') parent[--plen] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash) { *(slash + 1) = '\0'; }
        strncpy(d->path, parent, MAX_PATH_LEN - 1);
        d->path[MAX_PATH_LEN - 1] = '\0';
        d->is_dir = 1;
    }

    DIR *dir = opendir(g_dirpicker_current);
    if (!dir) return;
    struct dirent *ent;
    struct stat    st;
    char           fullpath[MAX_PATH_LEN];

    while ((ent = readdir(dir)) != NULL && g_dirpicker_count < DIRPICKER_MAX) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_name[0] == '.') continue;
        snprintf(fullpath, MAX_PATH_LEN, "%s%s", g_dirpicker_current, ent->d_name);
        if (stat(fullpath, &st) != 0) continue;
        int entry_is_dir = S_ISDIR(st.st_mode);
        int entry_type   = entry_is_dir ? ENTRY_DIR : get_rom_type(ent->d_name);
        /* Include directories and ROM files; skip everything else */
        if (!entry_is_dir && !is_rom_type(entry_type)) continue;
        DirEntry *d = &g_dirpicker_dirs[g_dirpicker_count++];
        strncpy(d->path, fullpath, MAX_PATH_LEN - 1);
        strncpy(d->name, ent->d_name, MAX_NAME_LEN - 1);
        d->is_dir = entry_is_dir;
    }
    closedir(dir);

    if (g_dirpicker_count > 1)
        qsort(g_dirpicker_dirs, g_dirpicker_count, sizeof(DirEntry), compare_dirs);
}

/* ---- dp helpers ---- */
/*
 * All UI element SIZES are defined in dp and converted once.
 * Screen pixel dimensions (L.sw, L.sh) are only used for POSITIONING
 * (centering, anchoring to edges) — never for computing dimensions.
 * This keeps every button, bar, and popup physically constant across
 * foldable posture changes, cutout expansion, and window resizes.
 */
#define FP_ROW_DP      48    /* top bar button row height */
#define FP_POPUP_W_DP 400    /* standard popup width */
#define FP_WIDE_W_DP  440    /* wide popup (recent list, dirpicker, welcome) */

/* dp → pixels using the current display density. */
static int fp_px_dp(int dp) { return (int)(dp * g_display_density + 0.5f); }

/* Clamp a dp-derived popup width so it never bleeds off a small screen. */
static int fp_popup_w(int dp, int sw)
{
    int w = fp_px_dp(dp);
    int cap = sw * 9 / 10;
    return w < cap ? w : cap;
}

/* ---- Layout ---- */

typedef struct {
    int sw, sh;
    int top_h;
    int sep_y;
    int list_x, list_w;
    int list_top, item_h;
    int center_x, center_w;
    int ast_x, ast_y, ast_w, ast_h;
    int logo_x, logo_y, logo_sz;
    int gear_x, gear_y, gear_w;
    int resume_x, resume_y, resume_w, resume_h;
    int recent_x, recent_y, recent_w, recent_h;
    int font_scale;      /* base font scale for list and buttons */
    int title_scale;     /* font scale for popup/bar titles */
    int emu_scale;       /* font scale for the EMU7800 top-bar title */
} FPLayout;

static FPLayout compute_layout(SDL_Renderer *r)
{
    FPLayout L;
    memset(&L, 0, sizeof(L));

    SDL_Window *win = r ? SDL_RenderGetWindow(r) : NULL;
    if (win) {
        SDL_GL_GetDrawableSize(win, &L.sw, &L.sh);
        if (L.sw <= 0 || L.sh <= 0) SDL_GetWindowSize(win, &L.sw, &L.sh);
    }
    if (L.sw <= 0) { L.sw = 1024; L.sh = 600; }

    /* Font scale derived from display density to match Android's 16sp body text.
     * Android body text = 16sp = 16 * density px.  Our bitmap font is 8px/char
     * at scale 1, so font_scale = round(16 * density / 8) = round(2 * density). */
    {
        int fs = (int)(2.0f * g_display_density + 0.5f);
        if (fs < 2) fs = 2;
        if (fs > 8) fs = 8;
        L.font_scale = fs;
    }
    L.title_scale = L.font_scale + 1;

    /* Top bar: two rows of buttons — fixed dp, never from screen height */
    int row_h = fp_px_dp(FP_ROW_DP);
    L.top_h   = row_h * 2;

    L.sep_y  = L.top_h;

    /* Item height: double the padding around each text row for easier tapping */
    L.item_h = (8 * L.font_scale + 24) * 3 / 2;
    if (L.item_h < 72)  L.item_h = 72;
    if (L.item_h > 300) L.item_h = 300;

    /* Gear: fully above the separator line (bottom edge 4px above sep_y).
     * 10% larger than base size.  Use a 16px right margin so the gear and the
     * right-aligned EMU7800 title clear the rounded corners on modern phones. */
    L.gear_w = row_h * 3 / 4 * 11 / 10;
    L.gear_x = L.sw - L.gear_w - 16;
    L.gear_y = L.sep_y - L.gear_w - 4;

    /* EMU7800 title scale: as large as possible without moving the gear.
     * Vertical limit:   text runs from y=2 to gear_y with a 4px gap.
     * Horizontal limit: right-aligned to gear right edge, must not overlap buttons.
     * No arbitrary max cap — geometry is the only constraint. */
    {
        int avail_v   = L.gear_y - 6;   /* 2px top pad + 4px gap below text */
        L.emu_scale   = avail_v / 8;
        if (L.emu_scale < 2) L.emu_scale = 2;

        int btn_max_w = font_string_width("RESUME", L.font_scale) + 16;
        int rw_tmp    = font_string_width("RECENT", L.font_scale) + 16;
        if (rw_tmp > btn_max_w) btn_max_w = rw_tmp;
        int avail_h   = L.gear_x + L.gear_w - (btn_max_w + 16);
        while (L.emu_scale > 2 &&
               font_string_width("EMU7800", L.emu_scale) > avail_h) {
            L.emu_scale--;
        }
    }

    /* RESUME button — first row, sized to fit text */
    L.resume_w = font_string_width("RESUME", L.font_scale) + 16;
    L.resume_h = row_h - 4;
    L.resume_x = 2;
    L.resume_y = 2;

    /* RECENT button — second row, sized to fit text */
    L.recent_w = font_string_width("RECENT", L.font_scale) + 16;
    L.recent_h = row_h - 4;
    L.recent_x = 2;
    L.recent_y = row_h + 2;

    /* Center area for EMU7800 title: between left buttons and gear */
    {
        int btn_max_w = L.resume_w > L.recent_w ? L.resume_w : L.recent_w;
        L.center_x = btn_max_w + 16;
        L.center_w = L.gear_x - L.center_x;
        if (L.center_w < 0) L.center_w = 0;
    }

    /* Full-width file list; list_top includes one item_h of breathing room
     * above the first entry so the gap above "../" equals the gap below it. */
    L.list_x   = 0;
    L.list_w   = L.sw;
    L.list_top = L.sep_y + L.item_h;

    /* Logo: centered horizontally on the full screen width, but vertically within
     * the content area below the top bar so it is never obscured by the top bar.
     * Size is 90% of the smaller of (content height, screen width). */
    {
        int content_h = L.sh - L.sep_y;
        int max_dim   = content_h < L.sw ? content_h : L.sw;
        L.logo_sz = max_dim * 9 / 10;
        L.logo_x  = (L.sw - L.logo_sz) / 2;
        L.logo_y  = L.sep_y + (content_h - L.logo_sz) / 2;
    }

    /* Asteroids thumbnail: bottom-right corner.
     * Use a 16px margin on both edges to clear curved screen corners. */
    L.ast_w = L.sh * 3 / 10;   /* half of the previous 3/5 */
    if (L.ast_w > L.sw / 3) L.ast_w = L.sw / 3;
    if (L.ast_w < 96) L.ast_w = 96;
    L.ast_h = L.ast_w;
    L.ast_x = L.sw - L.ast_w - 24;
    L.ast_y = L.sh - L.ast_h - 24;

    return L;
}

/* ---- Draw helpers ---- */

static void fp_fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                         Uint8 rv, Uint8 gv, Uint8 bv, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, rv, gv, bv, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void fp_draw_rect(SDL_Renderer *r, int x, int y, int w, int h,
                         Uint8 rv, Uint8 gv, Uint8 bv, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, rv, gv, bv, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

static void fp_centered_text(SDL_Renderer *r, const char *text,
                             int x, int y, int w, int h, int scale,
                             Uint8 rv, Uint8 gv, Uint8 bv)
{
    int tw = font_string_width(text, scale);
    int th = 8 * scale;
    font_draw_string(r, text, x + (w - tw) / 2, y + (h - th) / 2, scale, rv, gv, bv);
}

static void fp_popup_bg(SDL_Renderer *r, int x, int y, int w, int h)
{
    fp_fill_rect(r, x, y, w, h, 20, 20, 20, 235);
    fp_draw_rect(r, x, y, w, h, 220, 140, 0, 255);
    /* Inner shadow */
    fp_draw_rect(r, x + 1, y + 1, w - 2, h - 2, 150, 80, 0, 120);
}

/* ---- Logo animation ---- */

static void update_logo_frame(void)
{
    if (!g_logo_tex) return;
    Uint32 now   = SDL_GetTicks();
    Uint32 delay = (Uint32)logo_frame_delays[g_logo_frame];
    if (now - g_logo_last_tick >= delay) {
        g_logo_frame = (g_logo_frame + 1) % LOGO_FRAME_COUNT;
        g_logo_last_tick = now;
        SDL_UpdateTexture(g_logo_tex, NULL,
                         logo_frame_ptr(g_logo_frame), LOGO_TEX_SIZE * 2);
    }
}

/* ---- Popup drawing ---- */

static void draw_recent_popup(SDL_Renderer *r, const FPLayout *L)
{
    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    int ph = fp_px_dp(400); if (ph > L->sh * 9 / 10) ph = L->sh * 9 / 10;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    int title_h = 10 * L->title_scale + 8;
    font_draw_string(r, "RECENTLY PLAYED", px + 12, py + 8, L->title_scale, 200, 100, 0);

    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    int btn_h   = L->resume_h;
    int list_h  = ph - title_h - btn_h - 20;
    int visible = list_h / L->item_h;
    int start   = (int)g_recent_scroll;
    if (start > g_recent_count - visible) start = g_recent_count - visible;
    if (start < 0) start = 0;

    if (g_recent_count == 0) {
        font_draw_string(r, "(empty)", px + 12, py + title_h + 8, L->font_scale, 160, 160, 160);
    } else {
        SDL_Rect clip = {px + 2, py + title_h + 2, pw - 4, list_h};
        SDL_RenderSetClipRect(r, &clip);
        for (int i = start; i < g_recent_count && i < start + visible; i++) {
            int iy = py + title_h + 4 + (i - start) * L->item_h;
            const char *fname = strrchr(g_recent_paths[i], '/');
            fname = fname ? fname + 1 : g_recent_paths[i];
            char fname_noext[MAX_NAME_LEN];
            strncpy(fname_noext, fname, sizeof(fname_noext) - 1);
            fname_noext[sizeof(fname_noext) - 1] = '\0';
            char *dot = strrchr(fname_noext, '.');
            if (dot) *dot = '\0';
            font_draw_string(r, fname_noext, px + 12, iy + (L->item_h - 8 * L->font_scale) / 2,
                            L->font_scale, 255, 255, 255);
        }
        SDL_RenderSetClipRect(r, NULL);
    }

    /* CLEAR LIST button */
    int bw = font_string_width("CLEAR LIST", L->font_scale) + 24;
    if (bw < pw / 3) bw = pw / 3;
    int bx = px + (pw - bw) / 2;
    int by = py + ph - btn_h - 8;
    fp_fill_rect(r, bx, by, bw, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "CLEAR LIST", bx, by, bw, btn_h, L->font_scale, 200, 100, 0);

}

static void draw_settings_popup(SDL_Renderer *r, const FPLayout *L)
{
    int btn_h   = L->resume_h;
    int row_h   = btn_h + 16;          /* 8px padding above and below button */
    int title_h = 10 * L->title_scale + 8;
    int nrows   = 6;
    int pw      = fp_popup_w(FP_POPUP_W_DP, L->sw);
    {   /* Widen if the "About EMU7800 vX.X.X" label would overlap the INFO button.
         * Available label area = 3/4 of pw minus ~28px of padding.
         * Required: pw >= (label_w + 28) * 4/3 */
        int label_w = font_string_width("About EMU7800 v" EMU7800_VERSION, L->font_scale);
        int pw_min  = (label_w + 28) * 4 / 3;
        if (pw_min > pw) pw = pw_min;
        if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    }
    int max_ph  = L->sh * 9 / 10;
    int ph      = title_h + 8 + nrows * row_h + 8;

    /* On skinny screens shrink row spacing (not button size) until everything fits. */
    if (ph > max_ph) {
        row_h = (max_ph - title_h - 16) / nrows;
        if (row_h < btn_h + 4) row_h = btn_h + 4;
        ph = title_h + 8 + nrows * row_h + 8;
        if (ph > max_ph) ph = max_ph;
    }

    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    font_draw_string(r, "SETTINGS", px + 12, py + 8, L->title_scale, 200, 100, 0);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    int iy        = py + title_h + 8;
    int bw        = pw / 4;
    int bx        = px + pw - bw - 8;
    int ty_off    = (row_h - 8 * L->font_scale) / 2;
    int btn_y_off = (row_h - btn_h) / 2;
    if (btn_y_off < 0) btn_y_off = 0;

    /* Clip rows to popup bounds — prevents overflow when popup is squeezed
     * on small screens with system bars visible */
    SDL_Rect settings_clip = {px, py + title_h, pw, ph - title_h};
    SDL_RenderSetClipRect(r, &settings_clip);

    int as = input_get_autosave();

    /* Auto-Save */
    font_draw_string(r, "Auto-Save", px + 12, iy + ty_off, L->font_scale, 255, 255, 255);
    fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, as ? "ON" : "OFF", bx, iy + btn_y_off, bw, btn_h, L->font_scale, 200, 100, 0);
    iy += row_h;

    /* Ask Before Saving (dimmed when Auto-Save is off) */
    {
        Uint8 lr = as ? 255 : 100, lg = as ? 255 : 100, lb = as ? 255 : 100;
        Uint8 ba = as ? 220 : 80;
        Uint8 ta = as ? 200 : 80;
        font_draw_string(r, "Ask Before Saving", px + 12, iy + ty_off, L->font_scale, lr, lg, lb);
        fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, ba);
        fp_centered_text(r, input_get_autosave_ask() ? "ON" : "OFF",
                         bx, iy + btn_y_off, bw, btn_h, L->font_scale, ta, ta / 2, 0);
    }
    iy += row_h;

    /* ROM Directory */
    font_draw_string(r, "ROM Directory", px + 12, iy + ty_off, L->font_scale, 255, 255, 255);
    fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "SET", bx, iy + btn_y_off, bw, btn_h, L->font_scale, 200, 100, 0);
    iy += row_h;

    /* Use Notch Area (greyed out when device has no cutout) */
    {
        Uint8 lr = g_has_cutout ? 255 : 100;
        Uint8 lg = g_has_cutout ? 255 : 100;
        Uint8 lb = g_has_cutout ? 255 : 100;
        Uint8 ba = g_has_cutout ? 220 :  80;
        Uint8 ta = g_has_cutout ? 200 :  80;
        font_draw_string(r, "Use Notch Area", px + 12, iy + ty_off, L->font_scale, lr, lg, lb);
        fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, ba);
        fp_centered_text(r, g_use_cutout ? "ON" : "OFF",
                         bx, iy + btn_y_off, bw, btn_h, L->font_scale, ta, ta / 2, 0);
    }
    iy += row_h;

    /* Bug Report */
    font_draw_string(r, "Bug Report", px + 12, iy + ty_off, L->font_scale, 255, 255, 255);
    fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "EMAIL", bx, iy + btn_y_off, bw, btn_h, L->font_scale, 200, 100, 0);
    iy += row_h;

    /* About EMU7800 — last row */
    font_draw_string(r, "About EMU7800 v" EMU7800_VERSION,
                     px + 12, iy + ty_off, L->font_scale, 255, 255, 255);
    fp_fill_rect(r, bx, iy + btn_y_off, bw, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "INFO", bx, iy + btn_y_off, bw, btn_h, L->font_scale, 200, 100, 0);

    SDL_RenderSetClipRect(r, NULL);
}

static void draw_autosave_warn_popup(SDL_Renderer *r, const FPLayout *L)
{
    int btn_h      = L->resume_h;
    int btn_w      = L->resume_w;
    int title_h    = 10 * L->title_scale + 8;
    int body_line_h = 8 * L->font_scale + 6;
    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    {
        int min_pw = font_string_width("This will automatically overwrite", L->font_scale) + 24;
        if (min_pw > pw) pw = min_pw;
        if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    }
    int ph = title_h + 8 + 2 * body_line_h + 16 + btn_h + 8;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    font_draw_string(r, "WARNING", px + 12, py + 8, L->title_scale, 200, 100, 0);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    int cy = py + title_h + 10;
    font_draw_string(r, "This will automatically overwrite",
                     px + 12, cy, L->font_scale, 255, 255, 255);
    cy += body_line_h;
    font_draw_string(r, "your save. Are you sure?",
                     px + 12, cy, L->font_scale, 255, 255, 255);

    int by      = py + ph - btn_h - 8;
    int gap     = 16;
    int total_w = btn_w * 2 + gap;
    int bx_yes  = px + (pw - total_w) / 2;
    int bx_no   = bx_yes + btn_w + gap;
    fp_fill_rect(r, bx_yes, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "YES", bx_yes, by, btn_w, btn_h, L->font_scale, 200, 100, 0);
    fp_fill_rect(r, bx_no,  by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "NO",  bx_no,  by, btn_w, btn_h, L->font_scale, 200, 100, 0);
}

static void draw_save_popup(SDL_Renderer *r, const FPLayout *L)
{
    /* Three equal-width buttons: YES (green), NO (grey), DELETE (red) */
    int btn_w = font_string_width("DELETE", L->font_scale) + fp_px_dp(16);
    {
        int w2 = font_string_width("YES", L->font_scale) + fp_px_dp(16);
        int w3 = font_string_width("NO",  L->font_scale) + fp_px_dp(16);
        if (w2 > btn_w) btn_w = w2;
        if (w3 > btn_w) btn_w = w3;
    }
    int gap = fp_px_dp(12);
    int total_btn_w = btn_w * 3 + gap * 2;

    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    {   /* ensure popup is wide enough for title and buttons */
        int pw_t1 = font_string_width("Would you like to continue", L->title_scale) + 24;
        int pw_t2 = font_string_width("from your Save?", L->title_scale) + 24;
        int pw_bn = total_btn_w + 24;
        if (pw_t1 > pw) pw = pw_t1;
        if (pw_t2 > pw) pw = pw_t2;
        if (pw_bn > pw) pw = pw_bn;
        if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    }

    int title_line_h = 10 * L->title_scale + 4;
    int btn_h        = L->resume_h;
    int ph = 12 + title_line_h * 2 + btn_h + 12;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    int ty = py + 12;
    font_draw_string(r, "Would you like to continue", px + 12, ty, L->title_scale, 200, 100, 0);
    ty += title_line_h;
    font_draw_string(r, "from your Save?", px + 12, ty, L->title_scale, 200, 100, 0);

    int by = py + ph - btn_h - 8;
    int bx = px + (pw - total_btn_w) / 2;
    fp_fill_rect(r, bx, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "YES", bx, by, btn_w, btn_h, L->font_scale, 200, 100, 0);
    bx += btn_w + gap;
    fp_fill_rect(r, bx, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "NO", bx, by, btn_w, btn_h, L->font_scale, 200, 100, 0);
    bx += btn_w + gap;
    fp_fill_rect(r, bx, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "DELETE", bx, by, btn_w, btn_h, L->font_scale, 200, 0, 0);
}

static void draw_delete_confirm_popup(SDL_Renderer *r, const FPLayout *L)
{
    int btn_w = font_string_width("YES", L->font_scale) + fp_px_dp(16);
    {
        int w2 = font_string_width("NO", L->font_scale) + fp_px_dp(16);
        if (w2 > btn_w) btn_w = w2;
    }
    int gap = fp_px_dp(12);
    int total_btn_w = btn_w * 2 + gap;

    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    {
        int pw_t1 = font_string_width("Are you sure you want to", L->title_scale) + 24;
        int pw_t2 = font_string_width("delete the save file?", L->title_scale) + 24;
        int pw_bn = total_btn_w + 24;
        if (pw_t1 > pw) pw = pw_t1;
        if (pw_t2 > pw) pw = pw_t2;
        if (pw_bn > pw) pw = pw_bn;
        if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    }

    int title_line_h = 10 * L->title_scale + 4;
    int btn_h        = L->resume_h;
    int ph = 12 + title_line_h * 2 + btn_h + 12;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    int ty = py + 12;
    font_draw_string(r, "Are you sure you want to", px + 12, ty, L->title_scale, 200, 100, 0);
    ty += title_line_h;
    font_draw_string(r, "delete the save file?", px + 12, ty, L->title_scale, 200, 100, 0);

    int by = py + ph - btn_h - 8;
    int bx = px + (pw - total_btn_w) / 2;
    fp_fill_rect(r, bx, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "YES", bx, by, btn_w, btn_h, L->font_scale, 200, 0, 0);
    bx += btn_w + gap;
    fp_fill_rect(r, bx, by, btn_w, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "NO", bx, by, btn_w, btn_h, L->font_scale, 200, 100, 0);
}

static void draw_notfound_popup(SDL_Renderer *r, const FPLayout *L)
{
    int pw = fp_popup_w(FP_POPUP_W_DP, L->sw);
    int ph = L->item_h + 10 * L->title_scale + 8 * L->font_scale + 50;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);
    font_draw_string(r, "File not found", px + 12, py + 10, L->title_scale, 255, 100, 80);
    font_draw_string(r, "Entry removed from list.", px + 12, py + 16 + 10 * L->title_scale,
                    L->font_scale, 180, 180, 180);
    int bw = pw / 3, by = py + ph - L->item_h - 8;
    fp_fill_rect(r, px + (pw - bw) / 2, by, bw, L->item_h, 80, 80, 80, 220);
    fp_centered_text(r, "OK", px + (pw - bw) / 2, by, bw, L->item_h, L->font_scale, 255, 255, 255);
}

static void draw_dirpicker_popup(SDL_Renderer *r, const FPLayout *L)
{
    int title_h = 10 * L->title_scale + 8;
    /* Ensure popup is wide enough for the title text */
    int pw_title = font_string_width("SELECT ROM DIRECTORY", L->title_scale) + 24;
    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    if (pw_title > pw) pw = pw_title;
    if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    int ph = L->sh;   /* intentionally full height — position anchor, not a size */
    int px = (L->sw - pw) / 2;
    int py = 0;
    fp_popup_bg(r, px, py, pw, ph);

    SDL_Rect title_clip = {px + 2, py, pw - 4, title_h};
    SDL_RenderSetClipRect(r, &title_clip);
    font_draw_string(r, "SELECT ROM DIRECTORY", px + 8, py + 6, L->title_scale, 200, 100, 0);
    SDL_RenderSetClipRect(r, NULL);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    int btn_h   = L->resume_h;
    int list_h  = ph - title_h - btn_h - 16;
    int start   = (int)g_dirpicker_scroll;
    if (start < 0) start = 0;

    /* Up-icon slot: drawn before the clip rect so it isn't clipped to one row. */
    int dp_has_back = (g_dirpicker_count > 0 && strcmp(g_dirpicker_dirs[0].name, "..") == 0);
    int dp_up_ih    = (L->item_h / 2 - 16) * 3;
    if (dp_up_ih < 1) dp_up_ih = 1;
    int dp_up_slot  = dp_has_back ? dp_up_ih + 16 : 0;

    if (dp_has_back && start == 0 && g_up_tex) {
        int iw = dp_up_ih * UP_IMG_W / UP_IMG_H;
        SDL_Rect usrc = {0, 0, UP_IMG_W, UP_IMG_H};
        SDL_Rect udst = {px + 8, py + title_h + 8, iw, dp_up_ih};
        /* Clip to popup bounds so it never bleeds outside the popup box */
        SDL_Rect popup_clip = {px, py + title_h, pw, ph - title_h};
        SDL_RenderSetClipRect(r, &popup_clip);
        SDL_RenderCopy(r, g_up_tex, &usrc, &udst);
        SDL_RenderSetClipRect(r, NULL);
    }

    /* Text items start below the up-icon slot (when slot is visible). */
    int text_top = py + title_h + 4 + (dp_has_back && start == 0 ? dp_up_slot : 0);
    int text_h   = (py + ph - btn_h - 8) - text_top;
    if (text_h < 0) text_h = 0;
    /* floor: used for scroll-max so the last fully-visible row is always reachable */
    int visible      = text_h / L->item_h;
    /* ceiling: used for the draw loop so the space right up to the SET button is filled;
     * the clip rect below clips any partial last row cleanly */
    int draw_visible = (text_h + L->item_h - 1) / L->item_h;

    /* Scrollbar */
    int sb_w = 6;
    int sb_x = px + pw - sb_w - 4;
    if (g_dirpicker_count > visible) {
        int bar_h   = list_h;
        int thumb_h = bar_h * visible / g_dirpicker_count;
        if (thumb_h < 20) thumb_h = 20;
        int max_scroll = g_dirpicker_count - visible;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = py + title_h + 2 + (int)((float)g_dirpicker_scroll / max_scroll * (bar_h - thumb_h));
        fp_fill_rect(r, sb_x, py + title_h + 2, sb_w, bar_h, 40, 40, 40, 180);
        fp_fill_rect(r, sb_x, thumb_y, sb_w, thumb_h, 200, 100, 0, 200);
    }

    SDL_Rect clip = {px + 2, text_top, pw - sb_w - 6, text_h};
    SDL_RenderSetClipRect(r, &clip);
    int draw_row = 0;
    for (int i = start; i < g_dirpicker_count && draw_row < draw_visible; i++) {
        if (strcmp(g_dirpicker_dirs[i].name, "..") == 0) continue; /* drawn above */
        int iy = text_top + draw_row * L->item_h;
        draw_row++;
        int r2 = 200, g2 = 100, b2 = 0;   /* orange for directories */
        if (!g_dirpicker_dirs[i].is_dir) { r2 = 255; g2 = 255; b2 = 255; } /* white for ROMs */
        font_draw_string(r, g_dirpicker_dirs[i].name, px + 8,
                        iy + (L->item_h - 8 * L->font_scale) / 2,
                        L->font_scale, r2, g2, b2);
    }
    SDL_RenderSetClipRect(r, NULL);

    int by = py + ph - L->resume_h - 8;
    int bx = px + (pw - L->resume_w) / 2;
    fp_fill_rect(r, bx, by, L->resume_w, L->resume_h, 60, 60, 60, 220);
    fp_centered_text(r, "SET", bx, by, L->resume_w, L->resume_h, L->font_scale, 200, 100, 0);
}

static void draw_first_launch_popup(SDL_Renderer *r, const FPLayout *L)
{
    int btn_h   = L->resume_h;
    int title_h = 10 * L->title_scale + 8;
    int line_h  = 8 * L->font_scale + 6;
    /* Compute popup width from the widest content element */
    int pw_title = font_string_width("Welcome to EMU7800!", L->title_scale) + 24;
    int pw_body  = font_string_width("Tap \"Set ROM Directory\" to browse", L->font_scale) + 24;
    int pw_btn   = (font_string_width("Set ROM Directory", L->font_scale) + 24) * 2 + 16;
    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    if (pw_title > pw) pw = pw_title;
    if (pw_body  > pw) pw = pw_body;
    if (pw_btn   > pw) pw = pw_btn;
    if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    int ph      = title_h + 8 + 2 * line_h + 16 + btn_h + 8;
    int px      = (L->sw - pw) / 2;
    int py      = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    /* Title — orange, matching Settings */
    SDL_Rect title_clip = {px + 2, py, pw - 4, title_h};
    SDL_RenderSetClipRect(r, &title_clip);
    font_draw_string(r, "Welcome to EMU7800!", px + 12, py + 8, L->title_scale, 200, 100, 0);
    SDL_RenderSetClipRect(r, NULL);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    /* Body — white, matching Settings labels */
    int cy = py + title_h + 10;
    font_draw_string(r, "Tap \"Set ROM Directory\" to browse",
                     px + 12, cy, L->font_scale, 255, 255, 255);
    cy += line_h;
    font_draw_string(r, "for a folder.",
                     px + 12, cy, L->font_scale, 255, 255, 255);

    /* Buttons — grey background, orange label, matching Settings style */
    int by   = py + ph - btn_h - 8;
    int half = pw / 2;
    int bpad = 8;

    /* SET ROM DIRECTORY (left half) */
    fp_fill_rect(r, px + bpad, by, half - bpad - 4, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "Set ROM Directory",
                     px + bpad, by, half - bpad - 4, btn_h, L->font_scale, 200, 100, 0);

    /* LATER (right half) */
    fp_fill_rect(r, px + half + 4, by, half - bpad - 4, btn_h, 60, 60, 60, 220);
    fp_centered_text(r, "Later",
                     px + half + 4, by, half - bpad - 4, btn_h, L->font_scale, 200, 100, 0);
}

static void draw_about_popup(SDL_Renderer *r, const FPLayout *L)
{
    static const char *lines[] = {
        "EMU7800 is a 100% Claude Code",
        "vibe-coded port by Alan Morford.",
        "",
        "EMU7800 was written by",
        "Mike Murphy.",
        "",
        "This port includes code from",
        "Stella 7.0 by Bradford W. Mott,",
        "Stephen Anthony and the",
        "Stella Team.",
        "",
        "Both apps and this port are",
        "licensed under GPL 2.0.",
    };
    int nlines = 13;
    int line_h  = 8 * L->font_scale + 4;
    int title_h = 10 * L->title_scale + 8;
    /* Compute popup width from the widest body line */
    int pw = fp_popup_w(FP_WIDE_W_DP, L->sw);
    for (int i = 0; i < nlines; i++) {
        if (lines[i][0]) {
            int lw = font_string_width(lines[i], L->font_scale) + 24;
            if (lw > pw) pw = lw;
        }
    }
    if (pw > L->sw * 9 / 10) pw = L->sw * 9 / 10;
    int ph = title_h + 8 + nlines * line_h + 8;
    if (ph > L->sh * 9 / 10) ph = L->sh * 9 / 10;
    int px = (L->sw - pw) / 2;
    int py = (L->sh - ph) / 2;
    fp_popup_bg(r, px, py, pw, ph);

    font_draw_string(r, "About EMU7800", px + 12, py + 8, L->title_scale, 200, 100, 0);
    SDL_SetRenderDrawColor(r, 180, 120, 0, 200);
    SDL_RenderDrawLine(r, px + 4, py + title_h, px + pw - 4, py + title_h);

    int ty = py + title_h + 8;
    SDL_Rect clip = {px + 4, py + title_h + 2, pw - 8, ph - title_h - 8};
    SDL_RenderSetClipRect(r, &clip);
    for (int i = 0; i < nlines; i++) {
        if (lines[i][0])
            font_draw_string(r, lines[i], px + 12, ty, L->font_scale, 220, 220, 220);
        ty += line_h;
    }
    SDL_RenderSetClipRect(r, NULL);
}

/* ---- Main draw ---- */

void filepicker_draw(SDL_Renderer *r)
{
    if (!r) return;

    FPLayout L = compute_layout(r);
    update_logo_frame();

    /* Background */
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    /* Logo: drawn first so everything else renders on top */
    if (g_logo_tex) {
        SDL_Rect src = {0, 0, LOGO_IMG_W, LOGO_IMG_H};
        SDL_Rect dst = {L.logo_x, L.logo_y, L.logo_sz, L.logo_sz};
        SDL_RenderCopy(r, g_logo_tex, &src, &dst);
    }

    /* Top bar background */
    fp_fill_rect(r, 0, 0, L.sw, L.top_h, 15, 15, 15, 255);

    /* RESUME button — only shown when there is a last ROM */
    if (g_has_last_rom) {
        fp_fill_rect(r, L.resume_x, L.resume_y, L.resume_w, L.resume_h, 60, 60, 60, 220);
        fp_centered_text(r, "RESUME", L.resume_x, L.resume_y, L.resume_w, L.resume_h,
                        L.font_scale, 200, 100, 0);
        /* Last ROM name shown to the right with generous gap */
        const char *fname = strrchr(g_last_rom_path, '/');
        fname = fname ? fname + 1 : g_last_rom_path;
        int nx = L.resume_x + L.resume_w + 24;
        int ny = L.resume_y + (L.resume_h - 8 * L.font_scale) / 2;
        char fname_noext[MAX_NAME_LEN];
        strncpy(fname_noext, fname, sizeof(fname_noext) - 1);
        fname_noext[sizeof(fname_noext) - 1] = '\0';
        char *dot = strrchr(fname_noext, '.');
        if (dot) *dot = '\0';
        if (nx + font_string_width(fname_noext, L.font_scale) < L.gear_x - 4)
            font_draw_string(r, fname_noext, nx, ny, L.font_scale, 255, 255, 255);
    }

    /* RECENT button — only shown when the recent list is non-empty */
    if (g_recent_count > 0) {
        fp_fill_rect(r, L.recent_x, L.recent_y, L.recent_w, L.recent_h, 60, 60, 60, 220);
        fp_centered_text(r, "RECENT", L.recent_x, L.recent_y, L.recent_w, L.recent_h,
                        L.font_scale, 200, 100, 0);
    }

    /* EMU7800 rainbow title: top-right, above the gear icon.
     * Colors cycle every second (webOS-style animation). */
    {
        static const char *emu_text = "EMU7800";
        static Uint32 last_tick  = 0;
        static int    color_step = 0;
        Uint32 now = SDL_GetTicks();
        if (now - last_tick >= 1000) { color_step = (color_step + 1) % 7; last_tick = now; }

        int   scale = L.emu_scale;
        int   cw    = font_string_width("E", scale);
        int   total = cw * 7;
        /* Right-align text with the right edge of the gear icon */
        int   tx    = L.gear_x + L.gear_w - total;
        int   ty    = 2;
        static const Uint8 colors[7][3] = {
            {255,  50,  50}, {255, 150,  50}, {255, 255,  50},
            { 50, 255,  50}, { 50, 150, 255}, {100,  50, 255}, {220,  50, 255}
        };
        for (int i = 0; i < 7; i++) {
            int ci = (i + color_step) % 7;
            char ch[2] = {emu_text[i], 0};
            font_draw_string(r, ch, tx + i * cw, ty, scale,
                            colors[ci][0], colors[ci][1], colors[ci][2]);
        }
    }

    /* Gear icon (top-right of top bar) */
    if (g_gear_tex) {
        SDL_Rect src = {0, 0, GEAR_IMG_W, GEAR_IMG_H};
        SDL_Rect dst = {L.gear_x, L.gear_y, L.gear_w, L.gear_w};
        SDL_RenderCopy(r, g_gear_tex, &src, &dst);
    } else {
        font_draw_string(r, "[S]", L.gear_x, L.gear_y + (L.gear_w - 8 * L.font_scale) / 2,
                        L.font_scale, 200, 200, 200);
    }

    /* Separator line */
    SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
    SDL_RenderDrawLine(r, 0, L.sep_y, L.sw, L.sep_y);

    /* File list panel: semi-transparent overlay covers from sep_y to bottom. */
    fp_fill_rect(r, L.list_x, L.sep_y + 2, L.list_w, L.sh - L.sep_y - 2, 0, 0, 0, 180);

    /* Up arrow icon — drawn just below the separator, outside the list clip rect.
     * Only shown when the list is not scrolled (i.e. the ".." entry is at position 0). */
    {
        int start = (int)g_scroll_offset;
        int has_back = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
        if (has_back && start == 0 && g_up_tex) {
            int ih = (L.item_h / 2 - 16) * 3;
            if (ih < 1) ih = 1;
            int iw = ih * UP_IMG_W / UP_IMG_H;
            SDL_Rect src = {0, 0, UP_IMG_W, UP_IMG_H};
            int up_y = L.sep_y + 8;
            SDL_Rect dst = {L.list_x + 6, up_y, iw, ih};
            SDL_RenderCopy(r, g_up_tex, &src, &dst);
        }
    }

    {
        /* When at root (no ".." entry) collapse the up-arrow slot so the list
         * starts immediately below the separator. */
        int has_back = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
        int up_ih    = (L.item_h / 2 - 16) * 3;
        int eff_top  = has_back ? L.sep_y + up_ih + 16 : L.sep_y + 2;

        SDL_Rect clip = {L.list_x, eff_top, L.list_w - 6, L.sh - eff_top};
        SDL_RenderSetClipRect(r, &clip);

        int visible = (L.sh - eff_top) / L.item_h + 1;
        int start   = (int)g_scroll_offset;
        if (start < 0) start = 0;

        int draw_row = 0;
        for (int i = start; i < g_file_count && draw_row < visible; i++) {
            const FileEntry *fe = &g_files[i];

            int is_back = (strcmp(fe->name, "..") == 0);
            if (is_back) continue;   /* drawn above the clip rect; skip without consuming a row */

            int iy = eff_top + draw_row * L.item_h;
            draw_row++;

            /* Draw a thin separator line at the dir→ROM boundary */
            if (i > 0 && g_files[i - 1].is_dir && !fe->is_dir) {
                SDL_SetRenderDrawColor(r, 80, 60, 0, 160);
                SDL_RenderDrawLine(r, L.list_x + 8, iy - 2, L.sw - 8, iy - 2);
            }

            {
                /* Build display string */
                char display[MAX_NAME_LEN + 2];
                if (fe->is_dir) {
                    snprintf(display, sizeof(display), "%s/", fe->name);
                } else {
                    strncpy(display, fe->name, sizeof(display) - 1);
                }
                display[sizeof(display) - 1] = '\0';

                /* Color: ROMs white, dirs orange, .sav purple */
                Uint8 tr, tg, tb;
                if (is_rom_type(fe->type)) {
                    tr = 255; tg = 255; tb = 255;
                } else if (fe->type == ENTRY_SAV) {
                    tr = 180; tg =  80; tb = 220;
                } else {
                    tr = 200; tg = 100; tb =   0;
                }

                int scale   = L.font_scale;
                int ty_item = iy + (L.item_h - 8 * scale) / 2;
                font_draw_string(r, display, L.list_x + 6, ty_item, scale, tr, tg, tb);
            }
        }
        SDL_RenderSetClipRect(r, NULL);
    }

    /* Asteroids thumbnail: drawn on top of the file list */
    if (g_ast_tex) {
        SDL_Rect src = {0, 0, ASTEROIDS_IMG_W, ASTEROIDS_IMG_H};
        SDL_Rect dst = {L.ast_x, L.ast_y, L.ast_w, L.ast_h};
        SDL_RenderCopy(r, g_ast_tex, &src, &dst);
        fp_draw_rect(r, L.ast_x - 1, L.ast_y - 1, L.ast_w + 2, L.ast_h + 2, 200, 140, 0, 200);
    }

    /* Popups */
    if (g_save_popup_visible)      draw_save_popup(r, &L);
    if (g_delete_confirm_visible)  draw_delete_confirm_popup(r, &L);
    if (g_notfound_visible)        draw_notfound_popup(r, &L);
    if (g_recent_popup_visible)    draw_recent_popup(r, &L);
    if (g_settings_popup_visible)  draw_settings_popup(r, &L);
    if (g_dirpicker_popup_visible) draw_dirpicker_popup(r, &L);
    if (g_about_popup_visible)     draw_about_popup(r, &L);
    if (g_autosave_warn_visible)   draw_autosave_warn_popup(r, &L);
    if (g_first_launch_popup)      draw_first_launch_popup(r, &L);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* ---- Touch handling ---- */

static int close_topmost_popup(void)
{
    if (g_about_popup_visible)     { g_about_popup_visible     = 0; return 1; }
    if (g_dirpicker_popup_visible) { g_dirpicker_popup_visible = 0; return 1; }
    if (g_settings_popup_visible)  { g_settings_popup_visible  = 0; return 1; }
    if (g_recent_popup_visible)    { g_recent_popup_visible    = 0; return 1; }
    if (g_notfound_visible)        { g_notfound_visible        = 0; return 1; }
    if (g_delete_confirm_visible)  { g_delete_confirm_visible  = 0; g_save_popup_visible = 1; return 1; }
    if (g_save_popup_visible)      { g_save_popup_visible      = 0; return 1; }
    if (g_first_launch_popup)      { g_first_launch_popup      = 0;
                                     filepicker_save_settings(); return 1; }
    return 0;
}

static int any_popup_visible(void)
{
    return g_first_launch_popup    || g_save_popup_visible || g_delete_confirm_visible ||
           g_notfound_visible ||
           g_recent_popup_visible  || g_settings_popup_visible ||
           g_dirpicker_popup_visible || g_about_popup_visible || g_autosave_warn_visible;
}

/* Close every popup so a newly-opened one is always on a clean slate. */
static void close_all_popups(void)
{
    g_settings_popup_visible  = 0;
    g_recent_popup_visible    = 0;
    g_dirpicker_popup_visible = 0;
    g_about_popup_visible     = 0;
    g_autosave_warn_visible   = 0;
    g_save_popup_visible      = 0;
    g_delete_confirm_visible  = 0;
    g_notfound_visible        = 0;
}

void filepicker_back(void)
{
    if (!close_topmost_popup()) {
        /* Navigate up one directory, or signal root to Java for double-back-to-exit */
        if (strcmp(g_current_dir, "/storage/emulated/0/") != 0 &&
            strcmp(g_current_dir, "/") != 0) {
            char parent[MAX_PATH_LEN];
            strncpy(parent, g_current_dir, MAX_PATH_LEN - 1);
            int plen = strlen(parent);
            if (plen > 1 && parent[plen - 1] == '/') parent[--plen] = '\0';
            char *slash = strrchr(parent, '/');
            if (slash) { *(slash + 1) = '\0'; }
            filepicker_scan(parent);
        } else {
            /* Already at root — Java shows Toast on first press, finish() on second */
            jni_filepicker_at_root();
        }
    }
}

void filepicker_touch_down(int x, int y)
{
    /* Always record the touch start and clear the moved flag so that popup
     * list-item handlers (which check !g_touch_moved) work correctly even
     * when a popup was already visible before the finger went down.
     * g_touch_active and g_scroll_at_touch are only set when no popup is
     * open, to prevent the underlying list from scrolling behind a popup. */
    g_touch_moved   = 0;
    g_touch_start_x = x;
    g_touch_start_y = y;
    g_touch_last_y  = y;

    if (g_dirpicker_popup_visible) {
        g_scroll_mode     = 1;
        g_scroll_at_touch = g_dirpicker_scroll;
        g_touch_active    = 1;
    } else if (g_recent_popup_visible) {
        g_scroll_mode     = 2;
        g_scroll_at_touch = g_recent_scroll;
        g_touch_active    = 1;
    } else if (any_popup_visible()) {
        return;  /* other popups: modal, no scroll */
    } else {
        g_scroll_mode     = 0;
        g_scroll_at_touch = g_scroll_offset;
        g_touch_active    = 1;
    }
}

void filepicker_touch_move(int x, int y)
{
    if (!g_touch_active) return;
    (void)x;
    int dy = y - g_touch_start_y;
    if (dy < 0) dy = -dy;
    if (dy > TAP_THRESHOLD) g_touch_moved = 1;

    if (g_touch_moved) {
        FPLayout L = compute_layout(video_get_renderer());
        float delta = (float)(y - g_touch_start_y) / L.item_h;
        float new_s = g_scroll_at_touch - delta;

        if (g_scroll_mode == 1) {
            /* Dirpicker popup */
            int ph       = L.sh;
            int py_dp    = 0;
            int title_h  = 10 * L.title_scale + 8;
            int btn_h    = L.resume_h;
            int up_ih_s  = (L.item_h / 2 - 16) * 3;
            int dp_back  = (g_dirpicker_count > 0 && strcmp(g_dirpicker_dirs[0].name, "..") == 0);
            int up_slot  = dp_back ? up_ih_s + 16 : 0;
            int text_top = py_dp + title_h + 4 + up_slot;
            int btn_y_s  = py_dp + ph - btn_h - 8;
            int text_h   = btn_y_s - text_top;
            if (text_h < 0) text_h = 0;
            int visible  = text_h / L.item_h;
            float max_s  = (float)(g_dirpicker_count - visible);
            if (max_s < 0) max_s = 0;
            if (new_s < 0) new_s = 0;
            if (new_s > max_s) new_s = max_s;
            g_dirpicker_scroll = new_s;
        } else if (g_scroll_mode == 2) {
            /* Recent popup: ph = sh*3/4, title_h, btn_h = item_h, list_h = ph-title_h-btn_h-20 */
            int ph      = L.sh * 3 / 4;
            int title_h = 10 * L.title_scale + 8;
            int list_h  = ph - title_h - L.item_h - 20;
            int visible = list_h / L.item_h;
            float max_s = (float)(g_recent_count - visible);
            if (max_s < 0) max_s = 0;
            if (new_s < 0) new_s = 0;
            if (new_s > max_s) new_s = max_s;
            g_recent_scroll = new_s;
        } else {
            /* Main file list */
            int hb    = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
            int up_ih2 = (L.item_h / 2 - 16) * 3;
            int eff   = hb ? L.sep_y + up_ih2 + 16 : L.sep_y + 2;
            if (new_s < 0) new_s = 0;
            float max_s = (float)(g_file_count - (L.sh - eff) / L.item_h);
            if (max_s < 0) max_s = 0;
            if (new_s > max_s) new_s = max_s;
            g_scroll_offset = new_s;
        }
    }
    g_touch_last_y = y;
}

/* Helper: launch a ROM by path+type, with save-state check */
static int try_launch(int *rom_selected_out,
                      const char *path, int type)
{
    if (!is_rom_type(type)) return 0;   /* not a ROM, ignore */

    strncpy(g_selected_path, path, MAX_PATH_LEN - 1);
    g_selected_path[MAX_PATH_LEN - 1] = '\0';
    g_selected_type    = type;
    g_should_load_save = 0;

    /* Check for save state — use same path logic as savestate.c: strip extension */
    char sav[MAX_PATH_LEN + 4];
    {
        const char *dot = strrchr(path, '.');
        int base_len = dot ? (int)(dot - path) : (int)strlen(path);
        if (base_len + 5 > (int)sizeof(sav)) base_len = (int)sizeof(sav) - 5;
        memcpy(sav, path, base_len);
        memcpy(sav + base_len, ".sav", 5);
    }
    if (access(sav, F_OK) == 0) {
        g_save_popup_visible = 1;
        strncpy(g_save_popup_path, path, MAX_PATH_LEN - 1);
        g_save_popup_type = type;
        return 0;   /* not launched yet — waiting for popup answer */
    }

    *rom_selected_out = 1;
    return 1;
}

int filepicker_touch_up(int x, int y)
{
    SDL_Renderer *r = video_get_renderer();
    FPLayout      L = compute_layout(r);
    int           rom_selected = 0;

    /* ---- Popup touch handling ---- */

    if (g_first_launch_popup) {
        int btn_h   = L.resume_h;
        int title_h = 10 * L.title_scale + 8;
        int line_h  = 8 * L.font_scale + 6;
        int pw_title = font_string_width("Welcome to EMU7800!", L.title_scale) + 24;
        int pw_body  = font_string_width("Tap \"Set ROM Directory\" to browse", L.font_scale) + 24;
        int pw_btn   = (font_string_width("Set ROM Directory", L.font_scale) + 24) * 2 + 16;
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        if (pw_title > pw) pw = pw_title;
        if (pw_body  > pw) pw = pw_body;
        if (pw_btn   > pw) pw = pw_btn;
        if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10;
        int ph      = title_h + 8 + 2 * line_h + 16 + btn_h + 8;
        int px      = (L.sw - pw) / 2;
        int py      = (L.sh - ph) / 2;
        int by      = py + ph - btn_h - 8;
        int half    = pw / 2;
        int bpad    = 8;

        /* SET ROM DIRECTORY button */
        if (x >= px + bpad && x <= px + half - 4 && y >= by && y <= by + btn_h) {
            dirpicker_scan("/storage/emulated/0/");
            g_first_launch_popup      = 0;
            filepicker_save_settings();
            g_dirpicker_popup_visible = 1;
            }
        /* LATER button */
        else if (x >= px + half + 4 && x <= px + pw - bpad && y >= by && y <= by + btn_h) {
            g_first_launch_popup = 0;
            filepicker_save_settings();
        }
        /* Tap anywhere else in the popup is ignored (popup is modal) */
        g_touch_active = 0;
        return 0;
    }

    if (g_notfound_visible) {
        int pw = fp_popup_w(FP_POPUP_W_DP, L.sw);
        int ph = L.item_h + 10 * L.title_scale + 8 * L.font_scale + 50;
        int px = (L.sw - pw) / 2, py = (L.sh - ph) / 2;
        int bw = pw / 3, by = py + ph - L.item_h - 8;
        int bx = px + (pw - bw) / 2;
        if (x >= bx && x <= bx + bw && y >= by && y <= by + L.item_h) {
            if (g_notfound_recent_idx >= 0)
                remove_recent_entry(g_notfound_recent_idx);
            else { g_has_last_rom = 0; g_last_rom_path[0] = '\0'; }
            g_notfound_visible = 0;
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_delete_confirm_visible) {
        /* Geometry matches draw_delete_confirm_popup */
        int btn_w = font_string_width("YES", L.font_scale) + fp_px_dp(16);
        { int w2 = font_string_width("NO", L.font_scale) + fp_px_dp(16); if (w2 > btn_w) btn_w = w2; }
        int gap = fp_px_dp(12);
        int total_btn_w = btn_w * 2 + gap;
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        { int pw_t = font_string_width("Are you sure you want to", L.title_scale) + 24;
          if (pw_t > pw) pw = pw_t;
          pw_t = font_string_width("delete the save file?", L.title_scale) + 24;
          if (pw_t > pw) pw = pw_t;
          int pw_bn = total_btn_w + 24; if (pw_bn > pw) pw = pw_bn;
          if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10; }
        int title_line_h = 10 * L.title_scale + 4;
        int btn_h = L.resume_h;
        int ph = 12 + title_line_h * 2 + btn_h + 12;
        int px = (L.sw - pw) / 2, py = (L.sh - ph) / 2;
        int by = py + ph - btn_h - 8;
        int bx = px + (pw - total_btn_w) / 2;
        /* YES — delete save file and launch fresh */
        if (x >= bx && x < bx + btn_w && y >= by && y < by + btn_h) {
            char sav[MAX_PATH_LEN + 4];
            {   /* Strip ROM extension and append .sav — matches try_launch path logic */
                const char *dot = strrchr(g_save_popup_path, '.');
                int base_len = dot ? (int)(dot - g_save_popup_path) : (int)strlen(g_save_popup_path);
                if (base_len + 5 > (int)sizeof(sav)) base_len = (int)sizeof(sav) - 5;
                memcpy(sav, g_save_popup_path, base_len);
                memcpy(sav + base_len, ".sav", 5);
            }
            remove(sav);
            g_delete_confirm_visible = 0;
            g_should_load_save = 0;
            strncpy(g_selected_path, g_save_popup_path, MAX_PATH_LEN - 1);
            g_selected_type = g_save_popup_type;
            add_to_recent(g_selected_path, g_selected_type);
            g_touch_active = 0;
            return 1;
        }
        bx += btn_w + gap;
        /* NO — return to save popup */
        if (x >= bx && x < bx + btn_w && y >= by && y < by + btn_h) {
            g_delete_confirm_visible = 0;
            g_save_popup_visible = 1;
            g_touch_active = 0;
            return 0;
        }
        /* Tap outside — return to save popup */
        if (x < px || x > px + pw || y < py || y > py + ph) {
            g_delete_confirm_visible = 0;
            g_save_popup_visible = 1;
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_save_popup_visible) {
        /* Geometry matches draw_save_popup — 3 equal-width buttons */
        int btn_w = font_string_width("DELETE", L.font_scale) + fp_px_dp(16);
        { int w2 = font_string_width("YES", L.font_scale) + fp_px_dp(16);
          int w3 = font_string_width("NO",  L.font_scale) + fp_px_dp(16);
          if (w2 > btn_w) btn_w = w2; if (w3 > btn_w) btn_w = w3; }
        int gap = fp_px_dp(12);
        int total_btn_w = btn_w * 3 + gap * 2;
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        { int pw_t = font_string_width("Would you like to continue", L.title_scale) + 24;
          if (pw_t > pw) pw = pw_t;
          pw_t = font_string_width("from your Save?", L.title_scale) + 24;
          if (pw_t > pw) pw = pw_t;
          int pw_bn = total_btn_w + 24; if (pw_bn > pw) pw = pw_bn;
          if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10; }
        int title_line_h = 10 * L.title_scale + 4;
        int btn_h = L.resume_h;
        int ph = 12 + title_line_h * 2 + btn_h + 12;
        int px = (L.sw - pw) / 2, py = (L.sh - ph) / 2;
        int by = py + ph - btn_h - 8;
        int bx = px + (pw - total_btn_w) / 2;
        /* YES — load save */
        if (x >= bx && x < bx + btn_w && y >= by && y < by + btn_h) {
            g_save_popup_visible = 0;
            g_should_load_save   = 1;
            strncpy(g_selected_path, g_save_popup_path, MAX_PATH_LEN - 1);
            g_selected_type = g_save_popup_type;
            add_to_recent(g_selected_path, g_selected_type);
            g_touch_active = 0;
            return 1;
        }
        bx += btn_w + gap;
        /* NO — launch fresh */
        if (x >= bx && x < bx + btn_w && y >= by && y < by + btn_h) {
            g_save_popup_visible = 0;
            g_should_load_save   = 0;
            strncpy(g_selected_path, g_save_popup_path, MAX_PATH_LEN - 1);
            g_selected_type = g_save_popup_type;
            add_to_recent(g_selected_path, g_selected_type);
            g_touch_active = 0;
            return 1;
        }
        bx += btn_w + gap;
        /* DELETE — show confirmation popup */
        if (x >= bx && x < bx + btn_w && y >= by && y < by + btn_h) {
            g_save_popup_visible     = 0;
            g_delete_confirm_visible = 1;
            g_touch_active = 0;
            return 0;
        }
        /* Tap outside → close popup */
        if (x < px || x > px + pw || y < py || y > py + ph) {
            g_save_popup_visible = 0;
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_about_popup_visible) {
        g_about_popup_visible = 0;
        g_touch_active = 0;
        return 0;
    }

    if (g_autosave_warn_visible) {
        int btn_h      = L.resume_h;
        int btn_w      = L.resume_w;
        int title_h    = 10 * L.title_scale + 8;
        int body_line_h = 8 * L.font_scale + 6;
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        { int min_pw = font_string_width("This will automatically overwrite", L.font_scale) + 24;
          if (min_pw > pw) pw = min_pw;
          if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10; }
        int ph = title_h + 8 + 2 * body_line_h + 16 + btn_h + 8;
        int px = (L.sw - pw) / 2;
        int py = (L.sh - ph) / 2;
        int by      = py + ph - btn_h - 8;
        int gap     = 16;
        int total_w = btn_w * 2 + gap;
        int bx_yes  = px + (pw - total_w) / 2;
        int bx_no   = bx_yes + btn_w + gap;
        /* Yes — confirm: set Ask Before Saving to OFF */
        if (x >= bx_yes && x <= bx_yes + btn_w && y >= by && y <= by + btn_h) {
            input_set_autosave_ask(0);
            filepicker_save_settings();
            g_autosave_warn_visible = 0;
        }
        /* No — cancel: keep Ask Before Saving ON, return to Settings */
        else if (x >= bx_no && x <= bx_no + btn_w && y >= by && y <= by + btn_h) {
            input_set_autosave_ask(1);
            g_autosave_warn_visible  = 0;
            g_settings_popup_visible = 1;
        }
        /* Tap outside — treat same as No */
        else if (x < px || x > px + pw || y < py || y > py + ph) {
            g_autosave_warn_visible  = 0;
            g_settings_popup_visible = 1;
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_dirpicker_popup_visible) {
        int pw_title = font_string_width("SELECT ROM DIRECTORY", L.title_scale) + 24;
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        if (pw_title > pw) pw = pw_title;
        if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10;
        int ph = L.sh;
        int px = (L.sw - pw) / 2, py = 0;
        /* Tap outside → close */
        if (x < px || x > px + pw || y < py || y > py + ph) {
            g_dirpicker_popup_visible = 0;
            g_touch_active = 0;
            return 0;
        }
        int title_h  = 10 * L.title_scale + 8;  /* must match draw_dirpicker_popup */
        int btn_h    = L.resume_h;
        int btn_x    = px + (pw - L.resume_w) / 2;
        int btn_y    = py + ph - btn_h - 8;
        /* SET */
        if (x >= btn_x && x <= btn_x + L.resume_w && y >= btn_y && y <= btn_y + btn_h) {
            strncpy(g_default_romdir, g_dirpicker_current, MAX_PATH_LEN - 1);
            filepicker_save_settings();
            g_dirpicker_popup_visible = 0;
            g_settings_popup_visible  = 0;
            g_first_launch_popup      = 0;
            filepicker_scan(g_default_romdir);
            g_touch_active = 0;
            return 0;
        }
        /* Up-icon slot */
        int dp_has_back = (g_dirpicker_count > 0 && strcmp(g_dirpicker_dirs[0].name, "..") == 0);
        int dp_up_ih    = (L.item_h / 2 - 16) * 3;
        int dp_up_slot  = dp_has_back ? dp_up_ih + 16 : 0;
        int start       = (int)g_dirpicker_scroll;
        if (dp_has_back && start == 0) {
            int up_top = py + title_h + 4;
            int up_bot = up_top + dp_up_slot;
            if (!g_touch_moved && y >= up_top && y < up_bot) {
                dirpicker_scan(g_dirpicker_dirs[0].path);
                g_touch_active = 0;
                return 0;
            }
        }
        /* Text items */
        int text_top = py + title_h + 4 + (dp_has_back && start == 0 ? dp_up_slot : 0);
        int text_h   = btn_y - text_top;
        if (text_h < 0) text_h = 0;
        int visible  = text_h / L.item_h;
        if (!g_touch_moved && y >= text_top && y < text_top + visible * L.item_h) {
            int row = (y - text_top) / L.item_h;
            int count = 0;
            for (int i = start; i < g_dirpicker_count; i++) {
                if (strcmp(g_dirpicker_dirs[i].name, "..") == 0) continue;
                if (count == row) {
                    if (g_dirpicker_dirs[i].is_dir)
                        dirpicker_scan(g_dirpicker_dirs[i].path);
                    break;
                }
                count++;
            }
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_settings_popup_visible) {
        int btn_h_s = L.resume_h;
        int row_h   = btn_h_s + 16;
        int title_h = 10 * L.title_scale + 8;
        int nrows   = 6;
        int pw = fp_popup_w(FP_POPUP_W_DP, L.sw);
        {
            int label_w = font_string_width("About EMU7800 v" EMU7800_VERSION, L.font_scale);
            int pw_min  = (label_w + 28) * 4 / 3;
            if (pw_min > pw) pw = pw_min;
            if (pw > L.sw * 9 / 10) pw = L.sw * 9 / 10;
        }
        int max_ph = L.sh * 9 / 10;
        int ph = title_h + 8 + nrows * row_h + 8;
        /* Mirror draw_settings_popup shrink logic exactly */
        if (ph > max_ph) {
            row_h = (max_ph - title_h - 16) / nrows;
            if (row_h < btn_h_s + 4) row_h = btn_h_s + 4;
            ph = title_h + 8 + nrows * row_h + 8;
            if (ph > max_ph) ph = max_ph;
        }
        int px = (L.sw - pw) / 2, py = (L.sh - ph) / 2;
        int iy = py + title_h + 8;
        /* Tap outside → close */
        if (x < px || x > px + pw || y < py || y > py + ph) {
            g_settings_popup_visible = 0;
            g_touch_active = 0;
            return 0;
        }
        /* Auto-Save */
        if (y >= iy && y <= iy + row_h) {
            int new_as = !input_get_autosave();
            input_set_autosave(new_as);
            if (!new_as) input_set_autosave_ask(1);
            filepicker_save_settings();
        }
        iy += row_h;
        /* Ask Before Saving */
        if (y >= iy && y <= iy + row_h && input_get_autosave()) {
            if (input_get_autosave_ask()) {
                g_autosave_warn_visible = 1;
            } else {
                input_set_autosave_ask(1);
                filepicker_save_settings();
            }
        }
        iy += row_h;
        /* ROM Directory */
        if (y >= iy && y <= iy + row_h) {
            close_all_popups();
            dirpicker_scan("/storage/emulated/0/");
            g_dirpicker_popup_visible = 1;
        }
        iy += row_h;
        /* Use Notch Area — disabled when device has no cutout */
        if (y >= iy && y <= iy + row_h && g_has_cutout) {
            g_use_cutout = !g_use_cutout;
            jni_set_cutout_mode(g_use_cutout);
            filepicker_save_settings();
        }
        iy += row_h;
        /* Bug Report */
        if (y >= iy && y <= iy + row_h) {
            jni_send_bug_report_email();
        }
        iy += row_h;
        /* About EMU7800 — last row */
        if (y >= iy && y <= iy + row_h) {
            close_all_popups();
            g_about_popup_visible = 1;
        }
        g_touch_active = 0;
        return 0;
    }

    if (g_recent_popup_visible) {
        int pw = fp_popup_w(FP_WIDE_W_DP, L.sw);
        int ph = fp_px_dp(400); if (ph > L.sh * 9 / 10) ph = L.sh * 9 / 10;
        int px = (L.sw - pw) / 2, py = (L.sh - ph) / 2;
        int title_h = 10 * L.title_scale + 8;
        /* Tap outside → close */
        if (x < px || x > px + pw || y < py || y > py + ph) {
            g_recent_popup_visible = 0;
            g_touch_active = 0;
            return 0;
        }
        /* CLEAR LIST */
        {
            int btn_h_r = L.resume_h;
            int bw = font_string_width("CLEAR LIST", L.font_scale) + 24;
            if (bw < pw / 3) bw = pw / 3;
            int bx = px + (pw - bw) / 2;
            int by = py + ph - btn_h_r - 8;
            if (x >= bx && x <= bx + bw && y >= by && y <= by + btn_h_r) {
                g_recent_count = 0;
                save_recent_list();
                g_recent_popup_visible = 0;
                g_touch_active = 0;
                return 0;
            }
        }
        /* List items */
        int btn_h   = L.resume_h;
        int list_h  = ph - title_h - btn_h - 20;
        int visible = list_h / L.item_h;
        int start   = (int)g_recent_scroll;
        if (start < 0) start = 0;
        int list_top = py + title_h + 4;
        if (!g_touch_moved && y >= list_top && y < list_top + visible * L.item_h) {
            int idx = start + (y - list_top) / L.item_h;
            if (idx >= 0 && idx < g_recent_count) {
                if (access(g_recent_paths[idx], F_OK) == 0) {
                    g_recent_popup_visible = 0;
                    try_launch(&rom_selected,
                               g_recent_paths[idx], g_recent_types[idx]);
                    if (rom_selected)
                        add_to_recent(g_recent_paths[idx], g_recent_types[idx]);
                } else {
                    g_notfound_visible      = 1;
                    g_notfound_recent_idx   = idx;
                    g_recent_popup_visible  = 0;
                }
            }
        }
        g_touch_active = 0;
        return rom_selected;
    }

    /* ---- Main filepicker touch ---- */

    /* Guard against the synthesized SDL_MOUSEBUTTONUP that Android emits for
     * every physical touch alongside SDL_FINGERUP.  touch_down only sets
     * g_touch_active when no popup is visible; popup handlers above always clear
     * it.  So if we reach here with g_touch_active==0 it means this is a
     * duplicate event that already fired as FINGERUP and was handled — drop it. */
    if (!g_touch_active) return 0;

    /* Ignore if finger moved (scroll gesture) */
    {
        int dx = x - g_touch_start_x; if (dx < 0) dx = -dx;
        int dy = y - g_touch_start_y; if (dy < 0) dy = -dy;
        if (g_touch_moved || dx > TAP_THRESHOLD || dy > TAP_THRESHOLD) {
            g_touch_active = 0;
            return 0;
        }
    }

    /* Top bar */
    if (y < L.top_h) {
        /* Gear icon */
        if (x >= L.gear_x && x <= L.gear_x + L.gear_w &&
            y >= L.gear_y && y <= L.gear_y + L.gear_w) {
            g_settings_popup_visible = 1;
        }
        /* RESUME */
        else if (x >= L.resume_x && x <= L.resume_x + L.resume_w &&
                 y >= L.resume_y && y <= L.resume_y + L.resume_h && g_has_last_rom) {
            if (access(g_last_rom_path, F_OK) == 0) {
                try_launch(&rom_selected, g_last_rom_path, g_last_rom_type);
            } else {
                g_notfound_visible    = 1;
                g_notfound_recent_idx = -1;
            }
        }
        /* RECENT (only when list is non-empty) */
        else if (g_recent_count > 0 &&
                 x >= L.recent_x && x <= L.recent_x + L.recent_w &&
                 y >= L.recent_y && y <= L.recent_y + L.recent_h) {
            g_recent_popup_visible = 1;
            g_recent_scroll = 0.0f;
            }
        g_touch_active = 0;
        return rom_selected;
    }

    /* Asteroids thumbnail */
    if (g_asteroids_path[0] != '\0' &&
        x >= L.ast_x && x <= L.ast_x + L.ast_w &&
        y >= L.ast_y && y <= L.ast_y + L.ast_h) {
        if (access(g_asteroids_path, F_OK) == 0) {
            strncpy(g_selected_path, g_asteroids_path, MAX_PATH_LEN - 1);
            g_selected_type    = MACHINE_7800;
            g_should_load_save = 0;
            add_to_recent(g_asteroids_path, MACHINE_7800);
            g_touch_active = 0;
            return 1;
        }
    }

    /* Compute effective list top (accounts for up-arrow slot when "../" is present). */
    {
    int hb2      = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
    int up_ih3   = (L.item_h / 2 - 16) * 3;
    int eff_top2 = hb2 ? L.sep_y + up_ih3 + 16 : L.sep_y + 2;
    int start2   = (int)g_scroll_offset;

    /* Up arrow tap: full image height, between separator and eff_top. */
    if (y >= L.sep_y && y < eff_top2) {
        if (hb2 && !g_touch_moved)
            filepicker_scan(g_files[0].path);
        g_touch_active = 0;
        return 0;
    }

    /* File list: tap target is only as wide as the item's text. */
    if (y >= eff_top2) {
        int row = (y - eff_top2) / L.item_h;
        /* When start==0 and "../" is at index 0, draw_row 0 = g_files[1], so skip +1 */
        int idx = start2 + row + (hb2 && start2 == 0 ? 1 : 0);
        if (idx >= 0 && idx < g_file_count) {
            const FileEntry *fe = &g_files[idx];
            /* Build display string to measure tap width */
            char disp[MAX_NAME_LEN + 2];
            int is_back = (strcmp(fe->name, "..") == 0);
            if (is_back)         snprintf(disp, sizeof(disp), "../");
            else if (fe->is_dir) snprintf(disp, sizeof(disp), "%s/", fe->name);
            else                 strncpy(disp, fe->name, sizeof(disp) - 1);
            disp[sizeof(disp) - 1] = '\0';
            if (is_back) goto list_done;   /* handled by the up-arrow zone above */
            int tap_w;
            {
                int scale = L.font_scale;
                tap_w = L.list_x + 6 + font_string_width(disp, scale) + 8;
            }
            if (x > tap_w) goto list_done;
            if (fe->is_dir) {
                filepicker_scan(fe->path);
            } else if (is_rom_type(fe->type)) {
                if (access(fe->path, F_OK) == 0) {
                    add_to_recent(fe->path, fe->type);
                    try_launch(&rom_selected, fe->path, fe->type);
                } else {
                    g_notfound_visible    = 1;
                    g_notfound_recent_idx = -1;
                }
            }
            /* ENTRY_FILE — unrecognized, do nothing */
        }
    }
    } /* end hb2/eff_top2 block */
    list_done:

    g_touch_active = 0;
    return rom_selected;
}

/* ---- Public accessors ---- */

const char *filepicker_get_selected_path(void)  { return g_selected_path; }
int         filepicker_get_selected_type(void)  { return g_selected_type; }
int         filepicker_should_load_save(void)   { return g_should_load_save; }

void filepicker_set_last_rom(const char *path, int type)
{
    if (!path) return;
    strncpy(g_last_rom_path, path, MAX_PATH_LEN - 1);
    g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
    g_last_rom_type = type;
    g_has_last_rom  = 1;
    save_last_rom();
    add_to_recent(path, type);
}

int         filepicker_has_last_rom(void)       { return g_has_last_rom; }
const char *filepicker_get_last_rom_path(void)  { return g_last_rom_path; }
int         filepicker_get_last_rom_type(void)  { return g_last_rom_type; }

void filepicker_show_notfound(void)
{
    g_notfound_visible    = 1;
    g_notfound_recent_idx = -1;
}

const char *filepicker_get_default_romdir(void)
{
    return g_default_romdir[0] ? g_default_romdir : "/storage/emulated/0/";
}

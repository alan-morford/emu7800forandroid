/*
 * updater.c
 *
 * Update checker state — Android port.
 *
 * The actual HTTP fetch is done on a Java background thread (HTTPS requires
 * Android's TLS stack; no native SSL library is bundled).  This file holds
 * the shared C state and the public API that the rest of native code uses.
 *
 * Flow:
 *   1. updater_check_start() → jni_start_update_check() → Java starts thread
 *   2. Java fetches api.github.com/repos/.../releases/latest via HTTPS
 *   3. Java parses tag_name / body / html_url, compares with BuildConfig.VERSION_NAME
 *   4. If newer: Java calls nativeUpdateResult (jni_bridge.c)
 *              → updater_set_result() stores strings, sets g_update_available
 *              → Java posts AlertDialog on the UI thread
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include "updater.h"

#define VERSION_SIZE  32
#define NOTE_SIZE     1024
#define URL_SIZE      256

/* Thread-shared state.
 * Written by updater_set_result() (called on the JNI thread from a Java
 * background thread), read by main thread via updater_has_update() etc.
 * Same write-barrier pattern as g_frame_ready in main.c. */
static volatile int g_check_started    = 0;
static volatile int g_update_available = 0;
static volatile int g_update_dismissed = 0;

static char g_new_version[VERSION_SIZE];
static char g_version_note[NOTE_SIZE];
static char g_release_url[URL_SIZE];

/* Declared in jni_bridge.c */
extern void jni_start_update_check(void);

/* ---- Public API ---- */

void updater_check_start(void)
{
    if (g_check_started) return;
    g_check_started    = 1;
    g_update_available = 0;
    g_update_dismissed = 0;
    g_new_version[0]   = '\0';
    g_version_note[0]  = '\0';
    g_release_url[0]   = '\0';
    jni_start_update_check();
}

int updater_has_update(void)
{
    return g_update_available && !g_update_dismissed;
}

const char *updater_get_version(void) { return g_new_version; }
const char *updater_get_note(void)    { return g_version_note; }
const char *updater_get_url(void)     { return g_release_url; }

void updater_dismiss(void)
{
    g_update_dismissed = 1;
}

/* Called from jni_bridge.c's nativeUpdateResult JNI export. */
void updater_set_result(const char *version, const char *note, const char *url)
{
    if (!version || !version[0]) return;

    strncpy(g_new_version, version, VERSION_SIZE - 1);
    g_new_version[VERSION_SIZE - 1] = '\0';

    strncpy(g_version_note, note ? note : "", NOTE_SIZE - 1);
    g_version_note[NOTE_SIZE - 1] = '\0';

    strncpy(g_release_url, url ? url : "", URL_SIZE - 1);
    g_release_url[URL_SIZE - 1] = '\0';

    /* Memory barrier — main thread sees strings before the flag */
    __sync_synchronize();
    g_update_available = 1;
}

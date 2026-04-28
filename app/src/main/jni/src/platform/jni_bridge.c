/*
 * jni_bridge.c
 *
 * JNI boundary between EMU7800Activity.java and native code.
 *
 * Exported JNI functions (called from Java):
 *   nativeSetCacheDir()    — stores app cache path at startup (for SAF ROM copies)
 *   nativeSetDataDir()     — stores app data path for filepicker persistence files
 *   nativeOnRomSelected()  — receives open fd + filename from SAF picker
 *
 * Native helper (called from main.c):
 *   jni_open_rom_picker()  — asks Java to launch the SAF document picker (fallback)
 *
 * Copyright (c) 2024 EMU7800
 */

#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <unistd.h>
#include <SDL.h>        /* SDL_AndroidGetJNIEnv, SDL_AndroidGetActivity */

#define LOG_TAG "EMU7800"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Forward declarations */
extern void app_load_rom(const char *path, int machine_type);
extern void filepicker_set_data_dir(const char *dir);
extern void filepicker_set_display_density(float density);
extern void filepicker_set_has_cutout(int has_cutout);
extern void input_set_display_density(float density);
extern void filepicker_rescan(void);
extern void updater_set_result(const char *version, const char *note, const char *url);

/* Cache directory set by nativeSetCacheDir() during Activity.onCreate(). */
static char g_cache_dir[512] = "/data/local/tmp";

/* Detect machine type from filename extension. */
static int detect_machine_type(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".a78") == 0) return 1; /* MACHINE_7800 */
    return 0;                                   /* MACHINE_2600 */
}

/* ---- JNI exports ---- */

/*
 * Called from EMU7800Activity.java during onCreate() to give native code
 * a writable path for cached ROMs.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeSetCacheDir(
    JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (path) {
        strncpy(g_cache_dir, path, sizeof(g_cache_dir) - 1);
        g_cache_dir[sizeof(g_cache_dir) - 1] = '\0';
        LOGI("cache dir set: %s", g_cache_dir);
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    }
}

/*
 * Called from EMU7800Activity.java during onCreate() to give native code
 * a persistent writable path for settings, recent list, and last ROM.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeSetDataDir(
    JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (path) {
        LOGI("data dir set: %s", path);
        filepicker_set_data_dir(path);
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    }
}

/*
 * Called from EMU7800Activity.java when the SAF picker delivers a ROM.
 * Used as a fallback / alternative entry point; the primary path goes
 * through the built-in filepicker which accesses the filesystem directly.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeOnRomSelected(
    JNIEnv *env, jclass cls, jint fdInt, jstring jfilename)
{
    (void)cls;

    if (fdInt < 0) {
        LOGW("nativeOnRomSelected: picker cancelled");
        return;
    }

    const char *filename = (*env)->GetStringUTFChars(env, jfilename, NULL);
    int machine_type = detect_machine_type(filename ? filename : "rom.bin");
    LOGI("ROM selected: %s (type=%d)", filename ? filename : "(null)", machine_type);

    char cache_path[600];
    snprintf(cache_path, sizeof(cache_path), "%s/current.rom", g_cache_dir);

    int src_fd = dup((int)fdInt);
    if (src_fd < 0) {
        LOGE("nativeOnRomSelected: dup() failed");
        if (filename) (*env)->ReleaseStringUTFChars(env, jfilename, filename);
        return;
    }

    FILE *src = fdopen(src_fd, "rb");
    FILE *dst = fopen(cache_path, "wb");
    int ok = 0;
    if (src && dst) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
            fwrite(buf, 1, n, dst);
        ok = 1;
    }
    if (src) fclose(src); else close(src_fd);
    if (dst) fclose(dst);

    if (filename) (*env)->ReleaseStringUTFChars(env, jfilename, filename);

    if (ok) {
        LOGI("ROM cached to: %s", cache_path);
        app_load_rom(cache_path, machine_type);
    } else {
        LOGE("nativeOnRomSelected: failed to cache ROM to %s", cache_path);
    }
}

/*
 * Called from EMU7800Activity.java with the display density so that the
 * filepicker can size its bitmap font to match the system's 16sp body text.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeSetDisplayDensity(
    JNIEnv *env, jclass cls, jfloat density)
{
    (void)env;
    (void)cls;
    LOGI("display density: %.2f", (double)density);
    filepicker_set_display_density((float)density);
    input_set_display_density((float)density);
}

/*
 * Called from EMU7800Activity.java with the result of DisplayCutout detection.
 * When has_cutout is 0 the "Use Notch Area" settings row is greyed out.
 * Re-called whenever the display changes (e.g. foldable open/close).
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeSetHasCutout(
    JNIEnv *env, jclass cls, jboolean hasCutout)
{
    (void)env;
    (void)cls;
    LOGI("nativeSetHasCutout: %d", (int)hasCutout);
    filepicker_set_has_cutout((int)hasCutout);
}

/*
 * Called from EMU7800Activity.java after the user grants MANAGE_EXTERNAL_STORAGE
 * ("All files access") via the in-app permission dialog.  SDL is already running;
 * this just triggers a fresh directory scan now that the FUSE layer will return
 * ROM files.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeRescan(
    JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    LOGI("nativeRescan: permission granted, rescanning");
    filepicker_rescan();
}

/*
 * Called from filepicker.c when the user taps the EMAIL button in Settings.
 * Launches the device email app via Java's sendBugReportEmail().
 */
void jni_send_bug_report_email(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) { LOGE("jni_send_bug_report_email: no JNIEnv"); return; }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) { LOGE("jni_send_bug_report_email: no activity"); return; }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        (*env)->DeleteLocalRef(env, activity);
        LOGE("jni_send_bug_report_email: GetObjectClass failed");
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "sendBugReportEmail", "()V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid);
    } else {
        LOGE("jni_send_bug_report_email: sendBugReportEmail method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from filepicker.c when the user presses Back while already at the
 * filesystem root.  Java implements the standard double-back-to-exit pattern:
 * first press shows a Toast, second press within 2 seconds calls finish().
 */
void jni_filepicker_at_root(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) { LOGE("jni_filepicker_at_root: no JNIEnv"); return; }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) { LOGE("jni_filepicker_at_root: no activity"); return; }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        (*env)->DeleteLocalRef(env, activity);
        LOGE("jni_filepicker_at_root: GetObjectClass failed");
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "onFilepickerAtRoot", "()V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid);
    } else {
        LOGE("jni_filepicker_at_root: onFilepickerAtRoot method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from filepicker.c when the user toggles "Use Notch Area" in Settings,
 * and once at startup when settings are loaded.
 * Asks Java to extend (or not extend) the window into the display cutout area.
 */
void jni_set_cutout_mode(int enabled)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) { LOGE("jni_set_cutout_mode: no JNIEnv"); return; }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) { LOGE("jni_set_cutout_mode: no activity"); return; }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        (*env)->DeleteLocalRef(env, activity);
        LOGE("jni_set_cutout_mode: GetObjectClass failed");
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "setCutoutMode", "(Z)V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid, (jboolean)(enabled != 0));
    } else {
        LOGE("jni_set_cutout_mode: setCutoutMode method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from main.c (SDL main thread) as a fallback ROM picker.
 * Launches the SAF document picker via Java.
 */
void jni_open_rom_picker(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) { LOGE("jni_open_rom_picker: no JNIEnv"); return; }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) { LOGE("jni_open_rom_picker: no activity"); return; }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        (*env)->DeleteLocalRef(env, activity);
        LOGE("jni_open_rom_picker: GetObjectClass failed");
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "openRomPicker", "()V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid);
    } else {
        LOGE("jni_open_rom_picker: openRomPicker method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from updater.c to ask Java to start the background GitHub update check.
 */
void jni_start_update_check(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) { LOGE("jni_start_update_check: no JNIEnv"); return; }

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) { LOGE("jni_start_update_check: no activity"); return; }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        (*env)->DeleteLocalRef(env, activity);
        LOGE("jni_start_update_check: GetObjectClass failed");
        return;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "startUpdateCheck", "()V");
    if (mid) {
        (*env)->CallStaticVoidMethod(env, cls, mid);
    } else {
        LOGE("jni_start_update_check: startUpdateCheck method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from filepicker.c when the user taps "UPDATE" in the update popup.
 * Asks Java to download the APK to the cache dir and launch the system installer.
 */
void jni_download_and_install_apk(const char *url)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !url || !url[0]) return;

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) return;

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) { (*env)->DeleteLocalRef(env, activity); return; }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "downloadAndInstallApk", "(Ljava/lang/String;)V");
    if (mid) {
        jstring jurl = (*env)->NewStringUTF(env, url);
        if (jurl) {
            (*env)->CallStaticVoidMethod(env, cls, mid, jurl);
            (*env)->DeleteLocalRef(env, jurl);
        }
    } else {
        LOGE("jni_download_and_install_apk: downloadAndInstallApk method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * Called from updater.c or the SDL main thread to open a URL in the browser.
 */
void jni_open_url(const char *url)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !url || !url[0]) return;

    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) return;

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) { (*env)->DeleteLocalRef(env, activity); return; }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "openUrl", "(Ljava/lang/String;)V");
    if (mid) {
        jstring jurl = (*env)->NewStringUTF(env, url);
        if (jurl) {
            (*env)->CallStaticVoidMethod(env, cls, mid, jurl);
            (*env)->DeleteLocalRef(env, jurl);
        }
    } else {
        LOGE("jni_open_url: openUrl method not found");
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/*
 * JNI export: called from Java background thread when a newer release is confirmed.
 * Stores the result in updater.c state and posts the update dialog to the UI thread.
 */
JNIEXPORT void JNICALL
Java_com_emu7800_android_EMU7800Activity_nativeUpdateResult(
    JNIEnv *env, jclass cls, jstring jversion, jstring jnote, jstring jurl)
{
    (void)cls;

    const char *version = (*env)->GetStringUTFChars(env, jversion, NULL);
    const char *note    = jnote ? (*env)->GetStringUTFChars(env, jnote, NULL) : "";
    const char *url     = jurl  ? (*env)->GetStringUTFChars(env, jurl,  NULL) : "";

    LOGI("nativeUpdateResult: version=%s", version ? version : "(null)");

    if (version && version[0]) {
        updater_set_result(version, note, url);
        /* UI is driven by the C filepicker — no Java dialog needed */
    }

    if (version) (*env)->ReleaseStringUTFChars(env, jversion, version);
    if (note && jnote) (*env)->ReleaseStringUTFChars(env, jnote, note);
    if (url  && jurl)  (*env)->ReleaseStringUTFChars(env, jurl,  url);
}

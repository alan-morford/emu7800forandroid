/*
 * updater.h
 *
 * Silent update checker against GitHub Releases.
 * A Java background thread (started via jni_start_update_check) performs the
 * HTTPS GET; nativeUpdateResult() feeds the result back into C state.
 * Main thread polls updater_has_update() and the AlertDialog is shown on the
 * Java UI thread automatically when a newer release is found.
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef UPDATER_H
#define UPDATER_H

/* Start the background update check.  Idempotent — safe to call multiple times. */
void updater_check_start(void);

/* Returns 1 if a newer version was found and not yet dismissed this session. */
int updater_has_update(void);

/* Version string of the latest release, e.g. "1.0.2".
 * Valid only when updater_has_update() returns 1. */
const char *updater_get_version(void);

/* Release notes body text.  Valid only when updater_has_update() returns 1. */
const char *updater_get_note(void);

/* APK asset browser_download_url (or html_url fallback when no .apk asset).
 * Valid only when updater_has_update() returns 1. */
const char *updater_get_url(void);

/* Suppress the update notification for the rest of this session. */
void updater_dismiss(void);

/* Called from jni_bridge.c when Java confirms a newer release is available. */
void updater_set_result(const char *version, const char *note, const char *url);

#endif /* UPDATER_H */

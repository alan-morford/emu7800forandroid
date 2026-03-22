/*
 * log_android.c
 *
 * Redirects log_msg() to Android logcat.
 * Satisfies the extern log_msg() link dependency required by core files.
 *
 * Copyright (c) 2024 EMU7800
 */

#include <android/log.h>

void log_msg(const char *msg)
{
    __android_log_print(ANDROID_LOG_DEBUG, "EMU7800", "%s", msg);
}

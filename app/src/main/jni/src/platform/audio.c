/*
 * audio.c
 *
 * SDL2 Audio Output — Android port
 * Ring-buffer SPSC queue feeds the SDL2 audio callback.
 * SDL_OpenAudio -> SDL_OpenAudioDevice (only API difference from webOS).
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <SDL.h>
#include "audio.h"
#include "machine.h"

/* TIA generates audio at 31440 Hz for NTSC (3.58 MHz / 114).
 * Android AAudio/OpenSLES prefers small buffers for low latency. */
#define AUDIO_SAMPLE_RATE     31440
#define AUDIO_BUFFER_SAMPLES  256    /* ~8ms — low latency on Android */
#define AUDIO_CHANNELS        1

/* Ring buffer: 4096 samples @ 31440 Hz ≈ 130ms (≈8 frames headroom). */
#define RING_SIZE  4096              /* must be power of 2 */
#define RING_MASK  (RING_SIZE - 1)

static int16_t  g_ring[RING_SIZE];
static volatile int g_ring_read  = 0;
static volatile int g_ring_write = 0;

static SDL_AudioDeviceID g_audio_dev   = 0;
static int               g_actual_rate = AUDIO_SAMPLE_RATE;

/* SDL audio callback — runs on SDL's audio thread. */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    int n = len / (int)sizeof(int16_t);
    (void)userdata;

    for (int i = 0; i < n; i++) {
        int r = g_ring_read;
        int w = g_ring_write;
        if (r != w) {
            out[i] = g_ring[r];
            g_ring_read = (r + 1) & RING_MASK;
        } else {
            out[i] = 0;  /* underrun — output silence */
        }
    }
}

int audio_init(void)
{
    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    want.freq     = AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples  = AUDIO_BUFFER_SAMPLES;
    want.callback = audio_callback;
    want.userdata = NULL;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (g_audio_dev == 0) {
        return -1;
    }

    g_actual_rate = got.freq;
    g_ring_read   = 0;
    g_ring_write  = 0;

    SDL_PauseAudioDevice(g_audio_dev, 0);  /* start playback */
    return 0;
}

void audio_shutdown(void)
{
    if (g_audio_dev > 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
}

/*
 * Push the current frame's samples into the ring buffer.
 * Called from the emulator thread after machine_run_frame().
 * Lock-free SPSC: this is the sole producer; audio_callback is sole consumer.
 */
void audio_update(void)
{
    if (g_audio_dev == 0) return;

    int16_t *buf = machine_get_sound_buffer();
    int      n   = machine_get_sound_samples();
    if (!buf || n <= 0) return;

    /* Snapshot read position once — avoids repeated volatile reads. */
    int read_pos = g_ring_read;

    for (int i = 0; i < n; i++) {
        int next = (g_ring_write + 1) & RING_MASK;
        if (next != read_pos) {
            g_ring[g_ring_write] = buf[i];
            g_ring_write = next;
        }
        /* Buffer full: drop samples silently to keep timing stable. */
    }
}

void audio_pause(int pause)
{
    if (g_audio_dev > 0)
        SDL_PauseAudioDevice(g_audio_dev, pause);
}

int audio_get_sample_rate(void)
{
    return g_actual_rate;
}

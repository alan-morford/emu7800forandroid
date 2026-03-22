/*
 * audio.h
 *
 * SDL2 Audio Output Header — Android port
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef AUDIO_H
#define AUDIO_H

/* Initialize audio subsystem. Returns 0 on success, -1 on failure. */
int audio_init(void);

/* Shutdown audio subsystem. */
void audio_shutdown(void);

/* Push the current frame's audio samples into the ring buffer.
 * Called from the emulator thread after machine_run_frame(). */
void audio_update(void);

/* Pause (pause=1) or resume (pause=0) audio output. */
void audio_pause(int pause);

/* Return the actual sample rate negotiated by SDL. */
int audio_get_sample_rate(void);

#endif /* AUDIO_H */

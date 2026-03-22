/*
 * tiasound.c
 *
 * TIA Sound Emulator
 * Based on TIASound by Ron Fries
 *
 * Copyright (c) 1997 Ron Fries
 * Copyright (c) 2003-2004 Mike Murphy
 * C port Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdlib.h>
#include "tiasound.h"
#include "savestate.h"

/* TIA sound register addresses */
#define AUDC0   0x15
#define AUDC1   0x16
#define AUDF0   0x17
#define AUDF1   0x18
#define AUDV0   0x19
#define AUDV1   0x1A

/* Sound constants */
#define SET_TO_1    0x00
#define POLY9       0x08

/* Audio buffer size */
#define AUDIO_BUFFER_SIZE 2048

/*
 * TIA audio clock rate: color_clock / 114 = 3,579,545 / 114 = 31,399.5 Hz
 * Reference EMU7800 uses CPU_TICKS_PER_AUDIO_SAMPLE = 38, giving the same rate.
 * We decouple the divider tick rate from the output sample rate using a
 * fixed-point phase accumulator.
 */
#define TIA_AUDIO_CLOCK_HZ  31400
#define PHASE_FP_SHIFT      16
#define PHASE_FP_ONE        (1 << PHASE_FP_SHIFT)

/* State */
static int g_sample_rate = 44100;
static int16_t g_audio_buffer[AUDIO_BUFFER_SIZE];
static int g_buffer_index = 0;
static uint32_t g_phase_accum = 0;
static uint32_t g_phase_inc = 0;  /* fixed-point 16.16: TIA_AUDIO_CLOCK / sample_rate */

/* 4-bit poly pattern */
static const uint8_t bit4[15] = {1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0};

/* 5-bit poly pattern */
static const uint8_t bit5[31] = {
    0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0,
    0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1
};

/* Divide by 31 pattern */
static const uint8_t div31[31] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* 9-bit poly (random) */
static uint8_t bit9[511];

/* Channel state */
static uint8_t audc[2];
static uint8_t audf[2];
static uint8_t audv[2];

static int p4[2];
static int p5[2];
static int p9[2];

static int div_n_counter[2];
static int div_n_maximum[2];

static uint8_t output_vol[2];

/* Initialize TIA sound */
void tiasound_init(int sample_rate)
{
    int i;

    g_sample_rate = sample_rate;
    g_buffer_index = 0;
    g_phase_accum = 0;
    g_phase_inc = (uint32_t)((double)TIA_AUDIO_CLOCK_HZ / sample_rate * PHASE_FP_ONE);

    /* Initialize random 9-bit poly */
    srand(12345);
    for (i = 0; i < 511; i++) {
        bit9[i] = rand() & 0x01;
    }

    tiasound_reset();
}

/* Reset TIA sound */
void tiasound_reset(void)
{
    int chan;

    for (chan = 0; chan < 2; chan++) {
        output_vol[chan] = 0;
        div_n_counter[chan] = 0;
        div_n_maximum[chan] = 0;
        audc[chan] = 0;
        audf[chan] = 0;
        audv[chan] = 0;
        p4[chan] = 0;
        p5[chan] = 0;
        p9[chan] = 0;
    }

    g_buffer_index = 0;
    g_phase_accum = 0;
    memset(g_audio_buffer, 0, sizeof(g_audio_buffer));
}

/* TIA clocks per frame: 228 clocks/scanline * 262 scanlines */
#define TIA_CLOCKS_PER_FRAME (228 * 262)

/* Start a new frame */
void tiasound_start_frame(void)
{
    g_buffer_index = 0;
}

/*
 * Render audio samples up to a proportional position within the frame.
 * Called BEFORE a register change so the old values produce sound up to
 * the correct point in time. This makes mid-frame frequency/volume
 * changes audible at the right moment instead of only at frame end.
 */
void tiasound_render_to_position(int tia_clocks_into_frame)
{
    int samples_per_frame = g_sample_rate / 60;
    int target_sample;

    if (tia_clocks_into_frame <= 0) return;
    if (tia_clocks_into_frame > TIA_CLOCKS_PER_FRAME)
        tia_clocks_into_frame = TIA_CLOCKS_PER_FRAME;

    target_sample = (int)((long)tia_clocks_into_frame * samples_per_frame / TIA_CLOCKS_PER_FRAME);
    if (target_sample > samples_per_frame) target_sample = samples_per_frame;
    if (target_sample > AUDIO_BUFFER_SIZE) target_sample = AUDIO_BUFFER_SIZE;

    while (g_buffer_index < target_sample) {
        tiasound_render(&g_audio_buffer[g_buffer_index], 1);
        g_buffer_index++;
    }
}

/* End the current frame */
void tiasound_end_frame(void)
{
    /* Fill remainder of buffer with current sound state */
    int samples_per_frame = g_sample_rate / 60;
    while (g_buffer_index < samples_per_frame && g_buffer_index < AUDIO_BUFFER_SIZE) {
        tiasound_render(&g_audio_buffer[g_buffer_index], 1);
        g_buffer_index++;
    }
}

/* Process a channel */
static void process_channel(int chan)
{
    /* Increment P5 counter */
    if (++p5[chan] >= 31) {
        p5[chan] = 0;
    }

    /* Check clock modifier for clock tick */
    if ((audc[chan] & 0x02) == 0 ||
        ((audc[chan] & 0x01) == 0 && div31[p5[chan]] == 1) ||
        ((audc[chan] & 0x01) == 1 && bit5[p5[chan]] == 1)) {

        if ((audc[chan] & 0x04) != 0) {
            /* Pure modified clock */
            output_vol[chan] = (output_vol[chan] != 0) ? 0 : audv[chan];
        } else if ((audc[chan] & 0x08) != 0) {
            /* Poly5 or Poly9 */
            if (audc[chan] == POLY9) {
                if (++p9[chan] >= 511) {
                    p9[chan] = 0;
                }
                output_vol[chan] = (bit9[p9[chan]] == 1) ? audv[chan] : 0;
            } else {
                output_vol[chan] = (bit5[p5[chan]] == 1) ? audv[chan] : 0;
            }
        } else {
            /* Poly4 */
            if (++p4[chan] >= 15) {
                p4[chan] = 0;
            }
            output_vol[chan] = (bit4[p4[chan]] == 1) ? audv[chan] : 0;
        }
    }
}

/* Tick divider for one channel */
static void tick_channel(int chan)
{
    if (div_n_counter[chan] > 1) {
        div_n_counter[chan]--;
    } else if (div_n_counter[chan] == 1) {
        div_n_counter[chan] = div_n_maximum[chan];
        process_channel(chan);
    }
}

/*
 * Render audio samples.
 * The phase accumulator ensures dividers tick at ~31,400 Hz (TIA audio clock)
 * regardless of the output sample rate (e.g. 44,100 Hz).
 */
void tiasound_render(int16_t *buffer, int num_samples)
{
    int i;

    for (i = 0; i < num_samples; i++) {
        g_phase_accum += g_phase_inc;
        while (g_phase_accum >= PHASE_FP_ONE) {
            g_phase_accum -= PHASE_FP_ONE;
            tick_channel(0);
            tick_channel(1);
        }

        /* Mix and output */
        buffer[i] = (int16_t)((output_vol[0] + output_vol[1]) * 1024);
    }
}

/* Update TIA sound register */
void tiasound_update(uint16_t addr, uint8_t data)
{
    int chan;
    uint8_t new_div_max;

    switch (addr) {
        case AUDC0: audc[0] = data & 0x0F; chan = 0; break;
        case AUDC1: audc[1] = data & 0x0F; chan = 1; break;
        case AUDF0: audf[0] = data & 0x1F; chan = 0; break;
        case AUDF1: audf[1] = data & 0x1F; chan = 1; break;
        case AUDV0: audv[0] = data & 0x0F; chan = 0; break;
        case AUDV1: audv[1] = data & 0x0F; chan = 1; break;
        default: return;
    }

    if (audc[chan] == SET_TO_1) {
        new_div_max = 0;
        output_vol[chan] = audv[chan];
    } else {
        new_div_max = audf[chan] + 1;
        if ((audc[chan] & 0x0C) == 0x0C) {
            new_div_max *= 3;
        }
    }

    if (new_div_max != div_n_maximum[chan]) {
        div_n_maximum[chan] = new_div_max;
        if (div_n_counter[chan] == 0 || new_div_max == 0) {
            div_n_counter[chan] = new_div_max;
        }
    }
}

/* Get the sound buffer */
int16_t *tiasound_get_buffer(void)
{
    return g_audio_buffer;
}

/* Get the number of samples in the buffer */
int tiasound_get_buffer_samples(void)
{
    return g_buffer_index;
}

/* Save state: get internal state */
void tiasound_get_state(TiaSoundState *s)
{
    int i;
    for (i = 0; i < 2; i++) {
        s->audc[i] = audc[i];
        s->audf[i] = audf[i];
        s->audv[i] = audv[i];
        s->p4[i] = p4[i];
        s->p5[i] = p5[i];
        s->p9[i] = p9[i];
        s->div_n_counter[i] = div_n_counter[i];
        s->div_n_maximum[i] = div_n_maximum[i];
        s->output_vol[i] = output_vol[i];
    }
    s->phase_accum = g_phase_accum;
    s->buffer_index = g_buffer_index;
}

/* Save state: set internal state */
void tiasound_set_state(const TiaSoundState *s)
{
    int i;
    for (i = 0; i < 2; i++) {
        audc[i] = s->audc[i];
        audf[i] = s->audf[i];
        audv[i] = s->audv[i];
        p4[i] = s->p4[i];
        p5[i] = s->p5[i];
        p9[i] = s->p9[i];
        div_n_counter[i] = s->div_n_counter[i];
        div_n_maximum[i] = s->div_n_maximum[i];
        output_vol[i] = s->output_vol[i];
    }
    g_phase_accum = s->phase_accum;
    g_buffer_index = s->buffer_index;
}

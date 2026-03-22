/*
 * savestate.c
 *
 * Save/Load State Implementation
 * Binary save/load of full emulation state.
 *
 * File format:
 *   Header (16 bytes): magic "E78S", version(4B), machine_type(4B), reserved(4B)
 *   Sections (repeated): tag(4B), length(4B), data(NB)
 *   Tags: "CPU\0", "PIA\0", "CART", "MACH", "TIA\0", "MRIA", "TSND",
 *         "RM26", "RM07", "RM17", "CRAM", "SCST", "DPCS"
 *
 * Copyright (c) 2026 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "savestate.h"
#include "machine.h"
#include "m6502.h"
#include "tia.h"
#include "tiasound.h"
#include "pia.h"
#include "cart.h"
#include "maria.h"

/* External logging function */
extern void log_msg(const char *msg);

#define SAVE_MAGIC   "E78S"
#define SAVE_VERSION 1

/* --- File I/O helpers --- */

static int write_u32(FILE *f, uint32_t v)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
    return fwrite(buf, 1, 4, f) == 4 ? 0 : -1;
}

static int read_u32(FILE *f, uint32_t *v)
{
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return -1;
    *v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return 0;
}

static int write_u64(FILE *f, uint64_t v)
{
    if (write_u32(f, (uint32_t)(v & 0xFFFFFFFF)) != 0) return -1;
    return write_u32(f, (uint32_t)(v >> 32));
}

static int read_u64(FILE *f, uint64_t *v)
{
    uint32_t lo, hi;
    if (read_u32(f, &lo) != 0) return -1;
    if (read_u32(f, &hi) != 0) return -1;
    *v = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

static int write_section_header(FILE *f, const char *tag, uint32_t len)
{
    if (fwrite(tag, 1, 4, f) != 4) return -1;
    return write_u32(f, len);
}

static int read_section_header(FILE *f, char *tag, uint32_t *len)
{
    if (fread(tag, 1, 4, f) != 4) return -1;
    return read_u32(f, len);
}

/* --- Build save path from ROM path --- */
static void build_save_path(const char *rom_path, char *save_path, int save_path_size)
{
    const char *dot;
    int base_len;

    dot = strrchr(rom_path, '.');
    if (dot) {
        base_len = (int)(dot - rom_path);
    } else {
        base_len = (int)strlen(rom_path);
    }

    if (base_len + 5 >= save_path_size) {
        base_len = save_path_size - 5;
    }

    memcpy(save_path, rom_path, base_len);
    memcpy(save_path + base_len, ".sav", 4);
    save_path[base_len + 4] = '\0';
}

/* --- Check if save file exists --- */

int savestate_exists(const char *rom_path)
{
    char save_path[512];
    FILE *f;

    build_save_path(rom_path, save_path, sizeof(save_path));
    f = fopen(save_path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* --- Save --- */

int savestate_save(const char *rom_path)
{
    char save_path[512];
    char msg[256];
    FILE *f;
    int machine_type;
    M6502 *cpu;
    TIA *tia;
    PIA *pia;
    Cart *cart;
    Maria *maria;
    TiaSoundState tsnd;
    int nmi_delay_frames;
    int frame_count_7800, frame_count_2600;
    uint32_t distinct_accesses;

    build_save_path(rom_path, save_path, sizeof(save_path));

    snprintf(msg, sizeof(msg), "SAVE: Writing to %s", save_path);
    log_msg(msg);

    f = fopen(save_path, "wb");
    if (!f) {
        snprintf(msg, sizeof(msg), "SAVE: Failed to open %s for writing", save_path);
        log_msg(msg);
        return -1;
    }

    machine_type = machine_get_type();
    cpu = machine_get_cpu();
    tia = machine_get_tia();
    pia = machine_get_pia();
    cart = machine_get_cart();
    maria = machine_get_maria();

    /* Header */
    fwrite(SAVE_MAGIC, 1, 4, f);
    write_u32(f, SAVE_VERSION);
    write_u32(f, (uint32_t)machine_type);
    write_u32(f, 0); /* reserved */

    /* CPU section */
    {
        /* clock(8) + run_clocks(4) + run_clocks_multiple(4) + jammed(4) +
           irq(4) + nmi(4) + PC(2) + A(1) + X(1) + Y(1) + S(1) + P(1) = 35 */
        uint32_t len = 35;
        write_section_header(f, "CPU\0", len);
        write_u64(f, cpu->clock);
        write_u32(f, (uint32_t)cpu->run_clocks);
        write_u32(f, (uint32_t)cpu->run_clocks_multiple);
        write_u32(f, (uint32_t)cpu->jammed);
        write_u32(f, (uint32_t)cpu->irq_interrupt_request);
        write_u32(f, (uint32_t)cpu->nmi_interrupt_request);
        fwrite(&cpu->PC, 1, 2, f); /* little-endian on ARM */
        fwrite(&cpu->A, 1, 1, f);
        fwrite(&cpu->X, 1, 1, f);
        fwrite(&cpu->Y, 1, 1, f);
        fwrite(&cpu->S, 1, 1, f);
        fwrite(&cpu->P, 1, 1, f);
    }

    /* PIA section */
    {
        /* ram(128) + timer_target(8) + timer_shift(4) + irq_enabled(4) +
           irq_triggered(4) + ddra(1) + ddrb(1) + written_port_a(1) + written_port_b(1) = 152 */
        uint32_t len = 152;
        write_section_header(f, "PIA\0", len);
        fwrite(pia->ram, 1, 0x80, f);
        write_u64(f, pia->timer_target);
        write_u32(f, (uint32_t)pia->timer_shift);
        write_u32(f, (uint32_t)pia->irq_enabled);
        write_u32(f, (uint32_t)pia->irq_triggered);
        fwrite(&pia->ddra, 1, 1, f);
        fwrite(&pia->ddrb, 1, 1, f);
        fwrite(&pia->written_port_a, 1, 1, f);
        fwrite(&pia->written_port_b, 1, 1, f);
    }

    /* Cart section */
    {
        /* type(4) + bank(4) + bank_count(4) + banks[4](16) = 28 */
        uint32_t len = 28;
        write_section_header(f, "CART", len);
        write_u32(f, (uint32_t)cart->type);
        write_u32(f, (uint32_t)cart->bank);
        write_u32(f, (uint32_t)cart->bank_count);
        write_u32(f, (uint32_t)cart->banks[0]);
        write_u32(f, (uint32_t)cart->banks[1]);
        write_u32(f, (uint32_t)cart->banks[2]);
        write_u32(f, (uint32_t)cart->banks[3]);
    }

    /* Machine flags section */
    {
        /* 4 x 4 bytes = 16 */
        uint32_t len = 16;
        machine_get_state_flags(&nmi_delay_frames, &frame_count_7800,
                                &frame_count_2600, &distinct_accesses);
        write_section_header(f, "MACH", len);
        write_u32(f, (uint32_t)nmi_delay_frames);
        write_u32(f, (uint32_t)frame_count_7800);
        write_u32(f, (uint32_t)frame_count_2600);
        write_u32(f, distinct_accesses);
    }

    /* TIA Sound section */
    {
        tiasound_get_state(&tsnd);
        /* 2+2+2 + 4*2*6 + 2 + 4 + 4 = 62 */
        uint32_t len = 62;
        write_section_header(f, "TSND", len);
        fwrite(tsnd.audc, 1, 2, f);
        fwrite(tsnd.audf, 1, 2, f);
        fwrite(tsnd.audv, 1, 2, f);
        write_u32(f, (uint32_t)tsnd.p4[0]); write_u32(f, (uint32_t)tsnd.p4[1]);
        write_u32(f, (uint32_t)tsnd.p5[0]); write_u32(f, (uint32_t)tsnd.p5[1]);
        write_u32(f, (uint32_t)tsnd.p9[0]); write_u32(f, (uint32_t)tsnd.p9[1]);
        write_u32(f, (uint32_t)tsnd.div_n_counter[0]); write_u32(f, (uint32_t)tsnd.div_n_counter[1]);
        write_u32(f, (uint32_t)tsnd.div_n_maximum[0]); write_u32(f, (uint32_t)tsnd.div_n_maximum[1]);
        fwrite(tsnd.output_vol, 1, 2, f);
        write_u32(f, tsnd.phase_accum);
        write_u32(f, (uint32_t)tsnd.buffer_index);
    }

    if (machine_type == MACHINE_2600) {
        /* TIA section - write entire TIA struct */
        {
            uint32_t len = sizeof(TIA);
            write_section_header(f, "TIA\0", len);
            fwrite(tia, 1, sizeof(TIA), f);
        }

        /* 2600 RAM section */
        {
            write_section_header(f, "RM26", 0x80);
            fwrite(machine_get_ram_2600(), 1, 0x80, f);
        }

        /* TIA vblank off scanline */
        {
            int vbl = tia_get_vblank_off_scanline();
            write_section_header(f, "TVBL", 4);
            write_u32(f, (uint32_t)vbl);
        }

        /* Supercharger state (if present) */
        if (cart->sc) {
            SCState *sc = cart->sc;
            /* image(8192) + header(256) + image_offset[2](8) + num_loads(1) +
               write_enabled(1) + power(1) + data_hold(1) + distinct_set(4) +
               write_pending(1) = 8464 */
            uint32_t len = 8464;
            write_section_header(f, "SCST", len);
            fwrite(sc->image, 1, 8192, f);
            fwrite(sc->header, 1, 256, f);
            write_u32(f, sc->image_offset[0]);
            write_u32(f, sc->image_offset[1]);
            fwrite(&sc->num_loads, 1, 1, f);
            fwrite(&sc->write_enabled, 1, 1, f);
            fwrite(&sc->power, 1, 1, f);
            fwrite(&sc->data_hold, 1, 1, f);
            write_u32(f, sc->distinct_set);
            fwrite(&sc->write_pending, 1, 1, f);
        }

        /* DPC state (if present) */
        if (cart->dpc) {
            DPCState *dpc = cart->dpc;
            /* counters(16) + tops(8) + bots(8) + flags(8) + music_mode(3) +
               shift_register(1) + last_system_clock(8) + fractional_clocks(8) = 60 */
            uint32_t len = 60;
            write_section_header(f, "DPCS", len);
            fwrite(dpc->counters, 1, 16, f);
            fwrite(dpc->tops, 1, 8, f);
            fwrite(dpc->bots, 1, 8, f);
            fwrite(dpc->flags, 1, 8, f);
            fwrite(dpc->music_mode, 1, 3, f);
            fwrite(&dpc->shift_register, 1, 1, f);
            write_u64(f, dpc->last_system_clock);
            write_u64(f, (uint64_t)0); /* fractional_clocks placeholder (written as 0, resynced on load) */
        }

        /* Cart RAM (CBS12K 256B RAM) */
        if (cart->ram && cart->ram_size > 0) {
            write_section_header(f, "CRAM", (uint32_t)cart->ram_size);
            fwrite(cart->ram, 1, cart->ram_size, f);
        }
    } else {
        /* Maria internal state section */
        {
            MariaInternalState mis;
            maria_get_internal_state(&mis);
            write_section_header(f, "MRIA", (uint32_t)sizeof(MariaInternalState));
            fwrite(&mis, 1, sizeof(MariaInternalState), f);
        }

        /* Maria struct (scanline, wsync) */
        {
            write_section_header(f, "MAST", (uint32_t)sizeof(Maria));
            fwrite(maria, 1, sizeof(Maria), f);
        }

        /* 7800 RAM0 */
        {
            write_section_header(f, "RM07", 0x800);
            fwrite(machine_get_ram0_7800(), 1, 0x800, f);
        }

        /* 7800 RAM1 */
        {
            write_section_header(f, "RM17", 0x800);
            fwrite(machine_get_ram1_7800(), 1, 0x800, f);
        }

        /* Cart RAM (SuperGame+RAM) */
        if (cart->ram && cart->ram_size > 0) {
            write_section_header(f, "CRAM", (uint32_t)cart->ram_size);
            fwrite(cart->ram, 1, cart->ram_size, f);
        }
    }

    fclose(f);
    snprintf(msg, sizeof(msg), "SAVE: Complete (%s)", save_path);
    log_msg(msg);
    return 0;
}

/* --- Load --- */

int savestate_load(const char *rom_path)
{
    char save_path[512];
    char msg[256];
    FILE *f;
    char magic[4];
    uint32_t version, saved_type, reserved;
    int machine_type;
    M6502 *cpu;
    TIA *tia;
    PIA *pia;
    Cart *cart;
    Maria *maria;
    char tag[4];
    uint32_t section_len;

    build_save_path(rom_path, save_path, sizeof(save_path));

    snprintf(msg, sizeof(msg), "LOAD: Reading from %s", save_path);
    log_msg(msg);

    f = fopen(save_path, "rb");
    if (!f) {
        snprintf(msg, sizeof(msg), "LOAD: File not found: %s", save_path);
        log_msg(msg);
        return -1;
    }

    /* Read and verify header */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SAVE_MAGIC, 4) != 0) {
        log_msg("LOAD: Invalid magic");
        fclose(f);
        return -2;
    }

    if (read_u32(f, &version) != 0 || version != SAVE_VERSION) {
        snprintf(msg, sizeof(msg), "LOAD: Version mismatch (got %u, want %u)", version, SAVE_VERSION);
        log_msg(msg);
        fclose(f);
        return -3;
    }

    read_u32(f, &saved_type);
    read_u32(f, &reserved);

    machine_type = machine_get_type();
    if ((int)saved_type != machine_type) {
        snprintf(msg, sizeof(msg), "LOAD: Machine type mismatch (save=%u, current=%d)", saved_type, machine_type);
        log_msg(msg);
        fclose(f);
        return -4;
    }

    cpu = machine_get_cpu();
    tia = machine_get_tia();
    pia = machine_get_pia();
    cart = machine_get_cart();
    maria = machine_get_maria();

    /* Read sections */
    while (read_section_header(f, tag, &section_len) == 0) {
        if (memcmp(tag, "CPU\0", 4) == 0) {
            uint32_t tmp;
            uint16_t pc_val;
            read_u64(f, &cpu->clock);
            read_u32(f, &tmp); cpu->run_clocks = (int)tmp;
            read_u32(f, &tmp); cpu->run_clocks_multiple = (int)tmp;
            read_u32(f, &tmp); cpu->jammed = (int)tmp;
            read_u32(f, &tmp); cpu->irq_interrupt_request = (int)tmp;
            read_u32(f, &tmp); cpu->nmi_interrupt_request = (int)tmp;
            fread(&pc_val, 1, 2, f); cpu->PC = pc_val;
            fread(&cpu->A, 1, 1, f);
            fread(&cpu->X, 1, 1, f);
            fread(&cpu->Y, 1, 1, f);
            fread(&cpu->S, 1, 1, f);
            fread(&cpu->P, 1, 1, f);
            cpu->emulator_preempt_request = 0;
        }
        else if (memcmp(tag, "PIA\0", 4) == 0) {
            uint32_t tmp;
            fread(pia->ram, 1, 0x80, f);
            read_u64(f, &pia->timer_target);
            read_u32(f, &tmp); pia->timer_shift = (int)tmp;
            read_u32(f, &tmp); pia->irq_enabled = (int)tmp;
            read_u32(f, &tmp); pia->irq_triggered = (int)tmp;
            fread(&pia->ddra, 1, 1, f);
            fread(&pia->ddrb, 1, 1, f);
            fread(&pia->written_port_a, 1, 1, f);
            fread(&pia->written_port_b, 1, 1, f);
        }
        else if (memcmp(tag, "CART", 4) == 0) {
            uint32_t tmp;
            read_u32(f, &tmp); /* type - verify but don't overwrite (ROM already loaded) */
            read_u32(f, &tmp); cart->bank = (int)tmp;
            read_u32(f, &tmp); cart->bank_count = (int)tmp;
            read_u32(f, &tmp); cart->banks[0] = (int)tmp;
            read_u32(f, &tmp); cart->banks[1] = (int)tmp;
            read_u32(f, &tmp); cart->banks[2] = (int)tmp;
            read_u32(f, &tmp); cart->banks[3] = (int)tmp;
        }
        else if (memcmp(tag, "MACH", 4) == 0) {
            uint32_t nd, f7, f2, da;
            read_u32(f, &nd);
            read_u32(f, &f7); read_u32(f, &f2); read_u32(f, &da);
            machine_set_state_flags((int)nd, (int)f7, (int)f2, da);
        }
        else if (memcmp(tag, "TSND", 4) == 0) {
            TiaSoundState tsnd;
            uint32_t tmp;
            fread(tsnd.audc, 1, 2, f);
            fread(tsnd.audf, 1, 2, f);
            fread(tsnd.audv, 1, 2, f);
            read_u32(f, &tmp); tsnd.p4[0] = (int)tmp;
            read_u32(f, &tmp); tsnd.p4[1] = (int)tmp;
            read_u32(f, &tmp); tsnd.p5[0] = (int)tmp;
            read_u32(f, &tmp); tsnd.p5[1] = (int)tmp;
            read_u32(f, &tmp); tsnd.p9[0] = (int)tmp;
            read_u32(f, &tmp); tsnd.p9[1] = (int)tmp;
            read_u32(f, &tmp); tsnd.div_n_counter[0] = (int)tmp;
            read_u32(f, &tmp); tsnd.div_n_counter[1] = (int)tmp;
            read_u32(f, &tmp); tsnd.div_n_maximum[0] = (int)tmp;
            read_u32(f, &tmp); tsnd.div_n_maximum[1] = (int)tmp;
            fread(tsnd.output_vol, 1, 2, f);
            read_u32(f, &tsnd.phase_accum);
            read_u32(f, &tmp); tsnd.buffer_index = (int)tmp;
            tiasound_set_state(&tsnd);
        }
        else if (memcmp(tag, "TIA\0", 4) == 0) {
            if (section_len == sizeof(TIA)) {
                fread(tia, 1, sizeof(TIA), f);
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else if (memcmp(tag, "RM26", 4) == 0) {
            fread(machine_get_ram_2600(), 1, 0x80, f);
        }
        else if (memcmp(tag, "TVBL", 4) == 0) {
            uint32_t vbl;
            read_u32(f, &vbl);
            tia_set_vblank_off_scanline((int)vbl);
        }
        else if (memcmp(tag, "SCST", 4) == 0) {
            if (cart->sc) {
                SCState *sc = cart->sc;
                fread(sc->image, 1, 8192, f);
                fread(sc->header, 1, 256, f);
                read_u32(f, &sc->image_offset[0]);
                read_u32(f, &sc->image_offset[1]);
                fread(&sc->num_loads, 1, 1, f);
                fread(&sc->write_enabled, 1, 1, f);
                fread(&sc->power, 1, 1, f);
                fread(&sc->data_hold, 1, 1, f);
                read_u32(f, &sc->distinct_set);
                fread(&sc->write_pending, 1, 1, f);
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else if (memcmp(tag, "DPCS", 4) == 0) {
            if (cart->dpc && section_len == 60) {
                DPCState *dpc = cart->dpc;
                uint64_t dummy_u64;
                fread(dpc->counters, 1, 16, f);
                fread(dpc->tops, 1, 8, f);
                fread(dpc->bots, 1, 8, f);
                fread(dpc->flags, 1, 8, f);
                fread(dpc->music_mode, 1, 3, f);
                fread(&dpc->shift_register, 1, 1, f);
                read_u64(f, &dpc->last_system_clock);
                read_u64(f, &dummy_u64); /* fractional_clocks placeholder */
                dpc->fractional_clocks = 0.0;
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else if (memcmp(tag, "MRIA", 4) == 0) {
            if (section_len == sizeof(MariaInternalState)) {
                MariaInternalState mis;
                fread(&mis, 1, sizeof(MariaInternalState), f);
                maria_set_internal_state(&mis);
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else if (memcmp(tag, "MAST", 4) == 0) {
            if (section_len == sizeof(Maria)) {
                fread(maria, 1, sizeof(Maria), f);
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else if (memcmp(tag, "RM07", 4) == 0) {
            fread(machine_get_ram0_7800(), 1, 0x800, f);
        }
        else if (memcmp(tag, "RM17", 4) == 0) {
            fread(machine_get_ram1_7800(), 1, 0x800, f);
        }
        else if (memcmp(tag, "CRAM", 4) == 0) {
            if (cart->ram && (int)section_len <= cart->ram_size) {
                fread(cart->ram, 1, section_len, f);
            } else {
                fseek(f, section_len, SEEK_CUR);
            }
        }
        else {
            /* Unknown section - skip */
            snprintf(msg, sizeof(msg), "LOAD: Unknown section '%.4s' (%u bytes), skipping", tag, section_len);
            log_msg(msg);
            fseek(f, section_len, SEEK_CUR);
        }
    }

    fclose(f);
    snprintf(msg, sizeof(msg), "LOAD: Complete (%s)", save_path);
    log_msg(msg);
    return 0;
}

/* apu.c - audio processing unit (register + timing model).
 *
 * Implements the parts the Blargg dmg_sound tests check: the NRxx register
 * file with correct read masks, NR52 power (off clears registers; DMG still
 * allows length writes while off), the 512 Hz frame sequencer clocked by the
 * falling edge of DIV bit 12, length counters (with the trigger/enable
 * "extra clock" quirk), volume envelopes, and the ch1 frequency sweep.
 * Actual sample generation / audio output is a later (cpal) item.
 */
#include "gb.h"

/* read-OR masks for NR10..NR52 (index = addr - 0xFF10) */
static const u8 REG_OR[0x17] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,   /* NR10-NR14 */
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,   /* FF15, NR21-NR24 */
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,   /* NR30-NR34 */
    0xFF, 0xFF, 0x00, 0x00, 0xBF,   /* FF1F, NR41-NR44 */
    0x00, 0x00, 0x70,               /* NR50, NR51, NR52 */
};

static const u16 LEN_MAX[4] = {64, 64, 256, 64};

static void length_clock(GB *g) {
    Apu *a = &g->apu;
    for (int c = 0; c < 4; c++)
        if (a->length_en[c] && a->length[c] > 0)
            if (--a->length[c] == 0) a->ch_on[c] = false;
}

static u16 sweep_calc(GB *g) {
    Apu *a = &g->apu;
    u16 nf = a->sweep_shadow >> a->sweep_shift;
    nf = a->sweep_dir ? (a->sweep_shadow - nf) : (a->sweep_shadow + nf);
    if (a->sweep_dir) a->sweep_neg_used = true;
    return nf;
}

static void sweep_clock(GB *g) {
    Apu *a = &g->apu;
    if (a->sweep_timer > 0) a->sweep_timer--;
    if (a->sweep_timer != 0) return;
    a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
    if (a->sweep_enabled && a->sweep_period > 0) {
        u16 nf = sweep_calc(g);
        if (nf > 2047) { a->ch_on[0] = false; }
        else if (a->sweep_shift > 0) {
            a->sweep_shadow = nf;
            a->reg[0x03] = nf & 0xFF;
            a->reg[0x04] = (a->reg[0x04] & 0xF8) | ((nf >> 8) & 7);
            if (sweep_calc(g) > 2047) a->ch_on[0] = false;
        }
    }
}

static void env_clock(GB *g) {
    Apu *a = &g->apu;
    for (int c = 0; c < 4; c++) {
        if (c == 2) continue;                /* wave channel has no envelope */
        if (a->env_period[c] == 0) continue;
        if (a->env_timer[c] > 0) a->env_timer[c]--;
        if (a->env_timer[c] == 0) {
            a->env_timer[c] = a->env_period[c];
            if (a->env_dir[c] && a->env_vol[c] < 15) a->env_vol[c]++;
            else if (!a->env_dir[c] && a->env_vol[c] > 0) a->env_vol[c]--;
        }
    }
}

static void fs_step(GB *g) {
    Apu *a = &g->apu;
    a->fs_step = (a->fs_step + 1) & 7;
    switch (a->fs_step) {
        case 0: case 4: length_clock(g); break;
        case 2: case 6: length_clock(g); sweep_clock(g); break;
        case 7: env_clock(g); break;
        default: break;
    }
}

/* ---- audible sample synthesis ----
 * Each channel produces a *bipolar* digital level (~ -15..+15) so the mix is
 * naturally DC-free; the four are panned (NR51) and scaled by the master volume
 * (NR50), then resampled to APU_SAMPLE_RATE into a stereo ring buffer. */
#define CPU_HZ 4194304
static const u8 DUTY[4] = {0x01, 0x83, 0x87, 0x7C};   /* 12.5/25/50/75% */
static const u8 WAVE_SH[4] = {4, 0, 1, 2};            /* NR32: mute/100/50/25% */

static i16 g_samp[8192 * 2];   /* stereo ring (output only; not part of save state) */
static int g_samp_n;

static int ch_output(Apu *a, int c) {
    if (!a->ch_on[c] || !a->ch_dac[c]) return 0;
    switch (c) {
        case 0: case 1: {
            u8 duty = a->reg[c == 0 ? 0x01 : 0x06] >> 6;
            int v = a->env_vol[c];
            return ((DUTY[duty] >> a->sq_step[c]) & 1) ? v : -v;
        }
        case 2: {
            u8 byte = a->wave[a->wv_pos >> 1];
            int nib = (a->wv_pos & 1) ? (byte & 0xF) : (byte >> 4);
            int sh = WAVE_SH[(a->reg[0x0C] >> 5) & 3];
            return (nib >> sh) * 2 - (15 >> sh);     /* bipolar */
        }
        default: {
            int v = a->env_vol[3];
            return (~a->lfsr & 1) ? v : -v;
        }
    }
}

static void synth_tick(GB *g, int t) {
    Apu *a = &g->apu;
    for (int c = 0; c < 2; c++) {                    /* square ch1/ch2 */
        if (!a->ch_on[c]) continue;
        int freq = a->reg[c == 0 ? 0x03 : 0x08] | ((a->reg[c == 0 ? 0x04 : 0x09] & 7) << 8);
        int period = (2048 - freq) * 4;
        a->sq_timer[c] -= t;
        while (a->sq_timer[c] <= 0) { a->sq_timer[c] += period; a->sq_step[c] = (a->sq_step[c] + 1) & 7; }
    }
    if (a->ch_on[2]) {                               /* wave ch3 */
        int freq = a->reg[0x0D] | ((a->reg[0x0E] & 7) << 8);
        int period = (2048 - freq) * 2;
        a->wv_timer -= t;
        while (a->wv_timer <= 0) { a->wv_timer += period; a->wv_pos = (a->wv_pos + 1) & 31; }
    }
    if (a->ch_on[3]) {                               /* noise ch4 */
        u8 nr43 = a->reg[0x12];
        int shift = nr43 >> 4, code = nr43 & 7, width = nr43 & 8;
        int period = (code ? code * 16 : 8) << shift;
        if (shift < 14 && period > 0) {
            a->ns_timer -= t;
            while (a->ns_timer <= 0) {
                a->ns_timer += period;
                int b = (a->lfsr ^ (a->lfsr >> 1)) & 1;
                a->lfsr = (u16)((a->lfsr >> 1) | (b << 14));
                if (width) a->lfsr = (u16)((a->lfsr & ~0x40) | (b << 6));
            }
        }
    }
    a->sample_acc += t * APU_SAMPLE_RATE;            /* exact resample, no drift */
    while (a->sample_acc >= CPU_HZ) {
        a->sample_acc -= CPU_HZ;
        int nr51 = a->reg[0x15], nr50 = a->reg[0x14], l = 0, r = 0;
        for (int c = 0; c < 4; c++) {
            int o = ch_output(a, c);
            if (nr51 & (0x10 << c)) l += o;          /* left  enables (bits 4-7) */
            if (nr51 & (1 << c))    r += o;          /* right enables (bits 0-3) */
        }
        l *= ((nr50 >> 4) & 7) + 1;                  /* master volume 1..8 */
        r *= (nr50 & 7) + 1;
        if (g_samp_n < 8192) {                       /* l,r in -480..480 -> i16 */
            g_samp[g_samp_n * 2]     = (i16)(l * 64);
            g_samp[g_samp_n * 2 + 1] = (i16)(r * 64);
            g_samp_n++;
        }
    }
}

int apu_drain_samples(GB *g, i16 *out, int max_pairs) {
    (void)g;
    int n = g_samp_n < max_pairs ? g_samp_n : max_pairs;
    for (int i = 0; i < n * 2; i++) out[i] = g_samp[i];
    int rem = (g_samp_n - n) * 2;
    for (int i = 0; i < rem; i++) g_samp[i] = g_samp[n * 2 + i];
    g_samp_n -= n;
    return n;
}

void apu_tick(GB *g, int tcycles) {
    Apu *a = &g->apu;
    if (a->power) synth_tick(g, tcycles);
    /* Frame sequencer steps on the falling edge of DIV bit 12 (4.19MHz/8192). */
    bool bit = (g->div_counter & (1 << 12)) != 0;
    if (a->div_bit_prev && !bit && a->power) fs_step(g);
    a->div_bit_prev = bit;
}

/* whether the *next* FS step will clock the length counter */
static bool next_step_clocks_len(Apu *a) {
    /* length clocks on steps 0,2,4,6; next step = (fs_step+1)&7 */
    u8 next = (a->fs_step + 1) & 7;
    return (next & 1) == 0;
}

static void trigger(GB *g, int c) {
    Apu *a = &g->apu;
    a->ch_on[c] = a->ch_dac[c];
    if (a->length[c] == 0) {
        a->length[c] = LEN_MAX[c];
        /* trigger-with-length-enabled extra clock when next step won't clock */
        if (a->length_en[c] && !next_step_clocks_len(a)) a->length[c]--;
    }
    /* envelope reload */
    if (c != 2) {
        u8 nrx2 = a->reg[c == 0 ? 0x02 : c == 1 ? 0x07 : 0x11];
        a->env_vol[c] = nrx2 >> 4;
        a->env_dir[c] = (nrx2 & 0x08) != 0;
        a->env_period[c] = nrx2 & 0x07;
        a->env_timer[c] = a->env_period[c];
    }
    /* reload the synthesis frequency timer (and restart the wave/noise generators) */
    if (c == 0 || c == 1) {
        int freq = a->reg[c == 0 ? 0x03 : 0x08] | ((a->reg[c == 0 ? 0x04 : 0x09] & 7) << 8);
        a->sq_timer[c] = (2048 - freq) * 4;
    } else if (c == 2) {
        int freq = a->reg[0x0D] | ((a->reg[0x0E] & 7) << 8);
        a->wv_timer = (2048 - freq) * 2;
        a->wv_pos = 0;
    } else {
        u8 nr43 = a->reg[0x12];
        a->ns_timer = (nr43 & 7 ? (nr43 & 7) * 16 : 8) << (nr43 >> 4);
        a->lfsr = 0x7FFF;
    }
    if (c == 0) {
        /* sweep init */
        u8 nr10 = a->reg[0x00];
        a->sweep_shadow = a->reg[0x03] | ((a->reg[0x04] & 7) << 8);
        a->sweep_period = (nr10 >> 4) & 7;
        a->sweep_dir = (nr10 & 0x08) != 0;
        a->sweep_shift = nr10 & 7;
        a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
        a->sweep_enabled = (a->sweep_period || a->sweep_shift);
        a->sweep_neg_used = false;
        if (a->sweep_shift && sweep_calc(g) > 2047) a->ch_on[0] = false;
    }
}

void apu_init(GB *g) {
    Apu *a = &g->apu;
    for (int i = 0; i < 0x17; i++) a->reg[i] = 0;
    a->power = true;
    a->fs_step = 0;
    a->div_bit_prev = false;
    for (int c = 0; c < 4; c++) {
        a->ch_on[c] = false; a->ch_dac[c] = false;
        a->length[c] = 0; a->length_en[c] = false;
    }
    /* DMG post-boot register values */
    a->reg[0x00] = 0x80; a->reg[0x01] = 0xBF; a->reg[0x02] = 0xF3; a->reg[0x04] = 0xBF;
    a->reg[0x06] = 0x3F; a->reg[0x07] = 0x00; a->reg[0x09] = 0xBF;
    a->reg[0x0A] = 0x7F; a->reg[0x0B] = 0xFF; a->reg[0x0C] = 0x9F; a->reg[0x0E] = 0xBF;
    a->reg[0x10] = 0xFF; a->reg[0x11] = 0x00; a->reg[0x13] = 0xBF;
    a->reg[0x14] = 0x77; a->reg[0x15] = 0xF3; a->reg[0x16] = 0xF1;
    a->ch_dac[0] = true;  /* NR12=0xF3 -> DAC on */
    a->ch_on[0] = true;   /* channel 1 is active post-boot (NR52 reads 0xF1) */
    /* synthesis state */
    for (int c = 0; c < 2; c++) { a->sq_timer[c] = 0; a->sq_step[c] = 0; }
    a->wv_timer = 0; a->wv_pos = 0;
    a->ns_timer = 0; a->lfsr = 0x7FFF;
    a->sample_acc = 0;
}

u8 apu_read(GB *g, u16 addr) {
    Apu *a = &g->apu;
    if (addr >= 0xFF30 && addr <= 0xFF3F) return a->wave[addr - 0xFF30];
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x17) return 0xFF;
    if (i == 0x16) {   /* NR52 */
        u8 v = (a->power ? 0x80 : 0) | 0x70;
        for (int c = 0; c < 4; c++) if (a->ch_on[c]) v |= (1 << c);
        return v;
    }
    return a->reg[i] | REG_OR[i];
}

void apu_write(GB *g, u16 addr, u8 val) {
    Apu *a = &g->apu;
    if (addr >= 0xFF30 && addr <= 0xFF3F) { a->wave[addr - 0xFF30] = val; return; }
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x17) return;

    if (i == 0x16) {                      /* NR52 power control */
        bool on = (val & 0x80) != 0;
        if (!on && a->power) {
            /* power off: clear all registers + channels (DMG keeps length) */
            for (int r = 0; r < 0x16; r++) a->reg[r] = 0;
            for (int c = 0; c < 4; c++) { a->ch_on[c] = false; a->ch_dac[c] = false; a->length_en[c] = false; }
            a->sweep_enabled = false;
        } else if (on && !a->power) {
            a->fs_step = 0;               /* power on resets the frame sequencer */
        }
        a->power = on;
        return;
    }

    /* While powered off, only the length-load fields of NRx1 are writable (DMG). */
    if (!a->power) {
        switch (i) {
            case 0x01: a->length[0] = LEN_MAX[0] - (val & 0x3F); return;
            case 0x06: a->length[1] = LEN_MAX[1] - (val & 0x3F); return;
            case 0x0B: a->length[2] = LEN_MAX[2] - val; return;
            case 0x10: a->length[3] = LEN_MAX[3] - (val & 0x3F); return;
            default: return;
        }
    }

    a->reg[i] = val;
    switch (i) {
        case 0x01: a->length[0] = LEN_MAX[0] - (val & 0x3F); break;          /* NR11 */
        case 0x06: a->length[1] = LEN_MAX[1] - (val & 0x3F); break;          /* NR21 */
        case 0x0B: a->length[2] = LEN_MAX[2] - val; break;                   /* NR31 */
        case 0x10: a->length[3] = LEN_MAX[3] - (val & 0x3F); break;          /* NR41 */

        case 0x02: case 0x07: case 0x11: {                                   /* NRx2 envelope */
            int c = (i == 0x02) ? 0 : (i == 0x07) ? 1 : 3;
            a->ch_dac[c] = (val & 0xF8) != 0;
            if (!a->ch_dac[c]) a->ch_on[c] = false;
            break;
        }
        case 0x0A:                                                          /* NR30 wave DAC */
            a->ch_dac[2] = (val & 0x80) != 0;
            if (!a->ch_dac[2]) a->ch_on[2] = false;
            break;

        case 0x04: case 0x09: case 0x0E: case 0x13: {                       /* NRx4 */
            int c = (i == 0x04) ? 0 : (i == 0x09) ? 1 : (i == 0x0E) ? 2 : 3;
            bool prev_en = a->length_en[c];
            a->length_en[c] = (val & 0x40) != 0;
            /* enabling length while the next step won't clock it -> extra clock */
            if (!prev_en && a->length_en[c] && !next_step_clocks_len(a) && a->length[c] > 0) {
                if (--a->length[c] == 0 && !(val & 0x80)) a->ch_on[c] = false;
            }
            if (val & 0x80) trigger(g, c);
            break;
        }
        default: break;
    }
}

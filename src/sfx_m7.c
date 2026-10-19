/*
 * Send FX, M7 side: a Juno-style chorus and a warm drive as per-track inserts, and a second delay (Rev Delay) as a
 * send FX.
 *
 * Chorus and drive: each track has its own On switch and Amount (on the track screen's sends page); their sound
 * (Mode, Rate, Depth, Width; Drive, Tone, Level) is set once, on their FX panels. sfx_strip, around the mixer's
 * per-channel call, runs a track's drive and then its chorus on a copy of the track's input and hands the mixer that
 * copy instead, so the track's fader, pan, mute, sends and outputs all come after them. Up to NSLOT tracks at once
 * (the CPU: about 3 % of the M7 per track with both on); a fifth one switched on stays dry until one is switched off.
 *
 * Rev Delay: the mixer sums the channels into bus 12 and their FX1/FX2 sends into bus 13 (the delay) and bus 14 (the
 * reverb); the graph then runs the reverb, the delay and node 0x30, which adds both returns into bus 12. Each channel
 * also has a send to the Rev Delay (FX5): sfx_strip adds the channel's post-fader signal times it into its input.
 * sfx_process runs next, in the reverb's vtable slot (0x0806adac, ahead of hall_process): its output goes into bus 12
 * (Level) and into the delay's and reverb's buses (FX1 Send, FX2 Send) before they run.
 *
 * Params (src/sfx_ids.h) come from the M4 as messages, which sfx_msg takes before the M7's dispatcher. An FX that is
 * off costs nothing.
 *
 * Memory: about 1 MB (delay 2 lines of 2^18 16-bit samples, 5.4 s, per side; no more than build 2's, which booted:
 * the firmware halts at boot when its SDRAM pool runs out, and 2 MB did) from the firmware's SDRAM allocator
 * FUN_080413d0, taken while the graph is built (the delay's constructor call @0x080518d2 is ours), so the pointer,
 * kept in backup SRAM, is fresh on every boot.
 */
#include "cpu_shared.h"
#include "sfx_ids.h"

#define FN(addr) ((addr) | 1u)
typedef unsigned (*process_fn)(void *obj, void *ctx);
typedef void *(*bus_fn)(void *ctx, unsigned port);
typedef unsigned (*frames_fn)(void *bus);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
typedef void (*stereo_fn)(void *bus, float **l, float **r);
typedef void *(*ctor_fn)(void *obj);
typedef uint32_t (*alloc_fn)(uint32_t size);
typedef void (*strip_fn)(void *mixer, void *in, unsigned idx, float *ch, void *main, void *ctx);
typedef void (*dispatch_fn)(void *ctx, void *event);
#define fw_reverb   ((process_fn)FN(HALL_PROCESS))     /* hall_process (src/hall_m7.c), from build.sh */
#define fw_dly_ctor ((ctor_fn)FN(0x08052e30))
#define fw_alloc    ((alloc_fn)FN(0x080413d0))
#define fw_bus      ((bus_fn)FN(0x08053cd4))
#define fw_frames   ((frames_fn)FN(0x0804d4c0))
#define fw_queue    ((queue_fn)FN(0x08053c8c))
#define fw_next_ev  ((event_fn)FN(0x0804e938))
#define fw_stereo   ((stereo_fn)FN(0x0804d598))
#define fw_strip    ((strip_fn)FN(0x0805012c))     /* the mixer's per-channel process (FUN_08050564 calls it) */
#define fw_dispatch ((dispatch_fn)FN(0x08052444))  /* one incoming message into its node's queue and the master queue (0xc) */
#define HEAP_TOP    (*(volatile uint32_t *)0x24000004u)    /* FUN_080413d0's SDRAM bump pointer */

struct fw_ev { uint8_t type, _p0[7]; uint32_t target; uint16_t id, _p1; int32_t value; uint32_t _p2; };
struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

#define FS 48000.f
#define MAXN 32u
#define NCH 12u
#define NSLOT 4u                    /* tracks with chorus or drive at once */
#define CHO_N 384u                  /* 8 ms (Depth 1000 sweeps up to 7.7 ms) */
#define D2_N 262144u                /* 5.46 s: a bar at 45 BPM */
#define D2_CLEAR 4096u              /* samples per side cleared per block after switching on */
#define TWO_PI 6.2831853f

/* one track's inserts: a slot, taken while the track has chorus or drive on (and their tails) */
struct ins {
    int32_t ch;                                                 /* the channel, -1 free */
    uint32_t quiet, seen;                                       /* blocks of silent input; run this block */
    float cg, dg;                                               /* chorus and drive amounts, gliding to the settings */
    float ph, pre, postl, postr;                                /* chorus: LFO phase 0..1, the BBD's in and out filters */
    uint32_t wp;
    float hpl[2], hpr[2], lpl, lpr;                             /* drive: DC blockers, Tone */
    int16_t buf[CHO_N];                                         /* chorus line, +-2 as +-32767 */
};

struct sfx {
    uint32_t magic;
    struct sfx *self;
    /* params, as the M4 sends them */
    int32_t cho_mode, cho_rate, cho_depth, cho_width;
    int32_t drv_drive, drv_tone, drv_level;
    int32_t d2_on, d2_fx1, d2_fx2, d2_time, d2_fb, d2_tone, d2_ping, d2_level, d2_beat, d2_sync, d2_rev, d2_drift;
    float bpm;                                                  /* the tempo d2_tgt was worked out for */
    /* per track: chorus and drive on and amount (0..1), Rev Delay send (0..1) */
    int32_t tc_on[NCH], td_on[NCH];
    float tc_amt[NCH], td_amt[NCH], ts_d2[NCH];
    int8_t slot_of[NCH];                                        /* the track's slot, -1 none */
    uint32_t act;                                               /* the Rev Delay runs, so it wants input */
    uint32_t has;                                               /* it has input this block */
    float in[2][MAXN];                                          /* its input, summed by sfx_strip */
    /* chorus and drive, derived */
    float cho_inc, cho_ctr, cho_dep, cho_w;
    float drv_gain, drv_k, drv_lp, drv_g2;
    /* delay 2 */
    float d2_cur, d2_tgt, d2_fbg, d2_lp, d2_g, d2_gt;
    float d2_lpl, d2_lpr, d2_hpl[2], d2_hpr[2];
    uint32_t d2_wp, d2_clear;
    uint32_t d2_seg, d2_j, d2_T, d2_rng, d2_isrev;              /* Reverse: this repeat's length, where in it, backwards? */
    float d2_dph, d2_dnoise, d2_dtgt, d2_dm;                    /* Drift: LFO phase, wander, its target, extra delay */
    uint32_t d2_dcount, d2_drng;
    uint32_t dirty;                                             /* params changed by sfx_msg since the last update() */
    struct ins ins[NSLOT];
    int16_t d2_l[D2_N], d2_r[D2_N];                             /* +-1 as +-32767 (sat keeps them in range) */
};
#define SFX_MAGIC 0x58464453u       /* "SDFX" */

struct sfx_ptr { uint32_t magic; struct sfx *s; };
#define SP ((volatile struct sfx_ptr *)0x38800fa0u)   /* own 32-byte line, M7 only */

static void zero(void *p, uint32_t bytes)
{
    uint32_t *w = p;
    for (uint32_t i = 0; i < bytes / 4; i++)
        w[i] = 0;
}

static float exp2f_(float x)        /* -126 < x < 126 */
{
    float fl = (float)(int)x;
    if (fl > x) fl -= 1.f;
    float f = x - fl;
    union { float f; uint32_t u; } v = { 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f)))) };
    v.u += (uint32_t)(int32_t)fl << 23;
    return v.f;
}
/* one-pole low-pass coefficient for cutoff fc */
static float lp_coef(float fc)
{
    float w = TWO_PI * fc / FS;
    return w / (1.f + w);
}
/* soft clip, tanh-like: x (27 + x^2) / (27 + 9 x^2), exactly +-1 from |x| = 3 (as the master saturator's) */
static float sat(float x)
{
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

/* the drive's curve: like sat, but the negative side clips lower (0.6), so even harmonics stay at full drive */
static float warm(float x)
{
    return x >= 0.f ? sat(x) : 0.6f * sat(x * (1.f / 0.6f));
}

/* the drive's curve: the biased, uneven clipper (even harmonics, warmth), then on the top half a harder one */
static inline __attribute__((always_inline)) float drv_curve(float gain, float g2, float sb, float x)
{
    float v = warm(gain * x + 0.3f) - sb;
    return g2 > 1.f ? sat(g2 * v) : v;
}
static float drv_stage(const struct sfx *s, float x)
{
    return drv_curve(s->drv_gain, s->drv_g2, warm(0.3f), x);
}

static void defaults(struct sfx *s)
{
    s->cho_mode = 0; s->cho_rate = 500; s->cho_depth = 500; s->cho_width = 500;
    s->drv_drive = 500; s->drv_tone = 600; s->drv_level = 500;
    for (unsigned c = 0; c < NCH; c++)
        s->slot_of[c] = -1;
    for (unsigned j = 0; j < NSLOT; j++)
        s->ins[j].ch = -1;
    s->d2_on = 0; s->d2_fx1 = 0; s->d2_fx2 = 0; s->d2_time = 700; s->d2_fb = 400; s->d2_tone = 600; s->d2_ping = 0; s->d2_level = 700;
    s->d2_beat = 0; s->d2_sync = 8; s->d2_rev = 0; s->d2_drift = 0;
    s->bpm = 120.f;
}

/* derived values for the current params */
static void update(struct sfx *s)
{
    static const float rate[3] = { 0.513f, 0.863f, 9.75f };      /* Juno-60: I, II, I+II */
    static const float ctr[3] = { 3.505f, 3.505f, 3.5f };        /* ms: I and II sweep 1.66..5.35 ms */
    static const float dep[3] = { 1.845f, 1.845f, 0.2f };
    int m = s->cho_mode < 0 ? 0 : s->cho_mode > 2 ? 2 : s->cho_mode;
    s->cho_inc = rate[m] * exp2f_((s->cho_rate - 500) * 0.004f) / FS;  /* Rate: x0.25 .. x4 */
    float dp = dep[m] * s->cho_depth * 0.002f;                         /* Depth: 0 .. x2 */
    float c = ctr[m] > dp + 0.3f ? ctr[m] : dp + 0.3f;                 /* never closer than 0.3 ms */
    s->cho_ctr = c * FS * 0.001f;
    s->cho_dep = dp * FS * 0.001f;
    int wv = s->cho_width < 0 ? 0 : s->cho_width > 1000 ? 1000 : s->cho_width;
    s->cho_w = wv * 0.002f;                                           /* Width: the side signal x0 .. x2 */

    float d = s->drv_drive * 0.001f;
    s->drv_gain = exp2f_(6.f * d + 4.f * d * d);                      /* 0 .. +24 dB (500) .. +60 dB into the clipper */
    s->drv_g2 = 1.f + 6.f * (d > 0.5f ? 2.f * d - 1.f : 0.f);         /* the top half: a second, harder stage (x1 .. x7) */
    float up = drv_stage(s, 0.25f), dn = -drv_stage(s, -0.25f);
    int lv = s->drv_level < 0 ? 0 : s->drv_level > 1000 ? 1000 : s->drv_level;
    s->drv_k = 0.25f / (up > dn ? up : dn) * (lv * 0.002f);           /* -12 dBFS peaks stay put as Drive rises; Level 500 = that, 1000 = +6 dB */
    s->drv_lp = lp_coef(500.f * exp2f_(5.3f * s->drv_tone * 0.001f));   /* 500 Hz .. 20 kHz */

    static const uint16_t ticks[12] = { 60, 120, 240, 320, 360, 480, 639, 720, 960, 1280, 1920, 3840 };
    if (s->d2_beat) {               /* the stock delay's note values, in its ticks (960 a beat; its 1/4T is 639) */
        int k = s->d2_sync < 0 ? 0 : s->d2_sync > 11 ? 11 : s->d2_sync;
        s->d2_tgt = ticks[k] * (60.f / 960.f) * FS / s->bpm;
    } else
        s->d2_tgt = 10.f * exp2f_(7.643856f * s->d2_time * 0.001f) * FS * 0.001f;  /* 10 ms * 200^t */
    if (s->d2_tgt > (float)(D2_N - 4 - 1024)) s->d2_tgt = (float)(D2_N - 4 - 1024);   /* (room for Drift's 16 ms) */
    s->d2_fbg = s->d2_fb * 0.00098f;                                  /* up to 0.98 */
    s->d2_lp = lp_coef(800.f * exp2f_(4.5f * s->d2_tone * 0.001f));   /* 800 Hz .. 18 kHz in the loop */
    s->d2_gt = s->d2_on ? 1.f : 0.f;
}

/* Our state, once sfx_ctor has set it up in this heap's life; 0 before (backup SRAM keeps the pointer, and SDRAM
 * keeps the old state over a reset, so after one both look valid until the graph is built again: the bump pointer
 * tells, it is back below them until then). */
static struct sfx *state(void)
{
    bkp_enable();
    struct sfx *s = SP->s;
    uint32_t a = (uint32_t)s;
    if (SP->magic != SFX_MAGIC || a < 0xc0000000u || a + sizeof(struct sfx) > HEAP_TOP
        || s->magic != SFX_MAGIC || s->self != s)
        return 0;
    return s;
}

/* Replaces bl FUN_08052e30 @0x080518d2 (the delay's constructor, while the graph is built). */
void *sfx_ctor(void *obj)
{
    void *r = fw_dly_ctor(obj);
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    struct sfx *s = SP->s;
    uint32_t a = (uint32_t)s;
    /* the same heap life (a rebuilt graph): keep ours; after a reset the bump pointer is back below it */
    if (!(SP->magic == SFX_MAGIC && a >= 0xc0000000u && a + sizeof(struct sfx) <= HEAP_TOP
          && s->magic == SFX_MAGIC && s->self == s)) {
        s = (struct sfx *)fw_alloc(sizeof(struct sfx));
        zero(s, sizeof(struct sfx));
        defaults(s);
        update(s);
        s->self = s;
        s->magic = SFX_MAGIC;
        SP->s = s;
        SP->magic = SFX_MAGIC;
    }
    return r;
}

static int set(struct sfx *s, unsigned id, int32_t v)
{
    switch (id) {
    case SFX_CHO_MODE: s->cho_mode = v; break;
    case SFX_CHO_RATE: s->cho_rate = v; break;
    case SFX_CHO_DEPTH: s->cho_depth = v; break;
    case SFX_CHO_WIDTH: s->cho_width = v; break;
    case SFX_DRV_DRIVE: s->drv_drive = v; break;
    case SFX_DRV_TONE: s->drv_tone = v; break;
    case SFX_DRV_LEVEL: s->drv_level = v; break;
    case SFX_D2_ON:
        if (v && !s->d2_on)
            s->d2_clear = 0;                /* wipe what the lines held when it was last on */
        s->d2_on = v;
        break;
    case SFX_D2_FX1: s->d2_fx1 = v; break;
    case SFX_D2_FX2: s->d2_fx2 = v; break;
    case SFX_D2_TIME: s->d2_time = v; break;
    case SFX_D2_FB: s->d2_fb = v; break;
    case SFX_D2_TONE: s->d2_tone = v; break;
    case SFX_D2_PING: s->d2_ping = v; break;
    case SFX_D2_LEVEL: s->d2_level = v; break;
    case SFX_D2_BEAT: s->d2_beat = v; break;
    case SFX_D2_SYNC: s->d2_sync = v; break;
    case SFX_D2_REV: s->d2_rev = v; break;
    case SFX_D2_DRIFT: s->d2_drift = v; break;
    default: return 0;
    }
    return 1;
}

/* the per-track ones (each channel's set, the channel as target) */
static int is_ts(unsigned id)
{
    return (id >= SFX_TS_CHO && id <= SFX_TS_D2) || id == SFX_TS_CHO_ON || id == SFX_TS_DRV_ON;
}
static void set_ts(struct sfx *s, unsigned id, unsigned ch, int32_t v)
{
    if (ch >= NCH)
        return;
    float a = (v < 0 ? 0 : v > 1000 ? 1000 : v) * 0.001f;
    switch (id) {
    case SFX_TS_CHO: s->tc_amt[ch] = a; break;
    case SFX_TS_DRV: s->td_amt[ch] = a; break;
    case SFX_TS_D2: s->ts_d2[ch] = a; break;
    case SFX_TS_CHO_ON: s->tc_on[ch] = v != 0; break;
    case SFX_TS_DRV_ON: s->td_on[ch] = v != 0; break;
    }
}

/* A track's drive, then its chorus, from il, ir into ol, or (which may be the same), mixed in by the slot's
 * amounts: Drive crossfades to the driven sound, Chorus at full is the Juno's even mix of dry and chorus. */
static void inserts(const struct sfx *s, struct ins *k, const float *il, const float *ir, float *ol, float *or_, unsigned n)
{
    unsigned c = (unsigned)k->ch;
    float ct = s->tc_on[c] ? 0.5f * s->tc_amt[c] : 0.f, dt = s->td_on[c] ? s->td_amt[c] : 0.f;
    float dg = k->dg, cg = k->cg;
    if (dt > 0.f || dg > 1e-4f) {
        if (dg <= 1e-4f)                                /* starting: from fresh filters */
            k->hpl[0] = k->hpl[1] = k->hpr[0] = k->hpr[1] = k->lpl = k->lpr = 0.f;
        float kk = s->drv_k, a = s->drv_lp, gain = s->drv_gain, g2 = s->drv_g2;
        const float sb = warm(0.3f);
        float *hp[2] = { k->hpl, k->hpr }, *lp[2] = { &k->lpl, &k->lpr };
        for (unsigned i = 0; i < n; i++) {
            dg += 0.002f * (dt - dg);
            float x[2] = { il[i], ir[i] };
            for (int ch = 0; ch < 2; ch++) {
                float v = drv_curve(gain, g2, sb, x[ch]);
                float h = v - hp[ch][0] + 0.995f * hp[ch][1];   /* DC blocker, ~40 Hz */
                hp[ch][0] = v;
                hp[ch][1] = h;
                *lp[ch] += a * (h - *lp[ch]);
                x[ch] += dg * (*lp[ch] * kk - x[ch]);
            }
            ol[i] = x[0];
            or_[i] = x[1];
        }
        if (dt <= 0.f && dg < 1e-4f) dg = 0.f;
    } else if (ol != il) {
        for (unsigned i = 0; i < n; i++) { ol[i] = il[i]; or_[i] = ir[i]; }
    }
    k->dg = dg;
    if (ct > 0.f || cg > 1e-4f) {
        if (cg <= 1e-4f) {                              /* starting: an empty line */
            for (unsigned i = 0; i < CHO_N; i++) k->buf[i] = 0;
            k->pre = k->postl = k->postr = 0.f;
        }
        const float a = 0.6f, b = 0.55f;                /* ~9 kHz in, ~7 kHz out: the BBD's band limit */
        float pre = k->pre, pl = k->postl, pr = k->postr, ph = k->ph, w = s->cho_w;
        uint32_t wp = k->wp;
        for (unsigned i = 0; i < n; i++) {
            cg += 0.002f * (ct - cg);
            float xl = ol[i], xr = or_[i];
            pre += a * ((xl + xr) * 0.5f - pre);        /* mono in, as the Juno */
            float q = pre * 16383.f;
            k->buf[wp] = (int16_t)(q > 32767.f ? 32767.f : q < -32767.f ? -32767.f : q);
            float tri = ph < 0.5f ? 4.f * ph - 1.f : 3.f - 4.f * ph;
            ph += s->cho_inc;
            if (ph >= 1.f) ph -= 1.f;
            float out[2];
            for (int ch = 0; ch < 2; ch++) {
                float d = s->cho_ctr + (ch ? -tri : tri) * s->cho_dep;
                int di = (int)d;
                float f = d - (float)di;
                int r0 = (int)wp - di, r1 = r0 - 1;
                if (r0 < 0) r0 += CHO_N;
                if (r1 < 0) r1 += CHO_N;
                float x0 = k->buf[r0], x1 = k->buf[r1];
                out[ch] = (x0 + f * (x1 - x0)) * (1.f / 16383.f);
            }
            if (++wp == CHO_N) wp = 0;
            pl += b * (out[0] - pl);
            pr += b * (out[1] - pr);
            float m = 0.5f * (pl + pr), sd = 0.5f * (pl - pr) * w;
            ol[i] = xl + cg * (m + sd - xl);
            or_[i] = xr + cg * (m - sd - xr);
        }
        if (ct <= 0.f && cg < 1e-4f) cg = 0.f;
        k->pre = pre; k->postl = pl; k->postr = pr; k->ph = ph; k->wp = wp;
    }
    k->cg = cg;
}

/* the track's slot: kept while it has chorus or drive on or still fading out, taken when one comes on (0 if all
 * NSLOT are in use) */
static struct ins *slot(struct sfx *s, unsigned c)
{
    int want = s->tc_on[c] || s->td_on[c];
    int j = s->slot_of[c];
    if (j >= 0) {
        struct ins *k = &s->ins[j];
        if (!want && k->cg <= 0.f && k->dg <= 0.f) {
            k->ch = -1;
            s->slot_of[c] = -1;
            return 0;
        }
        return k;
    }
    if (!want)
        return 0;
    for (j = 0; j < (int)NSLOT; j++)
        if (s->ins[j].ch < 0) {
            struct ins *k = &s->ins[j];
            zero(k, sizeof *k);
            k->ch = (int32_t)c;
            s->slot_of[c] = (int8_t)j;
            return k;
        }
    return 0;
}

static float d2_read(const int16_t *line, uint32_t wp, float d)
{
    int di = (int)d;
    float f = d - (float)di;
    float x0 = (float)line[(wp - di) & (D2_N - 1)], x1 = (float)line[(wp - di - 1) & (D2_N - 1)];
    return (x0 + f * (x1 - x0)) * (1.f / 32767.f);
}

static void delay2(struct sfx *s, const float *il, const float *ir, float *wl_, float *wr_, unsigned n)
{
    if (s->d2_clear < D2_N) {           /* switching on: clear the lines first (a few ms, silent) */
        zero(s->d2_l + s->d2_clear, D2_CLEAR * 2);
        zero(s->d2_r + s->d2_clear, D2_CLEAR * 2);
        s->d2_clear += D2_CLEAR;
        s->d2_lpl = s->d2_lpr = 0.f;
        s->d2_hpl[0] = s->d2_hpl[1] = s->d2_hpr[0] = s->d2_hpr[1] = 0.f;
        s->d2_cur = s->d2_tgt;
        zero(wl_, n * 4);
        zero(wr_, n * 4);
        return;
    }
    const float send = 1.f;
    float fb = s->d2_fbg, a = s->d2_lp, cur = s->d2_cur;
    uint32_t wp = s->d2_wp;
    int ping = s->d2_ping;
    /* Reverse: time is cut into repeats of one delay time each; at each repeat's start a die (xorshift) decides
     * whether it plays the last repeat's worth backwards: read from 1 to 2 delay times back, the read point walking
     * back while the write point walks on. Faded in and out against the forward read over 2 ms, so no clicks. It
     * sits in the loop, so the feedback carries reversed repeats on (and rolls again on each pass). */
    uint32_t chance = s->d2_rev <= 0 ? 0u : s->d2_rev >= 1000 ? 0xffffffffu : (uint32_t)s->d2_rev * 4294967u;
    uint32_t seg = s->d2_seg, j = s->d2_j, T = s->d2_T, rev = s->d2_isrev, rng = s->d2_rng ? s->d2_rng : 0x9e3779b9u;
    /* Drift, as the FX1 delay's: a 0.6 Hz wobble plus a slower random wander, extra delay 0 .. 2A (A up to 8 ms,
     * about +-2.5 % pitch at full), ramped across the block */
    float depth = s->d2_drift <= 0 ? 0.f : s->d2_drift >= 1000 ? 1.f : (float)s->d2_drift * 1e-3f;
    float fa = depth * 0.008f * FS, m0 = s->d2_dm, m1 = 0.f;
    if (fa > 0.f) {
        s->d2_dph += 0.6f * (float)n / FS;
        if (s->d2_dph >= 1.f) s->d2_dph -= 1.f;
        if ((s->d2_dcount++ & 511u) == 0) {             /* a new wander target every ~0.34 s */
            s->d2_drng = s->d2_drng * 1664525u + 1013904223u;
            s->d2_dtgt = (float)(int32_t)s->d2_drng * (1.f / 2147483648.f);
        }
        s->d2_dnoise += (s->d2_dtgt - s->d2_dnoise) * 0.004f;
        float x = 2.f * s->d2_dph - 1.f;                /* a parabolic sine over the phase */
        float sn = 4.f * x * (1.f - (x < 0.f ? -x : x));
        m1 = fa * (1.f + 0.6f * sn + 0.4f * s->d2_dnoise);
        if (m0 <= 0.f) m0 = m1;                         /* just turned on: no jump from 0 */
    }
    float dm = (m1 - m0) / (float)n, m = m0;
    s->d2_dm = m1;
    for (unsigned i = 0; i < n; i++) {
        cur += 0.0002f * (s->d2_tgt - cur);             /* time changes glide, tape style (~100 ms) */
        m += dm;
        float rd = cur + m;
        float wl = d2_read(s->d2_l, wp, rd), wr = d2_read(s->d2_r, wp, rd);
        if (!(chance | rev)) {                          /* Reverse at 0: nothing to do (the next repeat starts afresh) */
        } else if (j >= seg) {                          /* a new repeat */
            T = (uint32_t)cur;
            seg = T;
            j = 0;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            rev = chance && rng <= chance && 2 * T + 4 < D2_N && T > 4 * 96;
        }
        if (rev) {                                      /* (j < seg = T here) */
            int32_t e = (int32_t)(j < T - j ? j : T - j);           /* to the nearer edge */
            float g = e >= 96 ? 1.f : (float)e * (1.f / 96.f);
            float d = (float)(2 * j + 1);
            float rl = d2_read(s->d2_l, wp, d), rr = d2_read(s->d2_r, wp, d);
            wl += g * (rl - wl);
            wr += g * (rr - wr);
        }
        j++;
        s->d2_lpl += a * (wl - s->d2_lpl);
        s->d2_lpr += a * (wr - s->d2_lpr);
        float fl = s->d2_lpl - s->d2_hpl[0] + 0.99f * s->d2_hpl[1];   /* ~80 Hz high-pass in the loop */
        s->d2_hpl[0] = s->d2_lpl; s->d2_hpl[1] = fl;
        float fr = s->d2_lpr - s->d2_hpr[0] + 0.99f * s->d2_hpr[1];
        s->d2_hpr[0] = s->d2_lpr; s->d2_hpr[1] = fr;
        float xl, xr;
        if (ping) {                                     /* mono in on the left, then left <-> right */
            xl = (il[i] + ir[i]) * 0.5f * send + fb * fr;
            xr = fb * fl;
        } else {
            xl = il[i] * send + fb * fl;
            xr = ir[i] * send + fb * fr;
        }
        s->d2_l[wp] = (int16_t)(sat(xl) * 32767.f);
        s->d2_r[wp] = (int16_t)(sat(xr) * 32767.f);
        wp = (wp + 1) & (D2_N - 1);
        wl_[i] = wl;
        wr_[i] = wr;
    }
    s->d2_cur = cur; s->d2_wp = wp;
    s->d2_seg = seg; s->d2_j = j; s->d2_T = T; s->d2_isrev = rev; s->d2_rng = rng;
}

/* An FX's wet output into the main mix (Level) and into the delay's and reverb's send buses (FX1 Send, FX2 Send);
 * g ramps it in and out as the FX is switched on and off. */
struct dest { float *l12, *r12, *l13, *r13, *l14, *r14; };
static void out(const struct dest *d, float *g, float gt, int32_t level, int32_t fx1, int32_t fx2,
                const float *wl, const float *wr, unsigned n)
{
    float lv = level * 0.001f, f1 = d->l13 ? fx1 * 0.001f : 0.f, f2 = d->l14 ? fx2 * 0.001f : 0.f, gg = *g;
    for (unsigned i = 0; i < n; i++) {
        gg += 0.002f * (gt - gg);
        float a = wl[i] * gg, b = wr[i] * gg;
        d->l12[i] += a * lv;
        d->r12[i] += b * lv;
        if (f1 != 0.f) { d->l13[i] += a * f1; d->r13[i] += b * f1; }
        if (f2 != 0.f) { d->l14[i] += a * f2; d->r14[i] += b * f2; }
    }
    *g = gg;
}

/*
 * Every message from the M4 (bl FUN_08052444 @0x0804c7a8, in the audio task's loop that drains them before each
 * block): ours are taken here, straight into the state. They used to be read from the master queue (0xc), but that
 * queue holds 64 messages and drops the rest, and loading a project sends hundreds at once (every channel's set,
 * our track sends last in each), so FX settings and sends went missing until a knob was turned again.
 */
void sfx_msg(void *ctx, struct fw_ev *ev)
{
    if (ev->type == 0x39) {
        struct sfx *s = state();
        if (s) {
            if (is_ts(ev->id) && ev->target < NCH) {
                set_ts(s, ev->id, ev->target, ev->value);
                return;
            }
            if (set(s, ev->id, ev->value)) {
                s->dirty = 1;
                return;
            }
        }
    }
    fw_dispatch(ctx, ev);
}

/* The reverb's process (vtable slot 0x0806adac): the Rev Delay, then the reverb (hall_process) and the chain. */
unsigned sfx_process(void *obj, void *ctx)
{
    struct sfx *s = state();
    if (!s)
        return fw_reverb(obj, ctx);
    void *q = fw_queue(ctx, 0xc);       /* (a fallback: sfx_msg takes ours before they get here) */
    struct fw_ev ev;
    int changed = s->dirty;
    s->dirty = 0;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++)
        if (ev.type == 0x39) {
            if (is_ts(ev.id))
                set_ts(s, ev.id, ev.target, ev.value);
            else
                changed |= set(s, ev.id, ev.value);
        }
    if (s->d2_beat) {               /* the song tempo, as the stock delay reads it (FUN_08053234: ctx[0] + 0x18) */
        float bpm = *(volatile float *)(*(uint8_t **)ctx + 0x18);
        if (!(bpm >= 20.f && bpm <= 400.f)) bpm = 120.f;
        if (bpm != s->bpm) { s->bpm = bpm; changed = 1; }
    }
    if (changed)
        update(s);
    /* a slot whose track's strip did not run (the channel switched off) and that it no longer wants: free */
    for (unsigned j = 0; j < NSLOT; j++) {
        struct ins *k = &s->ins[j];
        if (k->ch >= 0 && !k->seen && !s->tc_on[k->ch] && !s->td_on[k->ch]) {
            s->slot_of[k->ch] = -1;
            k->ch = -1;
        }
        k->seen = 0;
    }
    int d2 = s->d2_on || s->d2_g > 1e-4f;
    uint32_t has = s->has;
    s->has = 0;
    s->act = d2;                        /* sfx_strip fills its input only then, from next block */
    if (!d2)
        return fw_reverb(obj, ctx);

    struct bus *b = fw_bus(ctx, 12);
    unsigned n = fw_frames(b);
    if (n > MAXN) n = MAXN;
    if (!has)                           /* no input this block: it runs on silence */
        for (unsigned i = 0; i < n; i++)
            s->in[0][i] = s->in[1][i] = 0.f;
    struct dest d;
    fw_stereo(b, &d.l12, &d.r12);       /* zero-filled if it was silent */
    float wl[MAXN], wr[MAXN];
    /* the send buses only when something goes to them (fw_stereo wakes a silent bus, and its FX with it) */
    d.l13 = d.r13 = d.l14 = d.r14 = 0;
    if (s->d2_fx1) fw_stereo(fw_bus(ctx, 13), &d.l13, &d.r13);
    if (s->d2_fx2) fw_stereo(fw_bus(ctx, 14), &d.l14, &d.r14);
    delay2(s, s->in[0], s->in[1], wl, wr, n);
    out(&d, &s->d2_g, s->d2_gt, s->d2_level, s->d2_fx1, s->d2_fx2, wl, wr, n);
    return fw_reverb(obj, ctx);
}

/* Replaces the mixer's bl FUN_0805012c @0x0805070a (per channel, before the FX run): the track's inserts on a copy
 * of its input, which the channel then gets in place of it; after the channel, its post-fader signal (the bus at
 * ch + 20, after fader, pan and mute) times its Rev Delay send into that FX's input. */
void sfx_strip(void *mixer, void *in, unsigned idx, float *ch, void *mainb, void *ctx)
{
    struct sfx *s = state();            /* the mixer runs from boot, maybe before the graph's FX are built */
    if (!s || idx >= NCH) {
        fw_strip(mixer, in, idx, ch, mainb, ctx);
        return;
    }
    struct ins *k = slot(s, idx);
    struct bus tb;
    float tl[MAXN], tr[MAXN];
    if (k) {
        struct bus *ib = in;
        unsigned n = fw_frames(ib);
        k->seen = 1;
        if (ib->silent) {               /* the chorus's tail, then nothing to do */
            if (k->quiet <= CHO_N / MAXN + 2) k->quiet++;
        } else
            k->quiet = 0;
        if (n && n <= MAXN && k->quiet <= CHO_N / MAXN + 2) {
            float *il, *ir;
            if (ib->silent) {
                for (unsigned i = 0; i < n; i++) tl[i] = tr[i] = 0.f;
                il = tl; ir = tr;
            } else
                fw_stereo(ib, &il, &ir);    /* (a mono input: its right side made a copy of the left, as stock) */
            inserts(s, k, il, ir, tl, tr, n);
            tb.cap = n; tb.frames = n; tb.l = tl; tb.r = tr; tb.silent = 0; tb.stereo = 1;
            in = &tb;
        }
    }
    fw_strip(mixer, in, idx, ch, mainb, ctx);
    if (!s->act)
        return;
    float g = s->ts_d2[idx];
    if (g <= 0.f)
        return;
    struct bus *b = (struct bus *)((uint8_t *)ch + 20);
    if (b->silent || !b->l)
        return;
    unsigned n = b->frames > MAXN ? MAXN : b->frames;
    const float *l = b->l, *r = b->stereo && b->r ? b->r : b->l;
    float *il = s->in[0], *ir = s->in[1];
    if (s->has)
        for (unsigned i = 0; i < n; i++) { il[i] += g * l[i]; ir[i] += g * r[i]; }
    else
        for (unsigned i = 0; i < n; i++) { il[i] = g * l[i]; ir[i] = g * r[i]; }
    s->has = 1;
}

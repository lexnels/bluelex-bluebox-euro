/* Send FX (chorus, drive, delay 2): param ids, shared by the M7 engine (src/sfx_m7.c) and the M4 panel (src/dly_m4.c).
 * Undefined in both cores' param tables and handled by no stock node. They live in the reverb slot's param set, so
 * the M4 sends them with slot 0x15; src/sfx_m7.c takes them before the M7's dispatcher (the stock reverb setter would
 * ignore ids outside 0x13d..0x15c anyway). 0x44, 0x45, 0x47, 0x48, 0x49, 0x56, 0x57 were the chorus's and drive's
 * On, Level and FX1/FX2 Send while they were send FX (builds up to 14): not used now. */
#define SFX_CHO_MODE  0x46          /* a list: 0 = I, 1 = II, 2 = I+II */
#define SFX_DRV_DRIVE 0x4c
#define SFX_DRV_TONE  0x4d
#define SFX_DRV_LEVEL 0x4e          /* 500 = peaks as clean, 1000 = +6 dB */
#define SFX_D2_ON     0x4f
#define SFX_D2_FX1    0x50
#define SFX_D2_TIME   0x51          /* 0..1000: 10 ms * 200^(v / 1000), so 10 ms .. 2 s */
#define SFX_D2_FB     0x52
#define SFX_D2_TONE   0x53
#define SFX_D2_PING   0x54
#define SFX_D2_LEVEL  0x55
#define SFX_D2_FX2    0x58
#define SFX_CHO_RATE  0x5a          /* 500 = the mode's own LFO rate; x0.25 .. x4 */
#define SFX_CHO_DEPTH 0x5b          /* 500 = the mode's own depth; 0 .. x2 */
#define SFX_D2_BEAT   0x5c          /* Beat Sync: Time follows the tempo (SFX_D2_SYNC) */
#define SFX_D2_SYNC   0x5d          /* the stock delay's 12 note values (1/64 .. 1 bar), its names list too */
/* Per track (one per channel, in each channel's set, slots 0..11): chorus and drive as inserts (on/off, amount),
 * and how much of the channel goes into the Rev Delay */
#define SFX_TS_CHO    0x5e          /* chorus amount: 1000 = the Juno's even mix of dry and chorus */
#define SFX_TS_DRV    0x5f          /* drive amount: dry .. fully driven */
#define SFX_TS_D2     0x60
#define SFX_TS_CHO_ON 0x73
#define SFX_TS_DRV_ON 0x74
#define SFX_D2_REV    0x62          /* the chance (0..1000 = 0..100 %) that a repeat plays backwards */
#define SFX_D2_DRIFT  0x64          /* tape drift, as the FX1 delay's (0x3a): a wobbling, wandering extra delay up to 16 ms */
#define SFX_CHO_WIDTH 0x61          /* stereo width of its output: 0 = mono, 500 = as the Juno (opposite LFOs), 1000 = x2 */

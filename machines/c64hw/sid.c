/*
 * The SID, as programs POKE it ($D400-$D41C), played on the PC speaker,
 * which sounds one square wave at a time.
 *
 * Each of the three voices has a frequency (register x 0.0587 Hz, the
 * PAL machine's clock divided by 2^24), a waveform and a gate, and an
 * envelope: when the gate goes on, it rises over the attack time, falls
 * over the decay time to the sustain level and stays there; when the
 * gate goes off, it falls to nothing over the release time. (The times
 * are the chip's documented ones.) $D418's low nybble is the volume.
 *
 * As time passes (c64_tick), the voice whose envelope is loudest is the
 * one the speaker plays; noise is played as a frequency that jumps
 * about. The filter, the waveforms' tone colour, and the mixing of
 * voices are lost: real sound (a sound card's) would be a project of its
 * own.
 *
 * Reading: $D419-$D41A the paddles (255: there are none), $D41B voice
 * 3's waveform (programs use its noise as random numbers), $D41C voice
 * 3's envelope.
 */
#include "c64.h"
#include "platform.h"
#include "string.h"

static unsigned char regs[32];
static long level[3];                     // envelopes, 0-255000
static unsigned char phase[3];            // ATTACK, DECAY (to sustain), RELEASE
static unsigned long last_ms, random = 0x1D872B41UL;
static int playing;                       // what the speaker plays (0: nothing)

enum { ATTACK, DECAY, RELEASE };

static const unsigned short attack_ms[16] = {
    2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000,
};

static int next_random(void) {
    random = random * 1103515245UL + 12345UL;
    return random >> 16 & 0xFF;
}

static unsigned int freq(int v) { return regs[v * 7] | regs[v * 7 + 1] << 8; }

// The envelopes moved on by ms.
static void run_envelopes(unsigned long ms) {
    for (int v = 0; v < 3; v++) {
        int ad = regs[v * 7 + 5], sr = regs[v * 7 + 6];
        long sustain = (sr >> 4) * 17000L;
        if (phase[v] == ATTACK) {
            level[v] += 255000L * ms / attack_ms[ad >> 4];
            if (level[v] >= 255000L) {
                level[v] = 255000L;
                phase[v] = DECAY;
            }
        } else {
            long fall = 255000L * ms / (3L * attack_ms[phase[v] == DECAY ? ad & 15 : sr & 15]);
            long floor = phase[v] == DECAY ? sustain : 0;
            if (level[v] > floor) {
                level[v] -= fall;
                if (level[v] < floor) level[v] = floor;
            }
        }
    }
}

// The speaker: the loudest voice.
static void choose(void) {
    int best = -1;
    long loudest = 0;
    for (int v = 0; v < 3; v++) {
        int ctrl = regs[v * 7 + 4];
        if (!(ctrl & 0xF0) || (ctrl & 8) || !freq(v)) continue;   // (no wave, test bit)
        if (v == 2 && (regs[24] & 0x80)) continue;                 // (voice 3 off)
        if ((ctrl & 0xF0) == 0x40 && !(regs[v * 7 + 2] | (regs[v * 7 + 3] & 15))) continue;
        if (level[v] > loudest) {
            loudest = level[v];
            best = v;
        }
    }
    int hz = 0;
    if (best >= 0 && (regs[24] & 15)) {
        hz = (int)(freq(best) * 985248UL / 16777216UL);
        if (hz == 0) hz = 1;
        if (regs[best * 7 + 4] & 0x80) hz = hz / 2 + hz * next_random() / 256;   // noise
    }
    if (hz < 20) hz = 0;
    if (hz != playing) {
        playing = hz;
        plat_speaker(hz);
    }
}

void sid_tick(void) {
    unsigned long now = plat_timer_ms();
    unsigned long ms = now - last_ms;
    if (ms == 0) return;
    last_ms = now;
    if (ms > 1000) ms = 1000;
    run_envelopes(ms);
    choose();
}

void sid_write(int reg, int value) {
    reg &= 31;
    if (reg > 24) return;
    sid_tick();
    int v = reg / 7;
    if (reg < 21 && reg % 7 == 4) {       // a voice's control: the gate
        int was = regs[reg] & 1, gate = value & 1;
        if (gate && !was) phase[v] = ATTACK;
        if (!gate && was) phase[v] = RELEASE;
    }
    regs[reg] = value;
    choose();
}

int sid_read(int reg) {
    reg &= 31;
    sid_tick();
    switch (reg) {
    case 25:
    case 26: return 255;                  // (no paddles)
    case 27: {                            // voice 3's waveform
        int ctrl = regs[18];
        if (ctrl & 0x80) return next_random();
        unsigned long p = (unsigned long)freq(2) * (plat_timer_ms() * 985UL) >> 16 & 0xFF;
        if (ctrl & 0x20) return (int)p;
        if (ctrl & 0x10) return (int)(p < 128 ? p * 2 : (255 - p) * 2);
        if (ctrl & 0x40) return p < (unsigned)(regs[16] >> 4 | regs[17] << 4) ? 255 : 0;
        return 0;
    }
    case 28: return (int)(level[2] / 1000);
    }
    return 0;
}

void sid_init(void) {
    memset(regs, 0, sizeof regs);
    memset(level, 0, sizeof level);
    memset(phase, RELEASE, sizeof phase);
    last_ms = plat_timer_ms();
    if (playing) plat_speaker(0);
    playing = 0;
}

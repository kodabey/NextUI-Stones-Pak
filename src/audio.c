/* Tiny procedural sound engine: every sound is synthesised at startup. */
#include "audio.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 24

typedef struct { float *s; int n; } Sample;
typedef struct { int snd, pos, delay; float vl, vr; } Voice;

static SDL_AudioDeviceID dev;
static int rate = 44100;
static Sample snd[SND_COUNT];
static Voice voices[MAXV];
static int enabled = 1;
static unsigned seed = 1234567;

static float frand(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return (seed & 0xffffff) / (float)0xffffff * 2 - 1;
}

static float *alloc_s(Sample *s, float secs) {
    s->n = (int)(secs * rate);
    s->s = calloc(s->n, sizeof(float));
    return s->s;
}

/* damped sinusoid added into buffer */
static void add_mode(float *b, int n, float start, float freq, float amp, float decay) {
    int i0 = (int)(start * rate);
    for (int i = i0; i < n; i++) {
        float t = (i - i0) / (float)rate;
        b[i] += amp * expf(-t / decay) * sinf(2 * (float)M_PI * freq * t);
    }
}

static void make_stone(Sample *s, float pitch) {
    float *b = alloc_s(s, 0.16f);
    /* the sharp "pachi": filtered noise transient */
    float lp = 0, hp_prev = 0, hp = 0;
    for (int i = 0; i < s->n; i++) {
        float t = i / (float)rate;
        float nz = frand() * expf(-t / 0.0018f);
        lp += (nz - lp) * 0.55f;
        hp = 0.7f * (hp + lp - hp_prev);
        hp_prev = lp;
        b[i] += hp * 0.9f;
    }
    /* wooden board resonances */
    add_mode(b, s->n, 0.0005f, 1180 * pitch, 0.42f, 0.032f);
    add_mode(b, s->n, 0.0005f, 2350 * pitch, 0.22f, 0.014f);
    add_mode(b, s->n, 0.0005f, 560 * pitch, 0.30f, 0.050f);
    add_mode(b, s->n, 0.0005f, 3900 * pitch, 0.10f, 0.006f);
}

static void make_tick(Sample *s) {
    float *b = alloc_s(s, 0.06f);
    for (int i = 0; i < s->n; i++) b[i] += frand() * expf(-i / (float)rate / 0.0008f) * 0.4f;
    add_mode(b, s->n, 0, 3100, 0.30f, 0.010f);
    add_mode(b, s->n, 0, 4700, 0.16f, 0.006f);
    add_mode(b, s->n, 0, 1900, 0.12f, 0.016f);
}

static void bell(float *b, int n, float start, float f, float amp, float decay) {
    add_mode(b, n, start, f, amp, decay);
    add_mode(b, n, start, f * 2.0f, amp * 0.35f, decay * 0.6f);
    add_mode(b, n, start, f * 2.76f, amp * 0.22f, decay * 0.4f);
    add_mode(b, n, start, f * 5.4f, amp * 0.08f, decay * 0.2f);
}

static void normalize(Sample *s, float peak) {
    float m = 0;
    for (int i = 0; i < s->n; i++) m = fmaxf(m, fabsf(s->s[i]));
    if (m > 0) for (int i = 0; i < s->n; i++) s->s[i] *= peak / m;
    /* 2 ms fade-in to avoid clicks */
    int f = rate / 500;
    for (int i = 0; i < f && i < s->n; i++) s->s[i] *= i / (float)f;
}

static void synth_all(void) {
    make_stone(&snd[SND_STONE0], 1.0f);
    make_stone(&snd[SND_STONE1], 0.94f);
    make_stone(&snd[SND_STONE2], 1.07f);
    for (int i = SND_STONE0; i <= SND_STONE2; i++) normalize(&snd[i], 0.85f);
    make_tick(&snd[SND_TICK]);
    normalize(&snd[SND_TICK], 0.45f);
    { float *b = alloc_s(&snd[SND_MOVE], 0.03f); add_mode(b, snd[SND_MOVE].n, 0, 1650, 1, 0.004f); normalize(&snd[SND_MOVE], 0.12f); }
    { float *b = alloc_s(&snd[SND_SELECT], 0.35f); bell(b, snd[SND_SELECT].n, 0, 784, 1, 0.12f); bell(b, snd[SND_SELECT].n, 0.06f, 1175, 0.8f, 0.14f); normalize(&snd[SND_SELECT], 0.32f); }
    { float *b = alloc_s(&snd[SND_BACK], 0.3f); bell(b, snd[SND_BACK].n, 0, 880, 1, 0.08f); bell(b, snd[SND_BACK].n, 0.05f, 587, 0.8f, 0.1f); normalize(&snd[SND_BACK], 0.28f); }
    { float *b = alloc_s(&snd[SND_PASS], 0.6f); bell(b, snd[SND_PASS].n, 0, 523, 1, 0.25f); normalize(&snd[SND_PASS], 0.35f); }
    { float *b = alloc_s(&snd[SND_HINT], 0.9f); int n = snd[SND_HINT].n;
      bell(b, n, 0, 1047, 1, 0.3f); bell(b, n, 0.08f, 1319, 0.8f, 0.3f); bell(b, n, 0.16f, 1568, 0.7f, 0.35f);
      normalize(&snd[SND_HINT], 0.3f); }
    { float *b = alloc_s(&snd[SND_WIN], 2.6f); int n = snd[SND_WIN].n;
      float notes[] = {523.3f, 659.3f, 784.0f, 880.0f, 1046.5f, 1318.5f};
      for (int i = 0; i < 6; i++) bell(b, n, i * 0.11f, notes[i], 1.0f - i * 0.06f, 0.55f);
      bell(b, n, 0.75f, 1046.5f, 0.7f, 0.8f); bell(b, n, 0.75f, 784.0f, 0.5f, 0.8f);
      normalize(&snd[SND_WIN], 0.5f); }
    { float *b = alloc_s(&snd[SND_LOSE], 2.0f); int n = snd[SND_LOSE].n;
      bell(b, n, 0, 440, 1, 0.5f); bell(b, n, 0.25f, 392, 0.9f, 0.55f); bell(b, n, 0.5f, 329.6f, 0.9f, 0.8f);
      normalize(&snd[SND_LOSE], 0.42f); }
    { float *b = alloc_s(&snd[SND_ATARI], 0.4f); int n = snd[SND_ATARI].n;
      bell(b, n, 0, 1318.5f, 1, 0.06f); bell(b, n, 0.09f, 1318.5f, 0.8f, 0.07f);
      normalize(&snd[SND_ATARI], 0.18f); }
}

static void mix_chunk(Sint16 *out, int frames);
static void callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4;
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        mix_chunk(out, n);
        out += n * 2;
        frames -= n;
    }
}
static void mix_chunk(Sint16 *out, int frames) {
    static float mix[8192];
    memset(mix, 0, sizeof(float) * frames * 2);
    for (int v = 0; v < MAXV; v++) {
        Voice *vo = &voices[v];
        if (vo->snd < 0) continue;
        Sample *s = &snd[vo->snd];
        for (int i = 0; i < frames; i++) {
            if (vo->delay > 0) { vo->delay--; continue; }
            if (vo->pos >= s->n) { vo->snd = -1; break; }
            float x = s->s[vo->pos++];
            mix[2 * i] += x * vo->vl;
            mix[2 * i + 1] += x * vo->vr;
        }
    }
    for (int i = 0; i < frames * 2; i++) {
        float x = mix[i];
        x = x / (1.0f + fabsf(x) * 0.3f); /* gentle soft clip */
        out[i] = (Sint16)(x * 30000);
    }
}

int audio_init(void) {
    for (int v = 0; v < MAXV; v++) voices[v].snd = -1;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev) {
        SDL_Log("audio: %s", SDL_GetError());
        return -1;
    }
    rate = have.freq;
    synth_all();
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

void audio_quit(void) {
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}

void audio_set_enabled(int on) { enabled = on; }

void audio_play(int s, float vol, float pan, int delay_ms) {
    if (!dev || !enabled || s < 0 || s >= SND_COUNT) return;
    SDL_LockAudioDevice(dev);
    int slot = -1;
    for (int v = 0; v < MAXV; v++) if (voices[v].snd < 0) { slot = v; break; }
    if (slot < 0) slot = (int)(SDL_GetTicks() % MAXV);
    Voice *vo = &voices[slot];
    vo->snd = s;
    vo->pos = 0;
    vo->delay = delay_ms * rate / 1000;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    vo->vl = vol * (pan > 0 ? 1 - pan * 0.6f : 1);
    vo->vr = vol * (pan < 0 ? 1 + pan * 0.6f : 1);
    SDL_UnlockAudioDevice(dev);
}

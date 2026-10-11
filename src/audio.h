#pragma once

enum {
    SND_STONE0, SND_STONE1, SND_STONE2,
    SND_TICK,       /* capture: stone dropping into the lid */
    SND_MOVE,       /* cursor */
    SND_SELECT,
    SND_BACK,
    SND_PASS,
    SND_HINT,
    SND_WIN,
    SND_LOSE,
    SND_ATARI,
    SND_COUNT
};

int audio_init(void);
void audio_quit(void);
void audio_play(int snd, float vol, float pan, int delay_ms);
void audio_set_enabled(int on);

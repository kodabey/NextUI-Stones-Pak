/* Stones — a Go game for the TrimUI Brick (NextUI pak).
 * Native C + SDL2. Board/AI in go.c / ai.c, drawing helpers in gfx.c, synth in audio.c. */
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai.h"
#include "audio.h"
#include "gfx.h"
#include "go.h"

#define APP_VERSION "1.0"

/* ------------------------------------------------------------------ palette */
#define C_INK RGBA(14, 12, 10, 255)
#define C_PAPER RGBA(242, 234, 218, 255)
#define C_MUTED RGBA(176, 162, 140, 255)
#define C_DIM RGBA(120, 108, 92, 255)
#define C_RED RGBA(222, 74, 50, 255)
#define C_GOLD RGBA(246, 200, 110, 255)
#define C_LINE RGBA(52, 34, 16, 215)
static Col with_a(Col c, int a) { c.a = (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a); return c; }

/* ------------------------------------------------------------------ input */
enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L1, BTN_R1,
       BTN_SELECT, BTN_START, BTN_MENU, BTN_COUNT };
/* TrimUI tg5040 joystick button numbers (from NextUI platform.h) */
#define JOY_A 1
#define JOY_B 0
#define JOY_X 3
#define JOY_Y 2
#define JOY_L1 4
#define JOY_R1 5
#define JOY_SELECT 6
#define JOY_START 7
#define JOY_MENU 8

static int held[BTN_COUNT], pressed[BTN_COUNT];
static Uint32 held_since[BTN_COUNT], last_rep[BTN_COUNT];
static int hat_state;
static int running = 1;

static void btn_set(int b, int down) {
    if (b < 0) return;
    if (down && !held[b]) {
        pressed[b] = 1;
        held_since[b] = last_rep[b] = SDL_GetTicks();
    }
    held[b] = down;
}

static int key_to_btn(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: case SDLK_w: return BTN_UP;
    case SDLK_DOWN: case SDLK_s: return BTN_DOWN;
    case SDLK_LEFT: case SDLK_a: return BTN_LEFT;
    case SDLK_RIGHT: case SDLK_d: return BTN_RIGHT;
    case SDLK_SPACE: case SDLK_RETURN: return BTN_A;
    case SDLK_BACKSPACE: case SDLK_z: return BTN_B;
    case SDLK_p: return BTN_X;
    case SDLK_h: return BTN_Y;
    case SDLK_LEFTBRACKET: return BTN_L1;
    case SDLK_RIGHTBRACKET: return BTN_R1;
    case SDLK_t: case SDLK_TAB: return BTN_SELECT;
    case SDLK_ESCAPE: return BTN_START;
    case SDLK_q: return BTN_MENU;
    }
    return -1;
}
static int joy_to_btn(int b) {
    switch (b) {
    case JOY_A: return BTN_A;
    case JOY_B: return BTN_B;
    case JOY_X: return BTN_X;
    case JOY_Y: return BTN_Y;
    case JOY_L1: return BTN_L1;
    case JOY_R1: return BTN_R1;
    case JOY_SELECT: return BTN_SELECT;
    case JOY_START: return BTN_START;
    case JOY_MENU: return BTN_MENU;
    }
    return -1;
}

static void poll_input(void) {
    memset(pressed, 0, sizeof(pressed));
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: if (!e.key.repeat) btn_set(key_to_btn(e.key.keysym.sym), 1); break;
        case SDL_KEYUP: btn_set(key_to_btn(e.key.keysym.sym), 0); break;
        case SDL_JOYBUTTONDOWN: btn_set(joy_to_btn(e.jbutton.button), 1); break;
        case SDL_JOYBUTTONUP: btn_set(joy_to_btn(e.jbutton.button), 0); break;
        case SDL_JOYHATMOTION: {
            int v = e.jhat.value;
            hat_state = v;
            btn_set(BTN_UP, (v & SDL_HAT_UP) != 0);
            btn_set(BTN_DOWN, (v & SDL_HAT_DOWN) != 0);
            btn_set(BTN_LEFT, (v & SDL_HAT_LEFT) != 0);
            btn_set(BTN_RIGHT, (v & SDL_HAT_RIGHT) != 0);
            break;
        }
        case SDL_JOYAXISMOTION:
            if (e.jaxis.axis == 0) { btn_set(BTN_LEFT, e.jaxis.value < -16000); btn_set(BTN_RIGHT, e.jaxis.value > 16000); }
            if (e.jaxis.axis == 1) { btn_set(BTN_UP, e.jaxis.value < -16000); btn_set(BTN_DOWN, e.jaxis.value > 16000); }
            break;
        case SDL_JOYDEVICEADDED: SDL_JoystickOpen(e.jdevice.which); break;
        }
    }
    /* auto-repeat for the d-pad */
    Uint32 now = SDL_GetTicks();
    for (int b = BTN_UP; b <= BTN_RIGHT; b++) {
        if (held[b] && now - held_since[b] > 230 && now - last_rep[b] > 55) {
            pressed[b] = 1;
            last_rep[b] = now;
        }
    }
}

/* ------------------------------------------------------------------ personas */
enum { Q_START, Q_CAPTURES, Q_LOSES_STONES, Q_AHEAD, Q_BEHIND, Q_RESIGN, Q_WINS, Q_LOSES, Q_PASS, Q_ATARI, Q_COUNT };
typedef struct {
    const char *name, *kanji, *level, *blurb;
    int playouts[3];        /* per board size 9/13/19 */
    int threads, time_ms;
    float temp;
    Col tint;
    const char *q[Q_COUNT][3];
} Persona;

#define NPERSONA 4
#define OPP_FRIEND 4
static const Persona PERSONA[NPERSONA] = {
    {"Pebble", "石", "Beginner", "Learned the rules this morning. Loves corners. Easily distracted.",
     {220, 200, 180}, 1, 1500, 1.0f, {118, 150, 108, 255},
     {{"Ooh, a game! Go easy on me?", "I brought snacks. Let's play!", "Black goes first, right? Right?"},
      {"I caught some! Is that allowed?", "Yoink!", "Look, prisoners!"},
      {"Hey! Those were my favorites.", "Wait, where did they go?", "Ouch."},
      {"Am I... winning?", "This is going great!", NULL},
      {"This is fine. Everything's fine.", "The stones are confusing today.", NULL},
      {"I think the stones have spoken. You win!", NULL, NULL},
      {"I won?! I won!", "Beginner's luck counts!", NULL},
      {"Good game! Teach me that trick?", "Rematch? Please?", NULL},
      {"I'll pass. Is that a thing?", NULL, NULL},
      {"Atari! Did I say that right?", "Your stones look lonely.", NULL}}},
    {"Brook", "川", "Casual", "Patient. Flows around obstacles and waits for gaps.",
     {1600, 1300, 1100}, 2, 3000, 0.35f, {88, 140, 172, 255},
     {{"Let's see where the water flows.", "A calm board. Shall we?", NULL},
      {"The current carries them away.", "Water finds the gaps.", NULL},
      {"Hm. A dam I didn't see.", "Well played.", NULL},
      {"The river runs my way.", NULL, NULL},
      {"Rapids ahead...", "I'll find another channel.", NULL},
      {"The river yields. Well played.", NULL, NULL},
      {"Water wears down stone.", NULL, NULL},
      {"You were the stronger current today.", NULL, NULL},
      {"Nothing left to carve. I pass.", NULL, NULL},
      {"Atari. Your stones are taking on water.", NULL, NULL}}},
    {"Mountain", "山", "Club player", "Solid shape, thick walls. Very hard to push around.",
     {7000, 5000, 4000}, 3, 6000, 0.08f, {150, 118, 86, 255},
     {{"Build well. I will.", "Thickness first, then attack.", NULL},
      {"Weak shapes crumble.", "Rockslide.", NULL},
      {"A small stone falls. The mountain stands.", "Interesting.", NULL},
      {"Slow and steady.", NULL, NULL},
      {"The slope is steeper than I thought.", NULL, NULL},
      {"I concede. Your play was sound.", NULL, NULL},
      {"Solid shape wins in the end.", NULL, NULL},
      {"You moved the mountain. Impressive.", NULL, NULL},
      {"The borders are set. I pass.", NULL, NULL},
      {"Atari. Check your liberties.", NULL, NULL}}},
    {"Dragon", "龍", "Strong", "Reads deep and hunts weak groups. Thinks a while.",
     {30000, 22000, 16000}, 3, 11000, 0.0f, {200, 70, 52, 255},
     {{"Show me your fighting spirit.", "Let us dance among the stones.", NULL},
      {"Delicious.", "Your group was weak.", "Fire!"},
      {"A worthy strike.", "Hmph. A scratch.", NULL},
      {"Your groups look nervous.", "Move 37 energy.", NULL},
      {"You fight well, little one.", NULL, NULL},
      {"Remarkable. I resign.", NULL, NULL},
      {"The dragon is pleased.", NULL, NULL},
      {"You have slain a dragon. Rest well.", NULL, NULL},
      {"I've said all I need to. Pass.", NULL, NULL},
      {"Atari. Run if you can.", NULL, NULL}}},
};

/* ------------------------------------------------------------------ layout */
static int SW, SH;              /* screen */
static int slab_x, slab_y, slab; /* board slab */
static int px0, pw;              /* panel */
static int gn = 9;               /* grid size currently laid out */
static float gsp, gmar;          /* grid spacing / margin */
static float shake_x, shake_y, shake_amt;

static void layout_grid(int n) {
    gn = n;
    gsp = slab / (n + 0.25f);
    gmar = (slab - gsp * (n - 1)) / 2.0f;
    gfx_make_stones((int)(gsp * 0.485f));
}
static float GX(int x) { return slab_x + gmar + x * gsp + shake_x; }
static float GY(int y) { return slab_y + gmar + y * gsp + shake_y; }

static const char *COLS = "ABCDEFGHJKLMNOPQRST";
static void coord_name(const Board *b, int p, char *out) {
    if (p == PASS) { strcpy(out, "Pass"); return; }
    sprintf(out, "%c%d", COLS[pt_x(b, p)], b->n - pt_y(b, p));
}

/* ------------------------------------------------------------------ persistence */
static char data_dir[512] = "godata";
static int opt_sound = 1, opt_atari = 1;
static int set_opp = 1, set_size = 0, set_color = 0, set_handicap = 0; /* setup choices */
static const int SIZES[3] = {9, 13, 19};
static int stats[NPERSONA][2]; /* wins, losses for the human */

static void mkdirs(const char *path) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void data_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", data_dir, name); }

static void save_settings(void) {
    char p[600];
    data_path(p, sizeof(p), "settings.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d %d\n", opt_sound, opt_atari, set_opp, set_size, set_color, set_handicap);
    for (int i = 0; i < NPERSONA; i++) fprintf(f, "%d %d\n", stats[i][0], stats[i][1]);
    fclose(f);
}
static void load_settings(void) {
    char p[600];
    data_path(p, sizeof(p), "settings.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fscanf(f, "%d %d %d %d %d %d", &opt_sound, &opt_atari, &set_opp, &set_size, &set_color, &set_handicap) != 6) {
        opt_sound = opt_atari = 1;
    }
    for (int i = 0; i < NPERSONA; i++) if (fscanf(f, "%d %d", &stats[i][0], &stats[i][1]) != 2) break;
    fclose(f);
    if (set_opp < 0 || set_opp > OPP_FRIEND) set_opp = 1;
    if (set_size < 0 || set_size > 2) set_size = 0;
    if (set_color < 0 || set_color > 2) set_color = 0;
    if (set_handicap < 0 || set_handicap > 9) set_handicap = 0;
}

/* ------------------------------------------------------------------ background jobs */
enum { JOB_NONE, JOB_AI, JOB_PONDER, JOB_SCORE, JOB_ATTRACT };
typedef struct {
    int kind, active, tag;
    Board b;
    SearchParams sp;
    SearchResult *res;
    volatile int stop, finished;
    volatile float live;
    pthread_t th;
} Job;
static Job job;

static void *job_thread(void *arg) {
    (void)arg;
    search_run(&job.b, &job.sp, job.res, &job.stop, &job.live);
    __sync_synchronize();
    job.finished = 1;
    return NULL;
}
static void job_start(int kind, const Board *b, int playouts, int time_ms, int threads, int tag) {
    if (!job.res) job.res = malloc(sizeof(SearchResult));
    job.kind = kind;
    job.tag = tag;
    job.b = *b;
    job.sp.playouts = playouts;
    job.sp.time_ms = time_ms;
    job.sp.threads = threads;
    job.sp.seed = (unsigned)SDL_GetTicks() * 2654435761u + (unsigned)kind;
    job.stop = 0;
    job.finished = 0;
    job.live = -1;
    job.active = 1;
    pthread_create(&job.th, NULL, job_thread, NULL);
}
static int job_ready(void) {
    if (!job.active || !job.finished) return 0;
    pthread_join(job.th, NULL);
    job.active = 0;
    return 1;
}
static void job_cancel(void) {
    if (!job.active) return;
    job.stop = 1;
    pthread_join(job.th, NULL);
    job.active = 0;
    job.kind = JOB_NONE;
}

static int cpu_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) return 1;
    return n >= 4 ? 3 : (int)n - 1 > 0 ? (int)n - 1 : 1;
}

/* ------------------------------------------------------------------ effects */
enum { FX_FLY, FX_SPARK, FX_RING, FX_TEXT };
typedef struct {
    int kind, color;
    float x0, y0, x1, y1, x, y, vx, vy;
    float t, delay, life, size;
    Col c;
    char text[24];
    int font;
} Fx;
#define MAXFX 512
static Fx fx[MAXFX];
static int nfx;

static Fx *fx_new(int kind) {
    if (nfx >= MAXFX) return NULL;
    Fx *f = &fx[nfx++];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    return f;
}
static unsigned fxrng = 99;
static float frnd(void) { return (rng_next(&fxrng) & 0xffff) / 65535.0f; }

static void fx_sparks(float x, float y, int n, Col c, float speed) {
    for (int i = 0; i < n; i++) {
        Fx *f = fx_new(FX_SPARK);
        if (!f) return;
        float a = frnd() * 6.2832f, s = speed * (0.3f + frnd());
        f->x = x; f->y = y; f->vx = cosf(a) * s; f->vy = sinf(a) * s - speed * 0.2f;
        f->life = 0.45f + frnd() * 0.5f;
        f->size = gsp * (0.25f + frnd() * 0.35f);
        f->c = c;
    }
}
static void fx_ring(float x, float y, Col c, float size, float life) {
    Fx *f = fx_new(FX_RING);
    if (!f) return;
    f->x = x; f->y = y; f->c = c; f->size = size; f->life = life;
}
static void fx_text(float x, float y, const char *s, Col c, int font, float life) {
    Fx *f = fx_new(FX_TEXT);
    if (!f) return;
    f->x = x; f->y = y; f->c = c; f->font = font; f->life = life;
    snprintf(f->text, sizeof(f->text), "%s", s);
}

static void fx_update(float dt) {
    int j = 0;
    for (int i = 0; i < nfx; i++) {
        Fx *f = &fx[i];
        if (f->delay > 0) { f->delay -= dt; fx[j++] = *f; continue; }
        f->t += dt;
        if (f->kind == FX_SPARK) {
            f->x += f->vx * dt; f->y += f->vy * dt;
            f->vy += 380 * dt;
            f->vx *= 0.97f;
        }
        if (f->t < f->life) fx[j++] = *f;
        else if (f->kind == FX_FLY) audio_play(SND_TICK, 0.5f + frnd() * 0.3f, 0.8f, 0);
    }
    nfx = j;
}

static void fx_draw(void) {
    for (int i = 0; i < nfx; i++) {
        Fx *f = &fx[i];
        if (f->delay > 0) {
            if (f->kind == FX_FLY) { /* still sitting on the board, fading slightly */
                draw_sprite(G.stone[f->color], f->x0, f->y0, G.stone_r * 2 + 4, RGBA(255, 255, 255, 255));
            }
            continue;
        }
        float k = f->t / f->life;
        switch (f->kind) {
        case FX_FLY: {
            float e = k < 0.5f ? 2 * k * k : 1 - powf(-2 * k + 2, 2) / 2;
            float x = f->x0 + (f->x1 - f->x0) * e;
            float y = f->y0 + (f->y1 - f->y0) * e - sinf(k * 3.14159f) * 120;
            float sz = (G.stone_r * 2 + 4) * (1 + 0.35f * sinf(k * 3.14159f)) * (1 - 0.55f * e);
            draw_sprite(G.shadow, x + 10 * (1 - e), y + 18 * sinf(k * 3.14f), sz * 1.1f, RGBA(255, 255, 255, 150));
            draw_sprite(G.stone[f->color], x, y, sz, RGBA(255, 255, 255, 255));
            break;
        }
        case FX_SPARK:
            draw_sprite(G.glow, f->x, f->y, f->size * (1 - k * 0.5f), with_a(f->c, (int)(f->c.a * (1 - k))));
            break;
        case FX_RING: {
            float s = f->size * (0.6f + k * 0.9f);
            draw_sprite(G.ring, f->x, f->y, s, with_a(f->c, (int)(f->c.a * (1 - k) * (1 - k))));
            break;
        }
        case FX_TEXT: {
            float rise = 46 * (1 - (1 - k) * (1 - k));
            int a = k < 0.7f ? 255 : (int)(255 * (1 - (k - 0.7f) / 0.3f));
            int y = (int)(f->y - rise);
            draw_text(f->font, f->text, (int)f->x + 2, y + 3, RGBA(0, 0, 0, a / 2), ALIGN_C);
            draw_text(f->font, f->text, (int)f->x, y, with_a(f->c, a), ALIGN_C);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ game */
enum { SCR_TITLE, SCR_SETUP, SCR_PLAY, SCR_SCORE, SCR_RESULT, SCR_HELP };
static int screen = SCR_TITLE;

typedef struct {
    int size, opp, human, handicap;
    float komi;
    Board *hist;
    int *mv;
    int nh, cap;
    int cx, cy;
    Uint32 think_start;
    int an_valid, an_nh;
    SearchResult *an;
    float bar, bar_target;
    int show_hint, hint_pending, show_terr;
    int over, resigned;   /* resigned = colour that resigned */
    uint8_t dead[MAXP], terr[MAXP];
    float final_score;
    char quip[160];
    Uint32 quip_time;
    int said_ahead, said_behind;
    char status[96];
    Uint32 status_time;
    Uint32 placed[MAXP];
    int paused, pause_sel;
    int score_ready;
    int recorded;
    int confirm_resign;
} Game;
static Game gm;

static Board *cur(void) { return &gm.hist[gm.nh - 1]; }
static int is_ai(int color) { return gm.opp < OPP_FRIEND && color != gm.human; }
static const Persona *persona(void) { return gm.opp < OPP_FRIEND ? &PERSONA[gm.opp] : NULL; }

static void set_status(const char *s) {
    snprintf(gm.status, sizeof(gm.status), "%s", s);
    gm.status_time = SDL_GetTicks();
}
static void say(int q, int force) {
    const Persona *p = persona();
    if (!p) return;
    Uint32 now = SDL_GetTicks();
    if (!force && now - gm.quip_time < 4500 && gm.quip[0]) return;
    int n = 0;
    while (n < 3 && p->q[q][n]) n++;
    if (!n) return;
    snprintf(gm.quip, sizeof(gm.quip), "%s", p->q[q][rng_next(&fxrng) % n]);
    gm.quip_time = now;
}

static const char *color_name(int c) { return c == BLACK ? "Black" : "White"; }
static const char *player_name(int c) {
    if (gm.opp == OPP_FRIEND) return color_name(c);
    return c == gm.human ? "You" : PERSONA[gm.opp].name;
}

static void push_board(const Board *b, int mv) {
    if (gm.nh >= gm.cap) {
        gm.cap = gm.cap ? gm.cap * 2 : 256;
        gm.hist = realloc(gm.hist, sizeof(Board) * gm.cap);
        gm.mv = realloc(gm.mv, sizeof(int) * gm.cap);
    }
    gm.hist[gm.nh] = *b;
    gm.mv[gm.nh] = mv;
    gm.nh++;
}

static void make_start_board(Board *b, int size, int handicap, float komi) {
    board_init(b, size, komi);
    if (handicap >= 2) {
        int xy[18];
        board_handicap_points(size, handicap, xy);
        for (int i = 0; i < handicap; i++) {
            b->to_move = BLACK;
            board_play(b, pt(b, xy[2 * i], xy[2 * i + 1]));
        }
        b->to_move = WHITE;
        b->nmoves = 0;
        b->last = b->last2 = NONE;
        b->passes = 0;
        b->ko = 0;
    }
}

static void save_game(void) {
    char p[600];
    data_path(p, sizeof(p), "save.txt");
    if (screen != SCR_PLAY && screen != SCR_SCORE) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "STONES1 %d %d %d %d %.1f %d\n", gm.size, gm.opp, gm.human, gm.handicap, gm.komi, gm.nh - 1);
    for (int i = 1; i < gm.nh; i++) fprintf(f, "%d %d\n", gm.mv[i] == PASS ? -1 : pt_x(cur(), gm.mv[i]), gm.mv[i] == PASS ? -1 : pt_y(cur(), gm.mv[i]));
    fclose(f);
}
static void delete_save(void) {
    char p[600];
    data_path(p, sizeof(p), "save.txt");
    remove(p);
}
static int save_exists(void) {
    char p[600];
    data_path(p, sizeof(p), "save.txt");
    return access(p, R_OK) == 0;
}

static void game_reset_common(void) {
    gm.nh = 0;
    gm.an_valid = 0;
    gm.show_hint = gm.hint_pending = gm.show_terr = 0;
    gm.over = gm.resigned = 0;
    gm.quip[0] = gm.status[0] = 0;
    gm.said_ahead = gm.said_behind = 0;
    gm.paused = 0;
    gm.recorded = 0;
    gm.bar = gm.bar_target = 0.5f;
    memset(gm.placed, 0, sizeof(gm.placed));
    nfx = 0;
    if (!gm.an) gm.an = malloc(sizeof(SearchResult));
}

static void game_new(int opp, int size, int human, int handicap) {
    job_cancel();
    game_reset_common();
    gm.opp = opp;
    gm.size = size;
    gm.human = human;
    gm.handicap = handicap;
    gm.komi = handicap >= 2 ? 0.5f : 7.5f;
    Board b;
    make_start_board(&b, size, handicap, gm.komi);
    push_board(&b, NONE);
    gm.cx = gm.cy = size / 2;
    layout_grid(size);
    screen = SCR_PLAY;
    say(Q_START, 1);
    save_game();
}

static int game_load(void) {
    char p[600];
    data_path(p, sizeof(p), "save.txt");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int size, opp, human, hc, n;
    float komi;
    if (fscanf(f, "STONES1 %d %d %d %d %f %d", &size, &opp, &human, &hc, &komi, &n) != 6 ||
        (size != 9 && size != 13 && size != 19) || opp < 0 || opp > OPP_FRIEND) { fclose(f); return 0; }
    job_cancel();
    game_reset_common();
    gm.opp = opp; gm.size = size; gm.human = human; gm.handicap = hc; gm.komi = komi;
    Board b;
    make_start_board(&b, size, hc, komi);
    push_board(&b, NONE);
    for (int i = 0; i < n; i++) {
        int x, y;
        if (fscanf(f, "%d %d", &x, &y) != 2) break;
        int m = x < 0 ? PASS : pt(&b, x, y);
        if (board_play(&b, m) < 0) break;
        push_board(&b, m);
    }
    fclose(f);
    /* never resume straight into a finished game */
    if (cur()->passes >= 2) { gm.nh--; }
    gm.cx = gm.cy = size / 2;
    if (cur()->last > 0) { gm.cx = pt_x(cur(), cur()->last); gm.cy = pt_y(cur(), cur()->last); }
    layout_grid(size);
    screen = SCR_PLAY;
    set_status("Welcome back.");
    return 1;
}

/* Save finished game as SGF. */
static void save_sgf(void) {
    char dir[600], p[700];
    data_path(dir, sizeof(dir), "games");
    mkdirs(dir);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
    snprintf(p, sizeof(p), "%s/%s.sgf", dir, stamp);
    FILE *f = fopen(p, "w");
    if (!f) return;
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", tm);
    char res[32];
    if (gm.resigned) snprintf(res, sizeof(res), "%c+R", gm.resigned == BLACK ? 'W' : 'B');
    else snprintf(res, sizeof(res), "%c+%.1f", gm.final_score > 0 ? 'B' : 'W', fabsf(gm.final_score));
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]AP[Stones:%s]SZ[%d]KM[%.1f]DT[%s]PB[%s]PW[%s]RE[%s]RU[Chinese]",
            APP_VERSION, gm.size, gm.komi, date, player_name(BLACK), player_name(WHITE), res);
    if (gm.handicap >= 2) {
        int xy[18];
        board_handicap_points(gm.size, gm.handicap, xy);
        fprintf(f, "HA[%d]AB", gm.handicap);
        for (int i = 0; i < gm.handicap; i++) fprintf(f, "[%c%c]", 'a' + xy[2 * i], 'a' + xy[2 * i + 1]);
    }
    for (int i = 1; i < gm.nh; i++) {
        const Board *b = &gm.hist[i - 1];
        int m = gm.mv[i];
        if (m == PASS) fprintf(f, ";%c[]", b->to_move == BLACK ? 'B' : 'W');
        else fprintf(f, ";%c[%c%c]", b->to_move == BLACK ? 'B' : 'W', 'a' + pt_x(b, m), 'a' + pt_y(b, m));
    }
    fprintf(f, ")\n");
    fclose(f);
}

static void record_result(void) {
    if (gm.recorded) return;
    gm.recorded = 1;
    int winner = gm.resigned ? OPP(gm.resigned) : gm.final_score > 0 ? BLACK : WHITE;
    if (gm.opp < OPP_FRIEND) {
        if (winner == gm.human) stats[gm.opp][0]++;
        else stats[gm.opp][1]++;
        save_settings();
    }
    save_sgf();
    delete_save();
}

static void finish_game(void) {
    job_cancel();
    gm.over = 1;
    record_result();
    screen = SCR_RESULT;
    int winner = gm.resigned ? OPP(gm.resigned) : gm.final_score > 0 ? BLACK : WHITE;
    int human_won = gm.opp == OPP_FRIEND || winner == gm.human;
    if (gm.opp < OPP_FRIEND) say(winner == gm.human ? (gm.resigned ? Q_RESIGN : Q_LOSES) : Q_WINS, 1);
    audio_play(human_won ? SND_WIN : SND_LOSE, 0.9f, 0, 0);
    if (human_won) {
        Col cs[4] = {C_GOLD, C_RED, C_PAPER, RGBA(140, 200, 160, 255)};
        for (int i = 0; i < 7; i++) {
            float x = slab_x + slab * (0.15f + 0.7f * frnd()), y = slab_y + slab * (0.2f + 0.5f * frnd());
            int start = nfx;
            fx_sparks(x, y, 24, cs[i % 4], 520);
            for (int k = start; k < nfx; k++) { fx[k].delay = i * 0.2f; fx[k].size *= 1.4f; }
        }
    }
}

/* strings in atari that weren't before (for alerts) */
static void announce_atari(const Board *before, const Board *after, int victim) {
    uint8_t done[MAXP];
    memset(done, 0, sizeof(done));
    for (int p = 0; p < after->s * after->s; p++) {
        if (after->c[p] != victim) continue;
        int h = after->head[p];
        if (done[h]) continue;
        done[h] = 1;
        if (!board_in_atari(after, h)) continue;
        /* was this string (any stone of it) already in atari before? */
        int was = before->c[p] == victim && board_in_atari(before, before->head[p]);
        if (was) continue;
        /* find top-most stone of the string */
        int r = h, best = h;
        do { if (pt_y(after, r) < pt_y(after, best)) best = r; r = after->nxt[r]; } while (r != h);
        if (opt_atari) {
            fx_text(GX(pt_x(after, best)), GY(pt_y(after, best)) - G.stone_r - 30, "ATARI", C_RED, gm.size >= 19 ? F_SMALL : F_BODY, 1.4f);
            audio_play(SND_ATARI, 0.7f, 0, 120);
        }
        if (is_ai(OPP(victim)) && frnd() < 0.5f) say(Q_ATARI, 0);
        return;
    }
}

static int prisoner_target_y(int color);

static void do_move(int m) {
    Board *b0 = cur();
    Board nb = *b0;
    int mover = nb.to_move;
    int caps = board_play(&nb, m);
    if (caps < 0) return;
    job_cancel();
    gm.show_hint = gm.hint_pending = 0;
    gm.an_valid = 0;
    if (m == PASS) {
        audio_play(SND_PASS, 0.8f, 0, 0);
        char s[64];
        snprintf(s, sizeof(s), "%s passed.", player_name(mover));
        set_status(s);
        if (is_ai(mover)) say(Q_PASS, 1);
        push_board(&nb, m);
        save_game();
        if (nb.passes >= 2) {
            /* both passed: count */
            job_start(JOB_SCORE, &nb, nb.n == 9 ? 3000 : 2500, 5000, cpu_threads(), gm.nh);
            screen = SCR_SCORE;
            gm.score_ready = 0;
            set_status("Counting...");
        }
        return;
    }
    int x = pt_x(&nb, m), y = pt_y(&nb, m);
    gm.placed[m] = SDL_GetTicks();
    audio_play(SND_STONE0 + (int)(frnd() * 3) % 3, 0.95f, (x / (float)(nb.n - 1)) - 0.5f, 0);
    fx_ring(GX(x), GY(y), RGBA(255, 255, 255, 110), gsp * 1.4f, 0.35f);
    if (caps > 0) {
        int k = 0;
        for (int p = 0; p < nb.s * nb.s; p++) {
            if (b0->c[p] == OPP(mover) && nb.c[p] == EMPTY) {
                Fx *f = fx_new(FX_FLY);
                if (!f) break;
                f->color = OPP(mover);
                f->x0 = GX(pt_x(&nb, p)); f->y0 = GY(pt_y(&nb, p));
                f->x1 = px0 + 44; f->y1 = prisoner_target_y(mover);
                f->delay = 0.12f + k * 0.045f;
                f->life = 0.6f;
                fx_sparks(f->x0, f->y0, 5, mover == BLACK ? C_GOLD : C_PAPER, 260);
                k++;
            }
        }
        if (caps >= 3) shake_amt = fminf(18, 4 + caps * 1.5f);
        char s[16];
        snprintf(s, sizeof(s), "+%d", caps);
        fx_text(GX(x), GY(y) - G.stone_r - 8, s, C_GOLD, caps >= 5 ? F_HEAD : F_BODY, 1.1f);
        if (is_ai(mover)) say(Q_CAPTURES, caps >= 3);
        else if (gm.opp < OPP_FRIEND && (caps >= 2 || frnd() < 0.5f)) say(Q_LOSES_STONES, caps >= 3);
    }
    push_board(&nb, m);
    announce_atari(b0, cur(), OPP(mover));
    save_game();
}

static void undo(void) {
    if (gm.nh <= 1) { set_status("Nothing to undo."); return; }
    job_cancel();
    int target = gm.nh - 1;
    if (gm.opp < OPP_FRIEND) {
        /* back to the previous position where it is the human's turn */
        int t = gm.nh - 2;
        while (t >= 0 && gm.hist[t].to_move != gm.human) t--;
        if (t < 0) { set_status("Nothing to undo."); return; }
        target = t + 1;
    }
    gm.nh = target;
    gm.an_valid = 0;
    gm.show_hint = gm.hint_pending = 0;
    audio_play(SND_BACK, 0.7f, 0, 0);
    set_status("Move taken back.");
    if (cur()->last > 0) { gm.cx = pt_x(cur(), cur()->last); gm.cy = pt_y(cur(), cur()->last); }
    save_game();
}

static float ownership_score(const SearchResult *r, const Board *b) {
    float s = 0;
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) s += r->owner[pt(b, x, y)];
    return s - b->komi;
}

static void ai_apply(const SearchResult *r) {
    Board *b = cur();
    int me = b->to_move;
    float est = ownership_score(r, b);
    float my_est = me == BLACK ? est : -est;
    if (r->winrate < 0.06f && b->nmoves > b->n * b->n / 2 && r->playouts >= 200) {
        gm.resigned = me;
        say(Q_RESIGN, 1);
        finish_game();
        return;
    }
    int m;
    /* accept the opponent's pass only once the board is mostly settled */
    int unsettled = 0;
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++)
            if (b->c[pt(b, x, y)] == EMPTY && fabsf(r->owner[pt(b, x, y)]) < 0.6f) unsettled++;
    int settled = unsettled <= b->n * b->n / 12 || b->nmoves > b->n * b->n * 0.7f;
    if (b->last == PASS && my_est > 0.5f && r->winrate > 0.5f && settled) m = PASS;
    else {
        unsigned rs = SDL_GetTicks() ^ 0x5bd1e995u;
        m = search_pick(r, persona()->temp, &rs);
    }
    if (m != PASS && !board_legal(b, m, me)) m = r->best;
    if (m != PASS && !board_legal(b, m, me)) m = PASS;
    /* commentary on how things are going */
    if (r->winrate > 0.82f && !gm.said_ahead && b->nmoves > 10) { gm.said_ahead = 1; say(Q_AHEAD, 0); }
    if (r->winrate < 0.22f && !gm.said_behind && b->nmoves > 10) { gm.said_behind = 1; say(Q_BEHIND, 0); }
    gm.bar_target = r->black_wr;
    do_move(m);
    if (m != PASS) { gm.cx = pt_x(cur(), m); gm.cy = pt_y(cur(), m); }
}

static int size_idx(int n) { return n == 9 ? 0 : n == 13 ? 1 : 2; }

static void compute_dead_from(const SearchResult *r) {
    Board *b = cur();
    memset(gm.dead, 0, sizeof(gm.dead));
    for (int p = 0; p < b->s * b->s; p++) {
        if (b->c[p] == BLACK && r->owner[p] < -0.25f) gm.dead[p] = 1;
        if (b->c[p] == WHITE && r->owner[p] > 0.25f) gm.dead[p] = 1;
    }
    gm.final_score = board_area_score(b, gm.dead, gm.terr);
}

static void toggle_dead(int p) {
    Board *b = cur();
    int c = b->c[p];
    if (c != BLACK && c != WHITE) return;
    int nd = !gm.dead[p];
    /* flood over same-coloured stones, connected orthogonally or diagonally */
    int stack[MAXP], sp = 0;
    uint8_t seen[MAXP];
    memset(seen, 0, sizeof(seen));
    stack[sp++] = p;
    seen[p] = 1;
    int s = b->s;
    int d[8] = {-1, 1, -s, s, -s - 1, -s + 1, s - 1, s + 1};
    while (sp) {
        int q = stack[--sp];
        gm.dead[q] = (uint8_t)nd;
        for (int i = 0; i < 8; i++) {
            int t = q + d[i];
            if (t > 0 && t < s * s && !seen[t] && b->c[t] == c) { seen[t] = 1; stack[sp++] = t; }
        }
    }
    gm.final_score = board_area_score(b, gm.dead, gm.terr);
    audio_play(SND_TICK, 0.6f, 0, 0);
}

/* ------------------------------------------------------------------ board drawing */
static void draw_slab_shadow(void) {
    draw_sprite_rect(G.glow, slab_x - 60, slab_y - 30, slab + 120, slab + 110, RGBA(0, 0, 0, 230));
}

static void draw_grid(int n, int alpha) {
    SDL_Rect dst = {slab_x + (int)shake_x, slab_y + (int)shake_y, slab, slab};
    SDL_SetTextureAlphaMod(G.wood, 255);
    SDL_RenderCopy(G.r, G.wood, NULL, &dst);
    Col lc = with_a(C_LINE, alpha * 215 / 255);
    int lw = gsp > 50 ? 2 : gsp > 30 ? 2 : 1;
    float x0 = GX(0), y0 = GY(0), x1 = GX(n - 1), y1 = GY(n - 1);
    for (int i = 0; i < n; i++) {
        int w = (i == 0 || i == n - 1) ? lw + 1 : lw;
        fill_rect((int)(x0 - w / 2.0f), (int)lroundf(GY(i) - w / 2.0f), (int)(x1 - x0) + w, w, lc);
        fill_rect((int)lroundf(GX(i) - w / 2.0f), (int)(y0 - w / 2.0f), w, (int)(y1 - y0) + w, lc);
    }
    int stars[3][2] = {{2, 6}, {3, 9}, {3, 15}};
    int si = n == 9 ? 0 : n == 13 ? 1 : 2;
    int pts[3] = {stars[si][0], n / 2, stars[si][1]};
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            if (n == 9 && ((a == 1) != (b == 1))) continue;
            if (n == 13 && ((a == 1) != (b == 1))) continue;
            draw_sprite(G.disc, GX(pts[a]), GY(pts[b]), fmaxf(7, gsp * 0.15f), lc);
        }
}

static void draw_stone_at(int color, float x, float y, float scale, int alpha) {
    float d = (G.stone_r * 2 + 4) * scale;
    float lift = (scale - 1) * 40;
    draw_sprite(G.shadow, x + G.stone_r * 0.12f + lift * 0.4f, y + G.stone_r * 0.2f + lift * 0.6f, d * 1.04f, RGBA(255, 255, 255, alpha));
    draw_sprite(G.stone[color], x, y, d, RGBA(255, 255, 255, alpha));
}

static void draw_stones(const Board *b, const uint8_t *dead, int dim) {
    Uint32 now = SDL_GetTicks();
    /* hide stones currently flying away */
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) {
            int p = pt(b, x, y), c = b->c[p];
            if (c != BLACK && c != WHITE) continue;
            float scale = 1;
            Uint32 t = gm.placed[p] && now - gm.placed[p] < 260 ? now - gm.placed[p] : 260;
            if ((screen == SCR_PLAY || screen == SCR_TITLE) && t < 260) {
                float k = t / 260.0f;
                scale = 1 + 0.22f * (1 - k) * (1 - k) - 0.04f * sinf(k * 3.14159f) * (1 - k);
            }
            int a = dead && dead[p] ? 95 : dim ? 150 : 255;
            draw_stone_at(c, GX(x), GY(y), scale, a);
        }
}

static void draw_last_marker(const Board *b) {
    if (b->last <= 0 || b->c[b->last] == EMPTY) return;
    float x = GX(pt_x(b, b->last)), y = GY(pt_y(b, b->last));
    draw_sprite(G.ring, x, y, G.stone_r * 0.95f, C_RED);
}

static void draw_atari_alerts(const Board *b) {
    if (!opt_atari) return;
    float pulse = 0.5f + 0.5f * sinf(SDL_GetTicks() / 160.0f);
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) {
            int p = pt(b, x, y);
            if (b->c[p] != BLACK && b->c[p] != WHITE) continue;
            if (!board_in_atari(b, b->head[p])) continue;
            draw_sprite(G.ring, GX(x), GY(y), G.stone_r * 2.3f, with_a(C_RED, (int)(90 + 120 * pulse)));
        }
}

static void draw_ownership(const Board *b, const float *own) {
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) {
            int p = pt(b, x, y);
            float o = own[p];
            if (fabsf(o) < 0.15f) continue;
            if ((b->c[p] == BLACK && o > 0) || (b->c[p] == WHITE && o < 0)) continue;
            float s = gsp * 0.42f * fminf(1, fabsf(o));
            Col c = o > 0 ? RGBA(20, 20, 24, 190) : RGBA(246, 242, 232, 210);
            fill_rect((int)(GX(x) - s / 2), (int)(GY(y) - s / 2), (int)s, (int)s, c);
        }
}

static void draw_territory(const Board *b) {
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) {
            int p = pt(b, x, y);
            int own = gm.terr[p];
            if (own != BLACK && own != WHITE) continue;
            if (b->c[p] == own && !gm.dead[p]) continue;
            float s = gsp * 0.34f;
            Col c = own == BLACK ? RGBA(18, 18, 22, 235) : RGBA(248, 244, 236, 240);
            fill_rect((int)(GX(x) - s / 2), (int)(GY(y) - s / 2), (int)s, (int)s, c);
        }
}

static void draw_cursor(const Board *b, int color) {
    float x = GX(gm.cx), y = GY(gm.cy);
    int p = pt(b, gm.cx, gm.cy);
    int legal = board_legal(b, p, color);
    float pulse = 0.5f + 0.5f * sinf(SDL_GetTicks() / 210.0f);
    if (legal) draw_sprite(G.stone[color], x, y, G.stone_r * 2 + 4, RGBA(255, 255, 255, 120));
    float h = G.stone_r * 1.18f, L = G.stone_r * 0.5f;
    int t = gsp > 40 ? 4 : 3;
    Col c = legal ? with_a(C_GOLD, 170 + (int)(85 * pulse)) : with_a(C_RED, 220);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            float cx = x + sx * h, cy = y + sy * h;
            fill_rect((int)(sx < 0 ? cx : cx - L), (int)(cy - t / 2.0f), (int)L, t, c);
            fill_rect((int)(cx - t / 2.0f), (int)(sy < 0 ? cy : cy - L), t, (int)L, c);
        }
}

static int hint_moves(int *out, float *wr) {
    if (!gm.an_valid || gm.an_nh != gm.nh) return 0;
    int n = 0;
    for (int i = 0; i < gm.an->ncand && n < 3; i++) {
        Candidate *c = &gm.an->cand[i];
        if (c->move == PASS) continue;
        if (n > 0 && c->visits < gm.an->cand[0].visits * 0.06f) break;
        out[n] = c->move;
        wr[n] = c->winrate;
        n++;
    }
    return n;
}

static void draw_hints(const Board *b) {
    int mv[3];
    float wr[3];
    int n = hint_moves(mv, wr);
    float pulse = 0.5f + 0.5f * sinf(SDL_GetTicks() / 180.0f);
    for (int i = n - 1; i >= 0; i--) {
        float x = GX(pt_x(b, mv[i])), y = GY(pt_y(b, mv[i]));
        int a = i == 0 ? 255 : 170;
        draw_sprite(G.glow, x, y, G.stone_r * (3.2f + pulse * 0.5f), with_a(C_GOLD, a * 3 / 4));
        draw_sprite(G.ring, x, y, G.stone_r * 1.8f, with_a(C_GOLD, a));
        char s[4];
        snprintf(s, sizeof(s), "%d", (i + 1) % 10);
        int f = gsp > 50 ? F_BODY : F_SMALL;
        draw_text(f, s, (int)x + 1, (int)(y - text_height(f) / 2.0f) + 2, RGBA(0, 0, 0, 160), ALIGN_C);
        draw_text(f, s, (int)x, (int)(y - text_height(f) / 2.0f), RGBA(60, 30, 0, 255), ALIGN_C);
    }
}

/* ------------------------------------------------------------------ panel */
static void draw_button_glyph(int x, int y, const char *label, Col bg) {
    int w = text_width(F_SMALL, label);
    if (strlen(label) <= 1) {
        draw_sprite(G.disc, x + 15, y + 15, 32, bg);
        draw_text(F_SMALL, label, x + 15, y + 15 - text_height(F_SMALL) / 2, C_INK, ALIGN_C);
    } else {
        round_rect(x, y + 1, w + 14, 30, 15, bg);
        draw_text(F_SMALL, label, x + 7, y + 16 - text_height(F_SMALL) / 2, C_INK, ALIGN_L);
    }
}
static int button_width(const char *label) { return strlen(label) <= 1 ? 32 : text_width(F_SMALL, label) + 14; }

/* Legend of (button, label) pairs anchored to the panel bottom; cols = 1 or 2. */
static void draw_legend(const char *const *items, int n, int bottom, int cols) {
    int x0 = px0 + 18, w = pw - 36;
    int item_w[16], right_max = 0;
    for (int i = 0; i < n && i < 16; i++) {
        item_w[i] = button_width(items[2 * i]) + 6 + text_width(F_SMALL, items[2 * i + 1]);
        if (cols == 2 && i % 2 == 1 && item_w[i] > right_max) right_max = item_w[i];
    }
    int col2 = w / 2;
    if (cols == 2 && w - right_max < col2) col2 = w - right_max;
    int nrows = cols == 2 ? (n + 1) / 2 : n;
    int y0 = bottom - nrows * 42;
    for (int i = 0; i < n && i < 16; i++) {
        int col = cols == 2 ? i % 2 : 0, row = cols == 2 ? i / 2 : i;
        int xx = x0 + (col ? col2 : 0), yy = y0 + row * 42;
        const char *btn = items[2 * i], *lab = items[2 * i + 1];
        draw_button_glyph(xx, yy, btn, RGBA(196, 184, 164, 255));
        draw_text(F_SMALL, lab, xx + button_width(btn) + 6, yy + 16 - text_height(F_SMALL) / 2, C_MUTED, ALIGN_L);
    }
}

static void draw_portrait(int cx, int cy, int r, const char *kanji, Col tint, int glow) {
    if (glow) draw_sprite(G.glow, cx, cy, r * 3.2f, with_a(tint, 120));
    draw_sprite(G.disc, cx, cy, r * 2, tint);
    draw_sprite(G.ring, cx, cy, r * 2.1f, RGBA(255, 240, 220, 90));
    draw_text(F_HEAD, kanji, cx, cy - text_height(F_HEAD) / 2 - 2, RGBA(255, 248, 236, 255), ALIGN_C);
}

static int row_y[3];
static int prisoner_target_y(int color) { return row_y[color] ? row_y[color] + 22 : 300; }

static void draw_play_panel(void) {
    Board *b = cur();
    int x0 = px0 + 18, w = pw - 36;
    Uint32 now = SDL_GetTicks();
    const Persona *ps = persona();
    int ai_turn = !gm.over && screen == SCR_PLAY && is_ai(b->to_move);
    /* header */
    int y = 26;
    if (ps) {
        draw_portrait(x0 + 38, y + 38, 36, ps->kanji, ps->tint, ai_turn);
        int nf = text_width(F_HEAD, ps->name) > w - 88 ? F_BODY : F_HEAD;
        draw_text(nf, ps->name, x0 + 88, nf == F_HEAD ? y - 4 : y + 8, C_PAPER, ALIGN_L);
        draw_text(F_SMALL, ps->level, x0 + 90, y + 46, C_MUTED, ALIGN_L);
    } else {
        draw_portrait(x0 + 38, y + 38, 36, "友", RGBA(110, 120, 150, 255), 0);
        draw_text(F_HEAD, "Friends", x0 + 88, y - 4, C_PAPER, ALIGN_L);
        draw_text(F_SMALL, "Pass the Brick", x0 + 90, y + 46, C_MUTED, ALIGN_L);
    }
    y += 96;
    /* quip bubble */
    if (ps && gm.quip[0]) {
        Uint32 age = now - gm.quip_time;
        int a = age < 9000 ? 255 : age < 11000 ? 255 - (age - 9000) * 150 / 2000 : 105;
        round_rect(x0, y, w, 92, 14, RGBA(255, 246, 230, a * 22 / 255));
        draw_text_wrap(F_SMALL, gm.quip, x0 + 14, y + 10, w - 28, with_a(C_PAPER, a), 2);
    }
    y += 128;
    /* players */
    for (int c = BLACK; c <= WHITE; c++) {
        int ry = y + (c - 1) * 60;
        row_y[c] = ry;
        int active = !gm.over && b->to_move == c && screen == SCR_PLAY;
        if (active) round_rect(x0 - 8, ry - 6, w + 16, 58, 12, RGBA(255, 230, 190, 26));
        draw_sprite(G.shadow, x0 + 26 + 3, ry + 22 + 4, 44, RGBA(255, 255, 255, 200));
        draw_sprite(G.stone[c], x0 + 26, ry + 22, 42, RGBA(255, 255, 255, 255));
        char nm[48];
        snprintf(nm, sizeof(nm), "%s", player_name(c));
        if (active && is_ai(c)) {
            int dots = (now / 350) % 4;
            snprintf(nm, sizeof(nm), "%s%.*s", player_name(c), dots, "...");
        }
        draw_text(F_BODY, nm, x0 + 58, ry + 2, active ? C_PAPER : C_MUTED, ALIGN_L);
        char cp[32];
        snprintf(cp, sizeof(cp), "%d", b->caps[c]);
        draw_text(F_BODY, cp, x0 + w, ry + 2, active ? C_GOLD : C_DIM, ALIGN_R);
    }
    draw_text(F_SMALL, "prisoners", x0 + w, y - 28, C_DIM, ALIGN_R);
    y += 126;
    /* win bar */
    float wr = gm.bar;
    char bl[16], wl[16];
    snprintf(bl, sizeof(bl), "B %d%%", (int)lroundf(wr * 100));
    snprintf(wl, sizeof(wl), "W %d%%", 100 - (int)lroundf(wr * 100));
    draw_text(F_SMALL, bl, x0, y, C_PAPER, ALIGN_L);
    draw_text(F_SMALL, wl, x0 + w, y, C_PAPER, ALIGN_R);
    y += 34;
    int bw = (int)(w * wr);
    round_rect(x0 - 2, y - 2, w + 4, 26, 13, RGBA(255, 240, 220, 60));
    round_rect(x0, y, w, 22, 11, RGBA(238, 233, 222, 255));
    if (bw > 0) round_rect(x0, y, bw < 22 ? 22 : bw, 22, 11, RGBA(22, 22, 26, 255));
    fill_rect(x0 + w / 2, y - 4, 2, 30, RGBA(222, 74, 50, 180));
    y += 40;

    /* info */
    char info[64];
    if (!(gm.show_hint || gm.hint_pending) || !gm.show_terr) {
        snprintf(info, sizeof(info), "Move %d · Komi %.1f", b->nmoves, gm.komi);
        draw_text(F_SMALL, info, x0, y, C_DIM, ALIGN_L);
        y += 32;
    }
    if (gm.show_terr && gm.an_valid) {
        float est = ownership_score(gm.an, b);
        snprintf(info, sizeof(info), "Estimate: %c+%.1f", est > 0 ? 'B' : 'W', fabsf(est));
        draw_text(F_SMALL, info, x0, y, C_GOLD, ALIGN_L);
        y += 30;
    } else if (gm.show_terr) {
        draw_text(F_SMALL, "Estimating...", x0, y, C_GOLD, ALIGN_L);
        y += 30;
    }
    /* hints */
    if (gm.show_hint || gm.hint_pending) {
        int mv[3];
        float hw[3];
        int n = gm.show_hint ? hint_moves(mv, hw) : 0;
        if (!n) draw_text(F_SMALL, "Reading the board...", x0, y, C_GOLD, ALIGN_L);
        for (int i = 0; i < n; i++) {
            char nm[16], s[48];
            coord_name(b, mv[i], nm);
            snprintf(s, sizeof(s), "%d  %s", i + 1, nm);
            draw_text(F_SMALL, s, x0, y + i * 28, C_GOLD, ALIGN_L);
            snprintf(s, sizeof(s), "%d%%", (int)lroundf(hw[i] * 100));
            draw_text(F_SMALL, s, x0 + w, y + i * 28, C_GOLD, ALIGN_R);
        }
        y += 28 * (n ? n : 1) + 6;
    }
    /* status */
    if (gm.status[0] && now - gm.status_time < 3500) {
        int a = now - gm.status_time < 2800 ? 255 : 255 - (int)(now - gm.status_time - 2800) * 255 / 700;
        draw_text_wrap(F_SMALL, gm.status, x0, y, w, with_a(C_RED, a), 0);
    }
    /* legend */
    static const char *leg[] = {"A", "Place", "B", "Undo", "X", "Pass", "Y", "Hint", "SEL", "Map", "START", "Menu"};
    draw_legend(leg + 8, 2, SH - 12, 1);
    draw_legend(leg, 4, SH - 12 - 84, 2);
}

/* ------------------------------------------------------------------ pause menu */
enum { PM_RESUME, PM_UNDO, PM_SOUND, PM_ATARI, PM_RESIGN, PM_TITLE, PM_QUIT, PM_COUNT };
static void pause_label(int i, char *out) {
    switch (i) {
    case PM_RESUME: strcpy(out, "Resume"); break;
    case PM_UNDO: strcpy(out, "Take back move"); break;
    case PM_SOUND: sprintf(out, "Sound: %s", opt_sound ? "on" : "off"); break;
    case PM_ATARI: sprintf(out, "Atari alerts: %s", opt_atari ? "on" : "off"); break;
    case PM_RESIGN: strcpy(out, gm.confirm_resign ? "Really resign?" : "Resign"); break;
    case PM_TITLE: strcpy(out, "Save & exit to title"); break;
    case PM_QUIT: strcpy(out, "Quit to NextUI"); break;
    }
}

static void draw_pause(void) {
    fill_rect(0, 0, SW, SH, RGBA(8, 6, 4, 170));
    int w = 460, h = 70 + PM_COUNT * 58, x = slab_x + (slab - w) / 2, y = slab_y + (slab - h) / 2;
    draw_sprite_rect(G.glow, x - 50, y - 40, w + 100, h + 90, RGBA(0, 0, 0, 220));
    round_rect(x, y, w, h, 18, RGBA(34, 28, 23, 250));
    round_rect(x + 2, y + 2, w - 4, 4, 2, RGBA(222, 74, 50, 255));
    draw_text(F_SMALL, "PAUSED", x + w / 2, y + 22, C_DIM, ALIGN_C);
    for (int i = 0; i < PM_COUNT; i++) {
        char s[64];
        pause_label(i, s);
        int iy = y + 60 + i * 58;
        if (i == gm.pause_sel) round_rect(x + 18, iy, w - 36, 50, 12, RGBA(255, 230, 190, 34));
        draw_text(F_BODY, s, x + w / 2, iy + 25 - text_height(F_BODY) / 2, i == gm.pause_sel ? C_PAPER : C_MUTED, ALIGN_C);
    }
}

static void pause_update(void) {
    if (pressed[BTN_UP]) { gm.pause_sel = (gm.pause_sel + PM_COUNT - 1) % PM_COUNT; gm.confirm_resign = 0; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_DOWN]) { gm.pause_sel = (gm.pause_sel + 1) % PM_COUNT; gm.confirm_resign = 0; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_B] || pressed[BTN_START] || pressed[BTN_MENU]) { gm.paused = 0; gm.confirm_resign = 0; audio_play(SND_BACK, 0.6f, 0, 0); return; }
    if (!pressed[BTN_A]) return;
    switch (gm.pause_sel) {
    case PM_RESUME: gm.paused = 0; audio_play(SND_SELECT, 0.6f, 0, 0); break;
    case PM_UNDO: gm.paused = 0; undo(); break;
    case PM_SOUND: opt_sound = !opt_sound; audio_set_enabled(opt_sound); save_settings(); audio_play(SND_SELECT, 0.6f, 0, 0); break;
    case PM_ATARI: opt_atari = !opt_atari; save_settings(); audio_play(SND_SELECT, 0.6f, 0, 0); break;
    case PM_RESIGN:
        if (!gm.confirm_resign) { gm.confirm_resign = 1; break; }
        gm.paused = 0;
        gm.confirm_resign = 0;
        gm.resigned = gm.opp == OPP_FRIEND ? cur()->to_move : gm.human;
        finish_game();
        break;
    case PM_TITLE: gm.paused = 0; job_cancel(); save_game(); screen = SCR_TITLE; audio_play(SND_BACK, 0.6f, 0, 0); break;
    case PM_QUIT: save_game(); running = 0; break;
    }
}

/* ------------------------------------------------------------------ play screen */
static int min_think_ms(void) { return gm.opp == 0 ? 900 : 650; }

static void play_update(float dt) {
    Board *b = cur();
    (void)dt;
    if (gm.paused) { pause_update(); return; }
    if (pressed[BTN_START] || pressed[BTN_MENU]) { gm.paused = 1; gm.pause_sel = 0; audio_play(SND_SELECT, 0.5f, 0, 0); return; }

    int ai_turn = is_ai(b->to_move);
    /* background search management */
    if (job.active && job_ready()) {
        if (job.kind == JOB_AI && job.tag == gm.nh) {
            /* apply after a minimum "thinking" time so moves don't snap */
            gm.an_valid = 0;
            *gm.an = *job.res;
            gm.an_valid = 2; /* 2 = pending AI move */
            gm.an_nh = gm.nh;
        } else if (job.kind == JOB_PONDER && job.tag == gm.nh) {
            *gm.an = *job.res;
            gm.an_valid = 1;
            gm.an_nh = gm.nh;
            gm.bar_target = gm.an->black_wr;
            if (gm.hint_pending) { gm.hint_pending = 0; gm.show_hint = 1; audio_play(SND_HINT, 0.7f, 0, 0); }
        }
        job.kind = JOB_NONE;
    }
    if (ai_turn) {
        if (gm.an_valid == 2 && gm.an_nh == gm.nh) {
            if (SDL_GetTicks() - gm.think_start >= (Uint32)min_think_ms()) {
                gm.an_valid = 0;
                SearchResult *r = malloc(sizeof(SearchResult));
                *r = *gm.an;
                ai_apply(r);
                free(r);
                return;
            }
        } else if (!job.active) {
            const Persona *ps = persona();
            int si = size_idx(gm.size);
            job_start(JOB_AI, b, ps->playouts[si], ps->time_ms, ps->threads > cpu_threads() ? cpu_threads() : ps->threads, gm.nh);
            gm.think_start = SDL_GetTicks();
        }
    } else if (!job.active && !(gm.an_valid && gm.an_nh == gm.nh)) {
        int pl = gm.size == 9 ? 5000 : gm.size == 13 ? 3500 : 3000;
        job_start(JOB_PONDER, b, pl, 4500, cpu_threads() > 2 ? 2 : cpu_threads(), gm.nh);
    }
    if (job.active && job.live >= 0) gm.bar_target = job.live;

    /* cursor (allowed any time so you can look around) */
    int n = b->n;
    if (pressed[BTN_LEFT]) { gm.cx = (gm.cx + n - 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_RIGHT]) { gm.cx = (gm.cx + 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_UP]) { gm.cy = (gm.cy + n - 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_DOWN]) { gm.cy = (gm.cy + 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_L1] && b->last > 0) { gm.cx = pt_x(b, b->last); gm.cy = pt_y(b, b->last); }
    if (pressed[BTN_SELECT]) { gm.show_terr = !gm.show_terr; audio_play(SND_SELECT, 0.4f, 0, 0); }
    if (pressed[BTN_B]) { undo(); return; }
    if (ai_turn) return;

    if (pressed[BTN_A]) {
        int p = pt(b, gm.cx, gm.cy);
        if (board_legal(b, p, b->to_move)) do_move(p);
        else {
            set_status(b->c[p] != EMPTY ? "That point is taken." : p == b->ko ? "Ko! Play elsewhere first." : "No liberties there.");
            audio_play(SND_BACK, 0.5f, 0, 0);
            shake_amt = 3;
        }
    } else if (pressed[BTN_X]) {
        do_move(PASS);
    } else if (pressed[BTN_Y]) {
        if (gm.show_hint || gm.hint_pending) gm.show_hint = gm.hint_pending = 0;
        else if (gm.an_valid == 1 && gm.an_nh == gm.nh) { gm.show_hint = 1; audio_play(SND_HINT, 0.7f, 0, 0); }
        else gm.hint_pending = 1;
    }
}

static void play_draw(void) {
    Board *b = cur();
    draw_slab_shadow();
    draw_grid(b->n, 255);
    if (gm.show_terr && gm.an_valid && gm.an_nh == gm.nh) draw_ownership(b, gm.an->owner);
    draw_stones(b, NULL, 0);
    draw_atari_alerts(b);
    draw_last_marker(b);
    if (gm.show_hint) draw_hints(b);
    if (!is_ai(b->to_move) && !gm.paused) draw_cursor(b, b->to_move);
    fx_draw();
    draw_play_panel();
    if (gm.paused) draw_pause();
}

/* ------------------------------------------------------------------ scoring screen */
static void score_update(void) {
    Board *b = cur();
    if (!gm.score_ready && job.active && job_ready()) {
        compute_dead_from(job.res);
        *gm.an = *job.res;
        gm.score_ready = 1;
        set_status("");
    }
    if (!gm.score_ready) return;
    int n = b->n;
    if (pressed[BTN_LEFT]) { gm.cx = (gm.cx + n - 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_RIGHT]) { gm.cx = (gm.cx + 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_UP]) { gm.cy = (gm.cy + n - 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_DOWN]) { gm.cy = (gm.cy + 1) % n; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_A]) toggle_dead(pt(b, gm.cx, gm.cy));
    if (pressed[BTN_B]) {
        /* resume play: drop the final pass */
        gm.nh--;
        gm.an_valid = 0;
        screen = SCR_PLAY;
        set_status("Play continues.");
        audio_play(SND_BACK, 0.6f, 0, 0);
        save_game();
        return;
    }
    if (pressed[BTN_START] || pressed[BTN_X]) finish_game();
}

static void score_draw(void) {
    Board *b = cur();
    draw_slab_shadow();
    draw_grid(b->n, 255);
    draw_stones(b, gm.score_ready ? gm.dead : NULL, 0);
    if (gm.score_ready) {
        draw_territory(b);
        float x = GX(gm.cx), y = GY(gm.cy);
        draw_sprite(G.ring, x, y, G.stone_r * 2.6f, with_a(C_GOLD, 200 + (int)(55 * sinf(SDL_GetTicks() / 200.0f))));
    }
    fx_draw();
    int x0 = px0 + 18, w = pw - 36, y = 34;
    draw_text(F_HEAD, "Counting", x0, y, C_PAPER, ALIGN_L);
    y += 64;
    if (!gm.score_ready) {
        int dots = (SDL_GetTicks() / 350) % 4;
        char s[32];
        snprintf(s, sizeof(s), "Settling the board%.*s", dots, "...");
        draw_text(F_SMALL, s, x0, y, C_MUTED, ALIGN_L);
        return;
    }
    y += draw_text_wrap(F_SMALL, "Both players passed. Stones marked faded are counted as dead — tap a group with A if I got it wrong.", x0, y, w, C_MUTED, 3) + 18;
    /* tallies */
    int st[3] = {0}, te[3] = {0};
    for (int yy = 0; yy < b->n; yy++)
        for (int xx = 0; xx < b->n; xx++) {
            int p = pt(b, xx, yy);
            if ((b->c[p] == BLACK || b->c[p] == WHITE) && !gm.dead[p]) st[b->c[p]]++;
            else if (gm.terr[p] == BLACK || gm.terr[p] == WHITE) te[gm.terr[p]]++;
        }
    for (int c = BLACK; c <= WHITE; c++) {
        draw_sprite(G.stone[c], x0 + 18, y + 18, 34, RGBA(255, 255, 255, 255));
        char s[64];
        if (c == WHITE) snprintf(s, sizeof(s), "%d + %d + %.1f", st[c], te[c], gm.komi);
        else snprintf(s, sizeof(s), "%d + %d", st[c], te[c]);
        draw_text(F_BODY, s, x0 + 46, y + 2, C_PAPER, ALIGN_L);
        y += 48;
    }
    draw_text(F_SMALL, "stones + area (+ komi)", x0, y, C_DIM, ALIGN_L);
    y += 48;
    char res[32];
    snprintf(res, sizeof(res), "%s +%.1f", gm.final_score > 0 ? "Black" : "White", fabsf(gm.final_score));
    draw_text(F_HEAD, res, x0, y, C_GOLD, ALIGN_L);
    static const char *leg[] = {"A", "Dead/alive", "START", "Confirm", "B", "Keep playing"};
    draw_legend(leg, 3, SH - 12, 1);
}

/* ------------------------------------------------------------------ result screen */
static void result_update(void) {
    if (pressed[BTN_A]) { screen = SCR_SETUP; audio_play(SND_SELECT, 0.6f, 0, 0); layout_grid(SIZES[set_size]); }
    else if (pressed[BTN_B] || pressed[BTN_START]) { screen = SCR_TITLE; audio_play(SND_BACK, 0.6f, 0, 0); }
}

static void result_draw(void) {
    Board *b = cur();
    draw_slab_shadow();
    draw_grid(b->n, 255);
    draw_stones(b, gm.resigned ? NULL : gm.dead, 0);
    if (!gm.resigned) draw_territory(b);
    fx_draw();
    int winner = gm.resigned ? OPP(gm.resigned) : gm.final_score > 0 ? BLACK : WHITE;
    int x0 = px0 + 18, w = pw - 36, y = 30;
    const Persona *ps = persona();
    char big[48], sub[64];
    if (gm.opp == OPP_FRIEND) snprintf(big, sizeof(big), "%s wins", color_name(winner));
    else snprintf(big, sizeof(big), "%s", winner == gm.human ? "You win!" : "You lose");
    if (gm.resigned) snprintf(sub, sizeof(sub), "%s resigned", player_name(gm.resigned));
    else snprintf(sub, sizeof(sub), "%s by %.1f points", color_name(winner), fabsf(gm.final_score));
    draw_text(F_SMALL, "GAME OVER", x0, y, C_DIM, ALIGN_L);
    y += 34;
    draw_text(F_HEAD, big, x0, y, winner == gm.human || gm.opp == OPP_FRIEND ? C_GOLD : C_PAPER, ALIGN_L);
    y += 60;
    draw_text(F_SMALL, sub, x0, y, C_MUTED, ALIGN_L);
    y += 50;
    if (ps) {
        draw_portrait(x0 + 34, y + 34, 32, ps->kanji, ps->tint, 0);
        if (gm.quip[0]) draw_text_wrap(F_SMALL, gm.quip, x0 + 80, y + 4, w - 80, C_PAPER, 2);
        y += 110;
        char rec[64];
        snprintf(rec, sizeof(rec), "Your record vs %s: %d–%d", ps->name, stats[gm.opp][0], stats[gm.opp][1]);
        draw_text_wrap(F_SMALL, rec, x0, y, w, C_MUTED, 2);
        y += 70;
    }
    draw_text_wrap(F_SMALL, "Game record saved (SGF) in Stones/games.", x0, y, w, C_DIM, 2);
    static const char *leg[] = {"A", "New game", "B", "Title"};
    draw_legend(leg, 2, SH - 12, 2);
}

/* ------------------------------------------------------------------ setup screen */
enum { SU_OPP, SU_SIZE, SU_COLOR, SU_HANDI, SU_START, SU_COUNT };
static int setup_sel = SU_START;
static Board preview;

static int max_handicap(void) { return SIZES[set_size] == 9 ? 4 : 9; }

static void refresh_preview(void) {
    if (set_handicap > max_handicap()) set_handicap = max_handicap();
    make_start_board(&preview, SIZES[set_size], set_handicap, 7.5f);
    layout_grid(SIZES[set_size]);
}

static void setup_update(void) {
    if (pressed[BTN_UP]) { setup_sel = (setup_sel + SU_COUNT - 1) % SU_COUNT; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_DOWN]) { setup_sel = (setup_sel + 1) % SU_COUNT; audio_play(SND_MOVE, 1, 0, 0); }
    int d = pressed[BTN_RIGHT] ? 1 : pressed[BTN_LEFT] ? -1 : 0;
    if (pressed[BTN_A] && setup_sel != SU_START) d = 1;
    if (d) {
        audio_play(SND_MOVE, 1, 0, 0);
        switch (setup_sel) {
        case SU_OPP: set_opp = (set_opp + d + 5) % 5; break;
        case SU_SIZE: set_size = (set_size + d + 3) % 3; break;
        case SU_COLOR: set_color = (set_color + d + 3) % 3; break;
        case SU_HANDI: {
            int m = max_handicap();
            /* 0, 2..m */
            int v = set_handicap + d;
            if (v == 1) v = d > 0 ? 2 : 0;
            if (v < 0) v = m;
            if (v > m) v = 0;
            set_handicap = v;
            break;
        }
        }
        refresh_preview();
    }
    if (pressed[BTN_B]) { screen = SCR_TITLE; audio_play(SND_BACK, 0.6f, 0, 0); save_settings(); return; }
    if ((pressed[BTN_A] && setup_sel == SU_START) || pressed[BTN_START]) {
        save_settings();
        int human = set_color == 0 ? BLACK : set_color == 1 ? WHITE : (rng_next(&fxrng) & 1 ? BLACK : WHITE);
        if (set_opp == OPP_FRIEND) human = BLACK;
        audio_play(SND_SELECT, 0.7f, 0, 0);
        delete_save();
        game_new(set_opp, SIZES[set_size], human, set_handicap);
        if (set_color == 2 && set_opp != OPP_FRIEND) set_status(human == BLACK ? "Nigiri: you play Black." : "Nigiri: you play White.");
    }
}

static void setup_draw(void) {
    draw_slab_shadow();
    draw_grid(preview.n, 255);
    draw_stones(&preview, NULL, 0);
    int x0 = px0 + 18, w = pw - 36, y = 24;
    draw_text(F_HEAD, "New game", x0, y, C_PAPER, ALIGN_L);
    y += 66;
    const char *labels[SU_COUNT] = {"Opponent", "Board", "You play", "Handicap", ""};
    for (int i = 0; i < SU_COUNT; i++) {
        char val[48];
        int iy = y + i * 84;
        switch (i) {
        case SU_OPP: snprintf(val, sizeof(val), "%s", set_opp == OPP_FRIEND ? "A friend" : PERSONA[set_opp].name); break;
        case SU_SIZE: snprintf(val, sizeof(val), "%d × %d", SIZES[set_size], SIZES[set_size]); break;
        case SU_COLOR: snprintf(val, sizeof(val), "%s", set_opp == OPP_FRIEND ? "Hot seat" : set_color == 0 ? "Black" : set_color == 1 ? "White" : "Random"); break;
        case SU_HANDI: if (set_handicap) snprintf(val, sizeof(val), "%d stones", set_handicap); else snprintf(val, sizeof(val), "None"); break;
        default: snprintf(val, sizeof(val), "Start  →"); break;
        }
        int sel = i == setup_sel;
        if (sel) round_rect(x0 - 10, iy - 6, w + 20, i == SU_START ? 60 : 74, 12, i == SU_START ? RGBA(222, 74, 50, 230) : RGBA(255, 230, 190, 30));
        if (i == SU_START) {
            draw_text(F_BODY, val, x0 + w / 2, iy + 24 - text_height(F_BODY) / 2, sel ? C_PAPER : C_MUTED, ALIGN_C);
            continue;
        }
        draw_text(F_SMALL, labels[i], x0, iy, C_DIM, ALIGN_L);
        draw_text(F_BODY, val, x0, iy + 28, sel ? C_PAPER : C_MUTED, ALIGN_L);
        if (sel) {
            draw_text(F_BODY, "‹", x0 + w - 44, iy + 26, C_GOLD, ALIGN_C);
            draw_text(F_BODY, "›", x0 + w - 8, iy + 26, C_GOLD, ALIGN_C);
        }
    }
    y += SU_COUNT * 84 + 6;
    if (set_opp < OPP_FRIEND) {
        const Persona *ps = &PERSONA[set_opp];
        draw_portrait(x0 + 26, y + 26, 24, ps->kanji, ps->tint, 0);
        draw_text(F_SMALL, ps->level, x0 + 62, y + 2, C_GOLD, ALIGN_L);
        char rec[48];
        snprintf(rec, sizeof(rec), "Record %d–%d", stats[set_opp][0], stats[set_opp][1]);
        draw_text(F_SMALL, rec, x0 + 62, y + 28, C_DIM, ALIGN_L);
        draw_text_wrap(F_SMALL, ps->blurb, x0, y + 66, w, C_MUTED, 2);
    } else {
        draw_portrait(x0 + 26, y + 26, 24, "友", RGBA(110, 120, 150, 255), 0);
        draw_text_wrap(F_SMALL, "Two players, one Brick. Hand it over after each move.", x0 + 62, y + 2, w - 62, C_MUTED, 2);
    }
}

/* ------------------------------------------------------------------ title screen */
static Board attract;
static Uint32 attract_next;
static int attract_fade;
static int title_sel;

enum { TM_CONTINUE, TM_NEW, TM_HELP, TM_QUIT, TM_COUNT };

static void attract_reset(void) {
    board_init(&attract, 9, 7.5f);
    attract_next = SDL_GetTicks() + 1200;
    attract_fade = 0;
}

static void title_enter(void) {
    job_cancel();
    layout_grid(9);
    attract_reset();
    title_sel = save_exists() ? TM_CONTINUE : TM_NEW;
}

static void title_update(void) {
    Uint32 now = SDL_GetTicks();
    /* attract-mode game in the background */
    if (job.active && job.kind == JOB_ATTRACT && job_ready()) {
        if (attract.passes < 2 && attract.nmoves < 90) {
            unsigned rs = now;
            int m = search_pick(job.res, 0.6f, &rs);
            Board before = attract;
            if (board_play(&attract, m) >= 0 && m > 0) {
                gm.placed[m] = now;
                audio_play(SND_STONE0 + (int)(frnd() * 3) % 3, 0.22f, pt_x(&attract, m) / 8.0f - 0.5f, 0);
                for (int p = 0; p < attract.s * attract.s; p++)
                    if (before.c[p] != EMPTY && attract.c[p] == EMPTY)
                        fx_sparks(GX(pt_x(&attract, p)), GY(pt_y(&attract, p)), 3, C_GOLD, 180);
            }
        }
        job.kind = JOB_NONE;
        attract_next = now + 900;
    }
    if (!job.active && now > attract_next) {
        if (attract.passes >= 2 || attract.nmoves >= 90) {
            if (!attract_fade) { attract_fade = 1; attract_next = now + 4000; }
            else attract_reset();
        } else {
            job_start(JOB_ATTRACT, &attract, 300, 600, 1, 0);
        }
    }
    int has_save = save_exists();
    if (pressed[BTN_UP]) { do title_sel = (title_sel + TM_COUNT - 1) % TM_COUNT; while (title_sel == TM_CONTINUE && !has_save); audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_DOWN]) { do title_sel = (title_sel + 1) % TM_COUNT; while (title_sel == TM_CONTINUE && !has_save); audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_A] || pressed[BTN_START]) {
        audio_play(SND_SELECT, 0.7f, 0, 0);
        switch (title_sel) {
        case TM_CONTINUE: job_cancel(); if (!game_load()) title_sel = TM_NEW; break;
        case TM_NEW: job_cancel(); screen = SCR_SETUP; setup_sel = SU_START; refresh_preview(); break;
        case TM_HELP: job_cancel(); screen = SCR_HELP; break;
        case TM_QUIT: running = 0; break;
        }
    }
}

static void title_draw(void) {
    draw_slab_shadow();
    draw_grid(9, 255);
    draw_stones(&attract, NULL, 0);
    draw_last_marker(&attract);
    fx_draw();
    /* soft veil over the board */
    fill_rect(slab_x, slab_y, slab, slab, RGBA(16, 12, 9, 28));
    int x0 = px0 + 18, w = pw - 36;
    /* vertical calligraphy column */
    int ky = 18;
    draw_text(F_HUGE, "囲", px0 + pw / 2 - 30, ky, C_PAPER, ALIGN_C);
    draw_text(F_HUGE, "碁", px0 + pw / 2 - 30, ky + 170, C_PAPER, ALIGN_C);
    /* red hanko seal */
    int sx = px0 + pw / 2 + 74, sy = ky + 262, ss = 62;
    round_rect(sx - ss / 2, sy, ss, ss, 8, C_RED);
    draw_text(F_HEAD, "道", sx, sy + ss / 2 - text_height(F_HEAD) / 2 - 2, C_PAPER, ALIGN_C);
    int y = 400;
    draw_text(F_SMALL, "S T O N E S", x0 + w / 2, y, C_MUTED, ALIGN_C);
    y += 48;
    int has_save = save_exists();
    const char *items[TM_COUNT] = {"Continue", "New game", "How to play", "Quit"};
    for (int i = 0; i < TM_COUNT; i++) {
        if (i == TM_CONTINUE && !has_save) continue;
        int sel = i == title_sel;
        if (sel) round_rect(x0 - 6, y - 6, w + 12, 52, 12, RGBA(255, 230, 190, 30));
        if (sel) draw_sprite(G.stone[BLACK], x0 + 18, y + 20, 26, RGBA(255, 255, 255, 255));
        draw_text(F_BODY, items[i], x0 + 42, y + 20 - text_height(F_BODY) / 2, sel ? C_PAPER : C_MUTED, ALIGN_L);
        y += 58;
    }
    draw_text(F_SMALL, "v" APP_VERSION, x0 + w, SH - 40, C_DIM, ALIGN_R);
}

/* ------------------------------------------------------------------ help screen */
typedef struct { const char *title, *body; const char *rows[9]; } HelpPage;
static const HelpPage HELP[] = {
    {"The board", "Take turns placing stones on the intersections. Black plays first. Stones never move, but they can be captured.",
     {".........", ".........", "..X......", "......O..", "....X....", "..O......", "......X..", ".........", "........."}},
    {"Liberties", "The empty points touching a group are its liberties (marked). A group down to its last liberty is in atari.",
     {".........", ".........", "...**....", "..*XX*...", "...**....", ".........", "....O....", "...OX*...", "....O...."}},
    {"Capture", "Fill a group's last liberty and it's captured: off the board it goes, into your prisoners.",
     {".........", ".........", "...X.....", "..XOX....", "...+.....", ".........", ".........", ".........", "........."}},
    {"Ko", "If capturing one stone would let your opponent instantly capture back and repeat the board, they must play elsewhere first.",
     {".........", ".........", "...XO....", "..X#XO...", "...XO....", ".........", ".........", ".........", "........."}},
    {"Winning", "When both players pass, count your stones plus the empty area you surround. White gets komi (7.5) for moving second.",
     {"..X.O....", "..X.O....", "..X.O....", "..X.OO...", "..XX.O...", "...X.O...", "...X.O...", "...X.O...", "...X.O..."}},
    {"Controls", "D-pad moves. A places, B takes back, X passes. Y asks for a hint, SELECT shows who owns what, START opens the menu. L1 jumps to the last move.",
     {".........", ".........", ".........", ".........", ".........", ".........", ".........", ".........", "........."}},
    {"Your opponents", "AlphaGo beat Lee Sedol in 2016 with neural networks plus Monte Carlo tree search. These opponents use the classic MCTS half: thousands of quick random games per move, run on your Brick's four cores.",
     {".........", ".X.....O.", ".........", ".........", "....X....", ".........", ".........", ".O.....X.", "........."}},
};
#define NHELP (int)(sizeof(HELP) / sizeof(HELP[0]))
static int help_page;

static void help_update(void) {
    if (pressed[BTN_RIGHT] || pressed[BTN_A] || pressed[BTN_R1]) {
        if (help_page < NHELP - 1) { help_page++; audio_play(SND_MOVE, 1, 0, 0); }
        else if (pressed[BTN_A]) { screen = SCR_TITLE; help_page = 0; }
    }
    if ((pressed[BTN_LEFT] || pressed[BTN_L1]) && help_page > 0) { help_page--; audio_play(SND_MOVE, 1, 0, 0); }
    if (pressed[BTN_B] || pressed[BTN_START]) { screen = SCR_TITLE; help_page = 0; audio_play(SND_BACK, 0.6f, 0, 0); }
}

static void help_draw(void) {
    const HelpPage *hp = &HELP[help_page];
    if (gn != 9) layout_grid(9);
    draw_slab_shadow();
    draw_grid(9, 255);
    float pulse = 0.5f + 0.5f * sinf(SDL_GetTicks() / 200.0f);
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            char ch = hp->rows[y][x];
            if (ch == 'X' || ch == 'O') draw_stone_at(ch == 'X' ? BLACK : WHITE, GX(x), GY(y), 1, 255);
            if (ch == '*') draw_sprite(G.ring, GX(x), GY(y), G.stone_r * 1.1f, with_a(C_RED, 160 + (int)(80 * pulse)));
            if (ch == '+') {
                draw_sprite(G.stone[BLACK], GX(x), GY(y), G.stone_r * 2 + 4, RGBA(255, 255, 255, 90 + (int)(60 * pulse)));
                draw_sprite(G.ring, GX(x), GY(y), G.stone_r * 2.4f, with_a(C_GOLD, 220));
            }
            if (ch == '#') {
                draw_sprite(G.stone[WHITE], GX(x), GY(y), G.stone_r * 2 + 4, RGBA(255, 255, 255, 90 + (int)(60 * pulse)));
                draw_sprite(G.ring, GX(x), GY(y), G.stone_r * 2.4f, with_a(C_RED, 220));
            }
        }
    int x0 = px0 + 18, w = pw - 36, y = 30;
    for (int i = 0; i < NHELP; i++)
        draw_sprite(G.disc, x0 + 8 + i * 22, SH - 96, 12, i == help_page ? C_GOLD : RGBA(120, 108, 92, 200));
    draw_text(F_HEAD, hp->title, x0, y, C_PAPER, ALIGN_L);
    y += 74;
    draw_text_wrap(F_BODY, hp->body, x0, y, w, C_MUTED, 6);
    static const char *leg[] = {"‹ ›", "Pages", "B", "Back"};
    draw_legend(leg, 2, SH - 12, 2);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    const char *env = getenv("STONES_DATA");
    if (env && *env) snprintf(data_dir, sizeof(data_dir), "%s", env);
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "--data")) snprintf(data_dir, sizeof(data_dir), "%s", argv[i + 1]);
    mkdirs(data_dir);
    load_settings();

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "no audio: %s\n", SDL_GetError());
    SDL_DisplayMode dm;
    SW = 1024; SH = 768;
    const char *wenv = getenv("STONES_WINDOW"); /* e.g. 1280x720 for desktop testing */
    int windowed = wenv && sscanf(wenv, "%dx%d", &SW, &SH) == 2;
    if (!windowed && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) { SW = dm.w; SH = dm.h; }
    SDL_Window *win = SDL_CreateWindow("Stones", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW, SH,
                                       windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_GetRendererOutputSize(ren, &SW, &SH);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);
    fprintf(stderr, "Stones %s: %dx%d, data %s, %d cpus\n", APP_VERSION, SW, SH, data_dir, (int)sysconf(_SC_NPROCESSORS_ONLN));

    /* layout: square board slab on the left, panel on the right */
    int gap = 16;
    slab = SH - 2 * gap;
    if (SW - slab - 2 * gap < 300) slab = SW - 300 - 2 * gap;
    slab_x = gap;
    slab_y = (SH - slab) / 2;
    px0 = slab_x + slab + gap;
    pw = SW - px0;

    gfx_init(ren, SW, SH, slab);
    audio_init();
    audio_set_enabled(opt_sound);
    title_enter();

    /* screenshot mode for testing: STONES_SHOT=<frames>:<file.bmp> */
    const char *shot = getenv("STONES_SHOT");
    int shot_frames = 0;
    char shot_file[256] = "";
    if (shot) sscanf(shot, "%d:%255s", &shot_frames, shot_file);
    const char *script = getenv("STONES_SCRIPT"); /* simulated key presses, for testing */
    int frame = 0;

    Uint32 last = SDL_GetTicks();
    int prev_screen = screen;
    while (running) {
        poll_input();
        if (script && *script) {
            /* format: "frame:key,frame:key" with key one of UDLRABXYSTMlr */
            const char *s = script;
            while (*s) {
                int f; char k;
                if (sscanf(s, "%d:%c", &f, &k) == 2 && f == frame) {
                    const char *map = "UDLRABXYlrSTM";
                    const char *q = strchr(map, k);
                    if (q) pressed[q - map] = 1;
                }
                s = strchr(s, ',');
                if (!s) break;
                s++;
            }
        }
        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        switch (screen) {
        case SCR_TITLE: title_update(); break;
        case SCR_SETUP: setup_update(); break;
        case SCR_PLAY: play_update(dt); break;
        case SCR_SCORE: score_update(); break;
        case SCR_RESULT: result_update(); break;
        case SCR_HELP: help_update(); break;
        }
        if (screen != prev_screen) {
            if (screen == SCR_TITLE) title_enter();
            if (screen == SCR_SETUP) refresh_preview();
            nfx = screen == SCR_RESULT ? nfx : 0;
            prev_screen = screen;
        }
        fx_update(dt);
        gm.bar += (gm.bar_target - gm.bar) * fminf(1, dt * 4);
        if (shake_amt > 0.2f) {
            shake_x = (frnd() - 0.5f) * shake_amt;
            shake_y = (frnd() - 0.5f) * shake_amt;
            shake_amt *= powf(0.002f, dt);
        } else shake_x = shake_y = shake_amt = 0;

        SDL_RenderCopy(ren, G.bg, NULL, NULL);
        switch (screen) {
        case SCR_TITLE: title_draw(); break;
        case SCR_SETUP: setup_draw(); break;
        case SCR_PLAY: play_draw(); break;
        case SCR_SCORE: score_draw(); break;
        case SCR_RESULT: result_draw(); break;
        case SCR_HELP: help_draw(); break;
        }
        frame++;
        if (shot_frames && frame == shot_frames) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SW, SH, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, shot_file);
            SDL_FreeSurface(s);
            running = 0;
        }
        SDL_RenderPresent(ren);
        Uint32 spent = SDL_GetTicks() - now;
        if (spent < 15) SDL_Delay(16 - spent); /* frame cap in case vsync isn't honoured */
    }
    job_cancel();
    if (screen == SCR_PLAY || screen == SCR_SCORE) save_game();
    save_settings();
    audio_quit();
    gfx_quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

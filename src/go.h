/* Go rules engine: fast incremental board with pseudo-liberties.
 * Atari detection uses the sum / sum-of-squares trick: a string is in atari
 * iff all its pseudo-liberties are the same point. */
#pragma once
#include <stdint.h>

#define MAXN 19
#define MAXS (MAXN + 2)
#define MAXP (MAXS * MAXS)
#define PASS 0
#define NONE (-1)

enum { EMPTY = 0, BLACK = 1, WHITE = 2, EDGE = 3 };
#define OPP(c) (3 - (c))

typedef struct {
    int n, s;               /* board size, stride (n+2) */
    int to_move;
    int ko;                 /* forbidden point or 0 */
    int last, last2;        /* last two moves (PASS = 0, NONE = -1 at start) */
    int passes;             /* consecutive passes */
    int nmoves;
    int caps[3];            /* stones captured BY color */
    float komi;
    uint8_t c[MAXP];
    int16_t head[MAXP];
    int16_t nxt[MAXP];
    int16_t libs[MAXP];     /* pseudo-liberty count (at head) */
    int16_t size[MAXP];
    int32_t lsum[MAXP];
    int64_t lsum2[MAXP];
    int16_t empt[MAXP];     /* list of empty points */
    int16_t epos[MAXP];     /* index of point in empt */
    int nempty;
} Board;

void board_init(Board *b, int n, float komi);
static inline int pt(const Board *b, int x, int y) { return (y + 1) * b->s + (x + 1); }
static inline int pt_x(const Board *b, int p) { return p % b->s - 1; }
static inline int pt_y(const Board *b, int p) { return p / b->s - 1; }

int board_legal(const Board *b, int p, int color);
/* Plays a move for b->to_move (p may be PASS). Returns stones captured, or -1 if illegal. */
int board_play(Board *b, int p);

int board_libs_real(const Board *b, int p, int *liblist, int max); /* exact liberties of string at p */
static inline int board_in_atari(const Board *b, int h) {
    return (int64_t)b->libs[h] * b->lsum2[h] == (int64_t)b->lsum[h] * b->lsum[h];
}
static inline int board_atari_lib(const Board *b, int h) { return b->lsum[h] / b->libs[h]; }
int board_is_eyeish(const Board *b, int p, int color);   /* all 4 nbrs color/edge */
int board_is_true_eye(const Board *b, int p, int color);

/* Area score from Black's perspective, assuming stones in dead[] (1 = dead) are removed.
 * terr (optional) receives owner per point (BLACK/WHITE/EMPTY for dame). */
float board_area_score(const Board *b, const uint8_t *dead, uint8_t *terr);
void board_handicap_points(int n, int count, int *out_xy /* pairs */);

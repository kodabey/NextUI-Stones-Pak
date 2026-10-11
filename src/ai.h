/* Monte Carlo tree search (UCT + RAVE, root-parallel) for the Go engine. */
#pragma once
#include "go.h"

typedef struct {
    int playouts;      /* stop after this many playouts (total over threads) */
    int time_ms;       /* ...or after this much time */
    int threads;
    unsigned seed;
} SearchParams;

typedef struct {
    int move;
    int visits;
    float winrate;     /* for the side to move at the root */
} Candidate;

typedef struct {
    int best;                 /* most-visited move (PASS possible) */
    float winrate;            /* side-to-move win probability of best move */
    float black_wr;           /* black's win probability */
    int ncand;
    Candidate cand[MAXP];     /* sorted by visits, descending */
    float owner[MAXP];        /* -1 (white) .. +1 (black) */
    int playouts;
    int ms;
} SearchResult;

/* Runs a full search, blocking. *stop may be set from another thread to abort early.
 * *live_black_wr (optional) is updated as the search progresses. */
void search_run(const Board *b, const SearchParams *sp, SearchResult *out,
                volatile int *stop, volatile float *live_black_wr);

/* Pick a move from a finished search with some human-like randomness.
 * temperature 0 = always best. */
int search_pick(const SearchResult *r, float temperature, unsigned *rng);

/* Random number helpers shared with the UI. */
static inline unsigned rng_next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x ? x : 0x9E3779B9u;
}

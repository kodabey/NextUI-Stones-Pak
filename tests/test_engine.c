#include "../src/go.h"
#include "../src/ai.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int P(Board *b, int x, int y) { return pt(b, x, y); }
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int fails;

static void play(Board *b, int x, int y) {
    int r = board_play(b, x < 0 ? PASS : P(b, x, y));
    if (r < 0) { printf("illegal %d,%d\n", x, y); fails++; }
}

static int count_libs_brute(const Board *b, int p) { return board_libs_real(b, p, NULL, 0); }

static void test_rules(void) {
    Board b;
    board_init(&b, 9, 7.5f);
    /* capture a single stone in the corner */
    play(&b, 0, 1); /* B */
    play(&b, 0, 0); /* W corner */
    play(&b, 1, 0); /* B captures */
    CHECK(b.c[P(&b, 0, 0)] == EMPTY);
    CHECK(b.caps[BLACK] == 1);
    /* white cannot play suicide at 0,0 */
    CHECK(!board_legal(&b, P(&b, 0, 0), WHITE));

    /* ko */
    board_init(&b, 9, 7.5f);
    /*  . B W .
        B W . W
        . B W .   */
    play(&b, 1, 0); play(&b, 2, 0);
    play(&b, 0, 1); play(&b, 1, 1);
    play(&b, 1, 2); play(&b, 3, 1);
    play(&b, 5, 5); play(&b, 2, 2);
    play(&b, 2, 1); /* B captures W at 1,1 -> ko */
    CHECK(b.c[P(&b, 1, 1)] == EMPTY);
    CHECK(b.ko == P(&b, 1, 1));
    CHECK(!board_legal(&b, P(&b, 1, 1), WHITE));
    play(&b, 7, 7); /* W ko threat elsewhere */
    CHECK(b.ko == 0);
    play(&b, 7, 6);
    CHECK(board_legal(&b, P(&b, 1, 1), WHITE));
    play(&b, 1, 1); /* W retakes */
    CHECK(b.c[P(&b, 2, 1)] == EMPTY);

    /* liberties & atari detection agree with brute force over random games */
    unsigned s = 42;
    for (int g = 0; g < 200; g++) {
        board_init(&b, g % 3 == 0 ? 19 : 9, 7.5f);
        for (int m = 0; m < 300 && b.passes < 2; m++) {
            int tries = 0, p;
            do { p = b.empt[rng_next(&s) % b.nempty]; } while (!board_legal(&b, p, b.to_move) && ++tries < 50);
            if (tries >= 50) p = PASS;
            board_play(&b, p);
            for (int q = 0; q < b.s * b.s; q++) {
                if (b.c[q] != BLACK && b.c[q] != WHITE) continue;
                int h = b.head[q];
                int real = count_libs_brute(&b, q);
                if ((real == 1) != board_in_atari(&b, h)) { printf("atari mismatch g%d m%d\n", g, m); fails++; goto next; }
                if (real == 0) { printf("zero libs g%d\n", g); fails++; goto next; }
                if (real == 1) {
                    int lib;
                    board_libs_real(&b, q, &lib, 1);
                    if (lib != board_atari_lib(&b, h)) { printf("atari lib mismatch\n"); fails++; goto next; }
                }
            }
            /* empty list consistent */
            int ne = 0;
            for (int q = 0; q < b.s * b.s; q++) if (b.c[q] == EMPTY) { ne++; if (b.empt[b.epos[q]] != q) { printf("epos bad\n"); fails++; goto next; } }
            if (ne != b.nempty) { printf("nempty bad\n"); fails++; goto next; }
        }
    next:;
    }

    /* scoring: black wall at column 4 on 9x9, white wall at column 5 */
    board_init(&b, 9, 7.5f);
    for (int y = 0; y < 9; y++) { play(&b, 4, y); play(&b, 5, y); }
    uint8_t terr[MAXP];
    float sc = board_area_score(&b, NULL, terr);
    CHECK(sc == 45 - 36 - 7.5f);
    CHECK(terr[P(&b, 0, 0)] == BLACK && terr[P(&b, 8, 8)] == WHITE);
}

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int game(int n, int pa, int pb, int threads, unsigned seed, int verbose) {
    /* returns winner colour; player A = black (pa playouts), B = white */
    Board b;
    board_init(&b, n, 7.5f);
    SearchResult *r = malloc(sizeof *r);
    unsigned rng = seed;
    while (b.passes < 2 && b.nmoves < n * n * 3) {
        SearchParams sp = {b.to_move == BLACK ? pa : pb, 20000, threads, rng_next(&rng)};
        search_run(&b, &sp, r, NULL, NULL);
        int mv = r->best;
        if (r->winrate < 0.05f && b.nmoves > n * n / 3) { free(r); return OPP(b.to_move); } /* resign */
        if (verbose && b.nmoves < 3) printf("  move %d: %d playouts in %d ms (%.0f/s) wr %.2f\n", b.nmoves, r->playouts, r->ms, r->playouts * 1000.0 / (r->ms + 1), r->winrate);
        board_play(&b, mv);
    }
    float sc = board_area_score(&b, NULL, NULL);
    free(r);
    return sc > 0 ? BLACK : WHITE;
}

int main(int argc, char **argv) {
    test_rules();
    printf("rules: %s\n", fails ? "FAILED" : "ok");
    if (argc > 1 && !strcmp(argv[1], "rules")) return fails;

    /* raw speed */
    Board b;
    for (int n = 9; n <= 19; n += (n == 13 ? 6 : 4)) {
        board_init(&b, n, 7.5f);
        SearchParams sp = {n == 9 ? 20000 : 4000, 60000, 1, 7};
        SearchResult *r = malloc(sizeof *r);
        double t = ms();
        search_run(&b, &sp, r, NULL, NULL);
        printf("%dx%d: %d playouts %.0f ms -> %.0f playouts/s (1 thread), best %d,%d wr %.2f\n", n, n, r->playouts,
               ms() - t, r->playouts / ((ms() - t) / 1000), pt_x(&b, r->best), pt_y(&b, r->best), r->winrate);
        free(r);
    }
    if (argc > 1 && !strcmp(argv[1], "speed")) return fails;

    /* strength sanity: 3000 vs 300 playouts on 9x9, alternating colours */
    int strong = 0, games = argc > 2 ? atoi(argv[2]) : 10;
    for (int g = 0; g < games; g++) {
        int w;
        if (g % 2 == 0) { w = game(9, 3000, 300, 2, g + 1, 0); strong += w == BLACK; }
        else { w = game(9, 300, 3000, 2, g + 1, 0); strong += w == WHITE; }
        printf("game %d: strong %s\n", g, ((g % 2 == 0) == (w == BLACK)) ? "won" : "lost");
        fflush(stdout);
    }
    printf("strong won %d/%d\n", strong, games);
    return fails;
}

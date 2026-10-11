#include "go.h"
#include <string.h>

#define NB(b, p, i) ((p) + ((i) == 0 ? -1 : (i) == 1 ? 1 : (i) == 2 ? -(b)->s : (b)->s))

void board_init(Board *b, int n, float komi) {
    memset(b, 0, sizeof(*b));
    b->n = n;
    b->s = n + 2;
    b->to_move = BLACK;
    b->last = b->last2 = NONE;
    b->komi = komi;
    for (int p = 0; p < b->s * b->s; p++) {
        int x = p % b->s, y = p / b->s;
        b->c[p] = (x == 0 || y == 0 || x == n + 1 || y == n + 1) ? EDGE : EMPTY;
        if (b->c[p] == EMPTY) {
            b->epos[p] = b->nempty;
            b->empt[b->nempty++] = p;
        }
    }
}

static inline void lib_add(Board *b, int h, int q) {
    b->libs[h]++;
    b->lsum[h] += q;
    b->lsum2[h] += (int64_t)q * q;
}
static inline void lib_del(Board *b, int h, int q) {
    b->libs[h]--;
    b->lsum[h] -= q;
    b->lsum2[h] -= (int64_t)q * q;
}

static inline void empty_remove(Board *b, int p) {
    int i = b->epos[p];
    int last = b->empt[--b->nempty];
    b->empt[i] = last;
    b->epos[last] = i;
}
static inline void empty_add(Board *b, int p) {
    b->epos[p] = b->nempty;
    b->empt[b->nempty++] = p;
}

int board_legal(const Board *b, int p, int color) {
    if (p == PASS) return 1;
    if (b->c[p] != EMPTY || p == b->ko) return 0;
    int heads[4], cnt[4], nh = 0;
    for (int i = 0; i < 4; i++) {
        int q = NB(b, p, i);
        int cq = b->c[q];
        if (cq == EMPTY) return 1;
        if (cq == EDGE) continue;
        int h = b->head[q], j;
        for (j = 0; j < nh; j++)
            if (heads[j] == h) { cnt[j]++; break; }
        if (j == nh) { heads[nh] = h; cnt[nh] = 1; nh++; }
    }
    for (int j = 0; j < nh; j++) {
        int h = heads[j];
        if (b->c[h] == color) {
            if (b->libs[h] > cnt[j]) return 1;
        } else {
            if (b->libs[h] == cnt[j]) return 1; /* captures */
        }
    }
    return 0;
}

static int remove_string(Board *b, int h) {
    int n = 0, r = h;
    do {
        b->c[r] = EMPTY;
        empty_add(b, r);
        n++;
        r = b->nxt[r];
    } while (r != h);
    r = h;
    do {
        int nx = b->nxt[r];
        for (int i = 0; i < 4; i++) {
            int q = NB(b, r, i);
            if (b->c[q] == BLACK || b->c[q] == WHITE) lib_add(b, b->head[q], r);
        }
        b->head[r] = 0;
        r = nx;
    } while (r != h);
    return n;
}

static void merge(Board *b, int a, int c) {
    /* merge string c into string a (both heads) */
    if (b->size[a] < b->size[c]) { int t = a; a = c; c = t; }
    int r = c;
    do { b->head[r] = a; r = b->nxt[r]; } while (r != c);
    int t = b->nxt[a];
    b->nxt[a] = b->nxt[c];
    b->nxt[c] = t;
    b->size[a] += b->size[c];
    b->libs[a] += b->libs[c];
    b->lsum[a] += b->lsum[c];
    b->lsum2[a] += b->lsum2[c];
}

int board_play(Board *b, int p) {
    int color = b->to_move;
    if (p == PASS) {
        b->passes++;
        b->ko = 0;
        b->last2 = b->last;
        b->last = PASS;
        b->to_move = OPP(color);
        b->nmoves++;
        return 0;
    }
    if (!board_legal(b, p, color)) return -1;
    b->c[p] = color;
    empty_remove(b, p);
    b->head[p] = p;
    b->nxt[p] = p;
    b->size[p] = 1;
    b->libs[p] = 0;
    b->lsum[p] = 0;
    b->lsum2[p] = 0;
    for (int i = 0; i < 4; i++) {
        int q = NB(b, p, i);
        int cq = b->c[q];
        if (cq == EMPTY) lib_add(b, p, q);
        else if (cq != EDGE) lib_del(b, b->head[q], p);
    }
    int captured = 0, capt_pt = 0;
    for (int i = 0; i < 4; i++) {
        int q = NB(b, p, i);
        int cq = b->c[q];
        if (cq == color) {
            if (b->head[q] != b->head[p]) merge(b, b->head[p], b->head[q]);
        } else if (cq == OPP(color)) {
            int h = b->head[q];
            if (b->libs[h] == 0) {
                if (b->size[h] == 1) capt_pt = h;
                captured += remove_string(b, h);
            }
        }
    }
    int hp = b->head[p];
    b->ko = 0;
    if (captured == 1 && b->size[hp] == 1 && b->libs[hp] == 1) b->ko = capt_pt;
    b->caps[color] += captured;
    b->passes = 0;
    b->last2 = b->last;
    b->last = p;
    b->to_move = OPP(color);
    b->nmoves++;
    return captured;
}

int board_libs_real(const Board *b, int p, int *liblist, int max) {
    uint8_t seen[MAXP];
    memset(seen, 0, sizeof(seen));
    int h = b->head[p], r = h, n = 0;
    do {
        for (int i = 0; i < 4; i++) {
            int q = NB(b, r, i);
            if (b->c[q] == EMPTY && !seen[q]) {
                seen[q] = 1;
                if (liblist && n < max) liblist[n] = q;
                n++;
            }
        }
        r = b->nxt[r];
    } while (r != h);
    return n;
}

int board_is_eyeish(const Board *b, int p, int color) {
    for (int i = 0; i < 4; i++) {
        int c = b->c[NB(b, p, i)];
        if (c != color && c != EDGE) return 0;
    }
    return 1;
}

int board_is_true_eye(const Board *b, int p, int color) {
    if (!board_is_eyeish(b, p, color)) return 0;
    int s = b->s, bad = 0, edge = 0;
    int d[4] = {-s - 1, -s + 1, s - 1, s + 1};
    for (int i = 0; i < 4; i++) {
        int c = b->c[p + d[i]];
        if (c == EDGE) edge = 1;
        else if (c == OPP(color)) bad++;
    }
    return edge ? bad == 0 : bad <= 1;
}

float board_area_score(const Board *b, const uint8_t *dead, uint8_t *terr) {
    uint8_t col[MAXP];
    int N = b->s * b->s;
    for (int p = 0; p < N; p++) {
        col[p] = b->c[p];
        if (dead && dead[p] && (col[p] == BLACK || col[p] == WHITE)) col[p] = EMPTY;
    }
    int score[3] = {0, 0, 0};
    uint8_t region[MAXP];
    memset(region, 0, sizeof(region));
    int stack[MAXP], members[MAXP];
    for (int p = 0; p < N; p++) {
        if (terr) terr[p] = col[p] == BLACK || col[p] == WHITE ? col[p] : EMPTY;
        if (col[p] == BLACK || col[p] == WHITE) score[col[p]]++;
    }
    for (int p = 0; p < N; p++) {
        if (col[p] != EMPTY || region[p]) continue;
        int sp = 0, nm = 0, border = 0;
        stack[sp++] = p;
        region[p] = 1;
        while (sp) {
            int r = stack[--sp];
            members[nm++] = r;
            for (int i = 0; i < 4; i++) {
                int q = NB(b, r, i);
                if (col[q] == EMPTY && !region[q]) { region[q] = 1; stack[sp++] = q; }
                else if (col[q] == BLACK || col[q] == WHITE) border |= col[q];
            }
        }
        if (border == BLACK || border == WHITE) {
            score[border] += nm;
            if (terr) for (int i = 0; i < nm; i++) terr[members[i]] = border;
        }
    }
    return (float)score[BLACK] - (float)score[WHITE] - b->komi;
}

void board_handicap_points(int n, int count, int *out) {
    int lo = n >= 13 ? 3 : 2, hi = n - 1 - lo, mid = n / 2;
    /* order: TR, BL, BR, TL, center, L, R, (center), T, B */
    int pts[9][2] = {{hi, lo}, {lo, hi}, {hi, hi}, {lo, lo}, {mid, mid}, {lo, mid}, {hi, mid}, {mid, lo}, {mid, hi}};
    int order[10][9] = {
        {0}, {0}, {0, 1}, {0, 1, 2}, {0, 1, 2, 3}, {0, 1, 2, 3, 4},
        {0, 1, 2, 3, 5, 6}, {0, 1, 2, 3, 5, 6, 4}, {0, 1, 2, 3, 5, 6, 7, 8}, {0, 1, 2, 3, 5, 6, 7, 8, 4}};
    if (count > 9) count = 9;
    for (int i = 0; i < count; i++) {
        out[2 * i] = pts[order[count][i]][0];
        out[2 * i + 1] = pts[order[count][i]][1];
    }
}

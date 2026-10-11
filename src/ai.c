#include "ai.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_THREADS 4
#define RAVE_EQUIV 3500.0f
#define PRIOR_EVEN 10
#define POOL_NODES 300000

typedef struct {
    int32_t child;    /* first child index, -1 if leaf */
    int16_t nchild;
    int16_t move;
    float n, w;       /* visits / wins incl. priors (wins for player who moved into node) */
    float an, aw;     /* AMAF */
    int32_t rv;       /* real visits */
    float rw;         /* real wins */
} Node;

typedef struct {
    const Board *root;
    const SearchParams *sp;
    Node *pool;
    int npool;
    unsigned rng;
    float owner[MAXP];
    int nowner;
    int playouts;
    volatile int *stop;
    volatile float *live;
    int id;
} Worker;

static volatile int g_total;          /* playouts across all threads */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_deadline;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static inline int rnd(Worker *w, int n) { return (int)(rng_next(&w->rng) % (unsigned)n); }

/* ------------------------------------------------------------------ playouts */

static inline int nb(const Board *b, int p, int i) {
    return p + (i == 0 ? -1 : i == 1 ? 1 : i == 2 ? -b->s : b->s);
}

static int empty_nbrs(const Board *b, int p) {
    int n = 0;
    for (int i = 0; i < 4; i++) n += b->c[nb(b, p, i)] == EMPTY;
    return n;
}

/* Try to play a tactically motivated move near the last move. Returns 1 if played. */
static int tactical_move(Board *b, Worker *w) {
    int last = b->last;
    if (last <= 0) return 0;
    int me = b->to_move, opp = OPP(me);
    int cands[16], nc = 0;
    /* capture the last-moved string if it self-ataried */
    if (b->c[last] == opp) {
        int h = b->head[last];
        if (board_in_atari(b, h)) cands[nc++] = board_atari_lib(b, h);
    }
    /* save my strings next to the last move that are now in atari */
    for (int i = 0; i < 4 && nc < 12; i++) {
        int q = nb(b, last, i);
        if (b->c[q] != me) continue;
        int h = b->head[q];
        if (!board_in_atari(b, h)) continue;
        /* counter-capture an adjacent enemy string in atari */
        int r = h, steps = 0;
        do {
            for (int k = 0; k < 4 && nc < 12; k++) {
                int t = nb(b, r, k);
                if (b->c[t] == opp && board_in_atari(b, b->head[t])) cands[nc++] = board_atari_lib(b, b->head[t]);
            }
            r = b->nxt[r];
        } while (r != h && ++steps < 24);
        int lib = board_atari_lib(b, h);
        if (empty_nbrs(b, lib) >= 2) cands[nc++] = lib;
    }
    if (!nc) return 0;
    int start = rnd(w, nc);
    for (int i = 0; i < nc; i++) {
        int p = cands[(start + i) % nc];
        if (board_play(b, p) >= 0) return 1;
    }
    return 0;
}

static void random_move(Board *b, Worker *w) {
    int me = b->to_move;
    int n = b->nempty;
    if (n > 0) {
        int start = rnd(w, n);
        for (int i = 0; i < n; i++) {
            int p = b->empt[(start + i) % n];
            if (board_is_true_eye(b, p, me)) continue;
            if (board_play(b, p) >= 0) return;
        }
    }
    board_play(b, PASS);
}

/* Final ownership of each point after a playout: stones + single-colour-bordered empties. */
static float playout_score(const Board *b, int8_t *own) {
    int sc = 0;
    for (int y = 0; y < b->n; y++)
        for (int x = 0; x < b->n; x++) {
            int p = (y + 1) * b->s + x + 1, c = b->c[p], o = 0;
            if (c == BLACK) o = 1;
            else if (c == WHITE) o = -1;
            else {
                int seen = 0;
                for (int i = 0; i < 4; i++) {
                    int cq = b->c[nb(b, p, i)];
                    if (cq == BLACK || cq == WHITE) seen |= cq;
                }
                o = seen == BLACK ? 1 : seen == WHITE ? -1 : 0;
            }
            own[p] = (int8_t)o;
            sc += o;
        }
    return sc - b->komi;
}

/* ------------------------------------------------------------------ tree */

static int new_nodes(Worker *w, int n) {
    if (w->npool + n > POOL_NODES) return -1;
    int i = w->npool;
    w->npool += n;
    return i;
}

static void add_prior(Node *c, float n, float wins) {
    c->n += n; c->w += wins; c->an += n; c->aw += wins;
}

static void expand(Worker *w, Node *node, const Board *b) {
    int me = b->to_move, opp = OPP(me);
    int moves[MAXP + 1], nm = 0;
    for (int i = 0; i < b->nempty; i++) {
        int p = b->empt[i];
        if (board_is_true_eye(b, p, me)) continue;
        if (board_legal(b, p, me)) moves[nm++] = p;
    }
    moves[nm++] = PASS;
    int base = new_nodes(w, nm);
    if (base < 0) return;
    int total_stones = b->n * b->n - b->nempty;
    for (int i = 0; i < nm; i++) {
        Node *c = &w->pool[base + i];
        memset(c, 0, sizeof(*c));
        c->child = -1;
        c->move = (int16_t)moves[i];
        int p = moves[i];
        if (p == PASS) {
            /* passing is rarely right before the board fills up */
            add_prior(c, PRIOR_EVEN, total_stones > b->n * b->n / 2 ? 3 : 0.5f);
            continue;
        }
        add_prior(c, PRIOR_EVEN, PRIOR_EVEN / 2.0f);
        int x = p % b->s - 1, y = p / b->s - 1;
        /* proximity to last move */
        if (b->last > 0) {
            int lx = b->last % b->s - 1, ly = b->last / b->s - 1;
            int d = abs(lx - x) + abs(ly - y);
            if (d <= 1) add_prior(c, 14, 14);
            else if (d == 2) add_prior(c, 10, 10);
            else if (d == 3) add_prior(c, 4, 4);
        }
        /* captures and ataris on neighbours */
        for (int k = 0; k < 4; k++) {
            int q = nb(b, p, k);
            if (b->c[q] == opp) {
                int h = b->head[q];
                if (board_in_atari(b, h)) add_prior(c, 20 + b->size[h] * 3, 20 + b->size[h] * 3);
            } else if (b->c[q] == me) {
                int h = b->head[q];
                if (board_in_atari(b, h) && empty_nbrs(b, p) >= 2) add_prior(c, 16, 16);
            }
        }
        /* empty-area line heuristic */
        int line = x < y ? x : y;
        int far = (b->n - 1 - x) < (b->n - 1 - y) ? (b->n - 1 - x) : (b->n - 1 - y);
        if (far < line) line = far;
        int empty_area = 1;
        for (int dy = -3; dy <= 3 && empty_area; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= b->n || yy >= b->n) continue;
                int cc = b->c[(yy + 1) * b->s + xx + 1];
                if (cc == BLACK || cc == WHITE) { empty_area = 0; break; }
            }
        if (empty_area) {
            if (line <= 1) add_prior(c, 12, 0);
            else if (line == 2 || (line == 3 && b->n >= 13)) add_prior(c, 10, 10);
        } else if (line == 0) {
            add_prior(c, 6, 1);
        }
        /* playing into a point with no liberties and no captures = likely self-atari */
        if (empty_nbrs(b, p) == 0) {
            int capt = 0, friend_ok = 0;
            for (int k = 0; k < 4; k++) {
                int q = nb(b, p, k);
                if (b->c[q] == opp && board_in_atari(b, b->head[q])) capt = 1;
                if (b->c[q] == me && b->libs[b->head[q]] > 2) friend_ok = 1;
            }
            if (!capt && !friend_ok) add_prior(c, 10, 0);
        }
    }
    node->child = base;
    node->nchild = (int16_t)nm;
}

static inline float node_value(const Node *c) {
    float beta = c->an / (c->an + c->n + c->n * c->an / RAVE_EQUIV);
    float v = c->n > 0 ? c->w / c->n : 0.5f;
    float av = c->an > 0 ? c->aw / c->an : v;
    return (1.0f - beta) * v + beta * av;
}

static Node *select_child(Worker *w, Node *node) {
    Node *best = NULL;
    float bv = -1;
    for (int i = 0; i < node->nchild; i++) {
        Node *c = &w->pool[node->child + i];
        float v = node_value(c) + (rng_next(&w->rng) & 1023) * 1e-7f;
        if (v > bv) { bv = v; best = c; }
    }
    return best;
}

static void one_playout(Worker *w, Node *rootn) {
    Board b = *w->root;
    Node *path[MAXP * 3];
    int pcol[MAXP * 3];     /* colour that moved into path[i] */
    int depth = 0;
    int8_t amaf[MAXP];
    memset(amaf, 0, sizeof(amaf));
    int expand_at = b.n >= 19 ? 8 : b.n >= 13 ? 4 : 2;

    Node *node = rootn;
    path[depth] = node; pcol[depth] = 0; depth++;
    while (b.passes < 2) {
        if (node->child < 0) {
            if (node->rv >= expand_at || node == rootn) expand(w, node, &b);
            if (node->child < 0) break;
        }
        Node *c = select_child(w, node);
        if (!c) break;
        int color = b.to_move;
        if (board_play(&b, c->move) < 0) break; /* should not happen */
        if (c->move > 0 && !amaf[c->move]) amaf[c->move] = (int8_t)color;
        node = c;
        path[depth] = node; pcol[depth] = color; depth++;
        if (depth >= MAXP * 3 - 1) break;
    }
    int limit = b.nmoves + b.n * b.n * 3;
    while (b.passes < 2 && b.nmoves < limit) {
        int color = b.to_move;
        if (!(rng_next(&w->rng) % 10 < 9 && tactical_move(&b, w))) random_move(&b, w);
        int m = b.last;
        if (m > 0 && !amaf[m]) amaf[m] = (int8_t)color;
    }
    int8_t own[MAXP];
    float score = playout_score(&b, own);
    int winner = score > 0 ? BLACK : WHITE;
    for (int y = 0; y < b.n; y++)
        for (int x = 0; x < b.n; x++) {
            int p = (y + 1) * b.s + x + 1;
            w->owner[p] += own[p];
        }
    w->nowner++;

    for (int i = 0; i < depth; i++) {
        Node *nd = path[i];
        nd->n += 1;
        nd->rv += 1;
        if (pcol[i]) {
            float r = winner == pcol[i] ? 1.0f : 0.0f;
            nd->w += r;
            nd->rw += r;
        } else {
            /* root: store black wins in rw */
            nd->rw += winner == BLACK;
        }
        /* AMAF update for children of nd: colour to move at nd */
        if (nd->child >= 0) {
            int tomove = i == 0 ? w->root->to_move : OPP(pcol[i]);
            for (int k = 0; k < nd->nchild; k++) {
                Node *c = &w->pool[nd->child + k];
                if (c->move > 0 && amaf[c->move] == tomove) {
                    c->an += 1;
                    c->aw += winner == tomove;
                }
            }
        }
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    Node *root = &w->pool[0];
    w->npool = 1;
    memset(root, 0, sizeof(*root));
    root->child = -1;
    int k = 0;
    for (;;) {
        if (*w->stop) break;
        int t = __sync_fetch_and_add(&g_total, 1);
        if (t >= w->sp->playouts) break;
        one_playout(w, root);
        w->playouts++;
        if ((++k & 15) == 0 && now_ms() > g_deadline) break;
        if (w->id == 0 && w->live && (k & 63) == 0 && root->rv > 0) {
            /* best child's winrate, converted to black's perspective */
            Node *best = NULL;
            for (int i = 0; i < root->nchild; i++) {
                Node *c = &w->pool[root->child + i];
                if (!best || c->rv > best->rv) best = c;
            }
            if (best && best->rv > 20) {
                float wr = best->rw / best->rv;
                *w->live = w->root->to_move == BLACK ? wr : 1.0f - wr;
            }
        }
    }
    return NULL;
}

static int cand_cmp(const void *a, const void *b) {
    return ((const Candidate *)b)->visits - ((const Candidate *)a)->visits;
}

void search_run(const Board *b, const SearchParams *sp, SearchResult *out,
                volatile int *stop, volatile float *live) {
    static Node *pools[MAX_THREADS];
    static Worker workers[MAX_THREADS];
    int nt = sp->threads < 1 ? 1 : sp->threads > MAX_THREADS ? MAX_THREADS : sp->threads;
    static volatile int no_stop = 0;
    pthread_mutex_lock(&g_lock); /* one search at a time */
    double t0 = now_ms();
    g_total = 0;
    g_deadline = t0 + (sp->time_ms > 0 ? sp->time_ms : 1e9);
    pthread_t th[MAX_THREADS];
    for (int i = 0; i < nt; i++) {
        if (!pools[i]) pools[i] = malloc(sizeof(Node) * POOL_NODES);
        Worker *w = &workers[i];
        memset(w, 0, sizeof(*w));
        w->root = b;
        w->sp = sp;
        w->pool = pools[i];
        w->rng = (sp->seed ? sp->seed : 12345u) * 2654435761u + i * 7919u + 1u;
        w->stop = stop ? stop : &no_stop;
        w->live = live;
        w->id = i;
        if (i > 0) pthread_create(&th[i], NULL, worker_main, w);
    }
    worker_main(&workers[0]);
    for (int i = 1; i < nt; i++) pthread_join(th[i], NULL);

    /* merge root statistics */
    memset(out, 0, sizeof(*out));
    int vis[MAXP];
    float wins[MAXP];
    memset(vis, 0, sizeof(vis));
    memset(wins, 0, sizeof(wins));
    int total = 0, nown = 0;
    float own[MAXP];
    memset(own, 0, sizeof(own));
    for (int i = 0; i < nt; i++) {
        Worker *w = &workers[i];
        Node *root = &w->pool[0];
        total += w->playouts;
        for (int k = 0; root->child >= 0 && k < root->nchild; k++) {
            Node *c = &w->pool[root->child + k];
            vis[c->move] += c->rv;
            wins[c->move] += c->rw;
        }
        for (int p = 0; p < b->s * b->s; p++) own[p] += w->owner[p];
        nown += w->nowner;
    }
    for (int p = 0; p < b->s * b->s; p++) {
        out->owner[p] = nown ? own[p] / nown : 0;
        if (vis[p] > 0 || (p == PASS && vis[PASS] > 0)) {
            Candidate *c = &out->cand[out->ncand++];
            c->move = p;
            c->visits = vis[p];
            c->winrate = wins[p] / vis[p];
        }
    }
    qsort(out->cand, out->ncand, sizeof(Candidate), cand_cmp);
    if (out->ncand) {
        out->best = out->cand[0].move;
        out->winrate = out->cand[0].winrate;
    } else {
        out->best = PASS;
        out->winrate = 0.5f;
    }
    out->black_wr = b->to_move == BLACK ? out->winrate : 1.0f - out->winrate;
    out->playouts = total;
    out->ms = (int)(now_ms() - t0);
    pthread_mutex_unlock(&g_lock);
}

int search_pick(const SearchResult *r, float temperature, unsigned *rng) {
    if (r->ncand == 0) return PASS;
    if (temperature <= 0) return r->best;
    /* sample from visit distribution sharpened by 1/temperature, among reasonable moves */
    double weights[MAXP], sum = 0;
    int n = r->ncand < 12 ? r->ncand : 12;
    for (int i = 0; i < n; i++) {
        double v = r->cand[i].visits;
        weights[i] = pow(v > 0 ? v : 0, 1.0 / temperature);
        if (r->cand[i].winrate < r->cand[0].winrate - 0.35f) weights[i] = 0;
        sum += weights[i];
    }
    if (sum <= 0) return r->best;
    double x = (rng_next(rng) / 4294967296.0) * sum;
    for (int i = 0; i < n; i++) {
        x -= weights[i];
        if (x <= 0) return r->cand[i].move;
    }
    return r->best;
}

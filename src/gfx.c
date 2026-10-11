#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

Gfx G;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static SDL_Texture *make_tex(int w, int h, const Uint32 *px) {
    SDL_Texture *t = SDL_CreateTexture(G.r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static inline Uint32 argb(float r, float g, float b, float a) {
    int R = (int)clampf(r, 0, 255), Gc = (int)clampf(g, 0, 255), B = (int)clampf(b, 0, 255), A = (int)clampf(a * 255, 0, 255);
    return ((Uint32)A << 24) | ((Uint32)R << 16) | ((Uint32)Gc << 8) | (Uint32)B;
}

/* ---------------------------------------------------------------- noise */
static float hash2(int x, int y) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
static float vnoise(float x, float y) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float fbm(float x, float y) {
    return 0.5f * vnoise(x, y) + 0.25f * vnoise(x * 2.03f, y * 2.03f) + 0.125f * vnoise(x * 4.1f, y * 4.1f) +
           0.0625f * vnoise(x * 8.3f, y * 8.3f);
}

/* ---------------------------------------------------------------- sprites */
static SDL_Texture *make_radial(int kind) {
    const int S = 128;
    Uint32 *px = malloc(S * S * 4);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float dx = (x + 0.5f - S / 2.0f) / (S / 2.0f), dy = (y + 0.5f - S / 2.0f) / (S / 2.0f);
            float d = sqrtf(dx * dx + dy * dy), a;
            if (kind == 0) { a = clampf(1 - d, 0, 1); a = a * a; }
            else if (kind == 1) a = clampf((0.95f - d) * 64 + 0.5f, 0, 1) * clampf((d - 0.80f) * 64 + 0.5f, 0, 1);
            else a = clampf((1 - d) * 64 + 0.5f, 0, 1);
            px[y * S + x] = argb(255, 255, 255, a);
        }
    SDL_Texture *t = make_tex(S, S, px);
    free(px);
    return t;
}

void gfx_make_stones(int R) {
    if (G.stone_r == R) return;
    for (int c = 1; c <= 2; c++) if (G.stone[c]) SDL_DestroyTexture(G.stone[c]);
    if (G.shadow) SDL_DestroyTexture(G.shadow);
    G.stone_r = R;
    int D = 2 * R + 4;
    float cx = D / 2.0f;
    Uint32 *px = malloc(D * D * 4);
    float L[3] = {-0.45f, -0.65f, 0.62f};
    float ln = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int i = 0; i < 3; i++) L[i] /= ln;
    float H[3] = {L[0], L[1], L[2] + 1};
    float hn = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
    for (int i = 0; i < 3; i++) H[i] /= hn;
    for (int color = 1; color <= 2; color++) {
        for (int y = 0; y < D; y++)
            for (int x = 0; x < D; x++) {
                float dx = (x + 0.5f - cx) / R, dy = (y + 0.5f - cx) / R;
                float d = sqrtf(dx * dx + dy * dy);
                float cov = clampf((1 - d) * R + 0.5f, 0, 1);
                if (cov <= 0) { px[y * D + x] = 0; continue; }
                float dd = d > 1 ? 1 : d;
                float z = sqrtf(1 - dd * dd) * 0.55f + 0.08f; /* lens-shaped stone */
                float n[3] = {dx, dy, z};
                float nn = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int i = 0; i < 3; i++) n[i] /= nn;
                float diff = clampf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0, 1);
                float sp = clampf(n[0] * H[0] + n[1] * H[1] + n[2] * H[2], 0, 1);
                float r, g, b;
                if (color == 1) {
                    float s = powf(sp, 28) * 0.9f + powf(sp, 6) * 0.10f;
                    r = 16 + diff * 34 + s * 170;
                    g = 17 + diff * 35 + s * 172;
                    b = 20 + diff * 38 + s * 178;
                } else {
                    float sx = dx - 0.25f, sy = dy - 1.9f;
                    float stripe = 0.5f + 0.5f * sinf(sqrtf(sx * sx + sy * sy) * 34.0f);
                    float alb = 1.0f - 0.035f * stripe;
                    float sh = 0.66f + 0.34f * diff;
                    float s = powf(sp, 40) * 0.55f;
                    r = 242 * alb * sh + s * 60;
                    g = 238 * alb * sh + s * 60;
                    b = 228 * alb * sh + s * 60;
                }
                px[y * D + x] = argb(r, g, b, cov);
            }
        G.stone[color] = make_tex(D, D, px);
    }
    for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
            float dx = (x + 0.5f - cx) / R, dy = (y + 0.5f - cx) / R;
            float d = sqrtf(dx * dx + dy * dy);
            float a = clampf((1.08f - d) / 0.3f, 0, 1);
            px[y * D + x] = argb(0, 0, 0, a * a * 0.55f);
        }
    G.shadow = make_tex(D, D, px);
    free(px);
}

static void make_wood(int S) {
    Uint32 *px = malloc(S * S * 4);
    float lr = 236, lg = 198, lb = 132; /* light kaya */
    float dr = 200, dg = 150, db = 86;  /* grain */
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            /* straight-grain (masame) kaya: fine, slightly wandering parallel lines */
            float warp = fbm(x * 0.0016f, y * 0.006f) * 7.0f + 1.5f * sinf(x * 0.0031f + y * 0.002f);
            float t = y * 0.30f + warp * 2.2f;
            float g = 0.5f + 0.5f * sinf(t);
            g = powf(g, 6.0f);
            float t2 = y * 0.083f + warp;
            float g2 = 0.5f + 0.5f * sinf(t2);
            g2 = powf(g2, 3.0f);
            float fib = (fbm(x * 0.35f, y * 0.02f) - 0.5f) * 0.12f;
            float m = clampf(g * 0.32f + g2 * 0.30f + fib + 0.05f, 0, 1);
            float shade = 0.94f + 0.08f * fbm(x * 0.006f + 11, y * 0.006f);
            float ex = fminf(fminf(x, S - 1 - x), fminf(y, S - 1 - y));
            float edge = ex < 4 ? 0.82f + 0.045f * ex : 1.0f;
            float r = (lr + (dr - lr) * m) * shade * edge;
            float gg = (lg + (dg - lg) * m) * shade * edge;
            float b = (lb + (db - lb) * m) * shade * edge;
            if (ex < 2 && (x < 2 || y < 2)) { r += 18; gg += 14; b += 10; }
            px[y * S + x] = argb(r, gg, b, 1);
        }
    G.wood = make_tex(S, S, px);
    SDL_SetTextureBlendMode(G.wood, SDL_BLENDMODE_NONE);
    G.wood_size = S;
    free(px);
}

static void make_bg(int w, int h) {
    Uint32 *px = malloc(w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (x - w * 0.42f) / w, dy = (y - h * 0.45f) / h;
            float v = clampf(1.0f - sqrtf(dx * dx + dy * dy) * 1.6f, 0, 1);
            float n = (hash2(x, y) - 0.5f) * 5;
            px[y * w + x] = argb(13 + v * 22 + n, 11 + v * 17 + n, 9 + v * 12 + n, 1);
        }
    G.bg = make_tex(w, h, px);
    SDL_SetTextureBlendMode(G.bg, SDL_BLENDMODE_NONE);
    free(px);
}

void gfx_init(SDL_Renderer *r, int w, int h, int slab) {
    memset(&G, 0, sizeof(G));
    G.r = r;
    for (int f = 0; f < F_COUNT; f++) {
        const FontInfo *fi = &FONT_INFO[f];
        Uint32 *px = malloc(fi->atlas_w * fi->atlas_h * 4);
        for (int i = 0; i < fi->atlas_w * fi->atlas_h; i++) px[i] = ((Uint32)fi->px[i] << 24) | 0xffffff;
        G.font[f] = make_tex(fi->atlas_w, fi->atlas_h, px);
        free(px);
    }
    G.glow = make_radial(0);
    G.ring = make_radial(1);
    G.disc = make_radial(2);
    make_bg(w, h);
    make_wood(slab);
}

void gfx_quit(void) {
    for (int f = 0; f < F_COUNT; f++) if (G.font[f]) SDL_DestroyTexture(G.font[f]);
    SDL_Texture *ts[] = {G.glow, G.ring, G.disc, G.bg, G.wood, G.stone[1], G.stone[2], G.shadow};
    for (unsigned i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) if (ts[i]) SDL_DestroyTexture(ts[i]);
}

/* ---------------------------------------------------------------- text */
static const char *utf8_next(const char *s, Uint32 *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return s + 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return s + 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return s + 3; }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return s + 4;
    }
    *cp = '?';
    return s + 1;
}

static const GlyphInfo *glyph(int font, Uint32 cp) {
    const FontInfo *fi = &FONT_INFO[font];
    int lo = 0, hi = fi->nglyph - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fi->g[mid].cp == cp) return &fi->g[mid];
        if (fi->g[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp != '?' ? glyph(font, '?') : NULL;
}

static int text_width_n(int font, const char *s, const char *end) {
    int w = 0;
    while (*s && (!end || s < end)) {
        Uint32 cp;
        s = utf8_next(s, &cp);
        const GlyphInfo *g = glyph(font, cp);
        if (g) w += g->adv;
    }
    return w;
}

int text_width(int font, const char *s) { return text_width_n(font, s, NULL); }
int text_height(int font) { return FONT_INFO[font].line; }

void draw_text(int font, const char *s, int x, int y, Col c, int align) {
    if (align == ALIGN_C) x -= text_width(font, s) / 2;
    else if (align == ALIGN_R) x -= text_width(font, s);
    SDL_Texture *t = G.font[font];
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    while (*s) {
        Uint32 cp;
        s = utf8_next(s, &cp);
        const GlyphInfo *g = glyph(font, cp);
        if (!g) continue;
        if (g->w > 0) {
            SDL_Rect src = {g->x, g->y, g->w, g->h}, dst = {x + g->xo, y + g->yo, g->w, g->h};
            SDL_RenderCopy(G.r, t, &src, &dst);
        }
        x += g->adv;
    }
}

int draw_text_wrap(int font, const char *s, int x, int y, int maxw, Col c, int gap) {
    char line[512];
    int lh = text_height(font) + gap, y0 = y;
    while (*s) {
        /* find longest prefix that fits, break at spaces */
        const char *p = s, *brk = NULL;
        while (*p && *p != '\n') {
            const char *q = p;
            Uint32 cp;
            q = utf8_next(q, &cp);
            if (text_width_n(font, s, q) > maxw) break;
            if (cp == ' ') brk = p;
            p = q;
        }
        const char *end;
        if (!*p || *p == '\n') end = p;
        else end = brk ? brk : (p > s ? p : utf8_next(p, &(Uint32){0}));
        int n = (int)(end - s);
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, s, n);
        line[n] = 0;
        draw_text(font, line, x, y, c, ALIGN_L);
        y += lh;
        s = end;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return y - y0;
}

/* ---------------------------------------------------------------- shapes */
void fill_rect(int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawBlendMode(G.r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(G.r, c.r, c.g, c.b, c.a);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(G.r, &rc);
}

void draw_sprite(SDL_Texture *t, float cx, float cy, float size, Col c) {
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect dst = {(int)lroundf(cx - size / 2), (int)lroundf(cy - size / 2), (int)lroundf(size), (int)lroundf(size)};
    SDL_RenderCopy(G.r, t, NULL, &dst);
}

void draw_sprite_rect(SDL_Texture *t, int x, int y, int w, int h, Col c) {
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(G.r, t, NULL, &dst);
}

void round_rect(int x, int y, int w, int h, int rad, Col c) {
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    fill_rect(x + rad, y, w - 2 * rad, h, c);
    fill_rect(x, y + rad, rad, h - 2 * rad, c);
    fill_rect(x + w - rad, y + rad, rad, h - 2 * rad, c);
    /* corners: quarter discs via clipped disc sprite */
    int d = rad * 2;
    SDL_SetTextureColorMod(G.disc, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(G.disc, c.a);
    SDL_Rect src[4] = {{0, 0, 64, 64}, {64, 0, 64, 64}, {0, 64, 64, 64}, {64, 64, 64, 64}};
    SDL_Rect dst[4] = {{x, y, rad, rad}, {x + w - rad, y, rad, rad}, {x, y + h - rad, rad, rad}, {x + w - rad, y + h - rad, rad, rad}};
    (void)d;
    for (int i = 0; i < 4; i++) SDL_RenderCopy(G.r, G.disc, &src[i], &dst[i]);
}

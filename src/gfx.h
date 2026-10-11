#pragma once
#include <SDL.h>
#include "font_data.h"

typedef struct { Uint8 r, g, b, a; } Col;
#define RGBA(r, g, b, a) ((Col){(r), (g), (b), (a)})

enum { ALIGN_L = 0, ALIGN_C = 1, ALIGN_R = 2 };

typedef struct {
    SDL_Renderer *r;
    SDL_Texture *font[F_COUNT];
    SDL_Texture *glow;      /* radial soft glow, white */
    SDL_Texture *ring;      /* antialiased ring, white */
    SDL_Texture *disc;      /* antialiased filled disc, white */
    SDL_Texture *bg;        /* screen background */
    SDL_Texture *wood;      /* board slab */
    int wood_size;
    SDL_Texture *stone[3];  /* [BLACK], [WHITE] at current radius */
    SDL_Texture *shadow;
    int stone_r;
} Gfx;

extern Gfx G;

void gfx_init(SDL_Renderer *r, int w, int h, int slab);
void gfx_make_stones(int radius);
void gfx_quit(void);

int text_width(int font, const char *s);
int text_height(int font);
void draw_text(int font, const char *s, int x, int y, Col c, int align);
/* word-wrapped text; returns height used */
int draw_text_wrap(int font, const char *s, int x, int y, int maxw, Col c, int line_gap);

void fill_rect(int x, int y, int w, int h, Col c);
void draw_sprite(SDL_Texture *t, float cx, float cy, float size, Col c);
void draw_sprite_rect(SDL_Texture *t, int x, int y, int w, int h, Col c);
void round_rect(int x, int y, int w, int h, int rad, Col c);

/*
 * gfx_sdl.c - SDL2 implementation of the graphics API
 */

#include "gfx.h"
#include <SDL.h>
#include <string.h>
#include <math.h>

#define SCALE 4  /* Display scale factor */

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static uint8_t framebuffer[SCREEN_W * SCREEN_H];
static uint8_t spritesheet[128 * 64];  /* 16x8 grid of 8x8 sprites */

/* 8-bit palette (PICO-8 inspired, extended to 256) */
static const uint32_t palette[256] = {
    0x000000, /* 0  black */
    0x1D2B53, /* 1  dark blue */
    0x7E2553, /* 2  purple */
    0x008751, /* 3  green */
    0xAB5236, /* 4  brown */
    0x5F574F, /* 5  dark grey */
    0xC2C3C7, /* 6  light grey */
    0xFFF1E8, /* 7  white */
    0xFF004D, /* 8  red */
    0xFFA300, /* 9  orange */
    0xFFEC27, /* 10 yellow */
    0x00E436, /* 11 bright green */
    0x29ADFF, /* 12 blue */
    0x83769C, /* 13 lavender */
    0xFF77A8, /* 14 pink */
    0xFFCCAA, /* 15 peach */
    /* Rest filled with grayscale gradient for now */
    [16 ... 255] = 0x808080
};

/* Palette remap table (for palette swapping like PICO-8's pal()) */
static uint8_t pal_remap[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

/* Input state */
static bool key_left, key_right, key_up, key_down, key_jump, key_dash, key_quit, key_restart, key_teleport;

int gfx_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return -1;
    }

    window = SDL_CreateWindow("Celeste ESP32",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * SCALE, SCREEN_H * SCALE, 0);
    if (!window) return -1;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) return -1;

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);
    if (!texture) return -1;

    memset(framebuffer, 0, sizeof(framebuffer));
    return 0;
}

void gfx_quit(void) {
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();
}

void gfx_clear(uint8_t color) {
    memset(framebuffer, color, sizeof(framebuffer));
}

void gfx_pixel(int x, int y, uint8_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        framebuffer[y * SCREEN_W + x] = color;
    }
}

void gfx_rect(int x, int y, int w, int h, uint8_t color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            gfx_pixel(px, py, color);
        }
    }
}

void gfx_blit(const uint8_t* data, int x, int y, int w, int h, int key_color) {
    gfx_blit_ex(data, x, y, w, h, key_color, false, false);
}

void gfx_blit_ex(const uint8_t* data, int x, int y, int w, int h,
                 int key_color, bool flip_x, bool flip_y) {
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int src_x = flip_x ? (w - 1 - px) : px;
            int src_y = flip_y ? (h - 1 - py) : py;
            uint8_t c = data[src_y * w + src_x];
            if ((int)c != key_color) {
                gfx_pixel(x + px, y + py, c);
            }
        }
    }
}

static uint8_t getpixel(SDL_Surface* surf, int x, int y) {
    int bpp = surf->format->BytesPerPixel;
    uint8_t* p = (uint8_t*)surf->pixels + y * surf->pitch + x * bpp;
    switch (bpp) {
        case 1: return *p;
        case 2: return *(uint16_t*)p;
        case 3: return p[0];  /* Just take first byte for indexed */
        case 4: return *(uint32_t*)p;
    }
    return 0;
}

int gfx_load_sprites(const char* path) {
    SDL_Surface* surf = SDL_LoadBMP(path);
    if (!surf) return -1;

    SDL_LockSurface(surf);
    for (int y = 0; y < 64 && y < surf->h; y++) {
        for (int x = 0; x < 128 && x < surf->w; x++) {
            spritesheet[y * 128 + x] = getpixel(surf, x, y);
        }
    }
    SDL_UnlockSurface(surf);
    SDL_FreeSurface(surf);
    return 0;
}

void gfx_spr(int id, int x, int y, bool flip_x, bool flip_y) {
    if (id < 0 || id >= SHEET_COLS * SHEET_ROWS) return;

    int sx = (id % SHEET_COLS) * SPRITE_SIZE;
    int sy = (id / SHEET_COLS) * SPRITE_SIZE;

    for (int py = 0; py < SPRITE_SIZE; py++) {
        for (int px = 0; px < SPRITE_SIZE; px++) {
            int src_px = flip_x ? (SPRITE_SIZE - 1 - px) : px;
            int src_py = flip_y ? (SPRITE_SIZE - 1 - py) : py;
            uint8_t c = spritesheet[(sy + src_py) * 128 + (sx + src_px)];
            if (c != 0) {  /* Color 0 is transparent */
                /* Apply palette remap for PICO-8 colors (0-15) */
                if (c < 16) c = pal_remap[c];
                gfx_pixel(x + px, y + py, c);
            }
        }
    }
}

/* Simple 3x5 digit bitmaps for number display */
static const uint8_t digit_bitmaps[10][5] = {
    {0x7,0x5,0x5,0x5,0x7}, /* 0 */
    {0x2,0x2,0x2,0x2,0x2}, /* 1 */
    {0x7,0x1,0x7,0x4,0x7}, /* 2 */
    {0x7,0x1,0x7,0x1,0x7}, /* 3 */
    {0x5,0x5,0x7,0x1,0x1}, /* 4 */
    {0x7,0x4,0x7,0x1,0x7}, /* 5 */
    {0x7,0x4,0x7,0x5,0x7}, /* 6 */
    {0x7,0x1,0x1,0x1,0x1}, /* 7 */
    {0x7,0x5,0x7,0x5,0x7}, /* 8 */
    {0x7,0x5,0x7,0x1,0x7}, /* 9 */
};

/* 3x5 letter bitmaps for text display (A-Z, plus common punctuation) */
static const uint8_t letter_bitmaps[34][5] = {
    {0x2,0x5,0x7,0x5,0x5}, /* A */
    {0x6,0x5,0x6,0x5,0x6}, /* B */
    {0x3,0x4,0x4,0x4,0x3}, /* C */
    {0x6,0x5,0x5,0x5,0x6}, /* D */
    {0x7,0x4,0x6,0x4,0x7}, /* E */
    {0x7,0x4,0x6,0x4,0x4}, /* F */
    {0x3,0x4,0x5,0x5,0x3}, /* G */
    {0x5,0x5,0x7,0x5,0x5}, /* H */
    {0x7,0x2,0x2,0x2,0x7}, /* I */
    {0x1,0x1,0x1,0x5,0x2}, /* J */
    {0x5,0x5,0x6,0x5,0x5}, /* K */
    {0x4,0x4,0x4,0x4,0x7}, /* L */
    {0x5,0x7,0x7,0x5,0x5}, /* M */
    {0x5,0x7,0x7,0x7,0x5}, /* N */
    {0x2,0x5,0x5,0x5,0x2}, /* O */
    {0x6,0x5,0x6,0x4,0x4}, /* P */
    {0x2,0x5,0x5,0x7,0x3}, /* Q */
    {0x6,0x5,0x6,0x5,0x5}, /* R */
    {0x3,0x4,0x2,0x1,0x6}, /* S */
    {0x7,0x2,0x2,0x2,0x2}, /* T */
    {0x5,0x5,0x5,0x5,0x7}, /* U */
    {0x5,0x5,0x5,0x5,0x2}, /* V */
    {0x5,0x5,0x7,0x7,0x5}, /* W */
    {0x5,0x5,0x2,0x5,0x5}, /* X */
    {0x5,0x5,0x2,0x2,0x2}, /* Y */
    {0x7,0x1,0x2,0x4,0x7}, /* Z */
    {0x0,0x2,0x0,0x2,0x0}, /* : (colon) - index 26 */
    {0x0,0x0,0x0,0x0,0x2}, /* . (period) - index 27 */
    {0x0,0x0,0x7,0x0,0x0}, /* - (dash) - index 28 */
    {0x7,0x1,0x7,0x4,0x7}, /* x (for "x3" display) - index 29 - same as letter X but lowercase */
    {0x0,0x0,0x0,0x0,0x0}, /* space - index 30 */
    {0x2,0x2,0x2,0x0,0x2}, /* ! - index 31 */
    {0x0,0x7,0x0,0x7,0x0}, /* = - index 32 */
    {0x0,0x2,0x0,0x2,0x4}, /* ; - index 33 */
};

static void draw_digit(int x, int y, int digit, uint8_t color) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = digit_bitmaps[digit][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (4 >> col)) {
                gfx_pixel(x + col, y + row, color);
            }
        }
    }
}

int gfx_print_num(int x, int y, int num, uint8_t color) {
    int start_x = x;
    if (num < 0) {
        /* Draw minus sign */
        gfx_pixel(x, y + 2, color);
        gfx_pixel(x + 1, y + 2, color);
        x += 4;
        num = -num;
    }
    /* Count digits */
    int temp = num;
    int digits = 0;
    do { digits++; temp /= 10; } while (temp > 0);
    /* Draw from right to left */
    int draw_x = x + (digits - 1) * 4;
    do {
        draw_digit(draw_x, y, num % 10, color);
        num /= 10;
        draw_x -= 4;
    } while (num > 0);
    return x + digits * 4 - start_x;
}

static void draw_letter(int x, int y, int index, uint8_t color) {
    if (index < 0 || index > 33) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = letter_bitmaps[index][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (4 >> col)) {
                gfx_pixel(x + col, y + row, color);
            }
        }
    }
}

int gfx_print(int x, int y, const char* text, uint8_t color) {
    int start_x = x;
    while (*text) {
        char c = *text++;
        int index = -1;

        if (c >= 'A' && c <= 'Z') {
            index = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            index = c - 'a';  /* lowercase maps to same as uppercase */
        } else if (c >= '0' && c <= '9') {
            draw_digit(x, y, c - '0', color);
            x += 4;
            continue;
        } else if (c == ':') {
            index = 26;
        } else if (c == '.') {
            index = 27;
        } else if (c == '-') {
            index = 28;
        } else if (c == 'x' || c == 'X') {
            index = 23;  /* X letter */
        } else if (c == ' ') {
            x += 4;
            continue;
        } else if (c == '!') {
            index = 31;
        } else if (c == '=') {
            index = 32;
        } else if (c == ';') {
            index = 33;
        }

        if (index >= 0) {
            draw_letter(x, y, index, color);
        }
        x += 4;
    }
    return x - start_x;
}

void gfx_rectfill(int x0, int y0, int x1, int y1, uint8_t color) {
    /* PICO-8 style: x0,y0 and x1,y1 are inclusive corners */
    int minx = x0 < x1 ? x0 : x1;
    int maxx = x0 > x1 ? x0 : x1;
    int miny = y0 < y1 ? y0 : y1;
    int maxy = y0 > y1 ? y0 : y1;
    gfx_rect(minx, miny, maxx - minx + 1, maxy - miny + 1, color);
}

void gfx_flip(void) {
    /* Convert indexed framebuffer to RGB */
    static uint32_t pixels[SCREEN_W * SCREEN_H];
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        pixels[i] = palette[framebuffer[i]];
    }

    SDL_UpdateTexture(texture, NULL, pixels, SCREEN_W * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

void input_update(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            key_quit = true;
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            bool pressed = (e.type == SDL_KEYDOWN);
            switch (e.key.keysym.sym) {
                case SDLK_LEFT:   case SDLK_a: key_left  = pressed; break;
                case SDLK_RIGHT:  case SDLK_d: key_right = pressed; break;
                case SDLK_UP:     case SDLK_w: key_up    = pressed; break;
                case SDLK_DOWN:   case SDLK_s: key_down  = pressed; break;
                case SDLK_z:      case SDLK_c: key_jump  = pressed; break;
                case SDLK_x:      case SDLK_v: key_dash  = pressed; break;
                case SDLK_ESCAPE:              key_quit  = pressed; break;
                case SDLK_r:                   key_restart = pressed; break;
                case SDLK_t:                   key_teleport = pressed; break;
            }
        }
    }
}

bool input_left(void)     { return key_left; }
bool input_right(void)    { return key_right; }
bool input_up(void)       { return key_up; }
bool input_down(void)     { return key_down; }
bool input_jump(void)     { return key_jump; }
bool input_dash(void)     { return key_dash; }
bool input_quit(void)     { return key_quit; }
bool input_restart(void)  { return key_restart; }
bool input_teleport(void) { return key_teleport; }

/* PICO-8 style sin (input 0-1 maps to full cycle, output -1 to 1, inverted) */
float p8sin(float x) {
    return -sinf(x * 6.28318530718f);
}

/* Palette swapping (like PICO-8 pal()) */
void gfx_pal(uint8_t from, uint8_t to) {
    if (from < 16) {
        pal_remap[from] = to;
    }
}

void gfx_pal_reset(void) {
    for (int i = 0; i < 16; i++) {
        pal_remap[i] = i;
    }
}

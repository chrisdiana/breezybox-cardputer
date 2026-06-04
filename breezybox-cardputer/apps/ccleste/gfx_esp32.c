/*
 * gfx_esp32.c - ESP32-DOS implementation of the graphics API
 *
 * Delegates reusable primitives to rgb_gfx_* (already exported from firmware).
 * Game-specific logic (sprites, 3x5 font, palette remap, input) lives here.
 */

#include "gfx.h"
#include "sprites.h"
#include "cardputer_keyboard.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ESP32 display functions (linked from firmware) */
extern int rgb_display_set_mode(int mode);
extern unsigned char *rgb_display_get_framebuffer(void);
extern void rgb_display_set_vga_palette(const unsigned short palette[256]);
extern void rgb_display_wait_vsync(void);
extern void vTaskDelay(unsigned int ticks);

/* rgb_gfx primitives (already exported from firmware via rgb_display_init) */
extern void rgb_gfx_clear(uint8_t color);
extern void rgb_gfx_pixel(int x, int y, uint8_t color);
extern void rgb_gfx_rectfill(int x, int y, int w, int h, uint8_t color);
extern void rgb_gfx_blit(const uint8_t *data, int x, int y, int w, int h,
                          int src_stride, int transparent_color);
extern void rgb_gfx_blit_flip(const uint8_t *data, int x, int y, int w, int h,
                               int src_stride, int transparent_color,
                               bool flip_x, bool flip_y);

#define SM_TEXT  3
#define SM_150P  0x80

/* BT keyboard raw input API (linked from firmware) */
extern int bt_keyboard_is_pressed(unsigned char keycode);

/* HID keycodes for game controls */
#define HID_KEY_A       0x04
#define HID_KEY_C       0x06
#define HID_KEY_D       0x07
#define HID_KEY_L       0x0F
#define HID_KEY_Q       0x14
#define HID_KEY_R       0x15
#define HID_KEY_S       0x16
#define HID_KEY_T       0x17
#define HID_KEY_V       0x19
#define HID_KEY_W       0x1A
#define HID_KEY_X       0x1B
#define HID_KEY_Z       0x1D
#define HID_KEY_LEFTBRACKET 0x2F
#define HID_KEY_ESC     0x29
#define HID_KEY_RIGHT   0x4F
#define HID_KEY_LEFT    0x50
#define HID_KEY_DOWN    0x51
#define HID_KEY_UP      0x52

/* Input state */
static uint8_t keys_held = 0;
static uint8_t keys_prev = 0;
static bool key_restart = false;

#define KEY_LEFT  0x01
#define KEY_RIGHT 0x02
#define KEY_UP    0x04
#define KEY_DOWN  0x08
#define KEY_JUMP  0x10
#define KEY_DASH  0x20
#define KEY_QUIT  0x40
#define KEY_TELE  0x80

/* PICO-8 palette in RGB565 format */
static const uint16_t pico8_palette[16] = {
    0x0000,  /* 0  black */
    0x194A,  /* 1  dark blue */
    0x792A,  /* 2  purple */
    0x0429,  /* 3  green */
    0xAB49,  /* 4  brown */
    0x5AEB,  /* 5  dark grey */
    0xC618,  /* 6  light grey */
    0xFFDF,  /* 7  white */
    0xF809,  /* 8  red */
    0xFD20,  /* 9  orange */
    0xFFE4,  /* 10 yellow */
    0x07E4,  /* 11 bright green */
    0x2D7F,  /* 12 blue */
    0x83B3,  /* 13 lavender */
    0xFBB5,  /* 14 pink */
    0xFE75,  /* 15 peach */
};

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
    {0x7,0x1,0x7,0x4,0x7}, /* x - index 29 */
    {0x0,0x0,0x0,0x0,0x0}, /* space - index 30 */
    {0x2,0x2,0x2,0x0,0x2}, /* ! - index 31 */
    {0x0,0x7,0x0,0x7,0x0}, /* = - index 32 */
    {0x0,0x2,0x0,0x2,0x4}, /* ; - index 33 */
};

/* Palette remap table (for palette swapping like PICO-8's pal()) */
static uint8_t pal_remap[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

int gfx_init(void) {
    printf("gfx_init: entering 150P mode...\n");

    keys_prev = 0;
    keys_held = 0;
    key_restart = false;
    for (int i = 0; i < 16; ++i) {
        pal_remap[i] = (uint8_t)i;
    }

    if (rgb_display_set_mode(SM_150P) != 0) {
        printf("gfx_init: failed to set mode!\n");
        return -1;
    }

    if (!rgb_display_get_framebuffer()) {
        printf("gfx_init: no framebuffer!\n");
        rgb_display_set_mode(SM_TEXT);
        return -1;
    }

    /* Set up PICO-8 palette (first 16 colors) */
    uint16_t palette[256];
    memcpy(palette, pico8_palette, sizeof(pico8_palette));
    /* Fill rest with grayscale */
    for (int i = 16; i < 256; i++) {
        uint8_t gray = i;
        palette[i] = ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
    }
    rgb_display_set_vga_palette(palette);

    rgb_gfx_clear(0);
    printf("gfx_init: success!\n");
    return 0;
}

int gfx_load_sprites(const char* path) {
    /* Sprites are embedded in sprites.c, no loading needed */
    (void)path;
    return 0;
}

void gfx_quit(void) {
    keys_prev = 0;
    keys_held = 0;
    key_restart = false;
    rgb_display_set_mode(SM_TEXT);
    printf("gfx_quit: returned to text mode\n");
}

/* --- Primitives delegated to exported rgb_gfx_* --- */

void gfx_clear(uint8_t color) {
    rgb_gfx_clear(color);
}

void gfx_pixel(int x, int y, uint8_t color) {
    rgb_gfx_pixel(x, y, color);
}

/* gfx.h documents gfx_rect as a filled rectangle */
void gfx_rect(int x, int y, int w, int h, uint8_t color) {
    rgb_gfx_rectfill(x, y, w, h, color);
}

/* PICO-8 style: x0,y0 and x1,y1 are inclusive corners */
void gfx_rectfill(int x0, int y0, int x1, int y1, uint8_t color) {
    int minx = x0 < x1 ? x0 : x1;
    int maxx = x0 > x1 ? x0 : x1;
    int miny = y0 < y1 ? y0 : y1;
    int maxy = y0 > y1 ? y0 : y1;
    rgb_gfx_rectfill(minx, miny, maxx - minx + 1, maxy - miny + 1, color);
}

void gfx_blit(const uint8_t* data, int x, int y, int w, int h, int key_color) {
    rgb_gfx_blit(data, x, y, w, h, w, key_color);
}

void gfx_blit_ex(const uint8_t* data, int x, int y, int w, int h,
                 int key_color, bool flip_x, bool flip_y) {
    rgb_gfx_blit_flip(data, x, y, w, h, w, key_color, flip_x, flip_y);
}

/* --- Game-specific: sprite sheet with palette remap --- */

void gfx_spr(int id, int x, int y, bool flip_x, bool flip_y) {
    if (id < 0 || id >= SHEET_COLS * SHEET_ROWS) return;
    uint8_t *fb = rgb_display_get_framebuffer();
    if (!fb) return;

    int sx = (id % SHEET_COLS) * SPRITE_SIZE;
    int sy = (id / SHEET_COLS) * SPRITE_SIZE;

    for (int py = 0; py < SPRITE_SIZE; py++) {
        int dy = y + py;
        if (dy < 0 || dy >= SCREEN_H) continue;

        int src_py = flip_y ? (SPRITE_SIZE - 1 - py) : py;

        for (int px = 0; px < SPRITE_SIZE; px++) {
            int dx = x + px;
            if (dx < 0 || dx >= SCREEN_W) continue;

            int src_px = flip_x ? (SPRITE_SIZE - 1 - px) : px;
            uint8_t c = sprite_sheet[(sy + src_py) * 128 + (sx + src_px)];

            if (c != 0) {  /* Color 0 is transparent */
                if (c < 16) c = pal_remap[c];
                fb[dy * SCREEN_W + dx] = c;
            }
        }
    }
}

/* --- Game-specific: 3x5 pixel font --- */

static void draw_digit(uint8_t *fb, int x, int y, int digit, uint8_t color) {
    if (!fb || digit < 0 || digit > 9) return;
    for (int row = 0; row < 5; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        uint8_t bits = digit_bitmaps[digit][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (4 >> col)) {
                int dx = x + col;
                if (dx >= 0 && dx < SCREEN_W)
                    fb[dy * SCREEN_W + dx] = color;
            }
        }
    }
}

static void draw_letter(uint8_t *fb, int x, int y, int index, uint8_t color) {
    if (!fb || index < 0 || index > 33) return;
    for (int row = 0; row < 5; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        uint8_t bits = letter_bitmaps[index][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (4 >> col)) {
                int dx = x + col;
                if (dx >= 0 && dx < SCREEN_W)
                    fb[dy * SCREEN_W + dx] = color;
            }
        }
    }
}

int gfx_print_num(int x, int y, int num, uint8_t color) {
    uint8_t *fb = rgb_display_get_framebuffer();
    int start_x = x;
    if (num < 0) {
        if (fb) {
            fb[y * SCREEN_W + x] = color;
            fb[y * SCREEN_W + x + 1] = color;
        }
        x += 4;
        num = -num;
    }
    int temp = num;
    int digits = 0;
    do { digits++; temp /= 10; } while (temp > 0);
    int draw_x = x + (digits - 1) * 4;
    do {
        draw_digit(fb, draw_x, y, num % 10, color);
        num /= 10;
        draw_x -= 4;
    } while (num > 0);
    return x + digits * 4 - start_x;
}

int gfx_print(int x, int y, const char* text, uint8_t color) {
    uint8_t *fb = rgb_display_get_framebuffer();
    int start_x = x;
    while (*text) {
        char c = *text++;
        int index = -1;

        if (c >= 'A' && c <= 'Z') {
            index = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            index = c - 'a';
        } else if (c >= '0' && c <= '9') {
            draw_digit(fb, x, y, c - '0', color);
            x += 4;
            continue;
        } else if (c == ':') {
            index = 26;
        } else if (c == '.') {
            index = 27;
        } else if (c == '-') {
            index = 28;
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

        if (index >= 0)
            draw_letter(fb, x, y, index, color);
        x += 4;
    }
    return x - start_x;
}

void gfx_flip(void) {
    rgb_display_wait_vsync();
}

/* --- Palette swapping --- */

void gfx_pal(uint8_t from, uint8_t to) {
    if (from < 16) pal_remap[from] = to;
}

void gfx_pal_reset(void) {
    for (int i = 0; i < 16; i++) pal_remap[i] = i;
}

/* --- Input --- */

static bool key_down_local(char keycode)
{
    return cardputer_keyboard_key_is_down(keycode) != 0;
}

static bool key_down_bt(unsigned char keycode)
{
    return bt_keyboard_is_pressed(keycode) != 0;
}

static bool key_down_any(char local_key, unsigned char bt_key)
{
    return key_down_local(local_key) || key_down_bt(bt_key);
}

void input_update(void) {
    keys_prev = keys_held;
    keys_held = 0;

    if (key_down_bt(HID_KEY_LEFT) || key_down_local('a'))   keys_held |= KEY_LEFT;
    if (key_down_bt(HID_KEY_RIGHT) || key_down_local('d'))  keys_held |= KEY_RIGHT;
    if (key_down_bt(HID_KEY_UP) || key_down_local('e'))     keys_held |= KEY_UP;
    if (key_down_bt(HID_KEY_DOWN) || key_down_local('z'))   keys_held |= KEY_DOWN;

    if (key_down_any('l', HID_KEY_L))                       keys_held |= KEY_JUMP;
    if (key_down_any(';', HID_KEY_LEFTBRACKET))             keys_held |= KEY_DASH;

    if (key_down_any('t', HID_KEY_T))       keys_held |= KEY_TELE;
    if (key_down_any('q', HID_KEY_Q) || key_down_bt(HID_KEY_ESC)) keys_held |= KEY_QUIT;
    key_restart = key_down_any('r', HID_KEY_R);
}

bool input_left(void)     { return (keys_held & KEY_LEFT) != 0; }
bool input_right(void)    { return (keys_held & KEY_RIGHT) != 0; }
bool input_up(void)       { return (keys_held & KEY_UP) != 0; }
bool input_down(void)     { return (keys_held & KEY_DOWN) != 0; }
bool input_jump(void)     { return (keys_held & KEY_JUMP) != 0; }
bool input_dash(void)     { return (keys_held & KEY_DASH) != 0; }
bool input_quit(void)     { return (keys_held & KEY_QUIT) != 0; }
bool input_restart(void)  { return key_restart; }
bool input_teleport(void) { return (keys_held & KEY_TELE) != 0; }

/* PICO-8 style sin (input 0-1 maps to full cycle, output -1 to 1, inverted) */
float p8sin(float x) {
    return -sinf(x * 6.28318530718f);
}

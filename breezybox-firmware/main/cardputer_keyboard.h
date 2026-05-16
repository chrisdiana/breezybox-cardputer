#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define CARDPUTER_KEY_LEFT_CTRL  0x80
#define CARDPUTER_KEY_LEFT_SHIFT 0x81
#define CARDPUTER_KEY_LEFT_ALT   0x82
#define CARDPUTER_KEY_FN         0xff
#define CARDPUTER_KEY_OPT        0x00
#define CARDPUTER_KEY_BACKSPACE  0x2a
#define CARDPUTER_KEY_TAB        0x2b
#define CARDPUTER_KEY_ENTER      0x28

typedef void (*cardputer_keyboard_char_cb_t)(char c);
typedef struct {
    char base;
    char shifted;
    bool fn;
    bool shift;
    bool ctrl;
    bool alt;
    bool opt;
    bool enter;
    bool tab;
    bool backspace;
    bool pressed;
    uint8_t row;
    uint8_t col;
} cardputer_keyboard_key_event_t;
typedef bool (*cardputer_keyboard_key_cb_t)(const cardputer_keyboard_key_event_t *event);

esp_err_t cardputer_keyboard_init(cardputer_keyboard_char_cb_t cb);
void cardputer_keyboard_poll(void);
void cardputer_keyboard_poll_direct(void);
void cardputer_keyboard_set_poll_interval_ms(uint32_t ms);
uint32_t cardputer_keyboard_get_poll_interval_ms(void);
void cardputer_keyboard_set_char_callback(cardputer_keyboard_char_cb_t cb);
void cardputer_keyboard_set_key_callback(cardputer_keyboard_key_cb_t cb);
void cardputer_keyboard_set_background_poll_enabled(int enabled);
int cardputer_keyboard_pop_event(cardputer_keyboard_key_event_t *event);
int cardputer_keyboard_peek_event(cardputer_keyboard_key_event_t *event);
void cardputer_keyboard_flush_events_queue(void);
int cardputer_keyboard_key_is_down(char keycode);

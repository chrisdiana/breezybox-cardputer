#include "board_runtime.h"
#include "rgb_display.h"
#include "vterm.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <string.h>

#define STICKS3_BTN_A GPIO_NUM_11
#define STICKS3_BTN_B GPIO_NUM_12
#define STICKS3_LCD_BL GPIO_NUM_38

#define STICKS3_KBD_TEXT_MAX 96
#define STICKS3_KEY_ROWS 5
#define STICKS3_LONG_PRESS_MS 650
#define STICKS3_ACCEPT_PRESS_MS 900

#define STICKS3_BL_FREQ_HZ 5000
#define STICKS3_BL_TIMER LEDC_TIMER_1
#define STICKS3_BL_MODE LEDC_LOW_SPEED_MODE
#define STICKS3_BL_CHANNEL LEDC_CHANNEL_1
#define STICKS3_BL_RES LEDC_TIMER_8_BIT

static const char *TAG = "sticks3";

static const board_display_config_t s_board_cfg = {
    .name = "StickS3",
    .spi_host = SPI2_HOST,
    .lcd_pixel_clock_hz = 20 * 1000 * 1000,
    .draw_width = 240,
    .draw_height = 135,
    .gap_x = 40,
    .gap_y = 52,
    .pin_mosi = 39,
    .pin_sclk = 40,
    .pin_cs = 41,
    .pin_dc = 45,
    .pin_rst = 21,
    .invert_color = false,
    .swap_xy = true,
    .mirror_x = true,
    .mirror_y = false,
    .use_st7735_workaround = false,
};

static board_char_cb_t s_char_cb;
static uint32_t s_poll_interval_ms = 25;
static bool s_btn_a_prev;
static bool s_btn_b_prev;
static uint32_t s_btn_a_pressed_since;
static uint32_t s_btn_b_pressed_since;
static uint32_t s_both_pressed_since;
static bool s_both_long_handled;

static const char *s_rows[STICKS3_KEY_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm,./",
    "^_<>!",
};

static lcd_cell_t s_cells[DISPLAY_COLS * DISPLAY_ROWS];
static char s_text[STICKS3_KBD_TEXT_MAX + 1];
static int s_text_len;
static int s_row;
static int s_col;
static bool s_upper;
static bool s_active;

static void softkbd_clear(void)
{
    for (int i = 0; i < DISPLAY_COLS * DISPLAY_ROWS; ++i) {
        s_cells[i].ch = ' ';
        s_cells[i].attr = VTERM_ATTR(VTERM_WHITE, VTERM_BLACK);
    }
}

static void softkbd_puts(int row, int col, const char *text, uint8_t attr)
{
    if (row < 0 || row >= DISPLAY_ROWS || col < 0 || col >= DISPLAY_COLS) {
        return;
    }

    lcd_cell_t *cell = &s_cells[row * DISPLAY_COLS + col];
    while (*text && col < DISPLAY_COLS) {
        cell->ch = *text++;
        cell->attr = attr;
        ++cell;
        ++col;
    }
}

static char softkbd_selected_key(void)
{
    const char *row = s_rows[s_row];
    size_t len = strlen(row);
    if (len == 0) {
        return ' ';
    }
    if (s_col >= (int)len) {
        s_col = (int)len - 1;
    }

    return row[s_col];
}

static void softkbd_feed_line(void)
{
    if (!s_char_cb) {
        return;
    }
    for (int i = 0; i < s_text_len; ++i) {
        s_char_cb(s_text[i]);
    }
    s_char_cb('\n');
}

static void softkbd_close(bool accept)
{
    if (accept) {
        softkbd_feed_line();
    }
    s_active = false;
    rgb_display_direct_text_end();
}

static void softkbd_append_char(char c)
{
    if (s_text_len >= STICKS3_KBD_TEXT_MAX) {
        return;
    }
    s_text[s_text_len++] = c;
    s_text[s_text_len] = '\0';
}

static bool softkbd_apply_selection(void)
{
    char key = softkbd_selected_key();
    if (s_upper && key >= 'a' && key <= 'z') {
        key = (char)(key - 'a' + 'A');
    }

    switch (key) {
        case '^':
            s_upper = !s_upper;
            return false;
        case '_':
            softkbd_append_char(' ');
            return false;
        case '<':
            if (s_text_len > 0) {
                s_text[--s_text_len] = '\0';
            }
            return false;
        case '>':
            softkbd_close(false);
            return true;
        case '!':
            softkbd_close(true);
            return true;
        default:
            softkbd_append_char(key);
            return false;
    }
}

static void softkbd_prev_key(void)
{
    int len = (int)strlen(s_rows[s_row]);
    s_col--;
    if (s_col < 0) {
        s_col = len - 1;
    }
}

static void softkbd_next_key(void)
{
    int len = (int)strlen(s_rows[s_row]);
    s_col++;
    if (s_col >= len) {
        s_col = 0;
    }
}

static void softkbd_row_up(void)
{
    s_row--;
    if (s_row < 0) {
        s_row = STICKS3_KEY_ROWS - 1;
    }
    int len = (int)strlen(s_rows[s_row]);
    if (s_col >= len) {
        s_col = len - 1;
    }
}

static void softkbd_row_down(void)
{
    s_row++;
    if (s_row >= STICKS3_KEY_ROWS) {
        s_row = 0;
    }
    int len = (int)strlen(s_rows[s_row]);
    if (s_col >= len) {
        s_col = len - 1;
    }
}

static void softkbd_redraw(void)
{
    softkbd_clear();
    softkbd_puts(0, 0, "StickS3 Input", VTERM_ATTR(VTERM_YELLOW, VTERM_BLACK));
    softkbd_puts(1, 0, s_text_len ? s_text : "_", VTERM_ATTR(VTERM_WHITE, VTERM_BLUE));

    for (int r = 0; r < STICKS3_KEY_ROWS; ++r) {
        const char *line = s_rows[r];
        int len = (int)strlen(line);
        int y = 3 + r;

        for (int i = 0; i < len && i < DISPLAY_COLS; ++i) {
            char key = line[i];
            char draw = key;

            if (r == STICKS3_KEY_ROWS - 1) {
                switch (key) {
                    case '^': draw = s_upper ? 'U' : 'u'; break;
                    case '_': draw = 'S'; break;
                    case '<': draw = 'B'; break;
                    case '>': draw = 'X'; break;
                    case '!': draw = 'E'; break;
                    default: break;
                }
            } else if (s_upper && key >= 'a' && key <= 'z') {
                draw = (char)(key - 'a' + 'A');
            }

            lcd_cell_t *cell = &s_cells[y * DISPLAY_COLS + i];
            cell->ch = draw;
            cell->attr = (r == s_row && i == s_col)
                ? VTERM_ATTR(VTERM_BLACK, VTERM_CYAN)
                : VTERM_ATTR(VTERM_WHITE, VTERM_BLACK);
        }
    }

    softkbd_puts(DISPLAY_ROWS - 2, 0, "A< B> A+B sel", VTERM_ATTR(VTERM_CYAN, VTERM_BLACK));
    softkbd_puts(DISPLAY_ROWS - 1, 0, "hold A/B row A+B ok", VTERM_ATTR(VTERM_CYAN, VTERM_BLACK));
    rgb_display_direct_text_redraw(s_cells, -1, -1);
}

static void softkbd_open(void)
{
    s_active = true;
    s_text_len = 0;
    s_text[0] = '\0';
    s_row = 0;
    s_col = 0;
    s_upper = false;
    s_btn_a_pressed_since = 0;
    s_btn_b_pressed_since = 0;
    s_both_pressed_since = 0;
    s_both_long_handled = false;
    rgb_display_direct_text_begin();
    softkbd_redraw();
}

const char *board_runtime_name(void)
{
    return s_board_cfg.name;
}

const board_display_config_t *board_get_display_config(void)
{
    return &s_board_cfg;
}

esp_err_t board_display_power_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = STICKS3_BL_MODE,
        .timer_num = STICKS3_BL_TIMER,
        .duty_resolution = STICKS3_BL_RES,
        .freq_hz = STICKS3_BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .gpio_num = STICKS3_LCD_BL,
        .speed_mode = STICKS3_BL_MODE,
        .channel = STICKS3_BL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = STICKS3_BL_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
    return ESP_OK;
}

void board_set_backlight(uint8_t level)
{
    uint32_t max_duty = (1U << STICKS3_BL_RES) - 1U;
    uint32_t duty = (max_duty * level) / 255U;

    if (ledc_set_duty(STICKS3_BL_MODE, STICKS3_BL_CHANNEL, duty) != ESP_OK) {
        return;
    }
    (void)ledc_update_duty(STICKS3_BL_MODE, STICKS3_BL_CHANNEL);
}

esp_err_t board_input_init(board_char_cb_t cb)
{
    s_char_cb = cb;
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << STICKS3_BTN_A) | (1ULL << STICKS3_BTN_B),
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    return ESP_OK;
}

void board_input_poll(void)
{
    bool a_down = gpio_get_level(STICKS3_BTN_A) == 0;
    bool b_down = gpio_get_level(STICKS3_BTN_B) == 0;
    bool both_down = a_down && b_down;
    bool both_prev = s_btn_a_prev && s_btn_b_prev;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!s_active) {
        if ((a_down && !s_btn_a_prev) || (b_down && !s_btn_b_prev)) {
            softkbd_open();
        }
        s_btn_a_prev = a_down;
        s_btn_b_prev = b_down;
        return;
    }

    if (a_down && !s_btn_a_prev) {
        s_btn_a_pressed_since = now;
    }
    if (b_down && !s_btn_b_prev) {
        s_btn_b_pressed_since = now;
    }

    if (both_down) {
        if (s_both_pressed_since == 0) {
            s_both_pressed_since = now;
            s_both_long_handled = false;
        } else if (!s_both_long_handled &&
                   (now - s_both_pressed_since) >= STICKS3_ACCEPT_PRESS_MS) {
            softkbd_close(true);
            s_both_long_handled = true;
            s_btn_a_prev = a_down;
            s_btn_b_prev = b_down;
            return;
        }
    } else if (!both_prev) {
        s_both_pressed_since = 0;
        s_both_long_handled = false;
    }

    if (!both_down && both_prev && !s_both_long_handled) {
        if (!softkbd_apply_selection()) {
            softkbd_redraw();
        }
        s_both_pressed_since = 0;
    } else if (!a_down && s_btn_a_prev && !both_prev) {
        if ((now - s_btn_a_pressed_since) >= STICKS3_LONG_PRESS_MS) {
            softkbd_row_up();
        } else {
            softkbd_prev_key();
        }
        softkbd_redraw();
    } else if (!b_down && s_btn_b_prev && !both_prev) {
        if ((now - s_btn_b_pressed_since) >= STICKS3_LONG_PRESS_MS) {
            softkbd_row_down();
        } else {
            softkbd_next_key();
        }
        softkbd_redraw();
    }

    s_btn_a_prev = a_down;
    s_btn_b_prev = b_down;
}

uint32_t board_input_get_poll_interval_ms(void)
{
    return s_poll_interval_ms;
}

void board_input_set_poll_interval_ms(uint32_t ms)
{
    s_poll_interval_ms = ms ? ms : 25;
}

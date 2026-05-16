#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include <stdbool.h>
#include <stdint.h>

typedef void (*board_char_cb_t)(char c);

typedef struct {
    const char *name;
    int spi_host;
    int lcd_pixel_clock_hz;
    int draw_width;
    int draw_height;
    int gap_x;
    int gap_y;
    int pin_mosi;
    int pin_sclk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    bool invert_color;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool use_st7735_workaround;
} board_display_config_t;

const char *board_runtime_name(void);
const board_display_config_t *board_get_display_config(void);

esp_err_t board_display_power_init(void);
void board_set_backlight(uint8_t level);

esp_err_t board_input_init(board_char_cb_t cb);
void board_input_poll(void);
uint32_t board_input_get_poll_interval_ms(void);
void board_input_set_poll_interval_ms(uint32_t ms);


#include "board_runtime.h"
#include "cardputer_keyboard.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#define CARDPUTER_PIN_NUM_BK_LIGHT        38
#define CARDPUTER_BK_LIGHT_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define CARDPUTER_BK_LIGHT_LEDC_TIMER     LEDC_TIMER_1
#define CARDPUTER_BK_LIGHT_LEDC_CHANNEL   LEDC_CHANNEL_1
#define CARDPUTER_BK_LIGHT_LEDC_RES       LEDC_TIMER_8_BIT

static const board_display_config_t s_board_cfg = {
    .name = "Cardputer",
    .spi_host = SPI2_HOST,
    .lcd_pixel_clock_hz = 40 * 1000 * 1000,
    .draw_width = 240,
    .draw_height = 135,
    .gap_x = 40,
    .gap_y = 53,
    .pin_mosi = 35,
    .pin_sclk = 36,
    .pin_cs = 37,
    .pin_dc = 34,
    .pin_rst = 33,
    .invert_color = true,
    .swap_xy = true,
    .mirror_x = true,
    .mirror_y = false,
    .use_st7735_workaround = false,
};

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
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CARDPUTER_PIN_NUM_BK_LIGHT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(CARDPUTER_PIN_NUM_BK_LIGHT, 0);

    ledc_timer_config_t bk_timer_cfg = {
        .speed_mode = CARDPUTER_BK_LIGHT_LEDC_MODE,
        .timer_num = CARDPUTER_BK_LIGHT_LEDC_TIMER,
        .duty_resolution = CARDPUTER_BK_LIGHT_LEDC_RES,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bk_timer_cfg));

    ledc_channel_config_t bk_chan_cfg = {
        .gpio_num = CARDPUTER_PIN_NUM_BK_LIGHT,
        .speed_mode = CARDPUTER_BK_LIGHT_LEDC_MODE,
        .channel = CARDPUTER_BK_LIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = CARDPUTER_BK_LIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bk_chan_cfg));
    return ESP_OK;
}

void board_set_backlight(uint8_t level)
{
    uint32_t duty = level;
    ledc_set_duty(CARDPUTER_BK_LIGHT_LEDC_MODE, CARDPUTER_BK_LIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(CARDPUTER_BK_LIGHT_LEDC_MODE, CARDPUTER_BK_LIGHT_LEDC_CHANNEL);
}

esp_err_t board_input_init(board_char_cb_t cb)
{
    return cardputer_keyboard_init(cb);
}

void board_input_poll(void)
{
    cardputer_keyboard_poll();
}

uint32_t board_input_get_poll_interval_ms(void)
{
    return cardputer_keyboard_get_poll_interval_ms();
}

void board_input_set_poll_interval_ms(uint32_t ms)
{
    cardputer_keyboard_set_poll_interval_ms(ms);
}


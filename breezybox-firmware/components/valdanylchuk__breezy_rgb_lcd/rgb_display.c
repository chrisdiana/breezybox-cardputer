/*
 * rgb_display.c - Cardputer ADV text-mode display backend
 *
 * The original component targeted a 1024x600 RGB panel with DMA bounce buffers.
 * For Cardputer ADV we keep the same public API, but render a compact text mode
 * into a framebuffer and push it to the ST7789 SPI panel at a steady refresh rate.
 */

#include "rgb_display.h"
#include "rgb_gfx.h"
#include "board_runtime.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#define PROGMEM
#include "glcdfont.h"

static const char *TAG = "display";

#define LCD_MAX_DRAW_WIDTH      240
#define LCD_MAX_DRAW_HEIGHT     135
#define GFX_FB_VGA_WIDTH        320
#define GFX_FB_VGA_HEIGHT       200
#define GFX_FB_150P_WIDTH       240
#define GFX_FB_150P_HEIGHT      150
#define GFX_FB_150P_BYTES       (GFX_FB_150P_WIDTH * GFX_FB_150P_HEIGHT)

#define FONT_BITMAP_WIDTH       5
#define FONT_WIDTH              6
#define FONT_HEIGHT             8
#define TEXT_COLS               DISPLAY_COLS
#define TEXT_ROWS               DISPLAY_ROWS
#define CURSOR_HEIGHT           2
#define REFRESH_PERIOD_MS       33

static lcd_cell_t *s_display_buffer;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static const board_display_config_t *s_cfg;
static screen_mode_t s_screen_mode = SM_TEXT;
static const rgb_display_callbacks_t *s_callbacks;
static uint16_t *s_panel_buffer;
static uint8_t *s_gfx_buffer;
static int s_gfx_width;
static int s_gfx_height;
static size_t s_gfx_capacity_bytes;
static TaskHandle_t s_refresh_task;
static volatile int s_cursor_col = -1;
static volatile int s_cursor_row = -1;
static volatile bool s_text_refresh_enabled = true;
static uint32_t s_frame_count;
static uint32_t s_text_colors[16];
static uint16_t s_vga_palette[256];
static uint8_t s_backlight_level = 255;

static void release_gfx_buffer(void);
static void apply_backlight_level(uint8_t level);

static inline int lcd_draw_width(void)
{
    return s_cfg ? s_cfg->draw_width : LCD_MAX_DRAW_WIDTH;
}

static inline int lcd_draw_height(void)
{
    return s_cfg ? s_cfg->draw_height : LCD_MAX_DRAW_HEIGHT;
}

static inline int text_offset_y(void)
{
    return (lcd_draw_height() - (TEXT_ROWS * FONT_HEIGHT)) / 2;
}

static const uint16_t s_cga_colors[16] = {
    0x0000, 0x0015, 0x0540, 0x0555,
    0xA800, 0xA815, 0xA520, 0xAD55,
    0x52AA, 0x52BF, 0x57EA, 0x57FF,
    0xFAAA, 0xFABF, 0xFFE0, 0xFFFF,
};

static inline uint8_t glyph_col_bits(uint8_t ch, int col)
{
    return font[((uint8_t)ch * FONT_BITMAP_WIDTH) + col];
}

static void init_vga_palette(void)
{
    memcpy(s_vga_palette, s_cga_colors, sizeof(s_cga_colors));
    for (int i = 16; i < 256; ++i) {
        s_vga_palette[i] = s_cga_colors[i & 0x0F];
    }
}

static void rebuild_text_palette(void)
{
    const uint16_t *palette = (s_callbacks && s_callbacks->get_text_palette)
        ? s_callbacks->get_text_palette()
        : s_cga_colors;

    for (int i = 0; i < 16; ++i) {
        s_text_colors[i] = palette[i];
    }
}

static void clear_framebuffer(uint16_t color)
{
    for (int i = 0; i < lcd_draw_width() * lcd_draw_height(); ++i) {
        s_panel_buffer[i] = color;
    }
}

static esp_err_t ensure_gfx_buffer(size_t bytes)
{
    if (s_gfx_buffer && s_gfx_capacity_bytes >= bytes) {
        return ESP_OK;
    }

    release_gfx_buffer();

    s_gfx_buffer = heap_caps_malloc(
        bytes,
        MALLOC_CAP_8BIT
    );
    if (!s_gfx_buffer) {
        ESP_LOGE(TAG,
                 "Failed to allocate graphics framebuffer (%u bytes, free=%u, largest=%u)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    s_gfx_capacity_bytes = bytes;

    return ESP_OK;
}

static void release_gfx_buffer(void)
{
    if (s_gfx_buffer) {
        heap_caps_free(s_gfx_buffer);
        s_gfx_buffer = NULL;
    }
    s_gfx_capacity_bytes = 0;
}

static void apply_backlight_level(uint8_t level)
{
    board_set_backlight(level);
    s_backlight_level = level;
}

static void render_text_frame(void)
{
    const int blink_on = ((s_frame_count / 15U) & 1U) != 0;

    clear_framebuffer(s_text_colors[0]);
    if (!s_display_buffer) {
        return;
    }

    for (int row = 0; row < TEXT_ROWS; ++row) {
        const lcd_cell_t *cell_row = &s_display_buffer[row * TEXT_COLS];
        const int pixel_y = text_offset_y() + (row * FONT_HEIGHT);

        for (int col = 0; col < TEXT_COLS; ++col) {
            lcd_cell_t cell = cell_row[col];
            uint16_t fg = (uint16_t)s_text_colors[LCD_ATTR_FG(cell.attr)];
            uint16_t bg = (uint16_t)s_text_colors[LCD_ATTR_BG(cell.attr)];
            const int pixel_x = col * FONT_WIDTH;

            for (int glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
                uint16_t *dest = &s_panel_buffer[(pixel_y + glyph_row) * lcd_draw_width() + pixel_x];
                for (int glyph_col = 0; glyph_col < FONT_BITMAP_WIDTH; ++glyph_col) {
                    uint8_t bits = glyph_col_bits((uint8_t)cell.ch, glyph_col);
                    dest[glyph_col] = (bits & (1U << glyph_row)) ? fg : bg;
                }
                dest[FONT_BITMAP_WIDTH] = bg;
            }

            if (blink_on && row == s_cursor_row && col == s_cursor_col) {
                uint16_t *dest = &s_panel_buffer[(pixel_y + FONT_HEIGHT - CURSOR_HEIGHT) * lcd_draw_width() + pixel_x];
                for (int y = 0; y < CURSOR_HEIGHT; ++y) {
                    for (int x = 0; x < FONT_WIDTH; ++x) {
                        dest[x] = fg;
                    }
                    dest += lcd_draw_width();
                }
            }
        }
    }
}

static void render_text_cell_to_buffer(uint16_t *dst, lcd_cell_t cell, bool cursor)
{
    uint16_t fg = (uint16_t)s_text_colors[LCD_ATTR_FG(cell.attr)];
    uint16_t bg = (uint16_t)s_text_colors[LCD_ATTR_BG(cell.attr)];

    for (int glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
        uint16_t *row_dst = &dst[glyph_row * FONT_WIDTH];
        for (int glyph_col = 0; glyph_col < FONT_BITMAP_WIDTH; ++glyph_col) {
            uint8_t bits = glyph_col_bits((uint8_t)cell.ch, glyph_col);
            row_dst[glyph_col] = (bits & (1U << glyph_row)) ? fg : bg;
        }
        row_dst[FONT_BITMAP_WIDTH] = bg;
    }

    if (cursor) {
        uint16_t *cursor_dst = &dst[(FONT_HEIGHT - CURSOR_HEIGHT) * FONT_WIDTH];
        for (int y = 0; y < CURSOR_HEIGHT; ++y) {
            for (int x = 0; x < FONT_WIDTH; ++x) {
                cursor_dst[x] = fg;
            }
            cursor_dst += FONT_WIDTH;
        }
    }
}

static void direct_draw_cell_internal(int col, int row, lcd_cell_t cell, bool cursor)
{
    if (!s_panel || col < 0 || col >= TEXT_COLS || row < 0 || row >= TEXT_ROWS) {
        return;
    }

    uint16_t tile[FONT_WIDTH * FONT_HEIGHT];
    const int pixel_x = col * FONT_WIDTH;
    const int pixel_y = text_offset_y() + (row * FONT_HEIGHT);

    render_text_cell_to_buffer(tile, cell, cursor);
    esp_lcd_panel_draw_bitmap(s_panel,
                              pixel_x,
                              pixel_y,
                              pixel_x + FONT_WIDTH,
                              pixel_y + FONT_HEIGHT,
                              tile);
}

static void direct_draw_row_internal(const lcd_cell_t *row_cells, int row, int cursor_col)
{
    enum { STRIP_CAPACITY = LCD_MAX_DRAW_WIDTH * FONT_HEIGHT };
    if (!s_panel || !row_cells || row < 0 || row >= TEXT_ROWS) {
        return;
    }

    static uint16_t strip[STRIP_CAPACITY];
    const int draw_width = lcd_draw_width();
    const int pixel_y = text_offset_y() + (row * FONT_HEIGHT);

    for (int col = 0; col < TEXT_COLS; ++col) {
        uint16_t tile[FONT_WIDTH * FONT_HEIGHT];
        render_text_cell_to_buffer(tile, row_cells[col], cursor_col == col);
        for (int glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
            memcpy(&strip[glyph_row * draw_width + col * FONT_WIDTH],
                   &tile[glyph_row * FONT_WIDTH],
                   FONT_WIDTH * sizeof(uint16_t));
        }
    }

    esp_lcd_panel_draw_bitmap(s_panel,
                              0,
                              pixel_y,
                              draw_width,
                              pixel_y + FONT_HEIGHT,
                              strip);
}

static void refresh_task(void *arg)
{
    while (1) {
        if (s_screen_mode == SM_TEXT && s_panel && s_panel_buffer && s_text_refresh_enabled) {
            s_frame_count++;
            render_text_frame();
            esp_lcd_panel_draw_bitmap(s_panel, 0, 0, lcd_draw_width(), lcd_draw_height(), s_panel_buffer);
        }
        else if (s_screen_mode != SM_TEXT && s_panel && s_panel_buffer && s_gfx_buffer) {
            const int src_w = s_gfx_width;
            const int src_h = s_gfx_height;
            const int draw_width = lcd_draw_width();
            const int draw_height = lcd_draw_height();
            for (int y = 0; y < draw_height; ++y) {
                const int sy = (y * src_h) / draw_height;
                const uint8_t *src_row = &s_gfx_buffer[sy * src_w];
                uint16_t *dst_row = &s_panel_buffer[y * draw_width];

                for (int x = 0; x < draw_width; ++x) {
                    const int sx = (x * src_w) / draw_width;
                    dst_row[x] = s_vga_palette[src_row[sx]];
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, 0, draw_width, draw_height, s_panel_buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_PERIOD_MS));
    }
}

static int enter_graphics_mode(screen_mode_t mode)
{
    if (mode == SM_150P) {
        s_gfx_width = GFX_FB_150P_WIDTH;
        s_gfx_height = GFX_FB_150P_HEIGHT;
    } else {
        s_gfx_width = GFX_FB_VGA_WIDTH;
        s_gfx_height = GFX_FB_VGA_HEIGHT;
    }

    if (ensure_gfx_buffer((size_t)s_gfx_width * (size_t)s_gfx_height) != ESP_OK) {
        s_gfx_width = 0;
        s_gfx_height = 0;
        return -1;
    }

    memset(s_gfx_buffer, 0, s_gfx_width * s_gfx_height);
    if (s_callbacks && s_callbacks->enter_graphics) {
        if (s_callbacks->enter_graphics() != 0) {
            return -1;
        }
    }
    s_screen_mode = mode;
    return 0;
}

void rgb_display_init(void)
{
    s_cfg = board_get_display_config();

    spi_bus_config_t buscfg = {
        .sclk_io_num = s_cfg->pin_sclk,
        .mosi_io_num = s_cfg->pin_mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_cfg->draw_width * s_cfg->draw_height * sizeof(uint16_t),
    };

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = s_cfg->pin_dc,
        .cs_gpio_num = s_cfg->pin_cs,
        .pclk_hz = s_cfg->lcd_pixel_clock_hz,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = s_cfg->pin_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };

    volatile const void *exports[] = {
        (void *)rgb_display_refresh_palette,
        (void *)rgb_display_set_mode,
        (void *)rgb_display_get_mode,
        (void *)rgb_display_get_framebuffer,
        (void *)rgb_display_get_fb_width,
        (void *)rgb_display_get_fb_height,
        (void *)rgb_display_set_vga_palette,
        (void *)rgb_display_set_vga_palette_entry,
        (void *)rgb_display_get_vga_palette_entry,
        (void *)rgb_display_wait_vsync,
        (void *)rgb_gfx_clear,
        (void *)rgb_gfx_pixel,
        (void *)rgb_gfx_hline,
        (void *)rgb_gfx_vline,
        (void *)rgb_gfx_rect,
        (void *)rgb_gfx_rectfill,
        (void *)rgb_gfx_blit,
        (void *)rgb_gfx_blit_flip,
    };
    (void)exports;

    init_vga_palette();
    rebuild_text_palette();

    // Reserve the lighter 150p graphics framebuffer up front so later Lua/script
    // activity does not fragment the heap before first graphics-mode entry.
    if (ensure_gfx_buffer(GFX_FB_150P_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "150p graphics buffer was not preallocated at startup");
    }

    ESP_ERROR_CHECK(board_display_power_init());

    s_panel_buffer = heap_caps_malloc(
        s_cfg->draw_width * s_cfg->draw_height * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT
    );
    ESP_ERROR_CHECK(s_panel_buffer ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)s_cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_cfg->spi_host, &io_config, &s_panel_io));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, s_cfg->invert_color));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, s_cfg->gap_x, s_cfg->gap_y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, s_cfg->swap_xy));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, s_cfg->mirror_x, s_cfg->mirror_y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    clear_framebuffer(s_cga_colors[0]);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_cfg->draw_width, s_cfg->draw_height, s_panel_buffer));
    apply_backlight_level(255);

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            refresh_task, "lcd_refresh", 4096, NULL, 4, &s_refresh_task, 1
        );
        ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
    }

    ESP_LOGI(TAG, "%s display ready: %dx%d pixels, %dx%d chars",
             board_runtime_name(), s_cfg->draw_width, s_cfg->draw_height, TEXT_COLS, TEXT_ROWS);
}

void rgb_display_set_buffer(lcd_cell_t *cells)
{
    s_display_buffer = cells;
}

void rgb_display_set_callbacks(const rgb_display_callbacks_t *cb)
{
    s_callbacks = cb;
    rebuild_text_palette();
}

void rgb_display_refresh_palette(void)
{
    rebuild_text_palette();
}

void rgb_display_set_cursor(int col, int row)
{
    s_cursor_col = col;
    s_cursor_row = row;
}

void rgb_display_direct_text_begin(void)
{
    s_text_refresh_enabled = false;
}

void rgb_display_direct_text_end(void)
{
    s_text_refresh_enabled = true;
}

void rgb_display_direct_text_redraw(const lcd_cell_t *cells, int cursor_col, int cursor_row)
{
    if (!cells || !s_panel || !s_panel_buffer) {
        return;
    }

    const lcd_cell_t *saved = s_display_buffer;
    int saved_col = s_cursor_col;
    int saved_row = s_cursor_row;

    s_display_buffer = (lcd_cell_t *)cells;
    s_cursor_col = cursor_col;
    s_cursor_row = cursor_row;
    render_text_frame();
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, lcd_draw_width(), lcd_draw_height(), s_panel_buffer);

    s_display_buffer = (lcd_cell_t *)saved;
    s_cursor_col = saved_col;
    s_cursor_row = saved_row;
}

void rgb_display_direct_text_draw_cell(int col, int row, lcd_cell_t cell, int draw_cursor)
{
    direct_draw_cell_internal(col, row, cell, draw_cursor != 0);
}

void rgb_display_direct_text_draw_row(const lcd_cell_t *row_cells, int row, int cursor_col)
{
    direct_draw_row_internal(row_cells, row, cursor_col);
}

screen_mode_t rgb_display_get_mode(void)
{
    return s_screen_mode;
}

int rgb_display_set_mode(screen_mode_t mode)
{
    if (mode == SM_TEXT) {
        if (s_screen_mode != SM_TEXT && s_callbacks && s_callbacks->exit_graphics) {
            if (s_callbacks->exit_graphics() != 0) {
                return -1;
            }
        }
        s_screen_mode = SM_TEXT;
        // Keep the graphics buffer reserved across mode switches so later
        // Lua/demo graphics calls do not fail due to heap fragmentation.
        if (s_callbacks && s_callbacks->get_text_buffer) {
            s_display_buffer = s_callbacks->get_text_buffer();
        }
        if (s_callbacks && s_callbacks->flush_input) {
            s_callbacks->flush_input();
        }
        return 0;
    }

    if (mode == SM_VGA13H || mode == SM_150P) {
        return enter_graphics_mode(mode);
    }

    ESP_LOGW(TAG, "Unknown screen mode %d", mode);
    return -1;
}

uint8_t *rgb_display_get_framebuffer(void)
{
    return (s_screen_mode == SM_TEXT) ? NULL : s_gfx_buffer;
}

int rgb_display_get_fb_width(void)
{
    if (s_screen_mode == SM_VGA13H || s_screen_mode == SM_150P) {
        return s_gfx_width;
    }
    return 0;
}

int rgb_display_get_fb_height(void)
{
    if (s_screen_mode == SM_VGA13H || s_screen_mode == SM_150P) {
        return s_gfx_height;
    }
    return 0;
}

void rgb_display_set_vga_palette(const uint16_t palette[256])
{
    memcpy(s_vga_palette, palette, sizeof(s_vga_palette));
}

void rgb_display_set_vga_palette_entry(int index, uint16_t rgb565)
{
    if (index >= 0 && index < 256) {
        s_vga_palette[index] = rgb565;
    }
}

uint16_t rgb_display_get_vga_palette_entry(int index)
{
    if (index >= 0 && index < 256) {
        return s_vga_palette[index];
    }
    return 0;
}

void rgb_display_set_backlight(uint8_t level)
{
    apply_backlight_level(level);
}

uint8_t rgb_display_get_backlight(void)
{
    return s_backlight_level;
}

void rgb_display_wait_vsync(void)
{
}

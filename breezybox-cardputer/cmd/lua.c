#include "breezy_cmd.h"
#include "breezy_exec.h"
#include "breezy_vfs.h"
#if defined(BREEZY_BOARD_CARDPUTER)
#include "cardputer_keyboard.h"
#endif
#include "rgb_display.h"
#include "rgb_gfx.h"
#include "vterm.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"

#include "cJSON.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "linenoise/linenoise.h"
#include "esp_heap_caps.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LUA_LINE_MAX 256
#define LUA_UART_READ_MAX 512
#define LUA_TCP_SLOT_COUNT 4
#define LUA_CONFIG_DEFAULT_PATH "/root/.lua_config.json"
#define LUA_HTTP_BODY_MAX 2048
#define LUA_TONE_CHANNEL LEDC_CHANNEL_0
#define LUA_TONE_TIMER   LEDC_TIMER_0
#define LUA_TONE_MODE    LEDC_LOW_SPEED_MODE
#define LUA_TONE_RES     LEDC_TIMER_10_BIT
#define LUA_GFX_IMAGE_MT "breezy.gfx.image"
#define LUA_CARDPUTER_BATTERY_GPIO 10
#define LUA_CARDPUTER_BATTERY_MIN_UV 1575000
#define LUA_CARDPUTER_BATTERY_MAX_UV 2100000
#define LUA_CARDPUTER_SPK_BCLK 41
#define LUA_CARDPUTER_SPK_WS   43
#define LUA_CARDPUTER_SPK_DOUT 42

enum {
    LUA_GFX_FONT_SMALL = 0,
    LUA_GFX_FONT_TERM16 = 1,
};

typedef struct {
    bool open;
    uart_port_t port;
} lua_uart_slot_t;

static lua_uart_slot_t s_lua_uart_slots[UART_NUM_MAX];
typedef struct {
    bool open;
    int fd;
} lua_tcp_slot_t;

static esp_netif_t *lua_wifi_sta_netif(void);
static void lua_dns_info_to_string(const esp_netif_dns_info_t *dns, char *buf, size_t buf_len);
static esp_err_t lua_network_ensure_dns(char *main_buf, size_t main_buf_len,
                                        char *backup_buf, size_t backup_buf_len);

typedef struct {
    bool open;
    i2c_port_t port;
    int sda_pin;
    int scl_pin;
    uint32_t freq_hz;
} lua_i2c_bus_t;

typedef struct {
    bool open;
    int sclk_pin;
    int mosi_pin;
    int miso_pin;
    int cs_pin;
    int mode;
    int delay_us;
    bool msb_first;
} lua_spi_bus_t;

typedef struct {
    bool open;
    i2s_chan_handle_t handle;
    int sample_rate;
    int bits;
    int channels;
    int data_pin;
    int bclk_pin;
    int ws_pin;
    int mclk_pin;
} lua_i2s_chan_t;

typedef struct {
    bool open;
    i2s_chan_handle_t handle;
    int sample_rate;
    int volume;
} lua_builtin_speaker_t;

typedef struct {
    bool open;
    i2s_chan_handle_t handle;
    int sample_rate;
    int bits;
    int channels;
    int bclk_pin;
    int ws_pin;
    int din_pin;
} lua_mic_state_t;

typedef struct {
    int width;
    int height;
    uint8_t pixels[];
} lua_gfx_image_t;

static adc_oneshot_unit_handle_t s_lua_adc1 = NULL;
static int s_lua_gfx_font = LUA_GFX_FONT_SMALL;
static lua_i2c_bus_t s_lua_i2c = {0};
static lua_spi_bus_t s_lua_spi = {0};
static lua_i2s_chan_t s_lua_i2s_tx = {0};
static lua_i2s_chan_t s_lua_i2s_rx = {0};
static lua_tcp_slot_t s_lua_tcp_slots[LUA_TCP_SLOT_COUNT];
static lua_builtin_speaker_t s_lua_speaker = {0};
static lua_mic_state_t s_lua_mic = {0};

static void print_lua_error(lua_State *L, const char *prefix)
{
    const char *msg = lua_tostring(L, -1);
    if (!msg) {
        msg = "(unknown error)";
    }
    if (prefix) {
        printf("%s: %s\n", prefix, msg);
    } else {
        printf("%s\n", msg);
    }
    lua_pop(L, 1);
}

static esp_err_t lua_adc_ensure_unit(void);

static int l_breezy_exec(lua_State *L)
{
    const char *cmd = luaL_checkstring(L, 1);
    int rc = breezybox_exec(cmd);
    lua_pushinteger(L, rc);
    return 1;
}

static int l_breezy_cwd(lua_State *L)
{
    char cwd[BREEZYBOX_MAX_PATH * 2];
    breezybox_get_cwd(cwd, sizeof(cwd));
    lua_pushstring(L, cwd);
    return 1;
}

static int l_breezy_cd(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char resolved[BREEZYBOX_MAX_PATH * 2];
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "path too long: %s", path);
    }
    if (breezybox_set_cwd(resolved) != 0) {
        return luaL_error(L, "cannot change directory: %s", resolved);
    }
    return 0;
}

static int l_breezy_listdir(lua_State *L)
{
    const char *path = luaL_optstring(L, 1, ".");
    char resolved[BREEZYBOX_MAX_PATH * 2];
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "path too long: %s", path);
    }

    DIR *dir = opendir(resolved);
    if (!dir) {
        return luaL_error(L, "cannot open directory: %s", resolved);
    }

    lua_newtable(L);
    int idx = 1;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        lua_pushstring(L, ent->d_name);
        lua_rawseti(L, -2, idx++);
    }
    closedir(dir);
    return 1;
}

static int l_breezy_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char resolved[BREEZYBOX_MAX_PATH * 2];
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "path too long: %s", path);
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) {
        return luaL_error(L, "cannot open file: %s", resolved);
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return luaL_error(L, "cannot seek file: %s", resolved);
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return luaL_error(L, "cannot stat file: %s", resolved);
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return luaL_error(L, "out of memory");
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';

    lua_pushlstring(L, buf, n);
    free(buf);
    return 1;
}

static int l_breezy_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    int append = lua_toboolean(L, 3);

    char resolved[BREEZYBOX_MAX_PATH * 2];
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "path too long: %s", path);
    }

    FILE *f = fopen(resolved, append ? "ab" : "wb");
    if (!f) {
        return luaL_error(L, "cannot open file: %s", resolved);
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        return luaL_error(L, "short write: %s", resolved);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_breezy_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char resolved[BREEZYBOX_MAX_PATH * 2];
    struct stat st;
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, stat(resolved, &st) == 0);
    return 1;
}

static int l_breezy_sleep(lua_State *L)
{
    lua_Number seconds = luaL_checknumber(L, 1);
    if (seconds < 0) {
        return luaL_error(L, "sleep duration must be >= 0");
    }

    TickType_t ticks = (TickType_t)(seconds * 1000.0 / portTICK_PERIOD_MS);
    if (seconds > 0 && ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
    return 0;
}

static int l_breezy_sleep_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms < 0) {
        return luaL_error(L, "sleep duration must be >= 0");
    }

    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ms > 0 && ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
    return 0;
}

static int l_breezy_now_ms(lua_State *L)
{
    int64_t now_us = esp_timer_get_time();
    lua_pushinteger(L, (lua_Integer)(now_us / 1000));
    return 1;
}

static int l_breezy_clear(lua_State *L)
{
    (void)L;
    printf("\033[2J\033[H");
    fflush(stdout);
    return 0;
}

static void tui_move_to(int col, int row)
{
    if (col < 1) {
        col = 1;
    }
    if (row < 1) {
        row = 1;
    }
    printf("\033[%d;%dH", row, col);
}

static int l_breezy_term_size(lua_State *L)
{
    int rows = 0;
    int cols = 0;
    vterm_get_size(&rows, &cols);
    lua_pushinteger(L, cols);
    lua_pushinteger(L, rows);
    return 2;
}

static int l_breezy_readkey(lua_State *L)
{
    int timeout_ms = (int)luaL_optinteger(L, 1, -1);
    int vt = vterm_get_active();
    int c = vterm_getchar(vt, timeout_ms);
    if (c < 0) {
        lua_pushnil(L);
        return 1;
    }

    char buf[8];
    size_t len = 0;
    buf[len++] = (char)c;

    if (c == 27) {
        for (int i = 0; i < 6; ++i) {
            int next = vterm_getchar(vt, 1);
            if (next < 0) {
                break;
            }
            buf[len++] = (char)next;

            if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || next == '~') {
                break;
            }
        }
    }

    lua_pushlstring(L, buf, len);
    return 1;
}

static int lua_keycode_from_arg(lua_State *L, int idx)
{
#if !defined(BREEZY_BOARD_CARDPUTER)
    if (lua_isinteger(L, idx)) {
        return (int)lua_tointeger(L, idx);
    }
    size_t len = 0;
    const char *s = luaL_checklstring(L, idx, &len);
    return len == 0 ? 0 : (uint8_t)s[0];
#else
    if (lua_isinteger(L, idx)) {
        return (int)lua_tointeger(L, idx);
    }

    size_t len = 0;
    const char *s = luaL_checklstring(L, idx, &len);
    if (len == 0) {
        return 0;
    }
    if (strcmp(s, "enter") == 0) return CARDPUTER_KEY_ENTER;
    if (strcmp(s, "tab") == 0) return CARDPUTER_KEY_TAB;
    if (strcmp(s, "backspace") == 0) return CARDPUTER_KEY_BACKSPACE;
    if (strcmp(s, "ctrl") == 0) return CARDPUTER_KEY_LEFT_CTRL;
    if (strcmp(s, "shift") == 0) return CARDPUTER_KEY_LEFT_SHIFT;
    if (strcmp(s, "alt") == 0) return CARDPUTER_KEY_LEFT_ALT;
    if (strcmp(s, "fn") == 0) return CARDPUTER_KEY_FN;
    if (strcmp(s, "opt") == 0) return CARDPUTER_KEY_OPT;
    return (uint8_t)s[0];
#endif
}

#if defined(BREEZY_BOARD_CARDPUTER)
static void lua_push_keyboard_event(lua_State *L, const cardputer_keyboard_key_event_t *event)
{
    lua_newtable(L);
    lua_pushboolean(L, event->pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, (unsigned char)event->base);
    lua_setfield(L, -2, "code");

    char base_buf[2] = {event->base, '\0'};
    lua_pushstring(L, base_buf);
    lua_setfield(L, -2, "base");

    char shifted_buf[2] = {event->shifted, '\0'};
    lua_pushstring(L, shifted_buf);
    lua_setfield(L, -2, "shifted");

    lua_pushboolean(L, event->fn);
    lua_setfield(L, -2, "fn");
    lua_pushboolean(L, event->shift);
    lua_setfield(L, -2, "shift");
    lua_pushboolean(L, event->ctrl);
    lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, event->alt);
    lua_setfield(L, -2, "alt");
    lua_pushboolean(L, event->opt);
    lua_setfield(L, -2, "opt");
    lua_pushboolean(L, event->enter);
    lua_setfield(L, -2, "enter");
    lua_pushboolean(L, event->tab);
    lua_setfield(L, -2, "tab");
    lua_pushboolean(L, event->backspace);
    lua_setfield(L, -2, "backspace");
    lua_pushinteger(L, event->row);
    lua_setfield(L, -2, "row");
    lua_pushinteger(L, event->col);
    lua_setfield(L, -2, "col");
}

static int l_keyboard_read_event(lua_State *L)
{
    int timeout_ms = (int)luaL_optinteger(L, 1, -1);
    int elapsed_ms = 0;
    cardputer_keyboard_key_event_t event;

    while (true) {
        cardputer_keyboard_poll_direct();
        if (cardputer_keyboard_pop_event(&event)) {
            lua_push_keyboard_event(L, &event);
            return 1;
        }
        if (timeout_ms == 0) {
            lua_pushnil(L);
            return 1;
        }
        if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
            lua_pushnil(L);
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        if (timeout_ms > 0) {
            elapsed_ms += 1;
        }
    }
}

static int l_keyboard_peek_event(lua_State *L)
{
    (void)L;
    cardputer_keyboard_key_event_t event;
    cardputer_keyboard_poll_direct();
    if (!cardputer_keyboard_peek_event(&event)) {
        lua_pushnil(L);
        return 1;
    }
    lua_push_keyboard_event(L, &event);
    return 1;
}

static int l_keyboard_flush(lua_State *L)
{
    (void)L;
    cardputer_keyboard_flush_events_queue();
    return 0;
}

static int l_keyboard_is_down(lua_State *L)
{
    int keycode = lua_keycode_from_arg(L, 1);
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)keycode));
    return 1;
}

static int l_keyboard_mods(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)CARDPUTER_KEY_LEFT_SHIFT));
    lua_setfield(L, -2, "shift");
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)CARDPUTER_KEY_LEFT_CTRL));
    lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)CARDPUTER_KEY_LEFT_ALT));
    lua_setfield(L, -2, "alt");
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)CARDPUTER_KEY_FN));
    lua_setfield(L, -2, "fn");
    lua_pushboolean(L, cardputer_keyboard_key_is_down((char)CARDPUTER_KEY_OPT));
    lua_setfield(L, -2, "opt");
    return 1;
}
#else
static int l_keyboard_read_event(lua_State *L)
{
    (void)L;
    lua_pushnil(L);
    return 1;
}

static int l_keyboard_peek_event(lua_State *L)
{
    (void)L;
    lua_pushnil(L);
    return 1;
}

static int l_keyboard_flush(lua_State *L)
{
    (void)L;
    return 0;
}

static int l_keyboard_is_down(lua_State *L)
{
    (void)lua_keycode_from_arg(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

static int l_keyboard_mods(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "shift");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "alt");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "fn");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "opt");
    return 1;
}
#endif

static int lua_battery_read_uv_internal(int *raw_out)
{
    if (lua_adc_ensure_unit() != ESP_OK) {
        return ESP_FAIL;
    }

    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    if (adc_oneshot_io_to_channel(LUA_CARDPUTER_BATTERY_GPIO, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
        return ESP_ERR_NOT_FOUND;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_lua_adc1, channel, &chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

    int raw = 0;
    err = adc_oneshot_read(s_lua_adc1, channel, &raw);
    if (err != ESP_OK) {
        return err;
    }
    if (raw_out) {
        *raw_out = raw;
    }
    return ESP_OK;
}

static int lua_battery_scaled_uv(void)
{
    int raw = 0;
    if (lua_battery_read_uv_internal(&raw) != ESP_OK) {
        return -1;
    }
    // Match the rough 1/2-divider scaling used by MicroHydra's helper.
    return raw * 2 * 1000;
}

static int l_battery_read_uv(lua_State *L)
{
    (void)L;
    int uv = lua_battery_scaled_uv();
    if (uv < 0) {
        return luaL_error(L, "battery ADC read failed");
    }
    lua_pushinteger(L, uv);
    return 1;
}

static int l_battery_read_pct(lua_State *L)
{
    (void)L;
    int uv = lua_battery_scaled_uv();
    int pct = 0;
    if (uv < 0) {
        return luaL_error(L, "battery ADC read failed");
    }

    if (uv <= LUA_CARDPUTER_BATTERY_MIN_UV) {
        pct = 0;
    } else if (uv >= LUA_CARDPUTER_BATTERY_MAX_UV) {
        pct = 100;
    } else {
        pct = (uv - LUA_CARDPUTER_BATTERY_MIN_UV) * 100 /
              (LUA_CARDPUTER_BATTERY_MAX_UV - LUA_CARDPUTER_BATTERY_MIN_UV);
    }
    lua_pushinteger(L, pct);
    return 1;
}

static int l_battery_read_level(lua_State *L)
{
    (void)L;
    int uv = lua_battery_scaled_uv();
    int low = LUA_CARDPUTER_BATTERY_MIN_UV +
              ((LUA_CARDPUTER_BATTERY_MAX_UV - LUA_CARDPUTER_BATTERY_MIN_UV) / 3);
    int high = low + ((LUA_CARDPUTER_BATTERY_MAX_UV - LUA_CARDPUTER_BATTERY_MIN_UV) / 3);

    if (uv < 0) {
        return luaL_error(L, "battery ADC read failed");
    }
    if (uv < LUA_CARDPUTER_BATTERY_MIN_UV) {
        lua_pushinteger(L, 0);
    } else if (uv < low) {
        lua_pushinteger(L, 1);
    } else if (uv < high) {
        lua_pushinteger(L, 2);
    } else {
        lua_pushinteger(L, 3);
    }
    return 1;
}

static int l_pin_mode(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    const char *mode = luaL_checkstring(L, 2);
    gpio_config_t cfg = {0};

    cfg.pin_bit_mask = 1ULL << pin;

    if (strcmp(mode, "in") == 0 || strcmp(mode, "input") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
    } else if (strcmp(mode, "out") == 0 || strcmp(mode, "output") == 0) {
        cfg.mode = GPIO_MODE_OUTPUT;
    } else if (strcmp(mode, "input_pullup") == 0 || strcmp(mode, "pullup") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    } else if (strcmp(mode, "input_pulldown") == 0 || strcmp(mode, "pulldown") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else if (strcmp(mode, "od") == 0 || strcmp(mode, "open_drain") == 0) {
        cfg.mode = GPIO_MODE_OUTPUT_OD;
    } else {
        return luaL_error(L, "unknown pin mode: %s", mode);
    }

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_config failed: %d", (int)err);
    }
    return 0;
}

static int l_pin_read(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, gpio_get_level((gpio_num_t)pin));
    return 1;
}

static int l_pin_write(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int value = lua_toboolean(L, 2) ? 1 : 0;
    esp_err_t err = gpio_set_level((gpio_num_t)pin, value);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_set_level failed: %d", (int)err);
    }
    return 0;
}

static adc_atten_t lua_adc_parse_atten(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return ADC_ATTEN_DB_12;
    }
    if (lua_isnumber(L, idx)) {
        int v = (int)lua_tointeger(L, idx);
        switch (v) {
        case 0: return ADC_ATTEN_DB_0;
        case 1: return ADC_ATTEN_DB_2_5;
        case 2: return ADC_ATTEN_DB_6;
        case 3: return ADC_ATTEN_DB_12;
        default: return ADC_ATTEN_DB_12;
        }
    }

    const char *s = luaL_checkstring(L, idx);
    if (strcmp(s, "0db") == 0) return ADC_ATTEN_DB_0;
    if (strcmp(s, "2.5db") == 0) return ADC_ATTEN_DB_2_5;
    if (strcmp(s, "6db") == 0) return ADC_ATTEN_DB_6;
    if (strcmp(s, "11db") == 0 || strcmp(s, "12db") == 0) return ADC_ATTEN_DB_12;
    return ADC_ATTEN_DB_12;
}

static esp_err_t lua_adc_ensure_unit(void)
{
    if (s_lua_adc1) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    return adc_oneshot_new_unit(&cfg, &s_lua_adc1);
}

static int l_adc_read(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    adc_atten_t atten = lua_adc_parse_atten(L, 2);

    if (lua_adc_ensure_unit() != ESP_OK) {
        return luaL_error(L, "ADC init failed");
    }
    if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
        return luaL_error(L, "GPIO %d is not a usable ADC1 pin", pin);
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_lua_adc1, channel, &chan_cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "adc config failed: %d", (int)err);
    }

    int raw = 0;
    err = adc_oneshot_read(s_lua_adc1, channel, &raw);
    if (err != ESP_OK) {
        return luaL_error(L, "adc read failed: %d", (int)err);
    }

    lua_pushinteger(L, raw);
    return 1;
}

static int l_i2c_open(lua_State *L)
{
    int sda_pin = (int)luaL_checkinteger(L, 1);
    int scl_pin = (int)luaL_checkinteger(L, 2);
    int port = I2C_NUM_0;
    int freq_hz = 400000;
    int pullup = 1;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "port");
        if (!lua_isnil(L, -1)) {
            port = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "freq");
        if (!lua_isnil(L, -1)) {
            freq_hz = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "pullup");
        if (!lua_isnil(L, -1)) {
            pullup = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    if (port != I2C_NUM_0) {
        return luaL_error(L, "only I2C port 0 is available to Lua");
    }
    if (freq_hz <= 0) {
        return luaL_error(L, "freq must be > 0");
    }

    if (s_lua_i2c.open) {
        i2c_driver_delete(s_lua_i2c.port);
        s_lua_i2c.open = false;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .scl_pullup_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .master.clk_speed = (uint32_t)freq_hz,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config((i2c_port_t)port, &conf);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c_param_config failed: %d", (int)err);
    }

    err = i2c_driver_install((i2c_port_t)port, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c_driver_install failed: %d", (int)err);
    }

    s_lua_i2c.open = true;
    s_lua_i2c.port = (i2c_port_t)port;
    s_lua_i2c.sda_pin = sda_pin;
    s_lua_i2c.scl_pin = scl_pin;
    s_lua_i2c.freq_hz = (uint32_t)freq_hz;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_i2c_close(lua_State *L)
{
    (void)L;
    if (s_lua_i2c.open) {
        i2c_driver_delete(s_lua_i2c.port);
        s_lua_i2c.open = false;
    }
    return 0;
}

static int l_i2c_write(lua_State *L)
{
    int addr = (int)luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (!s_lua_i2c.open) {
        return luaL_error(L, "I2C bus not open");
    }
    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "I2C address must be 0-127");
    }

    esp_err_t err = i2c_master_write_to_device(
        s_lua_i2c.port,
        (uint8_t)addr,
        (const uint8_t *)data,
        len,
        pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write failed: %d", (int)err);
    }
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int l_i2c_read(lua_State *L)
{
    int addr = (int)luaL_checkinteger(L, 1);
    int len = (int)luaL_checkinteger(L, 2);
    if (!s_lua_i2c.open) {
        return luaL_error(L, "I2C bus not open");
    }
    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "I2C address must be 0-127");
    }
    if (len < 1 || len > 256) {
        return luaL_error(L, "read length must be 1-256");
    }

    uint8_t buf[256];
    esp_err_t err = i2c_master_read_from_device(
        s_lua_i2c.port,
        (uint8_t)addr,
        buf,
        len,
        pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read failed: %d", (int)err);
    }
    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}

static int l_i2c_write_reg(lua_State *L)
{
    int addr = (int)luaL_checkinteger(L, 1);
    int reg = (int)luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    uint8_t buf[257];

    if (!s_lua_i2c.open) {
        return luaL_error(L, "I2C bus not open");
    }
    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "I2C address must be 0-127");
    }
    if (reg < 0 || reg > 0xFF) {
        return luaL_error(L, "register must be 0-255");
    }
    if (len > 256) {
        return luaL_error(L, "write too large");
    }

    buf[0] = (uint8_t)reg;
    memcpy(&buf[1], data, len);
    esp_err_t err = i2c_master_write_to_device(
        s_lua_i2c.port,
        (uint8_t)addr,
        buf,
        len + 1,
        pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write_reg failed: %d", (int)err);
    }
    return 0;
}

static int l_i2c_read_reg(lua_State *L)
{
    int addr = (int)luaL_checkinteger(L, 1);
    int reg = (int)luaL_checkinteger(L, 2);
    int len = (int)luaL_checkinteger(L, 3);
    uint8_t reg8;
    uint8_t buf[256];

    if (!s_lua_i2c.open) {
        return luaL_error(L, "I2C bus not open");
    }
    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "I2C address must be 0-127");
    }
    if (reg < 0 || reg > 0xFF) {
        return luaL_error(L, "register must be 0-255");
    }
    if (len < 1 || len > 256) {
        return luaL_error(L, "read length must be 1-256");
    }

    reg8 = (uint8_t)reg;
    esp_err_t err = i2c_master_write_read_device(
        s_lua_i2c.port,
        (uint8_t)addr,
        &reg8,
        1,
        buf,
        len,
        pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read_reg failed: %d", (int)err);
    }
    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}

static int l_i2c_scan(lua_State *L)
{
    if (!s_lua_i2c.open) {
        return luaL_error(L, "I2C bus not open");
    }

    lua_newtable(L);
    int idx = 1;
    for (int addr = 1; addr < 0x7F; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(s_lua_i2c.port, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            lua_pushinteger(L, addr);
            lua_rawseti(L, -2, idx++);
        }
    }
    return 1;
}

static void lua_spi_set_clock_level(int level)
{
    gpio_set_level((gpio_num_t)s_lua_spi.sclk_pin, level ? 1 : 0);
}

static void lua_spi_set_cs_level(int active)
{
    if (s_lua_spi.cs_pin >= 0) {
        gpio_set_level((gpio_num_t)s_lua_spi.cs_pin, active ? 0 : 1);
    }
}

static uint8_t lua_spi_transfer_byte(uint8_t tx)
{
    uint8_t rx = 0;
    int cpol = (s_lua_spi.mode >> 1) & 1;
    int cpha = s_lua_spi.mode & 1;

    lua_spi_set_clock_level(cpol);

    for (int bit = 0; bit < 8; ++bit) {
        int shift = s_lua_spi.msb_first ? (7 - bit) : bit;
        int out = (tx >> shift) & 1;

        if (!cpha) {
            if (s_lua_spi.mosi_pin >= 0) {
                gpio_set_level((gpio_num_t)s_lua_spi.mosi_pin, out);
            }
            esp_rom_delay_us((uint32_t)s_lua_spi.delay_us);
            lua_spi_set_clock_level(!cpol);
            esp_rom_delay_us((uint32_t)s_lua_spi.delay_us);
            if (s_lua_spi.miso_pin >= 0 && gpio_get_level((gpio_num_t)s_lua_spi.miso_pin)) {
                rx |= (uint8_t)(1U << shift);
            }
            lua_spi_set_clock_level(cpol);
        } else {
            lua_spi_set_clock_level(!cpol);
            if (s_lua_spi.mosi_pin >= 0) {
                gpio_set_level((gpio_num_t)s_lua_spi.mosi_pin, out);
            }
            esp_rom_delay_us((uint32_t)s_lua_spi.delay_us);
            lua_spi_set_clock_level(cpol);
            esp_rom_delay_us((uint32_t)s_lua_spi.delay_us);
            if (s_lua_spi.miso_pin >= 0 && gpio_get_level((gpio_num_t)s_lua_spi.miso_pin)) {
                rx |= (uint8_t)(1U << shift);
            }
        }
    }

    return rx;
}

static int l_spi_open(lua_State *L)
{
    int sclk_pin = (int)luaL_checkinteger(L, 1);
    int mosi_pin = (int)luaL_optinteger(L, 2, -1);
    int miso_pin = (int)luaL_optinteger(L, 3, -1);
    int cs_pin = (int)luaL_optinteger(L, 4, -1);
    int mode = 0;
    int delay_us = 1;
    int msb_first = 1;

    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "mode");
        if (!lua_isnil(L, -1)) {
            mode = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "delay_us");
        if (!lua_isnil(L, -1)) {
            delay_us = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "msb_first");
        if (!lua_isnil(L, -1)) {
            msb_first = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    if (mode < 0 || mode > 3) {
        return luaL_error(L, "SPI mode must be 0-3");
    }
    if (delay_us < 0) {
        delay_us = 0;
    }

    gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << sclk_pin),
    };
    if (mosi_pin >= 0) {
        out_cfg.pin_bit_mask |= (1ULL << mosi_pin);
    }
    if (cs_pin >= 0) {
        out_cfg.pin_bit_mask |= (1ULL << cs_pin);
    }
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "SPI gpio_config failed: %d", (int)err);
    }

    if (miso_pin >= 0) {
        gpio_config_t in_cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = (1ULL << miso_pin),
        };
        err = gpio_config(&in_cfg);
        if (err != ESP_OK) {
            return luaL_error(L, "SPI MISO config failed: %d", (int)err);
        }
    }

    s_lua_spi.open = true;
    s_lua_spi.sclk_pin = sclk_pin;
    s_lua_spi.mosi_pin = mosi_pin;
    s_lua_spi.miso_pin = miso_pin;
    s_lua_spi.cs_pin = cs_pin;
    s_lua_spi.mode = mode;
    s_lua_spi.delay_us = delay_us;
    s_lua_spi.msb_first = msb_first != 0;

    lua_spi_set_clock_level((mode >> 1) & 1);
    lua_spi_set_cs_level(0);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_spi_close(lua_State *L)
{
    (void)L;
    s_lua_spi.open = false;
    return 0;
}

static int l_spi_select(lua_State *L)
{
    int active = lua_toboolean(L, 1);
    if (!s_lua_spi.open) {
        return luaL_error(L, "SPI bus not open");
    }
    lua_spi_set_cs_level(active);
    return 0;
}

static int l_spi_transfer(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    int keep_selected = lua_toboolean(L, 2);
    if (!s_lua_spi.open) {
        return luaL_error(L, "SPI bus not open");
    }

    char *rx = malloc(len ? len : 1);
    if (!rx) {
        return luaL_error(L, "out of memory");
    }

    if (!keep_selected) {
        lua_spi_set_cs_level(1);
    }
    for (size_t i = 0; i < len; ++i) {
        rx[i] = (char)lua_spi_transfer_byte((uint8_t)data[i]);
    }
    if (!keep_selected) {
        lua_spi_set_cs_level(0);
    }

    lua_pushlstring(L, rx, len);
    free(rx);
    return 1;
}

static int l_spi_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    int keep_selected = lua_toboolean(L, 2);
    if (!s_lua_spi.open) {
        return luaL_error(L, "SPI bus not open");
    }

    if (!keep_selected) {
        lua_spi_set_cs_level(1);
    }
    for (size_t i = 0; i < len; ++i) {
        (void)lua_spi_transfer_byte((uint8_t)data[i]);
    }
    if (!keep_selected) {
        lua_spi_set_cs_level(0);
    }

    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int l_spi_read(lua_State *L)
{
    int len = (int)luaL_checkinteger(L, 1);
    int fill = (int)luaL_optinteger(L, 2, 0xFF);
    int keep_selected = lua_toboolean(L, 3);
    if (!s_lua_spi.open) {
        return luaL_error(L, "SPI bus not open");
    }
    if (len < 1 || len > 512) {
        return luaL_error(L, "SPI read length must be 1-512");
    }

    char *rx = malloc((size_t)len);
    if (!rx) {
        return luaL_error(L, "out of memory");
    }

    if (!keep_selected) {
        lua_spi_set_cs_level(1);
    }
    for (int i = 0; i < len; ++i) {
        rx[i] = (char)lua_spi_transfer_byte((uint8_t)fill);
    }
    if (!keep_selected) {
        lua_spi_set_cs_level(0);
    }

    lua_pushlstring(L, rx, (size_t)len);
    free(rx);
    return 1;
}

static i2s_data_bit_width_t lua_i2s_data_width(int bits)
{
    switch (bits) {
    case 8: return I2S_DATA_BIT_WIDTH_8BIT;
    case 16: return I2S_DATA_BIT_WIDTH_16BIT;
    case 24: return I2S_DATA_BIT_WIDTH_24BIT;
    case 32: return I2S_DATA_BIT_WIDTH_32BIT;
    default: return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

static i2s_std_slot_config_t lua_i2s_slot_config(const char *format, int bits, int channels)
{
    i2s_slot_mode_t slot_mode = channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_data_bit_width_t width = lua_i2s_data_width(bits);

    if (format && strcmp(format, "philips") == 0) {
        return (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, slot_mode);
    }
    if (format && strcmp(format, "pcm") == 0) {
        return (i2s_std_slot_config_t)I2S_STD_PCM_SLOT_DEFAULT_CONFIG(width, slot_mode);
    }
    return (i2s_std_slot_config_t)I2S_STD_MSB_SLOT_DEFAULT_CONFIG(width, slot_mode);
}

static void lua_i2s_close_channel(lua_i2s_chan_t *chan)
{
    if (!chan->open) {
        return;
    }
    i2s_channel_disable(chan->handle);
    i2s_del_channel(chan->handle);
    memset(chan, 0, sizeof(*chan));
}

static int lua_i2s_open_common(lua_State *L, bool tx)
{
    int bclk_pin = (int)luaL_checkinteger(L, 1);
    int ws_pin = (int)luaL_checkinteger(L, 2);
    int data_pin = (int)luaL_checkinteger(L, 3);
    int sample_rate = 16000;
    int bits = 16;
    int channels = 1;
    int mclk_pin = I2S_GPIO_UNUSED;
    int invert_bclk = 0;
    int invert_ws = 0;
    const char *format = "msb";
    lua_i2s_chan_t *chan = tx ? &s_lua_i2s_tx : &s_lua_i2s_rx;

    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "sample_rate");
        if (!lua_isnil(L, -1)) {
            sample_rate = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "bits");
        if (!lua_isnil(L, -1)) {
            bits = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "channels");
        if (!lua_isnil(L, -1)) {
            channels = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "mclk_pin");
        if (!lua_isnil(L, -1)) {
            mclk_pin = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "format");
        if (!lua_isnil(L, -1)) {
            format = luaL_checkstring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "invert_bclk");
        if (!lua_isnil(L, -1)) {
            invert_bclk = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 4, "invert_ws");
        if (!lua_isnil(L, -1)) {
            invert_ws = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    if (sample_rate <= 0) {
        return luaL_error(L, "sample_rate must be > 0");
    }
    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        return luaL_error(L, "bits must be 8, 16, 24, or 32");
    }
    if (!(channels == 1 || channels == 2)) {
        return luaL_error(L, "channels must be 1 or 2");
    }
    if (strcmp(format, "msb") != 0 && strcmp(format, "philips") != 0 && strcmp(format, "pcm") != 0) {
        return luaL_error(L, "format must be msb, philips, or pcm");
    }

    lua_i2s_close_channel(chan);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_chan_handle_t handle = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, tx ? &handle : NULL, tx ? NULL : &handle);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s_new_channel failed: %d", (int)err);
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = lua_i2s_slot_config(format, bits, channels),
        .gpio_cfg = {
            .mclk = mclk_pin,
            .bclk = bclk_pin,
            .ws = ws_pin,
            .dout = tx ? data_pin : I2S_GPIO_UNUSED,
            .din = tx ? I2S_GPIO_UNUSED : data_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = invert_bclk != 0,
                .ws_inv = invert_ws != 0,
            },
        },
    };

    err = i2s_channel_init_std_mode(handle, &cfg);
    if (err != ESP_OK) {
        i2s_del_channel(handle);
        return luaL_error(L, "i2s_channel_init_std_mode failed: %d", (int)err);
    }

    err = i2s_channel_enable(handle);
    if (err != ESP_OK) {
        i2s_channel_disable(handle);
        i2s_del_channel(handle);
        return luaL_error(L, "i2s_channel_enable failed: %d", (int)err);
    }

    chan->open = true;
    chan->handle = handle;
    chan->sample_rate = sample_rate;
    chan->bits = bits;
    chan->channels = channels;
    chan->data_pin = data_pin;
    chan->bclk_pin = bclk_pin;
    chan->ws_pin = ws_pin;
    chan->mclk_pin = mclk_pin;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_i2s_open_tx(lua_State *L)
{
    return lua_i2s_open_common(L, true);
}

static int l_i2s_open_rx(lua_State *L)
{
    return lua_i2s_open_common(L, false);
}

static int l_i2s_close(lua_State *L)
{
    const char *which = luaL_optstring(L, 1, "both");
    if (strcmp(which, "tx") == 0) {
        lua_i2s_close_channel(&s_lua_i2s_tx);
    } else if (strcmp(which, "rx") == 0) {
        lua_i2s_close_channel(&s_lua_i2s_rx);
    } else if (strcmp(which, "both") == 0) {
        lua_i2s_close_channel(&s_lua_i2s_tx);
        lua_i2s_close_channel(&s_lua_i2s_rx);
    } else {
        return luaL_error(L, "close target must be tx, rx, or both");
    }
    return 0;
}

static int l_i2s_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    int timeout_ms = (int)luaL_optinteger(L, 2, 1000);
    size_t written = 0;

    if (!s_lua_i2s_tx.open) {
        return luaL_error(L, "I2S TX not open");
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    esp_err_t err = i2s_channel_write(
        s_lua_i2s_tx.handle,
        data,
        len,
        &written,
        (uint32_t)timeout_ms);
    if (err != ESP_OK) {
        return luaL_error(L, "i2s write failed: %d", (int)err);
    }
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int l_i2s_read(lua_State *L)
{
    int len = (int)luaL_checkinteger(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 1000);
    size_t read_len = 0;
    char *buf = NULL;

    if (!s_lua_i2s_rx.open) {
        return luaL_error(L, "I2S RX not open");
    }
    if (len < 1 || len > 4096) {
        return luaL_error(L, "read length must be 1-4096");
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    buf = malloc((size_t)len);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }

    esp_err_t err = i2s_channel_read(
        s_lua_i2s_rx.handle,
        buf,
        (size_t)len,
        &read_len,
        (uint32_t)timeout_ms);
    if (err != ESP_OK) {
        free(buf);
        return luaL_error(L, "i2s read failed: %d", (int)err);
    }

    lua_pushlstring(L, buf, read_len);
    free(buf);
    return 1;
}

static int l_i2s_state(lua_State *L)
{
    lua_newtable(L);

    lua_pushboolean(L, s_lua_i2s_tx.open);
    lua_setfield(L, -2, "tx_open");
    if (s_lua_i2s_tx.open) {
        lua_pushinteger(L, s_lua_i2s_tx.sample_rate);
        lua_setfield(L, -2, "tx_sample_rate");
        lua_pushinteger(L, s_lua_i2s_tx.bits);
        lua_setfield(L, -2, "tx_bits");
        lua_pushinteger(L, s_lua_i2s_tx.channels);
        lua_setfield(L, -2, "tx_channels");
    }

    lua_pushboolean(L, s_lua_i2s_rx.open);
    lua_setfield(L, -2, "rx_open");
    if (s_lua_i2s_rx.open) {
        lua_pushinteger(L, s_lua_i2s_rx.sample_rate);
        lua_setfield(L, -2, "rx_sample_rate");
        lua_pushinteger(L, s_lua_i2s_rx.bits);
        lua_setfield(L, -2, "rx_bits");
        lua_pushinteger(L, s_lua_i2s_rx.channels);
        lua_setfield(L, -2, "rx_channels");
    }

    return 1;
}

static int l_storage_sd_mounted(lua_State *L)
{
    lua_pushboolean(L, breezybox_sd_mounted());
    return 1;
}

static int l_storage_mounts(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, BREEZYBOX_MOUNT_POINT);
    lua_rawseti(L, -2, 1);
    if (breezybox_sd_mounted()) {
        lua_pushstring(L, BREEZYBOX_SD_MOUNT_POINT);
        lua_rawseti(L, -2, 2);
    }
    return 1;
}

static int l_storage_info(lua_State *L)
{
    const char *mount = luaL_optstring(L, 1, BREEZYBOX_MOUNT_POINT);
    lua_newtable(L);
    lua_pushstring(L, mount);
    lua_setfield(L, -2, "mount");

    if (strcmp(mount, BREEZYBOX_MOUNT_POINT) == 0) {
        size_t total_bytes = 0;
        size_t used_bytes = 0;
        esp_err_t err = breezybox_root_fs_info(&total_bytes, &used_bytes);
        if (err != ESP_OK) {
            return luaL_error(L, "storage info failed: %d", (int)err);
        }
        lua_pushstring(L, breezybox_root_fs_name());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)total_bytes);
        lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)used_bytes);
        lua_setfield(L, -2, "used");
        lua_pushinteger(L, (lua_Integer)(total_bytes - used_bytes));
        lua_setfield(L, -2, "free");
        return 1;
    }

    if (strcmp(mount, BREEZYBOX_SD_MOUNT_POINT) == 0) {
        if (!breezybox_sd_mounted()) {
            return luaL_error(L, "SD card not mounted");
        }
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        esp_err_t err = esp_vfs_fat_info(BREEZYBOX_SD_MOUNT_POINT, &total_bytes, &free_bytes);
        if (err != ESP_OK) {
            return luaL_error(L, "SD info failed: %d", (int)err);
        }
        lua_pushstring(L, "fat");
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)total_bytes);
        lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)(total_bytes - free_bytes));
        lua_setfield(L, -2, "used");
        lua_pushinteger(L, (lua_Integer)free_bytes);
        lua_setfield(L, -2, "free");
        return 1;
    }

    return luaL_error(L, "unknown mount: %s", mount);
}

static int l_network_is_connected(lua_State *L)
{
    wifi_ap_record_t ap;
    lua_pushboolean(L, esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    return 1;
}

static int l_network_info(lua_State *L)
{
    lua_newtable(L);

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "connected");
        return 1;
    }

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "connected");
    lua_pushstring(L, (const char *)ap.ssid);
    lua_setfield(L, -2, "ssid");
    lua_pushinteger(L, ap.rssi);
    lua_setfield(L, -2, "rssi");
    lua_pushinteger(L, ap.primary);
    lua_setfield(L, -2, "channel");

    esp_netif_t *netif = lua_wifi_sta_netif();
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip[16], gw[16], mask[16];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
            snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ip_info.gw));
            snprintf(mask, sizeof(mask), IPSTR, IP2STR(&ip_info.netmask));
            lua_pushstring(L, ip);
            lua_setfield(L, -2, "ip");
            lua_pushstring(L, gw);
            lua_setfield(L, -2, "gateway");
            lua_pushstring(L, mask);
            lua_setfield(L, -2, "netmask");
        }

        esp_netif_dns_info_t dns_main = {0};
        esp_netif_dns_info_t dns_backup = {0};
        char dns_main_str[16];
        char dns_backup_str[16];
        (void)esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);
        (void)esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        lua_dns_info_to_string(&dns_main, dns_main_str, sizeof(dns_main_str));
        lua_dns_info_to_string(&dns_backup, dns_backup_str, sizeof(dns_backup_str));
        lua_pushstring(L, dns_main_str);
        lua_setfield(L, -2, "dns");
        lua_pushstring(L, dns_backup_str);
        lua_setfield(L, -2, "dns_backup");
    }
    return 1;
}

static int l_network_dns_info(lua_State *L)
{
    lua_newtable(L);

    char main_buf[16];
    char backup_buf[16];
    esp_err_t err = lua_network_ensure_dns(main_buf, sizeof(main_buf), backup_buf, sizeof(backup_buf));

    lua_pushboolean(L, err == ESP_OK);
    lua_setfield(L, -2, "ready");
    lua_pushstring(L, main_buf);
    lua_setfield(L, -2, "main");
    lua_pushstring(L, backup_buf);
    lua_setfield(L, -2, "backup");
    return 1;
}

static cJSON *lua_value_to_cjson(lua_State *L, int idx);

static cJSON *lua_table_to_cjson(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    bool is_array = true;
    lua_Integer max_index = 0;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (!lua_isinteger(L, -2)) {
            is_array = false;
        } else {
            lua_Integer key = lua_tointeger(L, -2);
            if (key < 1) {
                is_array = false;
            } else if (key > max_index) {
                max_index = key;
            }
        }
        lua_pop(L, 1);
    }

    cJSON *node = is_array ? cJSON_CreateArray() : cJSON_CreateObject();
    if (!node) {
        return NULL;
    }

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        cJSON *child = lua_value_to_cjson(L, -1);
        if (!child) {
            lua_pop(L, 1);
            cJSON_Delete(node);
            return NULL;
        }

        if (is_array) {
            cJSON_AddItemToArray(node, child);
        } else {
            const char *key = lua_tostring(L, -2);
            if (!key && lua_isinteger(L, -2)) {
                char key_buf[24];
                snprintf(key_buf, sizeof(key_buf), "%lld", (long long)lua_tointeger(L, -2));
                cJSON_AddItemToObject(node, key_buf, child);
            } else {
                cJSON_AddItemToObject(node, key ? key : "", child);
            }
        }
        lua_pop(L, 1);
    }

    return node;
}

static cJSON *lua_value_to_cjson(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        return cJSON_CreateString(s);
    }
    case LUA_TTABLE:
        return lua_table_to_cjson(L, idx);
    default:
        return NULL;
    }
}

static void lua_push_from_cjson(lua_State *L, const cJSON *node)
{
    if (!node || cJSON_IsNull(node)) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsBool(node)) {
        lua_pushboolean(L, cJSON_IsTrue(node));
        return;
    }
    if (cJSON_IsNumber(node)) {
        lua_pushnumber(L, node->valuedouble);
        return;
    }
    if (cJSON_IsString(node)) {
        lua_pushstring(L, node->valuestring ? node->valuestring : "");
        return;
    }
    if (cJSON_IsArray(node)) {
        int i = 1;
        lua_newtable(L);
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            lua_push_from_cjson(L, child);
            lua_rawseti(L, -2, i++);
        }
        return;
    }
    if (cJSON_IsObject(node)) {
        const cJSON *child = NULL;
        lua_newtable(L);
        cJSON_ArrayForEach(child, node) {
            lua_push_from_cjson(L, child);
            lua_setfield(L, -2, child->string ? child->string : "");
        }
        return;
    }
    lua_pushnil(L);
}

static int lua_load_json_root(lua_State *L, const char *path, cJSON **root_out)
{
    char resolved[BREEZYBOX_MAX_PATH * 2];
    FILE *f = NULL;
    long size = 0;
    char *buf = NULL;
    cJSON *root = NULL;

    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "config path too long: %s", path);
    }

    f = fopen(resolved, "rb");
    if (!f) {
        *root_out = cJSON_CreateObject();
        return *root_out ? 0 : luaL_error(L, "out of memory");
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return luaL_error(L, "cannot seek config: %s", resolved);
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return luaL_error(L, "cannot stat config: %s", resolved);
    }
    rewind(f);

    buf = malloc((size_t)size + 1U);
    if (!buf) {
        fclose(f);
        return luaL_error(L, "out of memory");
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return luaL_error(L, "cannot read config: %s", resolved);
    }
    fclose(f);
    buf[size] = '\0';

    root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return luaL_error(L, "invalid JSON in %s", resolved);
    }
    *root_out = root;
    return 0;
}

static int lua_save_json_root(lua_State *L, const char *path, cJSON *root)
{
    char resolved[BREEZYBOX_MAX_PATH * 2];
    char *json = NULL;
    FILE *f = NULL;

    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        return luaL_error(L, "config path too long: %s", path);
    }

    json = cJSON_PrintUnformatted(root);
    if (!json) {
        return luaL_error(L, "failed to encode JSON");
    }

    f = fopen(resolved, "wb");
    if (!f) {
        cJSON_free(json);
        return luaL_error(L, "cannot open config for write: %s", resolved);
    }

    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, f);
    fclose(f);
    cJSON_free(json);
    if (written != len) {
        return luaL_error(L, "short config write: %s", resolved);
    }
    return 0;
}

static const char *lua_config_path(lua_State *L, int idx)
{
    return luaL_optstring(L, idx, LUA_CONFIG_DEFAULT_PATH);
}

static int l_config_load(lua_State *L)
{
    const char *path = lua_config_path(L, 1);
    cJSON *root = NULL;
    int rc = lua_load_json_root(L, path, &root);
    if (rc != 0) {
        return rc;
    }
    lua_push_from_cjson(L, root);
    cJSON_Delete(root);
    return 1;
}

static int l_config_save(lua_State *L)
{
    const char *path = lua_config_path(L, 2);
    cJSON *root = lua_value_to_cjson(L, 1);
    if (!root) {
        return luaL_error(L, "config.save only supports nil/bool/number/string/table");
    }
    int rc = lua_save_json_root(L, path, root);
    cJSON_Delete(root);
    if (rc != 0) {
        return rc;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_config_get(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *path = lua_config_path(L, 3);
    cJSON *root = NULL;
    int rc = lua_load_json_root(L, path, &root);
    if (rc != 0) {
        return rc;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        if (!lua_isnone(L, 2)) {
            lua_pushvalue(L, 2);
        } else {
            lua_pushnil(L);
        }
        cJSON_Delete(root);
        return 1;
    }
    lua_push_from_cjson(L, item);
    cJSON_Delete(root);
    return 1;
}

static int l_config_set(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *path = lua_config_path(L, 3);
    cJSON *root = NULL;
    int rc = lua_load_json_root(L, path, &root);
    if (rc != 0) {
        return rc;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON *value = lua_value_to_cjson(L, 2);
    if (!value) {
        cJSON_Delete(root);
        return luaL_error(L, "config.set only supports nil/bool/number/string/table");
    }
    cJSON_AddItemToObject(root, key, value);

    rc = lua_save_json_root(L, path, root);
    cJSON_Delete(root);
    if (rc != 0) {
        return rc;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static lua_tcp_slot_t *lua_tcp_slot_from_port(lua_Integer handle)
{
    if (handle < 1 || handle > LUA_TCP_SLOT_COUNT) {
        return NULL;
    }
    return &s_lua_tcp_slots[handle - 1];
}

static lua_tcp_slot_t *lua_tcp_alloc_slot(int *handle_out)
{
    for (int i = 0; i < LUA_TCP_SLOT_COUNT; ++i) {
        if (!s_lua_tcp_slots[i].open) {
            if (handle_out) {
                *handle_out = i + 1;
            }
            return &s_lua_tcp_slots[i];
        }
    }
    return NULL;
}

static esp_netif_t *lua_wifi_sta_netif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static bool lua_dns_info_valid(const esp_netif_dns_info_t *dns)
{
    return dns &&
           dns->ip.type == ESP_IPADDR_TYPE_V4 &&
           dns->ip.u_addr.ip4.addr != 0;
}

static void lua_dns_info_to_string(const esp_netif_dns_info_t *dns, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!lua_dns_info_valid(dns)) {
        strlcpy(buf, "0.0.0.0", buf_len);
        return;
    }
    esp_ip4addr_ntoa(&dns->ip.u_addr.ip4, buf, (int)buf_len);
}

static esp_err_t lua_network_ensure_dns(char *main_buf, size_t main_buf_len,
                                        char *backup_buf, size_t backup_buf_len)
{
    esp_netif_t *netif = lua_wifi_sta_netif();
    if (!netif) {
        if (main_buf && main_buf_len) {
            strlcpy(main_buf, "0.0.0.0", main_buf_len);
        }
        if (backup_buf && backup_buf_len) {
            strlcpy(backup_buf, "0.0.0.0", backup_buf_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dns_info_t main_dns = {0};
    esp_netif_dns_info_t backup_dns = {0};
    esp_err_t main_err = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &main_dns);
    esp_err_t backup_err = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup_dns);

    if (!lua_dns_info_valid(&main_dns)) {
        memset(&main_dns, 0, sizeof(main_dns));
        main_dns.ip.type = ESP_IPADDR_TYPE_V4;
        (void)esp_netif_str_to_ip4("1.1.1.1", &main_dns.ip.u_addr.ip4);
        (void)esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &main_dns);
    }
    if (!lua_dns_info_valid(&backup_dns)) {
        memset(&backup_dns, 0, sizeof(backup_dns));
        backup_dns.ip.type = ESP_IPADDR_TYPE_V4;
        (void)esp_netif_str_to_ip4("8.8.8.8", &backup_dns.ip.u_addr.ip4);
        (void)esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
    }

    (void)esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &main_dns);
    (void)esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
    lua_dns_info_to_string(&main_dns, main_buf, main_buf_len);
    lua_dns_info_to_string(&backup_dns, backup_buf, backup_buf_len);

    if (lua_dns_info_valid(&main_dns) || lua_dns_info_valid(&backup_dns)) {
        return ESP_OK;
    }
    return main_err != ESP_OK ? main_err : backup_err;
}

static int l_network_tcp_connect(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    const char *service = luaL_checkstring(L, 2);
    char dns_main[16];
    char dns_backup[16];
    int handle = 0;
    int fd = -1;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    lua_tcp_slot_t *slot = lua_tcp_alloc_slot(&handle);

    if (!slot) {
        return luaL_error(L, "no free TCP handles");
    }

    (void)lua_network_ensure_dns(dns_main, sizeof(dns_main), dns_backup, sizeof(dns_backup));
    int err = getaddrinfo(host, service, &hints, &result);
    if (err != 0 || !result) {
        return luaL_error(L, "getaddrinfo failed: %d (dns=%s backup=%s)",
                          err, dns_main, dns_backup);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int conn_ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (conn_ret == 0) {
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags);
            }
            break;
        }
        if (conn_ret < 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = {
                .tv_sec = 5,
                .tv_usec = 0,
            };
            int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0 && FD_ISSET(fd, &wfds)) {
                int so_error = 0;
                socklen_t so_len = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0) {
                    if (flags >= 0) {
                        (void)fcntl(fd, F_SETFL, flags);
                    }
                    break;
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        return luaL_error(L, "connect failed");
    }

    slot->open = true;
    slot->fd = fd;
    lua_pushinteger(L, handle);
    return 1;
}

static int l_network_tcp_close(lua_State *L)
{
    lua_Integer handle = luaL_checkinteger(L, 1);
    lua_tcp_slot_t *slot = lua_tcp_slot_from_port(handle);
    if (slot && slot->open) {
        close(slot->fd);
        slot->open = false;
        slot->fd = -1;
    }
    return 0;
}

static int l_network_tcp_write(lua_State *L)
{
    lua_Integer handle = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_tcp_slot_t *slot = lua_tcp_slot_from_port(handle);
    if (!slot || !slot->open) {
        return luaL_error(L, "TCP handle not open");
    }
    int sent = (int)send(slot->fd, data, len, 0);
    if (sent < 0) {
        return luaL_error(L, "send failed: %d", errno);
    }
    lua_pushinteger(L, sent);
    return 1;
}

static int l_network_tcp_read(lua_State *L)
{
    lua_Integer handle = luaL_checkinteger(L, 1);
    int maxlen = (int)luaL_optinteger(L, 2, 512);
    int timeout_ms = (int)luaL_optinteger(L, 3, 0);
    lua_tcp_slot_t *slot = lua_tcp_slot_from_port(handle);
    char *buf = NULL;
    if (!slot || !slot->open) {
        return luaL_error(L, "TCP handle not open");
    }
    if (maxlen < 1) {
        maxlen = 1;
    }
    if (maxlen > 4096) {
        maxlen = 4096;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(slot->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    buf = malloc((size_t)maxlen);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    int got = (int)recv(slot->fd, buf, (size_t)maxlen, 0);
    if (got <= 0) {
        free(buf);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, buf, (size_t)got);
    free(buf);
    return 1;
}

typedef struct {
    char *host;
    char *port;
    char *path;
} lua_http_url_t;

static void lua_http_free_url(lua_http_url_t *url)
{
    if (!url) {
        return;
    }
    free(url->host);
    free(url->port);
    free(url->path);
    memset(url, 0, sizeof(*url));
}

static int lua_http_parse_url(lua_State *L, const char *url_in, lua_http_url_t *out)
{
    const char *p = NULL;
    const char *host_start = NULL;
    const char *path_start = NULL;
    const char *port_sep = NULL;
    size_t host_len = 0;
    size_t path_len = 0;
    char *host = NULL;
    char *port = NULL;
    char *path = NULL;

    if (strncmp(url_in, "http://", 7) == 0) {
        p = url_in + 7;
    } else if (strncmp(url_in, "https://", 8) == 0) {
        return luaL_error(L, "https is not supported in lua http helpers");
    } else {
        return luaL_error(L, "url must start with http://");
    }

    host_start = p;
    path_start = strchr(p, '/');
    if (!path_start) {
        path_start = p + strlen(p);
    }
    port_sep = memchr(host_start, ':', (size_t)(path_start - host_start));
    if (port_sep) {
        host_len = (size_t)(port_sep - host_start);
    } else {
        host_len = (size_t)(path_start - host_start);
    }
    if (host_len == 0) {
        return luaL_error(L, "invalid url host");
    }

    host = malloc(host_len + 1);
    if (!host) {
        return luaL_error(L, "out of memory");
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (port_sep) {
        size_t port_len = (size_t)(path_start - (port_sep + 1));
        if (port_len == 0) {
            free(host);
            return luaL_error(L, "invalid url port");
        }
        port = malloc(port_len + 1);
        if (!port) {
            free(host);
            return luaL_error(L, "out of memory");
        }
        memcpy(port, port_sep + 1, port_len);
        port[port_len] = '\0';
    } else {
        port = strdup("80");
        if (!port) {
            free(host);
            return luaL_error(L, "out of memory");
        }
    }

    path_len = strlen(path_start);
    if (path_len == 0) {
        path = strdup("/");
    } else {
        path = malloc(path_len + 1);
        if (path) {
            memcpy(path, path_start, path_len + 1);
        }
    }
    if (!path) {
        free(host);
        free(port);
        return luaL_error(L, "out of memory");
    }

    out->host = host;
    out->port = port;
    out->path = path;
    return 0;
}

static int lua_http_buf_append(char **buf, size_t *len, size_t *cap, const void *data, size_t data_len)
{
    if (*len + data_len + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 512;
        while (new_cap < *len + data_len + 1) {
            new_cap *= 2U;
        }
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf) {
            return -1;
        }
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return 0;
}

static int lua_http_append_headers(lua_State *L, int idx, char **req, size_t *len, size_t *cap)
{
    if (!lua_istable(L, idx)) {
        return 0;
    }
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        size_t key_len = 0;
        size_t val_len = 0;
        const char *key = luaL_checklstring(L, -2, &key_len);
        const char *value = luaL_checklstring(L, -1, &val_len);
        if (lua_http_buf_append(req, len, cap, key, key_len) != 0 ||
            lua_http_buf_append(req, len, cap, ": ", 2) != 0 ||
            lua_http_buf_append(req, len, cap, value, val_len) != 0 ||
            lua_http_buf_append(req, len, cap, "\r\n", 2) != 0) {
            lua_pop(L, 1);
            return -1;
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int lua_http_request(lua_State *L, const char *method)
{
    const char *url = luaL_checkstring(L, 1);
    const char *body = NULL;
    size_t body_len = 0;
    const char *content_type = "application/x-www-form-urlencoded";
    lua_http_url_t parsed = {0};
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;
    int status = 0;
    int rc = 0;
    char dns_main[16];
    char dns_backup[16];
    char request[1024];
    char recv_buf[256];
    char header_buf[1024];
    char body_buf[LUA_HTTP_BODY_MAX + 1];
    size_t header_len = 0;
    size_t body_used = 0;
    bool headers_done = false;
    bool truncated = false;

    if (strcmp(method, "POST") == 0) {
        body = luaL_checklstring(L, 2, &body_len);
        content_type = luaL_optstring(L, 3, content_type);
        if (body_len > 256) {
            return luaL_error(L, "post body too large");
        }
    }

    rc = lua_http_parse_url(L, url, &parsed);
    if (rc != 0) {
        return rc;
    }

    int req_len = 0;
    if (strcmp(method, "POST") == 0) {
        req_len = snprintf(request, sizeof(request),
                           "%s %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: BreezyBox-Lua\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           method, parsed.path, parsed.host,
                           content_type, (unsigned)body_len, body);
    } else {
        req_len = snprintf(request, sizeof(request),
                           "%s %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: BreezyBox-Lua\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           method, parsed.path, parsed.host);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        lua_http_free_url(&parsed);
        return luaL_error(L, "request too large");
    }

    (void)lua_network_ensure_dns(dns_main, sizeof(dns_main), dns_backup, sizeof(dns_backup));
    int err = getaddrinfo(parsed.host, parsed.port, &hints, &result);
    if (err != 0 || !result) {
        lua_http_free_url(&parsed);
        return luaL_error(L, "getaddrinfo failed: %d (dns=%s backup=%s)",
                          err, dns_main, dns_backup);
    }
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        struct timeval tv = {
            .tv_sec = 5,
            .tv_usec = 0,
        };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int conn_ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (conn_ret == 0) {
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags);
            }
            break;
        }
        if (conn_ret < 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0 && FD_ISSET(fd, &wfds)) {
                int so_error = 0;
                socklen_t so_len = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0) {
                    if (flags >= 0) {
                        (void)fcntl(fd, F_SETFL, flags);
                    }
                    break;
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    lua_http_free_url(&parsed);
    if (fd < 0) {
        return luaL_error(L, "connect failed");
    }

    size_t sent = 0;
    while (sent < (size_t)req_len) {
        int n = (int)send(fd, request + sent, (size_t)req_len - sent, 0);
        if (n <= 0) {
            close(fd);
            return luaL_error(L, "send failed: %d", errno);
        }
        sent += (size_t)n;
    }

    while (1) {
        int n = (int)recv(fd, recv_buf, sizeof(recv_buf), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close(fd);
            return luaL_error(L, "recv failed: %d", errno);
        }

        if (!headers_done) {
            size_t copy_len = (size_t)n;
            if (header_len + copy_len >= sizeof(header_buf)) {
                copy_len = sizeof(header_buf) - header_len - 1;
            }
            if (copy_len > 0) {
                memcpy(header_buf + header_len, recv_buf, copy_len);
                header_len += copy_len;
                header_buf[header_len] = '\0';
            }

            char *header_end = strstr(header_buf, "\r\n\r\n");
            if (!header_end) {
                if (header_len >= sizeof(header_buf) - 1) {
                    close(fd);
                    return luaL_error(L, "http headers too large");
                }
                continue;
            }

            headers_done = true;
            if (sscanf(header_buf, "HTTP/%*d.%*d %d", &status) != 1) {
                close(fd);
                return luaL_error(L, "invalid http response");
            }

            if (strcmp(method, "HEAD") == 0) {
                break;
            }

            size_t header_bytes = (size_t)((header_end + 4) - header_buf);
            if (header_len > header_bytes) {
                size_t initial_body = header_len - header_bytes;
                if (initial_body > LUA_HTTP_BODY_MAX) {
                    initial_body = LUA_HTTP_BODY_MAX;
                    truncated = true;
                }
                memcpy(body_buf, header_buf + header_bytes, initial_body);
                body_used = initial_body;
            }
        } else {
            size_t copy_len = (size_t)n;
            if (body_used >= LUA_HTTP_BODY_MAX) {
                truncated = true;
                continue;
            }
            if (body_used + copy_len > LUA_HTTP_BODY_MAX) {
                copy_len = LUA_HTTP_BODY_MAX - body_used;
                truncated = true;
            }
            memcpy(body_buf + body_used, recv_buf, copy_len);
            body_used += copy_len;
        }
    }
    close(fd);

    if (!headers_done) {
        return luaL_error(L, "no http response received");
    }

    body_buf[body_used] = '\0';
    lua_newtable(L);
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, body_buf, body_used);
    lua_setfield(L, -2, "body");
    lua_pushboolean(L, truncated ? 1 : 0);
    lua_setfield(L, -2, "truncated");
    return 1;
}

static int l_network_http_get(lua_State *L)
{
    return lua_http_request(L, "GET");
}

static int l_network_http_post(lua_State *L)
{
    return lua_http_request(L, "POST");
}

static int l_network_http_head(lua_State *L)
{
    return lua_http_request(L, "HEAD");
}

static void lua_speaker_close_internal(void)
{
    if (!s_lua_speaker.open) {
        return;
    }
    i2s_channel_disable(s_lua_speaker.handle);
    i2s_del_channel(s_lua_speaker.handle);
    memset(&s_lua_speaker, 0, sizeof(s_lua_speaker));
}

static int lua_speaker_open_internal(int sample_rate, int volume)
{
    lua_speaker_close_internal();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_chan_handle_t handle = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, &handle, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LUA_CARDPUTER_SPK_BCLK,
            .ws = LUA_CARDPUTER_SPK_WS,
            .dout = LUA_CARDPUTER_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(handle, &cfg);
    if (err != ESP_OK) {
        i2s_del_channel(handle);
        return err;
    }
    err = i2s_channel_enable(handle);
    if (err != ESP_OK) {
        i2s_channel_disable(handle);
        i2s_del_channel(handle);
        return err;
    }

    s_lua_speaker.open = true;
    s_lua_speaker.handle = handle;
    s_lua_speaker.sample_rate = sample_rate;
    s_lua_speaker.volume = volume;
    return ESP_OK;
}

static int l_sound_speaker_open(lua_State *L)
{
    int sample_rate = (int)luaL_optinteger(L, 1, 16000);
    int volume = (int)luaL_optinteger(L, 2, 255);
    if (sample_rate <= 0) {
        return luaL_error(L, "sample_rate must be > 0");
    }
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    esp_err_t err = lua_speaker_open_internal(sample_rate, volume);
    if (err != ESP_OK) {
        return luaL_error(L, "speaker_open failed: %d", (int)err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_sound_speaker_close(lua_State *L)
{
    (void)L;
    lua_speaker_close_internal();
    return 0;
}

static int l_sound_speaker_state(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, s_lua_speaker.open);
    lua_setfield(L, -2, "open");
    if (s_lua_speaker.open) {
        lua_pushinteger(L, s_lua_speaker.sample_rate);
        lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, s_lua_speaker.volume);
        lua_setfield(L, -2, "volume");
    }
    lua_pushboolean(L, s_lua_mic.open);
    lua_setfield(L, -2, "mic_open");
    return 1;
}

static int l_sound_speaker_stop(lua_State *L)
{
    (void)L;
    if (!s_lua_speaker.open) {
        return 0;
    }
    i2s_channel_disable(s_lua_speaker.handle);
    i2s_channel_enable(s_lua_speaker.handle);
    return 0;
}

static int l_sound_speaker_play_raw(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    int sample_rate = (int)luaL_optinteger(L, 2, s_lua_speaker.open ? s_lua_speaker.sample_rate : 16000);
    size_t written = 0;

    if (!s_lua_speaker.open || s_lua_speaker.sample_rate != sample_rate) {
        esp_err_t err = lua_speaker_open_internal(sample_rate, s_lua_speaker.volume ? s_lua_speaker.volume : 255);
        if (err != ESP_OK) {
            return luaL_error(L, "speaker_open failed: %d", (int)err);
        }
    }

    esp_err_t err = i2s_channel_write(s_lua_speaker.handle, data, len, &written, 2000);
    if (err != ESP_OK) {
        return luaL_error(L, "speaker write failed: %d", (int)err);
    }
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int lua_speaker_write_pcm(const char *data, size_t len, int sample_rate)
{
    size_t written = 0;
    if (!s_lua_speaker.open || s_lua_speaker.sample_rate != sample_rate) {
        esp_err_t err = lua_speaker_open_internal(sample_rate, s_lua_speaker.volume ? s_lua_speaker.volume : 255);
        if (err != ESP_OK) {
            return err;
        }
    }
    return i2s_channel_write(s_lua_speaker.handle, data, len, &written, 2000);
}

static int16_t lua_scale_sample(int16_t sample, int volume)
{
    int scaled = (sample * volume) / 255;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static int l_sound_mix(lua_State *L)
{
    int count = 0;
    size_t max_len = 0;
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "mix expects an array of PCM byte strings");
    }

    count = (int)lua_rawlen(L, 1);
    if (count < 1) {
        lua_pushliteral(L, "");
        return 1;
    }

    for (int i = 1; i <= count; ++i) {
        size_t len = 0;
        lua_rawgeti(L, 1, i);
        (void)luaL_checklstring(L, -1, &len);
        if (len > max_len) {
            max_len = len;
        }
        lua_pop(L, 1);
    }
    max_len &= ~((size_t)1);

    char *out = calloc(1, max_len ? max_len : 2);
    if (!out) {
        return luaL_error(L, "out of memory");
    }

    for (int i = 1; i <= count; ++i) {
        size_t len = 0;
        lua_rawgeti(L, 1, i);
        const int16_t *src = (const int16_t *)luaL_checklstring(L, -1, &len);
        len &= ~((size_t)1);
        for (size_t off = 0; off < len; off += 2) {
            int sample = ((int16_t *)out)[off / 2] + src[off / 2];
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            ((int16_t *)out)[off / 2] = (int16_t)sample;
        }
        lua_pop(L, 1);
    }

    lua_pushlstring(L, out, max_len);
    free(out);
    return 1;
}

static double lua_note_frequency(const char *note)
{
    if (!note || !*note) {
        return 0.0;
    }
    if (note[0] == 'R' || note[0] == 'r') {
        return 0.0;
    }

    int semitone = 0;
    switch (note[0]) {
    case 'C': semitone = 0; break;
    case 'D': semitone = 2; break;
    case 'E': semitone = 4; break;
    case 'F': semitone = 5; break;
    case 'G': semitone = 7; break;
    case 'A': semitone = 9; break;
    case 'B': semitone = 11; break;
    default: return 0.0;
    }

    int idx = 1;
    if (note[idx] == '#') {
        semitone++;
        idx++;
    } else if (note[idx] == 'b') {
        semitone--;
        idx++;
    }
    int octave = atoi(note + idx);
    int midi = (octave + 1) * 12 + semitone;
    return 440.0 * pow(2.0, ((double)midi - 69.0) / 12.0);
}

static char *lua_build_tone_pcm(double freq, int duration_ms, int sample_rate, int volume, size_t *out_len)
{
    int samples = (sample_rate * duration_ms) / 1000;
    int16_t *buf = NULL;
    if (samples < 1) {
        samples = 1;
    }
    buf = malloc((size_t)samples * sizeof(int16_t));
    if (!buf) {
        return NULL;
    }

    double phase = 0.0;
    double step = freq > 0.0 ? (2.0 * M_PI * freq) / (double)sample_rate : 0.0;
    for (int i = 0; i < samples; ++i) {
        int16_t sample = 0;
        if (freq > 0.0) {
            sample = (sin(phase) >= 0.0) ? 24000 : -24000;
            sample = lua_scale_sample(sample, volume);
            phase += step;
        }
        buf[i] = sample;
    }
    *out_len = (size_t)samples * sizeof(int16_t);
    return (char *)buf;
}

static int l_sound_tone(lua_State *L)
{
    int freq = (int)luaL_checkinteger(L, 1);
    int duration_ms = (int)luaL_checkinteger(L, 2);
    int volume = (int)luaL_optinteger(L, 3, 180);
    size_t len = 0;
    char *pcm = NULL;

    if (freq <= 0) {
        return luaL_error(L, "frequency must be > 0");
    }
    if (duration_ms < 0) {
        return luaL_error(L, "duration must be >= 0");
    }
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    pcm = lua_build_tone_pcm((double)freq, duration_ms, 16000, volume, &len);
    if (!pcm) {
        return luaL_error(L, "out of memory");
    }
    esp_err_t err = lua_speaker_write_pcm(pcm, len, 16000);
    free(pcm);
    if (err != ESP_OK) {
        return luaL_error(L, "speaker write failed: %d", (int)err);
    }
    return 0;
}

static int l_sound_play_notes(lua_State *L)
{
    const char *sequence = luaL_checkstring(L, 1);
    int bpm = (int)luaL_optinteger(L, 2, 120);
    int volume = (int)luaL_optinteger(L, 3, 180);
    char token[16];
    const char *p = sequence;

    if (bpm <= 0) {
        return luaL_error(L, "bpm must be > 0");
    }
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) break;

        int ti = 0;
        while (*p && *p != ' ' && ti < (int)sizeof(token) - 1) {
            token[ti++] = *p++;
        }
        token[ti] = '\0';

        char *colon = strchr(token, ':');
        int beats = 4;
        if (colon) {
            *colon = '\0';
            beats = atoi(colon + 1);
            if (beats <= 0) beats = 4;
        }
        int duration_ms = (60000 / bpm) * 4 / beats;
        size_t tone_len = 0;
        char *tone = lua_build_tone_pcm(lua_note_frequency(token), duration_ms, 16000, volume, &tone_len);
        if (!tone) {
            return luaL_error(L, "out of memory");
        }
        esp_err_t err = lua_speaker_write_pcm(tone, tone_len, 16000);
        free(tone);
        if (err != ESP_OK) {
            return luaL_error(L, "speaker write failed: %d", (int)err);
        }
    }
    return 0;
}

static int l_sound_beep(lua_State *L)
{
    return l_sound_tone(L);
}

static int l_sound_mic_open(lua_State *L)
{
    int bclk = (int)luaL_checkinteger(L, 1);
    int ws = (int)luaL_checkinteger(L, 2);
    int din = (int)luaL_checkinteger(L, 3);
    int sample_rate = 16000;
    int bits = 16;
    int channels = 1;

    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "sample_rate");
        if (!lua_isnil(L, -1)) sample_rate = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "bits");
        if (!lua_isnil(L, -1)) bits = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "channels");
        if (!lua_isnil(L, -1)) channels = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }

    if (s_lua_mic.open) {
        i2s_channel_disable(s_lua_mic.handle);
        i2s_del_channel(s_lua_mic.handle);
        memset(&s_lua_mic, 0, sizeof(s_lua_mic));
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_chan_handle_t handle = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &handle);
    if (err != ESP_OK) {
        return luaL_error(L, "mic i2s_new_channel failed: %d", (int)err);
    }
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = lua_i2s_slot_config("msb", bits, channels),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(handle, &cfg);
    if (err != ESP_OK) {
        i2s_del_channel(handle);
        return luaL_error(L, "mic init failed: %d", (int)err);
    }
    err = i2s_channel_enable(handle);
    if (err != ESP_OK) {
        i2s_channel_disable(handle);
        i2s_del_channel(handle);
        return luaL_error(L, "mic enable failed: %d", (int)err);
    }

    s_lua_mic.open = true;
    s_lua_mic.handle = handle;
    s_lua_mic.sample_rate = sample_rate;
    s_lua_mic.bits = bits;
    s_lua_mic.channels = channels;
    s_lua_mic.bclk_pin = bclk;
    s_lua_mic.ws_pin = ws;
    s_lua_mic.din_pin = din;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_sound_mic_read(lua_State *L)
{
    int len = (int)luaL_checkinteger(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 1000);
    size_t read_len = 0;
    char *buf = NULL;
    if (!s_lua_mic.open) {
        return luaL_error(L, "mic not open");
    }
    if (len < 1 || len > 4096) {
        return luaL_error(L, "read length must be 1-4096");
    }
    buf = malloc((size_t)len);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    esp_err_t err = i2s_channel_read(s_lua_mic.handle, buf, (size_t)len, &read_len, (uint32_t)timeout_ms);
    if (err != ESP_OK) {
        free(buf);
        return luaL_error(L, "mic read failed: %d", (int)err);
    }
    lua_pushlstring(L, buf, read_len);
    free(buf);
    return 1;
}

static int l_sound_mic_close(lua_State *L)
{
    (void)L;
    if (s_lua_mic.open) {
        i2s_channel_disable(s_lua_mic.handle);
        i2s_del_channel(s_lua_mic.handle);
        memset(&s_lua_mic, 0, sizeof(s_lua_mic));
    }
    return 0;
}

static int l_tui_move(lua_State *L)
{
    int col = luaL_checkinteger(L, 1);
    int row = luaL_checkinteger(L, 2);
    tui_move_to(col, row);
    fflush(stdout);
    return 0;
}

static int l_tui_clear_line(lua_State *L)
{
    int row = (int)luaL_optinteger(L, 1, 0);
    if (row > 0) {
        tui_move_to(1, row);
    }
    printf("\033[2K");
    fflush(stdout);
    return 0;
}

static int l_tui_write_at(lua_State *L)
{
    int col = luaL_checkinteger(L, 1);
    int row = luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    int clear_to_eol = lua_toboolean(L, 4);

    tui_move_to(col, row);
    fwrite(text, 1, len, stdout);
    if (clear_to_eol) {
        printf("\033[K");
    }
    fflush(stdout);
    return 0;
}

static int l_tui_center(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    int row = (int)luaL_optinteger(L, 2, 1);
    int rows = 0;
    int cols = 0;
    vterm_get_size(&rows, &cols);
    int col = 1;
    if (cols > 0 && (int)len < cols) {
        col = ((cols - (int)len) / 2) + 1;
    }
    tui_move_to(col, row);
    fwrite(text, 1, len, stdout);
    fflush(stdout);
    return 0;
}

static int l_tui_status(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    int rows = 0;
    int cols = 0;
    vterm_get_size(&rows, &cols);

    tui_move_to(1, rows);
    printf("\033[7m");
    if ((int)len > cols) {
        fwrite(text, 1, (size_t)cols, stdout);
    } else {
        fwrite(text, 1, len, stdout);
        for (int i = (int)len; i < cols; ++i) {
            fputc(' ', stdout);
        }
    }
    printf("\033[0m");
    fflush(stdout);
    return 0;
}

static int l_tui_cursor(lua_State *L)
{
    int show = lua_toboolean(L, 1);
    printf("%s", show ? "\033[?25h" : "\033[?25l");
    fflush(stdout);
    return 0;
}

static int l_tui_box(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    const char *title = luaL_optstring(L, 5, NULL);

    if (w < 2 || h < 2) {
        return luaL_error(L, "box width and height must be >= 2");
    }

    tui_move_to(x, y);
    fputc('+', stdout);
    for (int i = 0; i < w - 2; ++i) {
        fputc('-', stdout);
    }
    fputc('+', stdout);

    for (int row = 1; row < h - 1; ++row) {
        tui_move_to(x, y + row);
        fputc('|', stdout);
        tui_move_to(x + w - 1, y + row);
        fputc('|', stdout);
    }

    tui_move_to(x, y + h - 1);
    fputc('+', stdout);
    for (int i = 0; i < w - 2; ++i) {
        fputc('-', stdout);
    }
    fputc('+', stdout);

    if (title && *title && w > 4) {
        size_t title_len = strlen(title);
        int max_title = w - 4;
        if ((int)title_len > max_title) {
            title_len = (size_t)max_title;
        }
        tui_move_to(x + 2, y);
        fwrite(title, 1, title_len, stdout);
    }

    fflush(stdout);
    return 0;
}

static bool lua_uart_port_valid(lua_Integer port)
{
    return port == UART_NUM_1 || port == UART_NUM_2;
}

static lua_uart_slot_t *lua_uart_get_slot(lua_Integer port)
{
    if (port < 0 || port >= UART_NUM_MAX) {
        return NULL;
    }
    return &s_lua_uart_slots[port];
}

static uart_word_length_t lua_uart_data_bits(int bits)
{
    switch (bits) {
    case 5: return UART_DATA_5_BITS;
    case 6: return UART_DATA_6_BITS;
    case 7: return UART_DATA_7_BITS;
    case 8: return UART_DATA_8_BITS;
    default: return UART_DATA_8_BITS;
    }
}

static uart_parity_t lua_uart_parity(const char *parity)
{
    if (!parity || strcmp(parity, "none") == 0) {
        return UART_PARITY_DISABLE;
    }
    if (strcmp(parity, "even") == 0) {
        return UART_PARITY_EVEN;
    }
    if (strcmp(parity, "odd") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

static uart_stop_bits_t lua_uart_stop_bits(int stop_bits)
{
    switch (stop_bits) {
    case 2: return UART_STOP_BITS_2;
    case 1:
    default:
        return UART_STOP_BITS_1;
    }
}

static int l_uart_open(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    int baud = (int)luaL_checkinteger(L, 2);
    int tx_pin = (int)luaL_checkinteger(L, 3);
    int rx_pin = (int)luaL_checkinteger(L, 4);
    int data_bits = 8;
    int stop_bits = 1;
    int rx_buffer = 1024;
    int tx_buffer = 1024;
    const char *parity = "none";

    if (!lua_uart_port_valid(port)) {
        return luaL_error(L, "UART port must be 1 or 2");
    }
    if (baud <= 0) {
        return luaL_error(L, "baud must be > 0");
    }

    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "data_bits");
        if (!lua_isnil(L, -1)) {
            data_bits = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "stop_bits");
        if (!lua_isnil(L, -1)) {
            stop_bits = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "parity");
        if (!lua_isnil(L, -1)) {
            parity = luaL_checkstring(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "rx_buffer");
        if (!lua_isnil(L, -1)) {
            rx_buffer = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "tx_buffer");
        if (!lua_isnil(L, -1)) {
            tx_buffer = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
    }

    if (data_bits < 5 || data_bits > 8) {
        return luaL_error(L, "data_bits must be 5-8");
    }
    if (!(stop_bits == 1 || stop_bits == 2)) {
        return luaL_error(L, "stop_bits must be 1 or 2");
    }
    if (rx_buffer < 256) {
        rx_buffer = 256;
    }
    if (tx_buffer < 256) {
        tx_buffer = 256;
    }

    lua_uart_slot_t *slot = lua_uart_get_slot(port);
    if (!slot) {
        return luaL_error(L, "invalid UART slot");
    }

    if (slot->open) {
        uart_driver_delete((uart_port_t)port);
        slot->open = false;
    }

    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = lua_uart_data_bits(data_bits),
        .parity = lua_uart_parity(parity),
        .stop_bits = lua_uart_stop_bits(stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config((uart_port_t)port, &cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "uart_param_config failed: %d", (int)err);
    }

    err = uart_set_pin((uart_port_t)port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return luaL_error(L, "uart_set_pin failed: %d", (int)err);
    }

    err = uart_driver_install((uart_port_t)port, rx_buffer, tx_buffer, 0, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "uart_driver_install failed: %d", (int)err);
    }

    slot->open = true;
    slot->port = (uart_port_t)port;
    lua_pushinteger(L, port);
    return 1;
}

static int l_uart_close(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    lua_uart_slot_t *slot = lua_uart_get_slot(port);
    if (!slot || !slot->open) {
        return 0;
    }
    uart_driver_delete(slot->port);
    slot->open = false;
    return 0;
}

static int l_uart_write(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_uart_slot_t *slot = lua_uart_get_slot(port);

    if (!slot || !slot->open) {
        return luaL_error(L, "UART not open");
    }

    int written = uart_write_bytes(slot->port, data, len);
    if (written < 0) {
        return luaL_error(L, "uart_write_bytes failed");
    }
    lua_pushinteger(L, written);
    return 1;
}

static int l_uart_read(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    int maxlen = (int)luaL_optinteger(L, 2, 64);
    int timeout_ms = (int)luaL_optinteger(L, 3, 0);
    lua_uart_slot_t *slot = lua_uart_get_slot(port);

    if (!slot || !slot->open) {
        return luaL_error(L, "UART not open");
    }
    if (maxlen < 1) {
        maxlen = 1;
    }
    if (maxlen > LUA_UART_READ_MAX) {
        maxlen = LUA_UART_READ_MAX;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    uint8_t buf[LUA_UART_READ_MAX];
    int n = uart_read_bytes(slot->port, buf, (uint32_t)maxlen, pdMS_TO_TICKS(timeout_ms));
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

static int l_uart_readline(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 1000);
    int maxlen = (int)luaL_optinteger(L, 3, 128);
    lua_uart_slot_t *slot = lua_uart_get_slot(port);
    int elapsed = 0;
    int step_ms = 20;
    int n = 0;
    char buf[129];

    if (!slot || !slot->open) {
        return luaL_error(L, "UART not open");
    }
    if (maxlen < 1) {
        maxlen = 1;
    }
    if (maxlen > (int)sizeof(buf) - 1) {
        maxlen = (int)sizeof(buf) - 1;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    while (n < maxlen) {
        int wait_ms = timeout_ms == 0 ? 0 : step_ms;
        if (timeout_ms > 0 && timeout_ms - elapsed < wait_ms) {
            wait_ms = timeout_ms - elapsed;
        }

        uint8_t ch = 0;
        int got = uart_read_bytes(slot->port, &ch, 1, pdMS_TO_TICKS(wait_ms));
        if (got > 0) {
            buf[n++] = (char)ch;
            if (ch == '\n') {
                break;
            }
            continue;
        }
        if (timeout_ms == 0) {
            break;
        }
        elapsed += wait_ms;
        if (elapsed >= timeout_ms) {
            break;
        }
    }

    if (n == 0) {
        lua_pushnil(L);
        return 1;
    }

    buf[n] = '\0';
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int l_uart_flush(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    lua_uart_slot_t *slot = lua_uart_get_slot(port);

    if (!slot || !slot->open) {
        return luaL_error(L, "UART not open");
    }

    esp_err_t err = uart_flush(slot->port);
    if (err != ESP_OK) {
        return luaL_error(L, "uart_flush failed: %d", (int)err);
    }
    return 0;
}

static lua_gfx_image_t *lua_gfx_check_image(lua_State *L, int idx)
{
    return (lua_gfx_image_t *)luaL_checkudata(L, idx, LUA_GFX_IMAGE_MT);
}

static int l_gfx_image_size(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    lua_pushinteger(L, img->width);
    lua_pushinteger(L, img->height);
    return 2;
}

static int l_gfx_image_clear(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    int color = (int)luaL_checkinteger(L, 2);
    memset(img->pixels, color & 0xFF, (size_t)img->width * (size_t)img->height);
    return 0;
}

static int l_gfx_image_pixel(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int color = (int)luaL_optinteger(L, 4, -1);
    if (x < 0 || x >= img->width || y < 0 || y >= img->height) {
        lua_pushnil(L);
        return 1;
    }
    if (color >= 0) {
        img->pixels[y * img->width + x] = (uint8_t)color;
        return 0;
    }
    lua_pushinteger(L, img->pixels[y * img->width + x]);
    return 1;
}

static int l_gfx_image_rectfill(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    int color = (int)luaL_checkinteger(L, 6) & 0xFF;
    for (int yy = 0; yy < h; ++yy) {
        int dy = y + yy;
        if (dy < 0 || dy >= img->height) continue;
        for (int xx = 0; xx < w; ++xx) {
            int dx = x + xx;
            if (dx < 0 || dx >= img->width) continue;
            img->pixels[dy * img->width + dx] = (uint8_t)color;
        }
    }
    return 0;
}

static int l_gfx_image_rect(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    int color = (int)luaL_checkinteger(L, 6) & 0xFF;
    for (int xx = 0; xx < w; ++xx) {
        if (y >= 0 && y < img->height && x + xx >= 0 && x + xx < img->width) {
            img->pixels[y * img->width + x + xx] = (uint8_t)color;
        }
        if (y + h - 1 >= 0 && y + h - 1 < img->height && x + xx >= 0 && x + xx < img->width) {
            img->pixels[(y + h - 1) * img->width + x + xx] = (uint8_t)color;
        }
    }
    for (int yy = 0; yy < h; ++yy) {
        if (x >= 0 && x < img->width && y + yy >= 0 && y + yy < img->height) {
            img->pixels[(y + yy) * img->width + x] = (uint8_t)color;
        }
        if (x + w - 1 >= 0 && x + w - 1 < img->width && y + yy >= 0 && y + yy < img->height) {
            img->pixels[(y + yy) * img->width + x + w - 1] = (uint8_t)color;
        }
    }
    return 0;
}

static int l_gfx_image_data(lua_State *L)
{
    lua_gfx_image_t *img = lua_gfx_check_image(L, 1);
    lua_pushlstring(L, (const char *)img->pixels, (size_t)img->width * (size_t)img->height);
    return 1;
}

static const luaL_Reg s_gfx_image_methods[] = {
    { "size", l_gfx_image_size },
    { "clear", l_gfx_image_clear },
    { "pixel", l_gfx_image_pixel },
    { "rect", l_gfx_image_rect },
    { "rectfill", l_gfx_image_rectfill },
    { "data", l_gfx_image_data },
    { NULL, NULL }
};

static void lua_gfx_init_image_mt(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_GFX_IMAGE_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, s_gfx_image_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

static int l_gfx_new_image(lua_State *L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    int fill = (int)luaL_optinteger(L, 3, 0);
    if (w < 1 || h < 1 || w > 512 || h > 512) {
        return luaL_error(L, "image size out of range");
    }
    size_t bytes = sizeof(lua_gfx_image_t) + ((size_t)w * (size_t)h);
    lua_gfx_image_t *img = (lua_gfx_image_t *)lua_newuserdatauv(L, bytes, 0);
    img->width = w;
    img->height = h;
    memset(img->pixels, fill & 0xFF, (size_t)w * (size_t)h);
    luaL_getmetatable(L, LUA_GFX_IMAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static const uint8_t *lua_gfx_image_source(lua_State *L, int idx, int *w, int *h, int *stride)
{
    if (luaL_testudata(L, idx, LUA_GFX_IMAGE_MT)) {
        lua_gfx_image_t *img = lua_gfx_check_image(L, idx);
        *w = img->width;
        *h = img->height;
        *stride = img->width;
        return img->pixels;
    }

    size_t len = 0;
    const char *data = luaL_checklstring(L, idx, &len);
    *w = (int)luaL_checkinteger(L, idx + 1);
    *h = (int)luaL_checkinteger(L, idx + 2);
    *stride = (int)luaL_optinteger(L, idx + 3, *w);
    if ((size_t)(*stride * *h) > len) {
        luaL_error(L, "blit source buffer too small");
        return NULL;
    }
    return (const uint8_t *)data;
}

static int l_gfx_hline(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int color = luaL_checkinteger(L, 4);
    rgb_gfx_hline(x, y, w, (uint8_t)color);
    return 0;
}

static int l_gfx_vline(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int h = luaL_checkinteger(L, 3);
    int color = luaL_checkinteger(L, 4);
    rgb_gfx_vline(x, y, h, (uint8_t)color);
    return 0;
}

static int l_gfx_blit(lua_State *L)
{
    int w = 0, h = 0, stride = 0;
    const uint8_t *src = lua_gfx_image_source(L, 1, &w, &h, &stride);
    int x = luaL_checkinteger(L, luaL_testudata(L, 1, LUA_GFX_IMAGE_MT) ? 2 : 4);
    int y = luaL_checkinteger(L, luaL_testudata(L, 1, LUA_GFX_IMAGE_MT) ? 3 : 5);
    int transparent = (int)luaL_optinteger(L, luaL_testudata(L, 1, LUA_GFX_IMAGE_MT) ? 4 : 6, -1);
    rgb_gfx_blit(src, x, y, w, h, stride, transparent);
    return 0;
}

static int l_gfx_blit_flip(lua_State *L)
{
    int w = 0, h = 0, stride = 0;
    int img_mode = luaL_testudata(L, 1, LUA_GFX_IMAGE_MT) != NULL;
    const uint8_t *src = lua_gfx_image_source(L, 1, &w, &h, &stride);
    int x = luaL_checkinteger(L, img_mode ? 2 : 4);
    int y = luaL_checkinteger(L, img_mode ? 3 : 5);
    int transparent = (int)luaL_optinteger(L, img_mode ? 4 : 6, -1);
    int flip_x = lua_toboolean(L, img_mode ? 5 : 7);
    int flip_y = lua_toboolean(L, img_mode ? 6 : 8);
    rgb_gfx_blit_flip(src, x, y, w, h, stride, transparent, flip_x != 0, flip_y != 0);
    return 0;
}

static int l_gfx_backlight(lua_State *L)
{
    if (lua_gettop(L) == 0) {
        lua_pushinteger(L, rgb_display_get_backlight());
        return 1;
    }
    int level = (int)luaL_checkinteger(L, 1);
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    rgb_display_set_backlight((uint8_t)level);
    lua_pushinteger(L, rgb_display_get_backlight());
    return 1;
}

static int l_gfx_font(lua_State *L)
{
    if (lua_gettop(L) == 0) {
        lua_pushstring(L, s_lua_gfx_font == LUA_GFX_FONT_TERM16 ? "term16" : "small");
        return 1;
    }

    const char *name = luaL_checkstring(L, 1);
    if (strcmp(name, "small") == 0 || strcmp(name, "5x7") == 0) {
        s_lua_gfx_font = LUA_GFX_FONT_SMALL;
    } else if (strcmp(name, "term16") == 0 || strcmp(name, "8x16") == 0) {
        s_lua_gfx_font = LUA_GFX_FONT_TERM16;
    } else {
        return luaL_error(L, "unknown font: %s", name);
    }

    lua_pushstring(L, s_lua_gfx_font == LUA_GFX_FONT_TERM16 ? "term16" : "small");
    return 1;
}

static int l_gfx_text(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char *text = luaL_checkstring(L, 3);
    int fg = luaL_checkinteger(L, 4);
    int bg = lua_isnoneornil(L, 5) ? -1 : (int)luaL_checkinteger(L, 5);

    if (s_lua_gfx_font == LUA_GFX_FONT_TERM16) {
        rgb_gfx_text_term16(x, y, text, (uint8_t)fg, bg);
    } else {
        rgb_gfx_text_small(x, y, text, (uint8_t)fg, bg);
    }
    return 0;
}

static int l_gfx_text_size(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    int char_w = s_lua_gfx_font == LUA_GFX_FONT_TERM16 ? 8 : 6;
    int char_h = s_lua_gfx_font == LUA_GFX_FONT_TERM16 ? 16 : 8;
    int width = 0;
    int line_width = 0;
    int lines = 1;

    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            if (line_width > width) {
                width = line_width;
            }
            line_width = 0;
            ++lines;
        } else {
            line_width += char_w;
        }
    }
    if (line_width > width) {
        width = line_width;
    }

    lua_pushinteger(L, width);
    lua_pushinteger(L, lines * char_h);
    return 2;
}

static int l_gfx_mode(lua_State *L)
{
    screen_mode_t mode = SM_TEXT;

    if (lua_isnumber(L, 1)) {
        mode = (screen_mode_t)luaL_checkinteger(L, 1);
    } else {
        const char *name = luaL_checkstring(L, 1);
        if (strcmp(name, "text") == 0) {
            mode = SM_TEXT;
        } else if (strcmp(name, "vga") == 0 || strcmp(name, "vga13h") == 0) {
            mode = SM_VGA13H;
        } else if (strcmp(name, "150p") == 0) {
            mode = SM_150P;
        } else {
            return luaL_error(L, "unknown graphics mode: %s", name);
        }
    }

    if (mode != SM_TEXT) {
        // Lua's heap competes directly with the graphics framebuffer on this target.
        // Force a full collection before we try to enter graphics mode.
        lua_gc(L, LUA_GCCOLLECT, 0);
    }

    if (rgb_display_set_mode(mode) != 0) {
        unsigned free_bytes = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return luaL_error(L,
                          "failed to switch display mode (internal free=%d largest=%d)",
                          (int)free_bytes,
                          (int)largest);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_gfx_get_mode(lua_State *L)
{
    switch (rgb_display_get_mode()) {
    case SM_TEXT:
        lua_pushstring(L, "text");
        break;
    case SM_VGA13H:
        lua_pushstring(L, "vga13h");
        break;
    case SM_150P:
        lua_pushstring(L, "150p");
        break;
    default:
        lua_pushstring(L, "unknown");
        break;
    }
    return 1;
}

static int l_gfx_size(lua_State *L)
{
    lua_pushinteger(L, rgb_display_get_fb_width());
    lua_pushinteger(L, rgb_display_get_fb_height());
    return 2;
}

static int l_gfx_clear(lua_State *L)
{
    int color = luaL_checkinteger(L, 1);
    rgb_gfx_clear((uint8_t)color);
    return 0;
}

static int l_gfx_pixel(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int color = luaL_checkinteger(L, 3);
    rgb_gfx_pixel(x, y, (uint8_t)color);
    return 0;
}

static int l_gfx_rect(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    rgb_gfx_rect(x, y, w, h, (uint8_t)color);
    return 0;
}

static int l_gfx_rectfill(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    rgb_gfx_rectfill(x, y, w, h, (uint8_t)color);
    return 0;
}

static int l_gfx_palette(lua_State *L)
{
    int index = luaL_checkinteger(L, 1);
    int r = luaL_checkinteger(L, 2);
    int g = luaL_checkinteger(L, 3);
    int b = luaL_checkinteger(L, 4);

    if (index < 0 || index > 255) {
        return luaL_error(L, "palette index out of range");
    }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        return luaL_error(L, "rgb values must be 0-255");
    }

    uint16_t rgb565 = ((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5) |
                      (uint16_t)(b >> 3);
    rgb_display_set_vga_palette_entry(index, rgb565);
    return 0;
}

static int l_gfx_wait_vsync(lua_State *L)
{
    (void)L;
    rgb_display_wait_vsync();
    return 0;
}

static const luaL_Reg s_breezy_lib[] = {
    { "exec", l_breezy_exec },
    { "cwd", l_breezy_cwd },
    { "cd", l_breezy_cd },
    { "listdir", l_breezy_listdir },
    { "read_file", l_breezy_read_file },
    { "write_file", l_breezy_write_file },
    { "exists", l_breezy_exists },
    { "sleep", l_breezy_sleep },
    { "sleep_ms", l_breezy_sleep_ms },
    { "now_ms", l_breezy_now_ms },
    { "clear", l_breezy_clear },
    { "term_size", l_breezy_term_size },
    { "readkey", l_breezy_readkey },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_pin_lib[] = {
    { "mode", l_pin_mode },
    { "read", l_pin_read },
    { "write", l_pin_write },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_keyboard_lib[] = {
    { "read_event", l_keyboard_read_event },
    { "peek_event", l_keyboard_peek_event },
    { "flush", l_keyboard_flush },
    { "is_down", l_keyboard_is_down },
    { "mods", l_keyboard_mods },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_adc_lib[] = {
    { "read", l_adc_read },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_battery_lib[] = {
    { "read_uv", l_battery_read_uv },
    { "read_pct", l_battery_read_pct },
    { "read_level", l_battery_read_level },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_i2c_lib[] = {
    { "open", l_i2c_open },
    { "close", l_i2c_close },
    { "write", l_i2c_write },
    { "read", l_i2c_read },
    { "write_reg", l_i2c_write_reg },
    { "read_reg", l_i2c_read_reg },
    { "scan", l_i2c_scan },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_i2s_lib[] = {
    { "open_tx", l_i2s_open_tx },
    { "open_rx", l_i2s_open_rx },
    { "close", l_i2s_close },
    { "write", l_i2s_write },
    { "read", l_i2s_read },
    { "state", l_i2s_state },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_spi_lib[] = {
    { "open", l_spi_open },
    { "close", l_spi_close },
    { "select", l_spi_select },
    { "transfer", l_spi_transfer },
    { "write", l_spi_write },
    { "read", l_spi_read },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_storage_lib[] = {
    { "sd_mounted", l_storage_sd_mounted },
    { "mounts", l_storage_mounts },
    { "info", l_storage_info },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_network_lib[] = {
    { "is_connected", l_network_is_connected },
    { "info", l_network_info },
    { "dns_info", l_network_dns_info },
    { "tcp_connect", l_network_tcp_connect },
    { "tcp_close", l_network_tcp_close },
    { "tcp_write", l_network_tcp_write },
    { "tcp_read", l_network_tcp_read },
    { "http_head", l_network_http_head },
    { "http_get", l_network_http_get },
    { "http_post", l_network_http_post },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_sound_lib[] = {
    { "speaker_open", l_sound_speaker_open },
    { "speaker_close", l_sound_speaker_close },
    { "speaker_state", l_sound_speaker_state },
    { "speaker_stop", l_sound_speaker_stop },
    { "play_raw", l_sound_speaker_play_raw },
    { "tone", l_sound_tone },
    { "play_notes", l_sound_play_notes },
    { "mix", l_sound_mix },
    { "mic_open", l_sound_mic_open },
    { "mic_read", l_sound_mic_read },
    { "mic_close", l_sound_mic_close },
    { "beep", l_sound_beep },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_config_lib[] = {
    { "load", l_config_load },
    { "save", l_config_save },
    { "get", l_config_get },
    { "set", l_config_set },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_tui_lib[] = {
    { "move", l_tui_move },
    { "clear_line", l_tui_clear_line },
    { "write_at", l_tui_write_at },
    { "center", l_tui_center },
    { "status", l_tui_status },
    { "cursor", l_tui_cursor },
    { "box", l_tui_box },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_gfx_lib[] = {
    { "mode", l_gfx_mode },
    { "get_mode", l_gfx_get_mode },
    { "font", l_gfx_font },
    { "size", l_gfx_size },
    { "clear", l_gfx_clear },
    { "pixel", l_gfx_pixel },
    { "hline", l_gfx_hline },
    { "vline", l_gfx_vline },
    { "rect", l_gfx_rect },
    { "rectfill", l_gfx_rectfill },
    { "text", l_gfx_text },
    { "text_size", l_gfx_text_size },
    { "new_image", l_gfx_new_image },
    { "blit", l_gfx_blit },
    { "blit_flip", l_gfx_blit_flip },
    { "backlight", l_gfx_backlight },
    { "palette", l_gfx_palette },
    { "wait_vsync", l_gfx_wait_vsync },
    { NULL, NULL }
};

static const luaL_Reg s_breezy_uart_lib[] = {
    { "open", l_uart_open },
    { "close", l_uart_close },
    { "write", l_uart_write },
    { "read", l_uart_read },
    { "readline", l_uart_readline },
    { "flush", l_uart_flush },
    { NULL, NULL }
};

static int lua_reg_count(const luaL_Reg *reg)
{
    int count = 0;
    while (reg && reg[count].name) {
        ++count;
    }
    return count;
}

static int lua_push_breezy_named_module(lua_State *L, const char *name)
{
    if (!name) {
        lua_pushnil(L);
        return 1;
    }

    if (strcmp(name, "keyboard") == 0) {
        luaL_newlib(L, s_breezy_keyboard_lib);
    } else if (strcmp(name, "pin") == 0) {
        luaL_newlib(L, s_breezy_pin_lib);
    } else if (strcmp(name, "adc") == 0) {
        luaL_newlib(L, s_breezy_adc_lib);
    } else if (strcmp(name, "battery") == 0) {
        luaL_newlib(L, s_breezy_battery_lib);
    } else if (strcmp(name, "i2c") == 0) {
        luaL_newlib(L, s_breezy_i2c_lib);
    } else if (strcmp(name, "i2s") == 0) {
        luaL_newlib(L, s_breezy_i2s_lib);
    } else if (strcmp(name, "spi") == 0) {
        luaL_newlib(L, s_breezy_spi_lib);
    } else if (strcmp(name, "storage") == 0) {
        luaL_newlib(L, s_breezy_storage_lib);
    } else if (strcmp(name, "network") == 0) {
        luaL_newlib(L, s_breezy_network_lib);
    } else if (strcmp(name, "sound") == 0) {
        luaL_newlib(L, s_breezy_sound_lib);
    } else if (strcmp(name, "config") == 0) {
        luaL_newlib(L, s_breezy_config_lib);
    } else if (strcmp(name, "tui") == 0) {
        luaL_newlib(L, s_breezy_tui_lib);
    } else if (strcmp(name, "gfx") == 0) {
        luaL_newlib(L, s_breezy_gfx_lib);
    } else if (strcmp(name, "uart") == 0) {
        luaL_newlib(L, s_breezy_uart_lib);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_breezy_index(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    lua_push_breezy_named_module(L, name);
    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, -1);
        lua_setfield(L, 1, name);
    }
    return 1;
}

static int luaopen_breezy(lua_State *L)
{
    lua_createtable(L, 0, lua_reg_count(s_breezy_lib));
    luaL_setfuncs(L, s_breezy_lib, 0);

    lua_newtable(L);
    lua_pushcfunction(L, l_breezy_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);

    return 1;
}

static void lua_uart_close_all(void)
{
    for (int i = 0; i < UART_NUM_MAX; ++i) {
        if (s_lua_uart_slots[i].open) {
            uart_driver_delete(s_lua_uart_slots[i].port);
            s_lua_uart_slots[i].open = false;
        }
    }
}

static void lua_adc_close_all(void)
{
    if (s_lua_adc1) {
        adc_oneshot_del_unit(s_lua_adc1);
        s_lua_adc1 = NULL;
    }
}

static void lua_i2c_close_all(void)
{
    if (s_lua_i2c.open) {
        i2c_driver_delete(s_lua_i2c.port);
        s_lua_i2c.open = false;
    }
}

static void lua_i2s_close_all(void)
{
    lua_i2s_close_channel(&s_lua_i2s_tx);
    lua_i2s_close_channel(&s_lua_i2s_rx);
}

static void lua_tcp_close_all(void)
{
    for (int i = 0; i < LUA_TCP_SLOT_COUNT; ++i) {
        if (s_lua_tcp_slots[i].open) {
            close(s_lua_tcp_slots[i].fd);
            s_lua_tcp_slots[i].open = false;
            s_lua_tcp_slots[i].fd = -1;
        }
    }
}

static void lua_sound_close_all(void)
{
    lua_speaker_close_internal();
    if (s_lua_mic.open) {
        i2s_channel_disable(s_lua_mic.handle);
        i2s_del_channel(s_lua_mic.handle);
        memset(&s_lua_mic, 0, sizeof(s_lua_mic));
    }
}

static void setup_lua_path(lua_State *L)
{
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_pushstring(
        L,
        "/root/?.lua;/root/?/init.lua;"
        "/root/apps/?.lua;/root/apps/?/init.lua;"
        "/sd/?.lua;/sd/?/init.lua;"
        "/sd/apps/?.lua;/sd/apps/?/init.lua;"
        "./?.lua;./?/init.lua");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

static void register_breezy_preload(lua_State *L)
{
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "preload");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushcfunction(L, luaopen_breezy);
    lua_setfield(L, -2, "breezy");
    lua_pop(L, 2);
}

static void open_selected_lua_libs(lua_State *L, bool reduced)
{
    static const luaL_Reg full_libs[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_LOADLIBNAME, luaopen_package },
        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { NULL, NULL },
    };
    static const luaL_Reg reduced_libs[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_LOADLIBNAME, luaopen_package },
        { NULL, NULL },
    };
    const luaL_Reg *libs = reduced ? reduced_libs : full_libs;

    for (int i = 0; libs[i].name != NULL; ++i) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
}

static lua_State *create_lua_state(bool reduced)
{
    lua_State *L = luaL_newstate();
    if (!L) {
        printf("lua: out of memory\n");
        return NULL;
    }

    open_selected_lua_libs(L, reduced);
    lua_gfx_init_image_mt(L);
    register_breezy_preload(L);
    setup_lua_path(L);
    return L;
}

static void set_arg_table(lua_State *L, int argc, char **argv, int start_index)
{
    lua_newtable(L);
    for (int i = start_index; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - start_index);
    }
    lua_setglobal(L, "arg");
}

static int run_lua_chunk(lua_State *L, const char *chunk, const char *name, bool print_results)
{
    int top = lua_gettop(L);
    int rc = luaL_loadbuffer(L, chunk, strlen(chunk), name);
    if (rc != LUA_OK) {
        print_lua_error(L, "lua");
        return 1;
    }

    rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        print_lua_error(L, "lua");
        lua_settop(L, top);
        return 1;
    }

    if (print_results) {
        int results = lua_gettop(L) - top;
        for (int i = 0; i < results; ++i) {
            if (i > 0) {
                printf("\t");
            }
            size_t len = 0;
            const char *s = luaL_tolstring(L, top + 1 + i, &len);
            fwrite(s, 1, len, stdout);
            lua_pop(L, 1);
        }
        if (results > 0) {
            printf("\n");
        }
    }

    lua_settop(L, top);
    return 0;
}

static int load_lua_file_source(const char *path, char **source_out, size_t *len_out, char *resolved_out, size_t resolved_len)
{
    char resolved[BREEZYBOX_MAX_PATH * 2];
    if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
        printf("lua: path too long: %s\n", path);
        return 1;
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) {
        printf("lua: cannot open %s: %s\n", resolved, strerror(errno));
        return 1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        printf("lua: failed to seek %s\n", resolved);
        return 1;
    }
    long file_len = ftell(f);
    if (file_len < 0) {
        fclose(f);
        printf("lua: failed to size %s\n", resolved);
        return 1;
    }
    rewind(f);

    char *source = malloc((size_t)file_len + 1);
    if (!source) {
        fclose(f);
        printf("lua: out of memory reading %s\n", resolved);
        return 1;
    }

    size_t read_len = fread(source, 1, (size_t)file_len, f);
    fclose(f);
    if (read_len != (size_t)file_len) {
        free(source);
        printf("lua: failed to read %s\n", resolved);
        return 1;
    }
    source[read_len] = '\0';

    if (resolved_out && resolved_len > 0) {
        strlcpy(resolved_out, resolved, resolved_len);
    }
    *source_out = source;
    if (len_out) {
        *len_out = read_len;
    }
    return 0;
}

static int run_lua_source(lua_State *L, const char *name, const char *source, size_t len,
                          int argc, char **argv, int arg_start)
{
    set_arg_table(L, argc, argv, arg_start);
    int rc = luaL_loadbuffer(L, source, len, name);
    if (rc != LUA_OK) {
        print_lua_error(L, "lua");
        return 1;
    }
    rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        print_lua_error(L, "lua");
        return 1;
    }
    return 0;
}

static int run_lua_file(lua_State *L, const char *path, int argc, char **argv, int arg_start)
{
    char resolved[BREEZYBOX_MAX_PATH * 2];
    char *source = NULL;
    size_t source_len = 0;
    int rc = load_lua_file_source(path, &source, &source_len, resolved, sizeof(resolved));
    if (rc != 0) {
        return rc;
    }
    rc = run_lua_source(L, resolved, source, source_len, argc, argv, arg_start);
    free(source);
    return rc;
}

static int run_lua_repl(lua_State *L)
{
    printf("Embedded Lua 5.4\n");
    printf("Type 'exit' or Ctrl-D to leave the REPL. Use '=expr' to print a value.\n");

    while (true) {
        char *line = linenoise("lua> ");
        if (!line) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len == 0) {
            linenoiseFree(line);
            continue;
        }
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            linenoiseFree(line);
            break;
        }

        linenoiseHistoryAdd(line);
        if (line[0] == '=') {
            char expr[LUA_LINE_MAX + 16];
            snprintf(expr, sizeof(expr), "return %s", line + 1);
            run_lua_chunk(L, expr, "=stdin", true);
        } else {
            run_lua_chunk(L, line, "=stdin", false);
        }
        linenoiseFree(line);
    }

    return 0;
}

int cmd_lua(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "guide") == 0) {
        return breezybox_exec("more /root/lua/GUIDE.txt");
    }

    bool run_file_mode = !(argc == 1 || (argc == 2 && strcmp(argv[1], "shell") == 0) ||
                           (argc >= 3 && strcmp(argv[1], "-e") == 0));
    char *preloaded_source = NULL;
    size_t preloaded_len = 0;
    char preloaded_name[BREEZYBOX_MAX_PATH * 2];
    preloaded_name[0] = '\0';

    if (run_file_mode) {
        int preload_rc = load_lua_file_source(argv[1], &preloaded_source, &preloaded_len,
                                              preloaded_name, sizeof(preloaded_name));
        if (preload_rc != 0) {
            return preload_rc;
        }
    }

    bool reduced_lua = run_file_mode || (argc >= 3 && strcmp(argv[1], "-e") == 0);
    lua_State *L = create_lua_state(reduced_lua);
    if (!L) {
        free(preloaded_source);
        return 1;
    }

    int rc = 0;

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "shell") == 0)) {
        rc = run_lua_repl(L);
    } else if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        set_arg_table(L, argc, argv, 3);
        rc = run_lua_chunk(L, argv[2], "=cmdline", false);
    } else {
        rc = run_lua_source(L, preloaded_name, preloaded_source, preloaded_len, argc, argv, 1);
    }

    lua_uart_close_all();
    lua_i2c_close_all();
    lua_i2s_close_all();
    lua_tcp_close_all();
    lua_sound_close_all();
    lua_adc_close_all();
    lua_close(L);
    free(preloaded_source);
    return rc;
}

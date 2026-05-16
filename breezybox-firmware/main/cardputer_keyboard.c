#include "cardputer_keyboard.h"
#include "my_console_io.h"
#include "vterm.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "cardputer_kbd";

#define KBD_I2C_PORT            I2C_NUM_1
#define KBD_I2C_SCL             GPIO_NUM_9
#define KBD_I2C_SDA             GPIO_NUM_8
#define KBD_I2C_FREQ_HZ         400000
#define KBD_I2C_ADDR            0x34

#define TCA8418_REG_CFG             0x01
#define TCA8418_REG_INT_STAT        0x02
#define TCA8418_REG_KEY_LCK_EC      0x03
#define TCA8418_REG_KEY_EVENT_A     0x04
#define TCA8418_REG_GPIO_INT_STAT_1 0x11
#define TCA8418_REG_GPIO_INT_STAT_2 0x12
#define TCA8418_REG_GPIO_INT_STAT_3 0x13
#define TCA8418_REG_GPIO_INT_EN_1   0x1A
#define TCA8418_REG_GPIO_INT_EN_2   0x1B
#define TCA8418_REG_GPIO_INT_EN_3   0x1C
#define TCA8418_REG_KP_GPIO_1       0x1D
#define TCA8418_REG_KP_GPIO_2       0x1E
#define TCA8418_REG_KP_GPIO_3       0x1F
#define TCA8418_REG_GPI_EM_1        0x20
#define TCA8418_REG_GPI_EM_2        0x21
#define TCA8418_REG_GPI_EM_3        0x22
#define TCA8418_REG_GPIO_DIR_1      0x23
#define TCA8418_REG_GPIO_DIR_2      0x24
#define TCA8418_REG_GPIO_DIR_3      0x25
#define TCA8418_REG_GPIO_INT_LVL_1  0x26
#define TCA8418_REG_GPIO_INT_LVL_2  0x27
#define TCA8418_REG_GPIO_INT_LVL_3  0x28
#define TCA8418_REG_CFG_GPI_IEN     0x02
#define TCA8418_REG_CFG_KE_IEN      0x01

#define SCROLLBACK_STEP (VTERM_ROWS - 2)
#define KEY_QUEUE_SIZE 32

typedef struct {
    char value_first;
    char value_second;
} key_value_t;

typedef struct {
    uint8_t row;
    uint8_t col;
    bool pressed;
} key_event_t;

static const key_value_t s_key_value_map[4][14] = {
    {{'`', '~'}, {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
     {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}, {CARDPUTER_KEY_BACKSPACE, CARDPUTER_KEY_BACKSPACE}},
    {{CARDPUTER_KEY_TAB, CARDPUTER_KEY_TAB}, {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'},
     {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}, {'\\', '|'}},
    {{CARDPUTER_KEY_FN, CARDPUTER_KEY_FN}, {CARDPUTER_KEY_LEFT_SHIFT, CARDPUTER_KEY_LEFT_SHIFT}, {'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'},
     {'g', 'G'}, {'h', 'H'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {CARDPUTER_KEY_ENTER, CARDPUTER_KEY_ENTER}},
    {{CARDPUTER_KEY_LEFT_CTRL, CARDPUTER_KEY_LEFT_CTRL}, {CARDPUTER_KEY_OPT, CARDPUTER_KEY_OPT}, {CARDPUTER_KEY_LEFT_ALT, CARDPUTER_KEY_LEFT_ALT}, {'z', 'Z'}, {'x', 'X'},
     {'c', 'C'}, {'v', 'V'}, {'b', 'B'}, {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}, {' ', ' '}},
};

static bool s_key_state[4][14];
static cardputer_keyboard_char_cb_t s_char_cb;
static cardputer_keyboard_key_cb_t s_key_cb;
static bool s_ready;
static volatile uint32_t s_poll_interval_ms = 10;
static volatile int s_background_poll_enabled = 1;
static cardputer_keyboard_key_event_t s_key_queue[KEY_QUEUE_SIZE];
static volatile uint8_t s_key_queue_head;
static volatile uint8_t s_key_queue_tail;
static volatile uint8_t s_key_queue_count;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_write_to_device(KBD_I2C_PORT, KBD_I2C_ADDR, payload, sizeof(payload), pdMS_TO_TICKS(20));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(KBD_I2C_PORT, KBD_I2C_ADDR, &reg, 1, value, 1, pdMS_TO_TICKS(20));
}

static void flush_events(void)
{
    uint8_t event = 0;
    do {
        if (read_reg(TCA8418_REG_KEY_EVENT_A, &event) != ESP_OK) {
            break;
        }
    } while (event != 0);

    (void)read_reg(TCA8418_REG_GPIO_INT_STAT_1, &event);
    (void)read_reg(TCA8418_REG_GPIO_INT_STAT_2, &event);
    (void)read_reg(TCA8418_REG_GPIO_INT_STAT_3, &event);
    (void)write_reg(TCA8418_REG_INT_STAT, 3);
}

static key_event_t decode_event(uint8_t raw)
{
    key_event_t event = {0};
    uint8_t code = raw & 0x7F;

    event.pressed = (raw & 0x80) != 0;
    if (code == 0) {
        return event;
    }

    code--;
    event.row = code / 10;
    event.col = code % 10;

    {
        uint8_t col = event.row * 2;
        if (event.col > 3) {
            col++;
        }
        event.row = (event.col + 4) % 4;
        event.col = col;
    }

    return event;
}

static bool modifier_pressed(char keycode)
{
    switch ((uint8_t)keycode) {
        case CARDPUTER_KEY_LEFT_CTRL: return true;
        case CARDPUTER_KEY_LEFT_SHIFT: return true;
        case CARDPUTER_KEY_LEFT_ALT: return true;
        case CARDPUTER_KEY_FN: return true;
        default: return false;
    }
}

static bool any_pressed(char keycode)
{
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 14; ++col) {
            if (s_key_state[row][col] && s_key_value_map[row][col].value_first == keycode) {
                return true;
            }
        }
    }
    return false;
}

static void emit_char(char c)
{
    if (s_char_cb) {
        s_char_cb(c);
    }
}

static void emit_str(const char *str)
{
    while (*str) {
        emit_char(*str++);
    }
}

static void queue_key_event(const cardputer_keyboard_key_event_t *event)
{
    if (!event) {
        return;
    }
    if (s_key_queue_count >= KEY_QUEUE_SIZE) {
        s_key_queue_head = (uint8_t)((s_key_queue_head + 1U) % KEY_QUEUE_SIZE);
        s_key_queue_count--;
    }
    s_key_queue[s_key_queue_tail] = *event;
    s_key_queue_tail = (uint8_t)((s_key_queue_tail + 1U) % KEY_QUEUE_SIZE);
    s_key_queue_count++;
}

static bool emit_ctrl_code(char base)
{
    if (base >= 'a' && base <= 'z') {
        emit_char((char)(base - 'a' + 1));
        return true;
    }
    if (base >= 'A' && base <= 'Z') {
        emit_char((char)(base - 'A' + 1));
        return true;
    }
    return false;
}

static void handle_press(uint8_t row, uint8_t col)
{
    char base = s_key_value_map[row][col].value_first;
    bool fn = any_pressed(CARDPUTER_KEY_FN);
    bool shift = any_pressed(CARDPUTER_KEY_LEFT_SHIFT);
    bool ctrl = any_pressed(CARDPUTER_KEY_LEFT_CTRL);
    bool alt = any_pressed(CARDPUTER_KEY_LEFT_ALT);
    bool opt = any_pressed(CARDPUTER_KEY_OPT);

    cardputer_keyboard_key_event_t event = {
        .base = base,
        .shifted = s_key_value_map[row][col].value_second,
        .fn = fn,
        .shift = shift,
        .ctrl = ctrl,
        .alt = alt,
        .opt = opt,
        .enter = ((uint8_t)base == CARDPUTER_KEY_ENTER),
        .tab = ((uint8_t)base == CARDPUTER_KEY_TAB),
        .backspace = ((uint8_t)base == CARDPUTER_KEY_BACKSPACE),
        .pressed = true,
        .row = row,
        .col = col,
    };
    queue_key_event(&event);

    if (modifier_pressed(base) || base == CARDPUTER_KEY_OPT) {
        return;
    }

    if (s_key_cb) {
        if (s_key_cb(&event)) {
            return;
        }
    }

    if (ctrl) {
        switch ((uint8_t)base) {
            case '1':
                vterm_switch(0);
                return;
            case '2':
                if (VTERM_COUNT > 1) vterm_switch(1);
                return;
            case '3':
                if (VTERM_COUNT > 2) vterm_switch(2);
                return;
            case '4':
                if (VTERM_COUNT > 3) vterm_switch(3);
                return;
            case ';':
                my_console_scrollback(SCROLLBACK_STEP);
                return;
            case '.':
                my_console_scrollback(-SCROLLBACK_STEP);
                return;
            default:
                break;
        }
    }

    if (my_console_scrollback_active()) {
        my_console_scrollback_bottom();
    }

    if (ctrl && emit_ctrl_code(base)) {
        return;
    }

    if (fn) {
        switch ((uint8_t)base) {
            case ';':
                emit_str("\x1b[A");
                return;
            case '.':
                emit_str("\x1b[B");
                return;
            case ',':
                emit_str("\x1b[D");
                return;
            case '/':
                emit_str("\x1b[C");
                return;
            case CARDPUTER_KEY_BACKSPACE:
                emit_char(0x7F);
                return;
            case '`':
                emit_char('\x1b');
                return;
            default:
                break;
        }
    }

    if (alt || opt) {
        emit_char('\x1b');
    }

    switch ((uint8_t)base) {
        case CARDPUTER_KEY_ENTER:
            emit_char('\n');
            return;
        case CARDPUTER_KEY_TAB:
            emit_char('\t');
            return;
        case CARDPUTER_KEY_BACKSPACE:
            emit_char('\b');
            return;
        default:
            break;
    }

    emit_char(shift ? s_key_value_map[row][col].value_second : base);
}

esp_err_t cardputer_keyboard_init(cardputer_keyboard_char_cb_t cb)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = KBD_I2C_SDA,
        .scl_io_num = KBD_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = KBD_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    s_char_cb = cb;

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_param_config(KBD_I2C_PORT, &conf));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_driver_install(KBD_I2C_PORT, conf.mode, 0, 0, 0));

    if (write_reg(TCA8418_REG_GPIO_DIR_1, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_DIR_2, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_DIR_3, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPI_EM_1, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_GPI_EM_2, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_GPI_EM_3, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_LVL_1, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_LVL_2, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_LVL_3, 0x00) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_EN_1, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_EN_2, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_GPIO_INT_EN_3, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_KP_GPIO_1, 0x7F) != ESP_OK ||
        write_reg(TCA8418_REG_KP_GPIO_2, 0xFF) != ESP_OK ||
        write_reg(TCA8418_REG_CFG, TCA8418_REG_CFG_GPI_IEN | TCA8418_REG_CFG_KE_IEN) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TCA8418 keyboard");
        return ESP_FAIL;
    }

    memset(s_key_state, 0, sizeof(s_key_state));
    flush_events();
    s_ready = true;
    ESP_LOGI(TAG, "Cardputer ADV keyboard ready");
    return ESP_OK;
}

static void cardputer_keyboard_poll_impl(void)
{
    uint8_t pending = 0;
    uint8_t raw = 0;
    bool saw_events = false;

    if (!s_ready || read_reg(TCA8418_REG_KEY_LCK_EC, &pending) != ESP_OK) {
        return;
    }

    pending &= 0x0F;
    if (pending == 0) {
        return;
    }

    while (pending-- > 0) {
        if (read_reg(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK || raw == 0) {
            break;
        }
        saw_events = true;

        key_event_t event = decode_event(raw);
        if (event.row >= 4 || event.col >= 14) {
            continue;
        }

        s_key_state[event.row][event.col] = event.pressed;
        if (event.pressed) {
            handle_press(event.row, event.col);
        } else {
            cardputer_keyboard_key_event_t key_event = {
                .base = s_key_value_map[event.row][event.col].value_first,
                .shifted = s_key_value_map[event.row][event.col].value_second,
                .fn = any_pressed(CARDPUTER_KEY_FN),
                .shift = any_pressed(CARDPUTER_KEY_LEFT_SHIFT),
                .ctrl = any_pressed(CARDPUTER_KEY_LEFT_CTRL),
                .alt = any_pressed(CARDPUTER_KEY_LEFT_ALT),
                .opt = any_pressed(CARDPUTER_KEY_OPT),
                .enter = ((uint8_t)s_key_value_map[event.row][event.col].value_first == CARDPUTER_KEY_ENTER),
                .tab = ((uint8_t)s_key_value_map[event.row][event.col].value_first == CARDPUTER_KEY_TAB),
                .backspace = ((uint8_t)s_key_value_map[event.row][event.col].value_first == CARDPUTER_KEY_BACKSPACE),
                .pressed = false,
                .row = event.row,
                .col = event.col,
            };
            queue_key_event(&key_event);
        }
    }

    if (saw_events) {
        (void)write_reg(TCA8418_REG_INT_STAT, 1);
    }
}

void cardputer_keyboard_poll(void)
{
    if (!s_background_poll_enabled) {
        return;
    }
    cardputer_keyboard_poll_impl();
}

void cardputer_keyboard_poll_direct(void)
{
    cardputer_keyboard_poll_impl();
}

void cardputer_keyboard_set_poll_interval_ms(uint32_t ms)
{
    if (ms == 0) {
        ms = 1;
    }
    s_poll_interval_ms = ms;
}

uint32_t cardputer_keyboard_get_poll_interval_ms(void)
{
    return s_poll_interval_ms;
}

void cardputer_keyboard_set_char_callback(cardputer_keyboard_char_cb_t cb)
{
    s_char_cb = cb;
}

void cardputer_keyboard_set_key_callback(cardputer_keyboard_key_cb_t cb)
{
    s_key_cb = cb;
}

void cardputer_keyboard_set_background_poll_enabled(int enabled)
{
    s_background_poll_enabled = enabled ? 1 : 0;
}

int cardputer_keyboard_pop_event(cardputer_keyboard_key_event_t *event)
{
    if (s_key_queue_count == 0) {
        return 0;
    }
    if (event) {
        *event = s_key_queue[s_key_queue_head];
    }
    s_key_queue_head = (uint8_t)((s_key_queue_head + 1U) % KEY_QUEUE_SIZE);
    s_key_queue_count--;
    return 1;
}

int cardputer_keyboard_peek_event(cardputer_keyboard_key_event_t *event)
{
    if (s_key_queue_count == 0) {
        return 0;
    }
    if (event) {
        *event = s_key_queue[s_key_queue_head];
    }
    return 1;
}

void cardputer_keyboard_flush_events_queue(void)
{
    s_key_queue_head = 0;
    s_key_queue_tail = 0;
    s_key_queue_count = 0;
}

int cardputer_keyboard_key_is_down(char keycode)
{
    return any_pressed(keycode) ? 1 : 0;
}

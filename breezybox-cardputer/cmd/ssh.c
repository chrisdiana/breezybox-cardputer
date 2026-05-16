#include "breezy_cmd.h"
#include "breezy_vfs.h"
#include "vterm.h"
#include "rgb_display.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <libssh/libssh.h>
#include <libssh/scp.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __XTENSA__
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#define ssh_delay_ms(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#else
#include <time.h>
static void ssh_delay_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}
#endif

#define SSH_BUF_SIZE 512
#define SSH_MAX_READS_PER_TICK 16
#define SSH_TERM_COLS DISPLAY_COLS
#define SSH_TERM_ROWS DISPLAY_ROWS

typedef struct {
    lcd_cell_t main_cells[SSH_TERM_ROWS * SSH_TERM_COLS];
    lcd_cell_t alt_cells[SSH_TERM_ROWS * SSH_TERM_COLS];
    bool alt_screen;
    int cursor_x;
    int cursor_y;
    int saved_x;
    int saved_y;
    int scroll_top;
    int scroll_bottom;
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool in_esc;
    bool in_csi;
    bool in_osc;
    bool csi_private;
    char csi_buf[64];
    int csi_len;
    int drawn_cursor_x;
    int drawn_cursor_y;
} ssh_term_t;

typedef struct {
    char *host;
    char *user;
    char *password;
    int port;
    int verbosity;
} ssh_target_t;

typedef struct {
    char *name;
    char *host;
    char *user;
    char *password;
    int port;
} ssh_profile_t;

typedef struct {
    bool is_remote;
    char *user;
    char *host;
    char *path;
} scp_location_t;

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
} cardputer_keyboard_key_event_t;

typedef bool (*cardputer_keyboard_key_cb_t)(const cardputer_keyboard_key_event_t *event);

typedef struct {
    ssh_session session;
    TaskHandle_t waiter;
    volatile int rc;
} ssh_interactive_ctx_t;

static bool s_ssh_lib_ready = false;
static QueueHandle_t s_ssh_input_queue = NULL;
static int (*s_ssh_app_mode_runner)(ssh_session session) = NULL;

void breezybox_set_ssh_app_mode_runner(int (*runner)(ssh_session session))
{
    s_ssh_app_mode_runner = runner;
}

void my_console_set_cursor_sync_enabled(int enabled);
void my_console_bt_receive(char c);
void cardputer_keyboard_poll_direct(void);
void cardputer_keyboard_set_poll_interval_ms(uint32_t ms);
uint32_t cardputer_keyboard_get_poll_interval_ms(void);
void cardputer_keyboard_set_char_callback(void (*cb)(char));
void cardputer_keyboard_set_key_callback(cardputer_keyboard_key_cb_t cb);
void cardputer_keyboard_set_background_poll_enabled(int enabled);

static void ssh_keyboard_receive(char c)
{
    if (!s_ssh_input_queue) {
        return;
    }
    if (c == '\b') {
        c = 0x7F;
    }
    (void)xQueueSend(s_ssh_input_queue, &c, 0);
}

static void ssh_keyboard_send_bytes(const char *buf, size_t len)
{
    if (!s_ssh_input_queue || !buf) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = buf[i];
        if (c == '\n') {
            c = '\r';
        }
        (void)xQueueSend(s_ssh_input_queue, &c, 0);
    }
}

static bool ssh_keyboard_handle_key(const cardputer_keyboard_key_event_t *event)
{
    if (!event) {
        return false;
    }

    if (event->fn) {
        switch ((uint8_t)event->base) {
            case 'q': {
                char quit = 0x11;
                ssh_keyboard_send_bytes(&quit, 1);
                return true;
            }
            case ';':
                ssh_keyboard_send_bytes("\x1b[A", 3);
                return true;
            case '.':
                ssh_keyboard_send_bytes("\x1b[B", 3);
                return true;
            case ',':
                ssh_keyboard_send_bytes("\x1b[D", 3);
                return true;
            case '/':
                ssh_keyboard_send_bytes("\x1b[C", 3);
                return true;
            default:
                break;
        }
    }

    if (event->ctrl) {
        if (event->base >= 'a' && event->base <= 'z') {
            char c = (char)(event->base - 'a' + 1);
            ssh_keyboard_send_bytes(&c, 1);
            return true;
        }
        if (event->base >= 'A' && event->base <= 'Z') {
            char c = (char)(event->base - 'A' + 1);
            ssh_keyboard_send_bytes(&c, 1);
            return true;
        }
        if (event->base == '[') {
            ssh_keyboard_send_bytes("\x1b", 1);
            return true;
        }
    }

    if (event->alt || event->opt) {
        ssh_keyboard_send_bytes("\x1b", 1);
    }

    if (event->enter) {
        ssh_keyboard_send_bytes("\r", 1);
        return true;
    }
    if (event->tab) {
        ssh_keyboard_send_bytes("\t", 1);
        return true;
    }
    if (event->backspace) {
        char del = 0x7F;
        ssh_keyboard_send_bytes(&del, 1);
        return true;
    }

    char c = event->shift ? event->shifted : event->base;
    if ((unsigned char)c >= 0x20) {
        ssh_keyboard_send_bytes(&c, 1);
        return true;
    }

    return false;
}

static inline lcd_cell_t *ssh_term_active_cells(ssh_term_t *term)
{
    return term->alt_screen ? term->alt_cells : term->main_cells;
}

static inline uint8_t ssh_term_attr(const ssh_term_t *term)
{
    return (uint8_t)(((term->bg & 0x0F) << 4) | (term->fg & 0x0F));
}

static void ssh_term_clear_buffer(lcd_cell_t *cells, uint8_t attr)
{
    for (int i = 0; i < SSH_TERM_ROWS * SSH_TERM_COLS; ++i) {
        cells[i].ch = ' ';
        cells[i].attr = attr;
    }
}

static void ssh_term_reset_attrs(ssh_term_t *term)
{
    term->fg = VTERM_WHITE;
    term->bg = VTERM_BLACK;
    term->bold = false;
}

static void ssh_term_init(ssh_term_t *term)
{
    memset(term, 0, sizeof(*term));
    ssh_term_reset_attrs(term);
    term->scroll_top = 0;
    term->scroll_bottom = SSH_TERM_ROWS - 1;
    ssh_term_clear_buffer(term->main_cells, ssh_term_attr(term));
    ssh_term_clear_buffer(term->alt_cells, ssh_term_attr(term));
}

static void ssh_term_refresh_display_buffer(ssh_term_t *term)
{
    rgb_display_set_buffer(ssh_term_active_cells(term));
}

static void ssh_term_sync_cursor(const ssh_term_t *term)
{
    rgb_display_set_cursor(term->cursor_x, term->cursor_y);
}

static void ssh_term_fill_row(ssh_term_t *term, lcd_cell_t *cells, int row)
{
    uint8_t attr = ssh_term_attr(term);
    for (int col = 0; col < SSH_TERM_COLS; ++col) {
        cells[row * SSH_TERM_COLS + col].ch = ' ';
        cells[row * SSH_TERM_COLS + col].attr = attr;
    }
}

static void ssh_term_scroll_up(ssh_term_t *term, int count, int from_row)
{
    lcd_cell_t *cells = ssh_term_active_cells(term);
    if (from_row < 0) {
        from_row = term->scroll_top;
    }
    if (from_row < 0) {
        from_row = 0;
    }

    while (count-- > 0) {
        for (int row = from_row; row < term->scroll_bottom; ++row) {
            memmove(&cells[row * SSH_TERM_COLS],
                    &cells[(row + 1) * SSH_TERM_COLS],
                    SSH_TERM_COLS * sizeof(lcd_cell_t));
        }
        ssh_term_fill_row(term, cells, term->scroll_bottom);
    }

}

static void ssh_term_scroll_down(ssh_term_t *term, int count, int from_row)
{
    lcd_cell_t *cells = ssh_term_active_cells(term);
    if (from_row < 0) {
        from_row = term->scroll_top;
    }
    if (from_row < 0) {
        from_row = 0;
    }

    while (count-- > 0) {
        for (int row = term->scroll_bottom; row > from_row; --row) {
            memmove(&cells[row * SSH_TERM_COLS],
                    &cells[(row - 1) * SSH_TERM_COLS],
                    SSH_TERM_COLS * sizeof(lcd_cell_t));
        }
        ssh_term_fill_row(term, cells, from_row);
    }

}

static void ssh_term_put_char(ssh_term_t *term, char ch)
{
    lcd_cell_t *cells = ssh_term_active_cells(term);

    if (term->cursor_x >= SSH_TERM_COLS) {
        term->cursor_x = 0;
        term->cursor_y++;
        if (term->cursor_y > term->scroll_bottom) {
            ssh_term_scroll_up(term, 1, -1);
            term->cursor_y = term->scroll_bottom;
        }
    }
    if (term->cursor_y >= SSH_TERM_ROWS) {
        term->cursor_y = SSH_TERM_ROWS - 1;
    }

    lcd_cell_t *cell = &cells[term->cursor_y * SSH_TERM_COLS + term->cursor_x];
    cell->ch = ch;
    cell->attr = ssh_term_attr(term);
    term->cursor_x++;
}

static void ssh_term_set_cell_blank(ssh_term_t *term, int row, int col)
{
    if (row < 0 || row >= SSH_TERM_ROWS || col < 0 || col >= SSH_TERM_COLS) {
        return;
    }
    lcd_cell_t *cells = ssh_term_active_cells(term);
    cells[row * SSH_TERM_COLS + col].ch = ' ';
    cells[row * SSH_TERM_COLS + col].attr = ssh_term_attr(term);
}

static void ssh_term_handle_sgr(ssh_term_t *term, const char *params)
{
    int values[8];
    int count = 0;
    const char *p = params;

    if (!params[0]) {
        ssh_term_reset_attrs(term);
        return;
    }

    while (*p && count < 8) {
        if (*p >= '0' && *p <= '9') {
            values[count++] = (int)strtol(p, (char **)&p, 10);
        } else if (*p == ';') {
            values[count++] = 0;
            ++p;
        } else {
            ++p;
        }
    }

    for (int i = 0; i < count; ++i) {
        int v = values[i];
        if (v == 0) {
            ssh_term_reset_attrs(term);
        } else if (v == 1) {
            term->bold = true;
        } else if (v == 22) {
            term->bold = false;
            term->fg &= 0x07;
        } else if (v == 7) {
            uint8_t tmp = term->fg;
            term->fg = term->bg;
            term->bg = tmp;
        } else if (v == 27) {
            ssh_term_reset_attrs(term);
        } else if (v >= 30 && v <= 37) {
            term->fg = (uint8_t)(v - 30) | (term->bold ? VTERM_BRIGHT : 0);
        } else if (v == 39) {
            term->fg = VTERM_WHITE;
        } else if (v >= 40 && v <= 47) {
            term->bg = (uint8_t)(v - 40);
        } else if (v == 49) {
            term->bg = VTERM_BLACK;
        } else if (v >= 90 && v <= 97) {
            term->fg = (uint8_t)(v - 90) | VTERM_BRIGHT;
        } else if (v >= 100 && v <= 107) {
            term->bg = (uint8_t)(v - 100) | VTERM_BRIGHT;
        }
    }
}

static void ssh_term_handle_csi(ssh_term_t *term, char final)
{
    int p[8];
    int pc = 0;
    char *s = term->csi_buf;

    memset(p, -1, sizeof(p));
    while (*s && pc < 8) {
        if (*s >= '0' && *s <= '9') {
            p[pc++] = (int)strtol(s, &s, 10);
        } else if (*s == ';') {
            if (p[pc] < 0) {
                p[pc] = 0;
            }
            ++pc;
            ++s;
        } else {
            ++s;
        }
    }

    int p1 = (p[0] < 0) ? 1 : p[0];
    int p2 = (p[1] < 0) ? 1 : p[1];
    lcd_cell_t *cells = ssh_term_active_cells(term);

    if (term->csi_private) {
        if (final == 'h' || final == 'l') {
            bool set = (final == 'h');
            int mode = (p[0] < 0) ? 0 : p[0];
            if (mode == 1049 || mode == 1047 || mode == 47) {
                if (set && !term->alt_screen) {
                    term->alt_screen = true;
                    term->saved_x = term->cursor_x;
                    term->saved_y = term->cursor_y;
                    term->cursor_x = 0;
                    term->cursor_y = 0;
                    ssh_term_clear_buffer(term->alt_cells, ssh_term_attr(term));
                    ssh_term_refresh_display_buffer(term);
                } else if (!set && term->alt_screen) {
                    term->alt_screen = false;
                    term->cursor_x = term->saved_x;
                    term->cursor_y = term->saved_y;
                    ssh_term_refresh_display_buffer(term);
                }
            }
        }
        term->csi_private = false;
        return;
    }

    switch (final) {
        case 'A':
            term->cursor_y -= p1;
            if (term->cursor_y < term->scroll_top) term->cursor_y = term->scroll_top;
            break;
        case 'B':
            term->cursor_y += p1;
            if (term->cursor_y > term->scroll_bottom) term->cursor_y = term->scroll_bottom;
            break;
        case 'C':
            term->cursor_x += p1;
            if (term->cursor_x >= SSH_TERM_COLS) term->cursor_x = SSH_TERM_COLS - 1;
            break;
        case 'D':
            term->cursor_x -= p1;
            if (term->cursor_x < 0) term->cursor_x = 0;
            break;
        case 'E':
            term->cursor_y += p1;
            term->cursor_x = 0;
            if (term->cursor_y > term->scroll_bottom) term->cursor_y = term->scroll_bottom;
            break;
        case 'F':
            term->cursor_y -= p1;
            term->cursor_x = 0;
            if (term->cursor_y < term->scroll_top) term->cursor_y = term->scroll_top;
            break;
        case 'G':
            term->cursor_x = p1 - 1;
            if (term->cursor_x < 0) term->cursor_x = 0;
            if (term->cursor_x >= SSH_TERM_COLS) term->cursor_x = SSH_TERM_COLS - 1;
            break;
        case 'H':
        case 'f':
            term->cursor_y = p1 - 1;
            term->cursor_x = p2 - 1;
            if (term->cursor_y < 0) term->cursor_y = 0;
            if (term->cursor_y >= SSH_TERM_ROWS) term->cursor_y = SSH_TERM_ROWS - 1;
            if (term->cursor_x < 0) term->cursor_x = 0;
            if (term->cursor_x >= SSH_TERM_COLS) term->cursor_x = SSH_TERM_COLS - 1;
            break;
        case 'J':
            if (p1 == 2 || p1 == 3) {
                ssh_term_clear_buffer(cells, ssh_term_attr(term));
                term->cursor_x = 0;
                term->cursor_y = 0;
            } else if (p1 == 1) {
                for (int row = 0; row < term->cursor_y; ++row) {
                    ssh_term_fill_row(term, cells, row);
                }
                for (int col = 0; col <= term->cursor_x; ++col) {
                    ssh_term_set_cell_blank(term, term->cursor_y, col);
                }
            } else {
                for (int col = term->cursor_x; col < SSH_TERM_COLS; ++col) {
                    ssh_term_set_cell_blank(term, term->cursor_y, col);
                }
                for (int row = term->cursor_y + 1; row < SSH_TERM_ROWS; ++row) {
                    ssh_term_fill_row(term, cells, row);
                }
            }
            break;
        case 'K':
            if (p1 == 2) {
                ssh_term_fill_row(term, cells, term->cursor_y);
            } else if (p1 == 1) {
                for (int col = 0; col <= term->cursor_x; ++col) {
                    ssh_term_set_cell_blank(term, term->cursor_y, col);
                }
            } else {
                for (int col = term->cursor_x; col < SSH_TERM_COLS; ++col) {
                    ssh_term_set_cell_blank(term, term->cursor_y, col);
                }
            }
            break;
        case 'L':
            ssh_term_scroll_down(term, p1, term->cursor_y);
            break;
        case 'M':
            ssh_term_scroll_up(term, p1, term->cursor_y);
            break;
        case 'P':
            for (int col = term->cursor_x; col < SSH_TERM_COLS; ++col) {
                int src = col + p1;
                cells[term->cursor_y * SSH_TERM_COLS + col] =
                    (src < SSH_TERM_COLS)
                        ? cells[term->cursor_y * SSH_TERM_COLS + src]
                        : (lcd_cell_t){ .ch = ' ', .attr = ssh_term_attr(term) };
            }
            break;
        case '@':
            for (int col = SSH_TERM_COLS - 1; col >= term->cursor_x; --col) {
                int src = col - p1;
                cells[term->cursor_y * SSH_TERM_COLS + col] =
                    (src >= term->cursor_x)
                        ? cells[term->cursor_y * SSH_TERM_COLS + src]
                        : (lcd_cell_t){ .ch = ' ', .attr = ssh_term_attr(term) };
            }
            break;
        case 'S':
            ssh_term_scroll_up(term, p1, -1);
            break;
        case 'T':
            ssh_term_scroll_down(term, p1, -1);
            break;
        case 'd':
            term->cursor_y = p1 - 1;
            if (term->cursor_y < 0) term->cursor_y = 0;
            if (term->cursor_y >= SSH_TERM_ROWS) term->cursor_y = SSH_TERM_ROWS - 1;
            break;
        case 'm':
            ssh_term_handle_sgr(term, term->csi_buf);
            break;
        case 'r':
            term->scroll_top = p1 - 1;
            term->scroll_bottom = ((p[1] < 0) ? SSH_TERM_ROWS : p[1]) - 1;
            if (term->scroll_top < 0) term->scroll_top = 0;
            if (term->scroll_bottom >= SSH_TERM_ROWS) term->scroll_bottom = SSH_TERM_ROWS - 1;
            if (term->scroll_top >= term->scroll_bottom) {
                term->scroll_top = 0;
                term->scroll_bottom = SSH_TERM_ROWS - 1;
            }
            term->cursor_x = 0;
            term->cursor_y = 0;
            break;
        case 's':
            term->saved_x = term->cursor_x;
            term->saved_y = term->cursor_y;
            break;
        case 'u':
            term->cursor_x = term->saved_x;
            term->cursor_y = term->saved_y;
            if (term->cursor_x >= SSH_TERM_COLS) term->cursor_x = SSH_TERM_COLS - 1;
            if (term->cursor_y >= SSH_TERM_ROWS) term->cursor_y = SSH_TERM_ROWS - 1;
            break;
        default:
            break;
    }
}

static void ssh_term_process_byte(ssh_term_t *term, uint8_t ch)
{
    if (term->in_osc) {
        if (ch == 0x07 || ch == 0x9C) {
            term->in_osc = false;
        } else if (ch == 0x1B) {
            term->in_osc = false;
            term->in_esc = true;
        }
        return;
    }

    if (term->in_csi) {
        if (ch >= 0x40 && ch <= 0x7E) {
            term->csi_buf[term->csi_len] = '\0';
            term->in_csi = false;
            ssh_term_handle_csi(term, (char)ch);
            term->csi_len = 0;
            return;
        }
        if (ch == '?') {
            term->csi_private = true;
            return;
        }
        if (term->csi_len < (int)sizeof(term->csi_buf) - 1) {
            term->csi_buf[term->csi_len++] = (char)ch;
        }
        return;
    }

    if (term->in_esc) {
        term->in_esc = false;
        if (ch == '[') {
            term->in_csi = true;
            term->csi_len = 0;
            term->csi_private = false;
        } else if (ch == ']') {
            term->in_osc = true;
        } else if (ch == '7') {
            term->saved_x = term->cursor_x;
            term->saved_y = term->cursor_y;
        } else if (ch == '8') {
            term->cursor_x = term->saved_x;
            term->cursor_y = term->saved_y;
        } else if (ch == 'M') {
            if (term->cursor_y > term->scroll_top) {
                term->cursor_y--;
            } else {
                ssh_term_scroll_down(term, 1, -1);
            }
        }
        return;
    }

    if (ch == 0x1B) {
        term->in_esc = true;
        return;
    }
    if (ch == '\r') {
        term->cursor_x = 0;
        return;
    }
    if (ch == '\n' || ch == 0x0B || ch == 0x0C) {
        term->cursor_y++;
        if (term->cursor_y > term->scroll_bottom) {
            ssh_term_scroll_up(term, 1, -1);
            term->cursor_y = term->scroll_bottom;
        }
        return;
    }
    if (ch == 0x08 || ch == 0x7F) {
        if (term->cursor_x > 0) {
            term->cursor_x--;
        }
        return;
    }
    if (ch == '\t') {
        int next = ((term->cursor_x / 8) + 1) * 8;
        while (term->cursor_x < next && term->cursor_x < SSH_TERM_COLS) {
            ssh_term_put_char(term, ' ');
        }
        return;
    }
    if (ch < 0x20) {
        return;
    }
    if (ch >= 0x80) {
        return;
    }

    ssh_term_put_char(term, (char)ch);
}

static void ssh_term_process_bytes(ssh_term_t *term, const char *buf, int len)
{
    for (int i = 0; i < len; ++i) {
        ssh_term_process_byte(term, (uint8_t)buf[i]);
    }
    ssh_term_refresh_display_buffer(term);
    ssh_term_sync_cursor(term);
}

static char *xstrdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

static int ensure_network_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("ssh: network stack init failed: %s\n", esp_err_to_name(err));
        return -1;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("ssh: event loop init failed: %s\n", esp_err_to_name(err));
        return -1;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        printf("ssh: WiFi is not initialized. Run 'wifi connect' first.\n");
        return -1;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        printf("ssh: WiFi is not connected. Run 'wifi connect' first.\n");
        return -1;
    }

    return 0;
}

static int ensure_ssh_library(void)
{
    if (s_ssh_lib_ready) {
        return 0;
    }

    if (ssh_init() != SSH_OK) {
        printf("ssh: failed to initialize libssh\n");
        return -1;
    }

    s_ssh_lib_ready = true;
    return 0;
}

static void free_target(ssh_target_t *target)
{
    if (!target) {
        return;
    }
    free(target->host);
    free(target->user);
    free(target->password);
    memset(target, 0, sizeof(*target));
}

static void free_location(scp_location_t *loc)
{
    if (!loc) {
        return;
    }
    free(loc->user);
    free(loc->host);
    free(loc->path);
    memset(loc, 0, sizeof(*loc));
}

static void free_profile(ssh_profile_t *profile)
{
    if (!profile) {
        return;
    }
    free(profile->name);
    free(profile->host);
    free(profile->user);
    free(profile->password);
    memset(profile, 0, sizeof(*profile));
}

static int prompt_line(const char *prompt, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return -1;
    }

    if (prompt && *prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }

    if (!fgets(buf, size, stdin)) {
        return -1;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 0;
}

static const char *ssh_storage_root(void)
{
    // Keep SSH state on internal storage. The Cardputer SD path is useful for
    // user files, but known_hosts access during session setup was provoking
    // instability in the SDSPI/FATFS path on this port.
    return BREEZYBOX_MOUNT_POINT;
}

static int ensure_ssh_dir(char *known_hosts_path, size_t size)
{
    char dir[BREEZYBOX_MAX_PATH * 2];
    const char *root = ssh_storage_root();

    if (snprintf(dir, sizeof(dir), "%s/.ssh", root) >= (int)sizeof(dir)) {
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    if (snprintf(known_hosts_path, size, "%s/known_hosts", dir) >= (int)size) {
        return -1;
    }
    return 0;
}

static int ssh_dir_path(char *dir, size_t size)
{
    const char *root = ssh_storage_root();
    if (snprintf(dir, size, "%s/.ssh", root) >= (int)size) {
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ssh_profiles_path(char *path, size_t size)
{
    char dir[BREEZYBOX_MAX_PATH * 2];
    if (ssh_dir_path(dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (snprintf(path, size, "%s/hosts", dir) >= (int)size) {
        return -1;
    }
    return 0;
}

static int valid_profile_name(const char *name)
{
    if (!name || !*name) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) {
            return 0;
        }
    }
    return 1;
}

static int profile_parse_line(const char *line, ssh_profile_t *profile)
{
    char *copy = NULL;
    char *save = NULL;
    char *name = NULL;
    char *host = NULL;
    char *user = NULL;
    char *port_s = NULL;
    char *password = NULL;
    int rc = -1;

    if (!line || !profile) {
        return -1;
    }

    copy = xstrdup(line);
    if (!copy) {
        return -1;
    }

    size_t len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '\n' || copy[len - 1] == '\r')) {
        copy[--len] = '\0';
    }

    name = strtok_r(copy, "\t", &save);
    host = strtok_r(NULL, "\t", &save);
    user = strtok_r(NULL, "\t", &save);
    port_s = strtok_r(NULL, "\t", &save);
    password = strtok_r(NULL, "\t", &save);
    if (!name || !host || !port_s) {
        goto out;
    }

    memset(profile, 0, sizeof(*profile));
    profile->name = xstrdup(name);
    profile->host = xstrdup(host);
    profile->user = (user && *user) ? xstrdup(user) : NULL;
    profile->password = (password && *password) ? xstrdup(password) : NULL;
    profile->port = atoi(port_s);
    if (!profile->name || !profile->host || profile->port <= 0) {
        free_profile(profile);
        goto out;
    }

    rc = 0;
out:
    free(copy);
    return rc;
}

static int ssh_profile_lookup(const char *name, ssh_profile_t *profile)
{
    char path[BREEZYBOX_MAX_PATH * 2];
    char line[256];
    FILE *f = NULL;
    int rc = -1;

    if (!name || !*name || !profile) {
        return -1;
    }
    if (ssh_profiles_path(path, sizeof(path)) != 0) {
        return -1;
    }

    f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        ssh_profile_t entry;
        if (profile_parse_line(line, &entry) != 0) {
            continue;
        }
        if (strcmp(entry.name, name) == 0) {
            *profile = entry;
            rc = 0;
            break;
        }
        free_profile(&entry);
    }

    fclose(f);
    return rc;
}

static int ssh_profile_save(const ssh_profile_t *profile)
{
    char path[BREEZYBOX_MAX_PATH * 2];
    char tmp[BREEZYBOX_MAX_PATH * 2];
    char line[256];
    FILE *in = NULL;
    FILE *out = NULL;
    int rc = -1;
    int wrote = 0;

    if (!profile || !profile->name || !profile->host || profile->port <= 0) {
        return -1;
    }
    if (ssh_profiles_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }

    in = fopen(path, "r");
    out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return -1;
    }

    if (in) {
        while (fgets(line, sizeof(line), in)) {
            ssh_profile_t entry;
            if (profile_parse_line(line, &entry) != 0) {
                continue;
            }
            if (strcmp(entry.name, profile->name) == 0) {
                fprintf(out, "%s\t%s\t%s\t%d\t%s\n",
                        profile->name,
                        profile->host,
                        profile->user ? profile->user : "",
                        profile->port,
                        profile->password ? profile->password : "");
                wrote = 1;
            } else {
                fprintf(out, "%s\t%s\t%s\t%d\t%s\n",
                        entry.name,
                        entry.host,
                        entry.user ? entry.user : "",
                        entry.port,
                        entry.password ? entry.password : "");
            }
            free_profile(&entry);
        }
        fclose(in);
    }

    if (!wrote) {
        fprintf(out, "%s\t%s\t%s\t%d\t%s\n",
                profile->name,
                profile->host,
                profile->user ? profile->user : "",
                profile->port,
                profile->password ? profile->password : "");
    }

    if (fclose(out) != 0) {
        return -1;
    }
    out = NULL;

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    rc = 0;
    return rc;
}

static int ssh_profile_remove(const char *name)
{
    char path[BREEZYBOX_MAX_PATH * 2];
    char tmp[BREEZYBOX_MAX_PATH * 2];
    char line[256];
    FILE *in = NULL;
    FILE *out = NULL;
    int removed = 0;

    if (!name || !*name) {
        return -1;
    }
    if (ssh_profiles_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }

    in = fopen(path, "r");
    if (!in) {
        return -1;
    }
    out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return -1;
    }

    while (fgets(line, sizeof(line), in)) {
        ssh_profile_t entry;
        if (profile_parse_line(line, &entry) != 0) {
            continue;
        }
        if (strcmp(entry.name, name) == 0) {
            removed = 1;
        } else {
            fprintf(out, "%s\t%s\t%s\t%d\t%s\n",
                    entry.name,
                    entry.host,
                    entry.user ? entry.user : "",
                    entry.port,
                    entry.password ? entry.password : "");
        }
        free_profile(&entry);
    }

    fclose(in);
    if (fclose(out) != 0) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return removed ? 0 : -1;
}

static int split_user_host(const char *input, char **user_out, char **host_out)
{
    const char *at = strchr(input, '@');
    if (!at) {
        *host_out = xstrdup(input);
        return *host_out ? 0 : -1;
    }

    size_t user_len = (size_t)(at - input);
    char *user = malloc(user_len + 1);
    char *host = xstrdup(at + 1);
    if (!user || !host) {
        free(user);
        free(host);
        return -1;
    }
    memcpy(user, input, user_len);
    user[user_len] = '\0';
    *user_out = user;
    *host_out = host;
    return 0;
}

static int parse_ssh_target(int argc, char **argv, ssh_target_t *target, int *command_index)
{
    bool explicit_port = false;

    memset(target, 0, sizeof(*target));
    target->port = 22;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            target->port = atoi(argv[++i]);
            explicit_port = true;
            continue;
        }
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            free(target->user);
            target->user = xstrdup(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "-pw") == 0 && i + 1 < argc) {
            free(target->password);
            target->password = xstrdup(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            target->verbosity++;
            continue;
        }

        if (!target->host) {
            char *parsed_user = NULL;
            char *parsed_host = NULL;
            ssh_profile_t profile;
            if (split_user_host(argv[i], &parsed_user, &parsed_host) != 0) {
                return -1;
            }
            if (parsed_host && ssh_profile_lookup(parsed_host, &profile) == 0) {
                free(parsed_host);
                parsed_host = xstrdup(profile.host);
                if (!parsed_host) {
                    free(parsed_user);
                    free_profile(&profile);
                    return -1;
                }
                if (!target->user && !parsed_user && profile.user) {
                    parsed_user = xstrdup(profile.user);
                    if (!parsed_user) {
                        free(parsed_host);
                        free_profile(&profile);
                        return -1;
                    }
                }
                if (!explicit_port) {
                    target->port = profile.port;
                }
                if (!target->password && profile.password) {
                    target->password = xstrdup(profile.password);
                    if (!target->password) {
                        free(parsed_user);
                        free(parsed_host);
                        free_profile(&profile);
                        return -1;
                    }
                }
                free_profile(&profile);
            }
            if (!target->user && parsed_user) {
                target->user = parsed_user;
            } else {
                free(parsed_user);
            }
            target->host = parsed_host;
            *command_index = i + 1;
            return 0;
        }
    }

    *command_index = argc;
    return target->host ? 0 : -1;
}

static int verify_known_host(ssh_session session, const char *host_label)
{
    enum ssh_known_hosts_e state;
    unsigned char *hash = NULL;
    size_t hlen = 0;
    ssh_key srv_pubkey = NULL;
    int rc;

    rc = ssh_get_server_publickey(session, &srv_pubkey);
    if (rc < 0) {
        return -1;
    }
    rc = ssh_get_publickey_hash(srv_pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
    ssh_key_free(srv_pubkey);
    if (rc < 0) {
        return -1;
    }

    state = ssh_session_is_known_server(session);
    switch (state) {
        case SSH_KNOWN_HOSTS_OK:
            break;
        case SSH_KNOWN_HOSTS_NOT_FOUND:
        case SSH_SERVER_NOT_KNOWN: {
            char answer[16];
            printf("Unknown host key for %s:\n", host_label);
            ssh_print_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
            if (prompt_line("Trust this host key? (yes/no): ", answer, sizeof(answer)) != 0 ||
                strcasecmp(answer, "yes") != 0) {
                ssh_clean_pubkey_hash(&hash);
                return -1;
            }
            if (ssh_session_update_known_hosts(session) != SSH_OK) {
                printf("ssh: failed to save known host: %s\n", ssh_get_error(session));
                ssh_clean_pubkey_hash(&hash);
                return -1;
            }
            break;
        }
        case SSH_KNOWN_HOSTS_CHANGED:
            printf("ssh: host key changed for %s\n", host_label);
            ssh_clean_pubkey_hash(&hash);
            return -1;
        case SSH_KNOWN_HOSTS_OTHER:
            printf("ssh: a different key type is already known for %s\n", host_label);
            ssh_clean_pubkey_hash(&hash);
            return -1;
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            printf("ssh: known hosts error: %s\n", ssh_get_error(session));
            ssh_clean_pubkey_hash(&hash);
            return -1;
    }

    ssh_clean_pubkey_hash(&hash);
    return 0;
}

static int authenticate_password_or_keyboard(ssh_session session, const char *password)
{
    int rc = ssh_userauth_password(session, NULL, password);
    if (rc == SSH_AUTH_SUCCESS) {
        return SSH_AUTH_SUCCESS;
    }
    if (rc == SSH_AUTH_DENIED) {
        rc = ssh_userauth_kbdint(session, NULL, NULL);
        while (rc == SSH_AUTH_INFO) {
            int n = ssh_userauth_kbdint_getnprompts(session);
            for (int i = 0; i < n; ++i) {
                char echo = 0;
                const char *prompt = ssh_userauth_kbdint_getprompt(session, i, &echo);
                if (!prompt) {
                    continue;
                }
                const char *answer = password;
                char buf[128];
                if (echo) {
                    if (prompt_line(prompt, buf, sizeof(buf)) != 0) {
                        return SSH_AUTH_ERROR;
                    }
                    answer = buf;
                }
                if (ssh_userauth_kbdint_setanswer(session, i, answer) < 0) {
                    return SSH_AUTH_ERROR;
                }
            }
            rc = ssh_userauth_kbdint(session, NULL, NULL);
        }
    }
    return rc;
}

static ssh_session connect_ssh_session(const ssh_target_t *target)
{
    char known_hosts[BREEZYBOX_MAX_PATH * 2];
    char password_buf[128];
    const char *password = target->password;
    int port = target->port;

    if (ensure_ssh_dir(known_hosts, sizeof(known_hosts)) != 0) {
        printf("ssh: unable to prepare known_hosts path\n");
        return NULL;
    }

    ssh_session session = ssh_new();
    if (!session) {
        printf("ssh: failed to allocate session\n");
        return NULL;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, target->host);
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &target->verbosity);
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, known_hosts);
    if (target->user && *target->user) {
        ssh_options_set(session, SSH_OPTIONS_USER, target->user);
    }

    if (ssh_connect(session) != SSH_OK) {
        printf("ssh: connect failed: %s\n", ssh_get_error(session));
        ssh_free(session);
        return NULL;
    }

    if (verify_known_host(session, target->host) != 0) {
        ssh_disconnect(session);
        ssh_free(session);
        return NULL;
    }

    int rc = ssh_userauth_publickey_auto(session, NULL, NULL);
    if (rc == SSH_AUTH_SUCCESS) {
        return session;
    }

    if (!password || !*password) {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "Password for %s%s%s: ",
                 target->user ? target->user : "",
                 target->user ? "@" : "",
                 target->host);
        if (prompt_line(prompt, password_buf, sizeof(password_buf)) != 0) {
            printf("ssh: authentication cancelled\n");
            ssh_disconnect(session);
            ssh_free(session);
            return NULL;
        }
        password = password_buf;
    }

    rc = authenticate_password_or_keyboard(session, password);
    if (rc != SSH_AUTH_SUCCESS) {
        printf("ssh: authentication failed: %s\n", ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return NULL;
    }

    return session;
}

static int run_exec_command(ssh_session session, int argc, char **argv, int command_index)
{
    ssh_channel channel = ssh_channel_new(session);
    char command[256];
    size_t used = 0;
    int rc = 1;

    if (!channel) {
        printf("ssh: failed to allocate channel\n");
        return 1;
    }
    if (ssh_channel_open_session(channel) != SSH_OK) {
        printf("ssh: failed to open channel: %s\n", ssh_get_error(session));
        goto out;
    }

    command[0] = '\0';
    for (int i = command_index; i < argc; ++i) {
        int n = snprintf(command + used, sizeof(command) - used, "%s%s",
                         (i == command_index) ? "" : " ", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(command) - used) {
            printf("ssh: command too long\n");
            goto out;
        }
        used += (size_t)n;
    }

    if (ssh_channel_request_exec(channel, command) != SSH_OK) {
        printf("ssh: remote exec failed: %s\n", ssh_get_error(session));
        goto out;
    }

    char buf[SSH_BUF_SIZE];
    int nread;
    while ((nread = ssh_channel_read(channel, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, (size_t)nread, stdout);
    }
    while ((nread = ssh_channel_read(channel, buf, sizeof(buf), 1)) > 0) {
        fwrite(buf, 1, (size_t)nread, stderr);
    }
    fflush(stdout);
    fflush(stderr);
    rc = ssh_channel_get_exit_status(channel);

out:
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return rc;
}

static int run_interactive_shell(ssh_session session)
{
    ssh_channel channel = ssh_channel_new(session);
    int rows = SSH_TERM_ROWS;
    int cols = SSH_TERM_COLS;
    int rc = 0;
    int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    const uint32_t old_poll_interval_ms = cardputer_keyboard_get_poll_interval_ms();
    QueueHandle_t old_ssh_queue = s_ssh_input_queue;
    ssh_term_t term;

    if (!channel) {
        printf("ssh: failed to allocate channel\n");
        return 1;
    }
    if (ssh_channel_open_session(channel) != SSH_OK) {
        printf("ssh: failed to open channel: %s\n", ssh_get_error(session));
        ssh_channel_free(channel);
        return 1;
    }

    if (ssh_channel_request_pty_size(channel, "xterm", cols, rows) != SSH_OK) {
        printf("ssh: warning: failed to request PTY\n");
    }
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        printf("ssh: failed to start shell: %s\n", ssh_get_error(session));
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return 1;
    }

    ssh_term_init(&term);
    rgb_display_set_buffer(term.main_cells);
    ssh_term_sync_cursor(&term);
    s_ssh_input_queue = xQueueCreate(64, sizeof(char));
    cardputer_keyboard_set_background_poll_enabled(0);
    cardputer_keyboard_set_char_callback(NULL);
    cardputer_keyboard_set_key_callback(ssh_keyboard_handle_key);
    my_console_set_cursor_sync_enabled(0);
    cardputer_keyboard_set_poll_interval_ms(12);

    if (old_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
    }
    ssh_set_blocking(session, 0);
    printf("[ssh] Connected. Ctrl+Q disconnects local session.\n");

    while (!ssh_channel_is_eof(channel)) {
        char buf[SSH_BUF_SIZE];
        int n;
        int reads = 0;

        cardputer_keyboard_poll_direct();

        while (reads < SSH_MAX_READS_PER_TICK &&
               (n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0)) > 0) {
            ssh_term_process_bytes(&term, buf, n);
            reads++;
        }

        reads = 0;
        while (reads < SSH_MAX_READS_PER_TICK &&
               (n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 1)) > 0) {
            ssh_term_process_bytes(&term, buf, n);
            reads++;
        }

        for (;;) {
            char c;
            if (s_ssh_input_queue && xQueueReceive(s_ssh_input_queue, &c, 0) == pdTRUE) {
                if (c == '\n') {
                    c = '\r';
                }
                if ((unsigned char)c == 0x11) {
                    rc = 0;
                    goto out;
                }
                if (ssh_channel_write(channel, &c, 1) == SSH_ERROR) {
                    printf("\nssh: write failed: %s\n", ssh_get_error(session));
                    rc = 1;
                    goto out;
                }
                continue;
            }

            ssize_t got = read(STDIN_FILENO, &c, 1);
            if (got <= 0) {
                break;
            }
            if ((unsigned char)c == 0x11) {
                rc = 0;
                goto out;
            }
            if (ssh_channel_write(channel, &c, 1) == SSH_ERROR) {
                printf("\nssh: write failed: %s\n", ssh_get_error(session));
                rc = 1;
                goto out;
            }
        }

        if (ssh_channel_is_closed(channel) || ssh_is_connected(session) == 0) {
            break;
        }
        ssh_delay_ms(1);
    }

out:
    if (old_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, old_flags);
    }

    cardputer_keyboard_set_key_callback(NULL);
    cardputer_keyboard_set_char_callback(my_console_bt_receive);
    cardputer_keyboard_set_background_poll_enabled(1);
    if (s_ssh_input_queue) {
        vQueueDelete(s_ssh_input_queue);
    }
    s_ssh_input_queue = old_ssh_queue;
    rgb_display_set_buffer((lcd_cell_t *)vterm_get_direct_buffer());
    rgb_display_set_cursor(-1, -1);
    my_console_set_cursor_sync_enabled(1);
    cardputer_keyboard_set_poll_interval_ms(old_poll_interval_ms);

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return rc;
}

static void ssh_interactive_task(void *arg)
{
    ssh_interactive_ctx_t *ctx = (ssh_interactive_ctx_t *)arg;
    int rc = 1;

    if (ctx && ctx->session) {
        if (s_ssh_app_mode_runner) {
            rc = s_ssh_app_mode_runner(ctx->session);
        } else {
            printf("ssh: interactive app mode unavailable\n");
            rc = 1;
        }
    }

    if (ctx) {
        ctx->rc = rc;
        if (ctx->waiter) {
            xTaskNotifyGive(ctx->waiter);
        }
    }

    vTaskDelete(NULL);
}

static int run_interactive_shell_tasked(ssh_session session)
{
    ssh_interactive_ctx_t *ctx = calloc(1, sizeof(*ctx));
    TaskHandle_t task = NULL;
    int rc = 1;
    static const uint32_t stack_sizes[] = { 24576, 20480, 16384 };

    if (!ctx) {
        printf("ssh: out of memory\n");
        return 1;
    }

    ctx->session = session;
    ctx->waiter = xTaskGetCurrentTaskHandle();
    ctx->rc = 1;

    BaseType_t ok = pdFAIL;
    for (size_t i = 0; i < sizeof(stack_sizes) / sizeof(stack_sizes[0]); ++i) {
        ok = xTaskCreatePinnedToCore(
            ssh_interactive_task,
            "ssh_term",
            stack_sizes[i],
            ctx,
            5,
            &task,
            1);
        if (ok == pdPASS && task) {
            break;
        }
        task = NULL;
    }

    if (ok != pdPASS || !task) {
        printf("ssh: failed to start interactive session task "
               "(heap=%u largest=%u)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(ctx);
        return 1;
    }

    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    rc = ctx->rc;
    free(ctx);
    return rc;
}

static int parse_remote_location(const char *spec, scp_location_t *loc)
{
    memset(loc, 0, sizeof(*loc));
    const char *colon = strchr(spec, ':');
    if (!colon || spec[0] == '/') {
        loc->is_remote = false;
        loc->path = xstrdup(spec);
        return loc->path ? 0 : -1;
    }

    loc->is_remote = true;
    size_t hostpart_len = (size_t)(colon - spec);
    char *hostpart = malloc(hostpart_len + 1);
    if (!hostpart) {
        return -1;
    }
    memcpy(hostpart, spec, hostpart_len);
    hostpart[hostpart_len] = '\0';

    if (split_user_host(hostpart, &loc->user, &loc->host) != 0) {
        free(hostpart);
        return -1;
    }
    free(hostpart);
    loc->path = xstrdup(colon + 1);
    if (!loc->path) {
        free_location(loc);
        return -1;
    }
    return 0;
}

static int resolve_local_path(const char *arg, char *buf, size_t size)
{
    if (!arg || !*arg) {
        return -1;
    }
    if (arg[0] == '/') {
        return snprintf(buf, size, "%s", arg) < (int)size ? 0 : -1;
    }
    return breezybox_resolve_path(arg, buf, size) ? 0 : -1;
}

static const char *path_basename_const(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int scp_upload(ssh_session session, const char *local_arg, const scp_location_t *remote)
{
    char local_path[BREEZYBOX_MAX_PATH * 2];
    struct stat st;
    FILE *f = NULL;
    ssh_scp scp = NULL;
    int rc = 1;

    if (resolve_local_path(local_arg, local_path, sizeof(local_path)) != 0) {
        printf("scp: local path too long: %s\n", local_arg);
        return 1;
    }
    if (stat(local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("scp: local file not found: %s\n", local_arg);
        return 1;
    }

    const char *filename = path_basename_const(local_path);
    char remote_dir[BREEZYBOX_MAX_PATH * 2];
    const char *remote_name = NULL;
    const char *slash = strrchr(remote->path, '/');
    if (!remote->path[0]) {
        printf("scp: remote path required\n");
        return 1;
    }
    if (remote->path[strlen(remote->path) - 1] == '/') {
        snprintf(remote_dir, sizeof(remote_dir), "%s", remote->path);
        remote_name = filename;
    } else if (slash) {
        size_t dir_len = (size_t)(slash - remote->path);
        if (dir_len == 0) {
            snprintf(remote_dir, sizeof(remote_dir), "/");
        } else {
            if (dir_len >= sizeof(remote_dir)) {
                printf("scp: remote path too long\n");
                return 1;
            }
            memcpy(remote_dir, remote->path, dir_len);
            remote_dir[dir_len] = '\0';
        }
        remote_name = slash + 1;
    } else {
        snprintf(remote_dir, sizeof(remote_dir), ".");
        remote_name = remote->path;
    }

    f = fopen(local_path, "rb");
    if (!f) {
        printf("scp: failed to open %s\n", local_arg);
        return 1;
    }

    scp = ssh_scp_new(session, SSH_SCP_WRITE, remote_dir);
    if (!scp || ssh_scp_init(scp) != SSH_OK) {
        printf("scp: failed to init remote copy: %s\n", ssh_get_error(session));
        goto out;
    }
    if (ssh_scp_push_file(scp, remote_name, (size_t)st.st_size, 0644) != SSH_OK) {
        printf("scp: failed to open remote file: %s\n", ssh_get_error(session));
        goto out;
    }

    char buf[SSH_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (ssh_scp_write(scp, buf, n) != SSH_OK) {
            printf("scp: write failed: %s\n", ssh_get_error(session));
            goto out;
        }
    }
    if (ferror(f)) {
        printf("scp: local read failed\n");
        goto out;
    }

    rc = 0;
out:
    if (scp) {
        ssh_scp_close(scp);
        ssh_scp_free(scp);
    }
    if (f) {
        fclose(f);
    }
    return rc;
}

static int scp_download(ssh_session session, const scp_location_t *remote, const char *local_arg)
{
    char local_path[BREEZYBOX_MAX_PATH * 2];
    struct stat st;
    FILE *f = NULL;
    ssh_scp scp = NULL;
    int rc = 1;

    if (resolve_local_path(local_arg, local_path, sizeof(local_path)) != 0) {
        printf("scp: local path too long: %s\n", local_arg);
        return 1;
    }

    if (stat(local_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *filename = path_basename_const(remote->path);
        size_t used = strlen(local_path);
        if (snprintf(local_path + used, sizeof(local_path) - used, "%s%s",
                     (used > 0 && local_path[used - 1] == '/') ? "" : "/", filename) >= (int)(sizeof(local_path) - used)) {
            printf("scp: local destination too long\n");
            return 1;
        }
    }

    scp = ssh_scp_new(session, SSH_SCP_READ, remote->path);
    if (!scp || ssh_scp_init(scp) != SSH_OK) {
        printf("scp: failed to init remote copy: %s\n", ssh_get_error(session));
        goto out;
    }

    int req;
    do {
        req = ssh_scp_pull_request(scp);
        if (req == SSH_SCP_REQUEST_NEWDIR) {
            ssh_scp_deny_request(scp, "directories not supported");
            continue;
        }
        if (req == SSH_ERROR) {
            printf("scp: read request failed: %s\n", ssh_get_error(session));
            goto out;
        }
    } while (req != SSH_SCP_REQUEST_NEWFILE);

    if (ssh_scp_accept_request(scp) != SSH_OK) {
        printf("scp: failed to accept remote file: %s\n", ssh_get_error(session));
        goto out;
    }

    f = fopen(local_path, "wb");
    if (!f) {
        printf("scp: failed to open local destination: %s\n", local_arg);
        goto out;
    }

    char buf[SSH_BUF_SIZE];
    int n;
    while ((n = ssh_scp_read(scp, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            printf("scp: local write failed\n");
            goto out;
        }
    }
    if (n == SSH_ERROR) {
        printf("scp: remote read failed: %s\n", ssh_get_error(session));
        goto out;
    }

    rc = 0;
out:
    if (scp) {
        ssh_scp_close(scp);
        ssh_scp_free(scp);
    }
    if (f) {
        fclose(f);
    }
    return rc;
}

int cmd_sshcfg(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sshcfg <add|list|show|rm> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        char path[BREEZYBOX_MAX_PATH * 2];
        char line[256];
        FILE *f = NULL;
        int found = 0;

        if (ssh_profiles_path(path, sizeof(path)) != 0) {
            printf("sshcfg: unable to access profile storage\n");
            return 1;
        }

        f = fopen(path, "r");
        if (!f) {
            printf("No saved SSH hosts.\n");
            return 0;
        }

        while (fgets(line, sizeof(line), f)) {
            ssh_profile_t entry;
            if (profile_parse_line(line, &entry) != 0) {
                continue;
            }
            printf("%s -> %s%s%s:%d\n",
                   entry.name,
                   entry.user ? entry.user : "",
                   entry.user ? "@" : "",
                   entry.host,
                   entry.port);
            found = 1;
            free_profile(&entry);
        }
        fclose(f);

        if (!found) {
            printf("No saved SSH hosts.\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "show") == 0) {
        ssh_profile_t profile;
        if (argc != 3) {
            printf("Usage: sshcfg show <name>\n");
            return 1;
        }
        if (ssh_profile_lookup(argv[2], &profile) != 0) {
            printf("sshcfg: no such host: %s\n", argv[2]);
            return 1;
        }
        printf("name: %s\nhost: %s\nport: %d\n",
               profile.name, profile.host, profile.port);
        if (profile.user && *profile.user) {
            printf("user: %s\n", profile.user);
        }
        printf("password: %s\n", (profile.password && *profile.password) ? "(saved)" : "(none)");
        free_profile(&profile);
        return 0;
    }

    if (strcmp(argv[1], "rm") == 0) {
        if (argc != 3) {
            printf("Usage: sshcfg rm <name>\n");
            return 1;
        }
        if (ssh_profile_remove(argv[2]) != 0) {
            printf("sshcfg: no such host: %s\n", argv[2]);
            return 1;
        }
        printf("Removed SSH host: %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "add") == 0) {
        ssh_profile_t profile = {0};
        char *parsed_user = NULL;
        char *parsed_host = NULL;
        int port = 22;

        if (argc < 4) {
            printf("Usage: sshcfg add <name> <host|user@host> [-l user] [-p port] [-pw password]\n");
            return 1;
        }
        if (!valid_profile_name(argv[2])) {
            printf("sshcfg: invalid name: %s\n", argv[2]);
            return 1;
        }

        profile.name = xstrdup(argv[2]);
        if (!profile.name || split_user_host(argv[3], &parsed_user, &parsed_host) != 0) {
            free_profile(&profile);
            free(parsed_user);
            free(parsed_host);
            printf("sshcfg: out of memory\n");
            return 1;
        }
        profile.host = parsed_host;
        profile.user = parsed_user;

        for (int i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
                free(profile.user);
                profile.user = xstrdup(argv[++i]);
                if (!profile.user) {
                    free_profile(&profile);
                    printf("sshcfg: out of memory\n");
                    return 1;
                }
            } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                port = atoi(argv[++i]);
            } else if (strcmp(argv[i], "-pw") == 0 && i + 1 < argc) {
                free(profile.password);
                profile.password = xstrdup(argv[++i]);
                if (!profile.password) {
                    free_profile(&profile);
                    printf("sshcfg: out of memory\n");
                    return 1;
                }
            } else {
                free_profile(&profile);
                printf("Usage: sshcfg add <name> <host|user@host> [-l user] [-p port] [-pw password]\n");
                return 1;
            }
        }

        if (port <= 0) {
            free_profile(&profile);
            printf("sshcfg: invalid port\n");
            return 1;
        }
        profile.port = port;

        if (ssh_profile_save(&profile) != 0) {
            free_profile(&profile);
            printf("sshcfg: failed to save host\n");
            return 1;
        }

        printf("Saved SSH host: %s -> %s%s%s:%d\n",
               profile.name,
               profile.user ? profile.user : "",
               profile.user ? "@" : "",
               profile.host,
               profile.port);
        free_profile(&profile);
        return 0;
    }

    printf("Usage: sshcfg <add|list|show|rm> ...\n");
    return 1;
}

int cmd_ssh(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ssh [-p port] [-l user] [-pw password] <host|alias> [command...]\n");
        return 1;
    }

    ssh_target_t target;
    int command_index = argc;
    if (parse_ssh_target(argc, argv, &target, &command_index) != 0 || !target.host) {
        printf("Usage: ssh [-p port] [-l user] [-pw password] <host|alias> [command...]\n");
        free_target(&target);
        return 1;
    }

    if (ensure_ssh_library() != 0) {
        free_target(&target);
        return 1;
    }

    if (ensure_network_stack() != 0) {
        free_target(&target);
        return 1;
    }

    ssh_session session = connect_ssh_session(&target);
    int rc = 1;
    if (session) {
        rc = (command_index < argc)
            ? run_exec_command(session, argc, argv, command_index)
            : run_interactive_shell_tasked(session);
        ssh_disconnect(session);
        ssh_free(session);
    }
    free_target(&target);
    return rc;
}

int cmd_scp(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: scp [-P port] [-l user] [-pw password] <src> <dst>\n");
        return 1;
    }

    ssh_target_t target = { .port = 22 };
    const char *src_arg = NULL;
    const char *dst_arg = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            target.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            free(target.user);
            target.user = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "-pw") == 0 && i + 1 < argc) {
            free(target.password);
            target.password = xstrdup(argv[++i]);
        } else if (!src_arg) {
            src_arg = argv[i];
        } else if (!dst_arg) {
            dst_arg = argv[i];
        } else {
            printf("scp: too many arguments\n");
            free_target(&target);
            return 1;
        }
    }

    if (!src_arg || !dst_arg) {
        printf("Usage: scp [-P port] [-l user] [-pw password] <src> <dst>\n");
        free_target(&target);
        return 1;
    }

    scp_location_t src;
    scp_location_t dst;
    if (parse_remote_location(src_arg, &src) != 0 || parse_remote_location(dst_arg, &dst) != 0) {
        printf("scp: failed to parse source or destination\n");
        free_target(&target);
        free_location(&src);
        free_location(&dst);
        return 1;
    }

    if (src.is_remote == dst.is_remote) {
        printf("scp: exactly one endpoint must be remote\n");
        free_target(&target);
        free_location(&src);
        free_location(&dst);
        return 1;
    }

    scp_location_t *remote = src.is_remote ? &src : &dst;
    if (!target.host) {
        target.host = xstrdup(remote->host);
    }
    if (!target.user && remote->user) {
        target.user = xstrdup(remote->user);
    }
    if (!target.host) {
        printf("scp: missing remote host\n");
        free_target(&target);
        free_location(&src);
        free_location(&dst);
        return 1;
    }

    if (ensure_ssh_library() != 0) {
        free_target(&target);
        free_location(&src);
        free_location(&dst);
        return 1;
    }

    if (ensure_network_stack() != 0) {
        free_target(&target);
        free_location(&src);
        free_location(&dst);
        return 1;
    }

    ssh_session session = connect_ssh_session(&target);
    int rc = 1;
    if (session) {
        rc = src.is_remote
            ? scp_download(session, &src, dst_arg)
            : scp_upload(session, src_arg, &dst);
        ssh_disconnect(session);
        ssh_free(session);
    }
    free_target(&target);
    free_location(&src);
    free_location(&dst);
    return rc;
}

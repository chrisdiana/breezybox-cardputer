#include "cardputer_keyboard.h"
#include "my_console_io.h"
#include "rgb_display.h"
#include "vterm.h"

#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
} ssh_term_t;

static QueueHandle_t s_ssh_input_queue = NULL;

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
                } else if (!set && term->alt_screen) {
                    term->alt_screen = false;
                    term->cursor_x = term->saved_x;
                    term->cursor_y = term->saved_y;
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
    if (ch < 0x20 || ch >= 0x80) {
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

int ssh_app_mode_run(ssh_session session)
{
    ssh_channel channel = ssh_channel_new(session);
    int rows = SSH_TERM_ROWS;
    int cols = SSH_TERM_COLS;
    int rc = 0;
    const uint32_t old_poll_interval_ms = cardputer_keyboard_get_poll_interval_ms();
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

    ssh_set_blocking(session, 0);

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
            if (!s_ssh_input_queue || xQueueReceive(s_ssh_input_queue, &c, 0) != pdTRUE) {
                break;
            }
            if ((unsigned char)c == 0x11) {
                rc = 0;
                goto out;
            }
            if (ssh_channel_write(channel, &c, 1) == SSH_ERROR) {
                rc = 1;
                goto out;
            }
        }

        if (ssh_channel_is_closed(channel) || ssh_is_connected(session) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }

out:
    cardputer_keyboard_set_key_callback(NULL);
    cardputer_keyboard_set_char_callback(my_console_bt_receive);
    cardputer_keyboard_set_background_poll_enabled(1);
    if (s_ssh_input_queue) {
        vQueueDelete(s_ssh_input_queue);
    }
    s_ssh_input_queue = NULL;
    rgb_display_set_buffer((lcd_cell_t *)vterm_get_direct_buffer());
    rgb_display_set_cursor(-1, -1);
    my_console_set_cursor_sync_enabled(1);
    cardputer_keyboard_set_poll_interval_ms(old_poll_interval_ms);

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return rc;
}

/*
* vterm.c - Virtual Terminal Manager
*
* Memory Architecture:
* - s_iram_buffer: 9.5KB render buffer in fast Internal RAM. The display reads this.
* - cells: Canonical backing store for each VT's visible text screen.
* - storage_cells: Per-VT backing store used in multi-VT mode.
*
* The active VT is rendered into s_iram_buffer on demand. This keeps scrollback
* composition separate from the terminal's real screen state.
*/

#include "vterm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define INPUT_QUEUE_SIZE    64
#define BUFFER_SIZE_BYTES   (VTERM_ROWS * VTERM_COLS * sizeof(vterm_cell_t))
#define HISTORY_SIZE_BYTES  (VTERM_SCROLLBACK_LINES * VTERM_COLS * sizeof(vterm_cell_t))
#define WRITE_YIELD_CHUNK   256

// The single "Hot" buffer used by the display and the active writer
// Must be 32-bit aligned for the optimized renderer
static vterm_cell_t *s_iram_buffer = NULL;

typedef struct {
    // Current visible text screen backing store.
    vterm_cell_t *cells;

    // Backing store in PSRAM (holds state when VT is not active)
    vterm_cell_t *storage_cells;
    vterm_cell_t *history_cells;
    int history_head;
    int history_count;
    bool history_enabled;
    bool sgr_enabled;

    int cursor_x;
    int cursor_y;
    int cursor_visible;    // 1 = show, 0 = hidden (DECTCEM)
    uint8_t current_attr;  // 4-bit fg + 4-bit bg

    QueueHandle_t input_queue;
    SemaphoreHandle_t mutex;

    // Escape parsing
    int escape_state;
    char escape_buf[32];
    int escape_len;

} vterm_t;

static vterm_t *s_vterms = NULL;
volatile int s_active_vt = 0;
static void (*s_on_switch_cb)(int new_vt) = NULL;
static int s_scrollback_offset = 0;

// Forward declarations
static void vterm_clear_internal(vterm_t *vt);
static void vterm_refresh_view(vterm_t *vt);
void vterm_send_input(int vt_id, char c);
void vterm_scrollback_bottom(void);

// ============ Internal Functions ============

// Scroll the entire screen up by 1 line
static void vterm_history_push(vterm_t *vt, const vterm_cell_t *line)
{
    if (!vt->history_enabled || !vt->history_cells) {
        return;
    }

    memcpy(&vt->history_cells[vt->history_head * VTERM_COLS], line,
           VTERM_COLS * sizeof(vterm_cell_t));
    vt->history_head = (vt->history_head + 1) % VTERM_SCROLLBACK_LINES;
    if (vt->history_count < VTERM_SCROLLBACK_LINES) {
        vt->history_count++;
    }
}

static const vterm_cell_t *vterm_history_get(const vterm_t *vt, int index)
{
    if (!vt->history_cells || index < 0 || index >= vt->history_count) {
        return NULL;
    }

    int oldest = (vt->history_head - vt->history_count + VTERM_SCROLLBACK_LINES) % VTERM_SCROLLBACK_LINES;
    int slot = (oldest + index) % VTERM_SCROLLBACK_LINES;
    return &vt->history_cells[slot * VTERM_COLS];
}

static void vterm_scroll(vterm_t *vt)
{
    vterm_history_push(vt, &vt->cells[0]);

    // Move lines 1..N-1 to 0..N-2
    // Calculate size of (ROWS - 1) lines
    size_t block_size = (VTERM_ROWS - 1) * VTERM_COLS * sizeof(vterm_cell_t);
    
    // memmove is safe for overlapping regions
    memmove(&vt->cells[0], &vt->cells[VTERM_COLS], block_size);
    
    // Clear last line
    vterm_cell_t *last_line = &vt->cells[(VTERM_ROWS - 1) * VTERM_COLS];
    for (int x = 0; x < VTERM_COLS; x++) {
        last_line[x].ch = ' ';
        last_line[x].attr = VTERM_DEFAULT_ATTR;
    }
    vt->cursor_y = VTERM_ROWS - 1;
    vterm_refresh_view(vt);
}

static void vterm_putchar_internal(vterm_t *vt, char c)
{
    // Direct pointer access for speed
    vterm_cell_t *cell = &vt->cells[vt->cursor_y * VTERM_COLS + vt->cursor_x];

    switch (c) {
    case '\n':
        vt->cursor_x = 0;
        vt->cursor_y++;
        if (vt->cursor_y >= VTERM_ROWS) vterm_scroll(vt);
        break;
    case '\r':
        vt->cursor_x = 0;
        break;
    case '\b':
        if (vt->cursor_x > 0) {
            vt->cursor_x--;
            // Backspace erases visually
            cell--; // Move pointer back
            cell->ch = ' ';
            cell->attr = vt->current_attr;
        }
        break;
    case '\t':
        do {
            cell->ch = ' ';
            cell->attr = vt->current_attr;
            cell++;
            vt->cursor_x++;
        } while (vt->cursor_x < VTERM_COLS && (vt->cursor_x % 8) != 0);
        if (vt->cursor_x >= VTERM_COLS) {
            vt->cursor_x = 0;
            vt->cursor_y++;
            if (vt->cursor_y >= VTERM_ROWS) vterm_scroll(vt);
        }
        break;
    default:
        if (c >= 32 && c < 127) {
            cell->ch = c;
            cell->attr = vt->current_attr;
            vt->cursor_x++;
            if (vt->cursor_x >= VTERM_COLS) {
                vt->cursor_x = 0;
                vt->cursor_y++;
                if (vt->cursor_y >= VTERM_ROWS) vterm_scroll(vt);
            }
        }
        break;
    }
}

static void vterm_clear_internal(vterm_t *vt)
{
    vterm_cell_t *p = vt->cells;
    vterm_cell_t *end = p + (VTERM_ROWS * VTERM_COLS);

    // Fill optimization: Construct a 32-bit pattern of two cells
    uint16_t fill = (VTERM_DEFAULT_ATTR << 8) | ' ';
    uint32_t fill32 = (fill << 16) | fill;

    // Align to 32-bit
    while ((uintptr_t)p & 3 && p < end) {
        p->ch = ' '; p->attr = VTERM_DEFAULT_ATTR; p++;
    }
    uint32_t *p32 = (uint32_t *)p;
    while (p32 < (uint32_t *)end) {
        *p32++ = fill32;
    }
    // Handle remaining
    p = (vterm_cell_t *)p32;
    while (p < end) {
        p->ch = ' '; p->attr = VTERM_DEFAULT_ATTR; p++;
    }

    vt->cursor_x = 0;
    vt->cursor_y = 0;
    vt->cursor_visible = 1;  // Cursor visible by default
    vt->current_attr = VTERM_DEFAULT_ATTR;
    vterm_refresh_view(vt);
}

static void vterm_refresh_view(vterm_t *vt)
{
    if (!vt || !s_iram_buffer) {
        return;
    }

    int total_lines = vt->history_count + VTERM_ROWS;
    int max_offset = total_lines - VTERM_ROWS;
    if (max_offset < 0) {
        max_offset = 0;
    }
    if (s_scrollback_offset > max_offset) {
        s_scrollback_offset = max_offset;
    }
    if (s_scrollback_offset < 0) {
        s_scrollback_offset = 0;
    }

    int start_line = total_lines - VTERM_ROWS - s_scrollback_offset;
    if (start_line < 0) {
        start_line = 0;
    }

    for (int row = 0; row < VTERM_ROWS; ++row) {
        int global_line = start_line + row;
        vterm_cell_t *dst = &s_iram_buffer[row * VTERM_COLS];

        if (global_line < vt->history_count) {
            const vterm_cell_t *src = vterm_history_get(vt, global_line);
            if (src) {
                memcpy(dst, src, VTERM_COLS * sizeof(vterm_cell_t));
            }
        } else {
            const int screen_row = global_line - vt->history_count;
            if (screen_row >= 0 && screen_row < VTERM_ROWS) {
                memcpy(dst, &vt->cells[screen_row * VTERM_COLS],
                       VTERM_COLS * sizeof(vterm_cell_t));
            } else {
                for (int col = 0; col < VTERM_COLS; ++col) {
                    dst[col].ch = ' ';
                    dst[col].attr = VTERM_DEFAULT_ATTR;
                }
            }
        }
    }
}

// Helper to parse a number from SGR params, advancing pointer
static int sgr_parse_num(const char **pp)
{
    int num = 0;
    while (**pp >= '0' && **pp <= '9') {
        num = num * 10 + (**pp - '0');
        (*pp)++;
    }
    if (**pp == ';') (*pp)++;
    return num;
}

static void vterm_apply_sgr(vterm_t *vt, const char *params)
{
    const char *p = params;
    int bright = 0;

    // Handle empty or "0" reset
    if (*p == '\0' || (*p == '0' && (p[1] == '\0' || p[1] == ';'))) {
        vt->current_attr = VTERM_DEFAULT_ATTR;
        if (*p == '0') p++;
        if (*p == ';') p++;
    }

    while (*p) {
        int num = sgr_parse_num(&p);

        // --- OPTIMIZATION: Check most common codes (Colors) first ---
        
        if (num >= 90 && num <= 97) {
            // Bright foreground colors (8-15)
            uint8_t fg = (num - 90) | VTERM_BRIGHT;
            uint8_t bg = VTERM_ATTR_BG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(fg, bg);
        } 
        else if (num >= 30 && num <= 37) {
            // Standard foreground colors (0-7)
            uint8_t fg = (num - 30) | bright;
            uint8_t bg = VTERM_ATTR_BG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(fg, bg);
        }
        else if (num == 0) {
            vt->current_attr = VTERM_DEFAULT_ATTR;
            bright = 0;
        } 
        // --- End Optimization ---
        
        else if (num == 1) {
            bright = VTERM_BRIGHT;
            uint8_t fg = VTERM_ATTR_FG(vt->current_attr);
            uint8_t bg = VTERM_ATTR_BG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(fg | VTERM_BRIGHT, bg);
        } else if (num == 22) {
            bright = 0;
            uint8_t fg = VTERM_ATTR_FG(vt->current_attr) & 0x07;
            uint8_t bg = VTERM_ATTR_BG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(fg, bg);
        } else if (num == 38) {
            // Extended foreground: 38;5;N (256-color) or 38;2;R;G;B (truecolor)
            // Gracefully skip - we only support 16-color mode
            int mode = sgr_parse_num(&p);
            if (mode == 5) {
                sgr_parse_num(&p);  // Skip color index
            } else if (mode == 2) {
                sgr_parse_num(&p); sgr_parse_num(&p); sgr_parse_num(&p);  // Skip R, G, B
            }
        } else if (num == 39) {
            // Default foreground
            uint8_t bg = VTERM_ATTR_BG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(VTERM_WHITE, bg);
        } else if (num >= 40 && num <= 47) {
            // Standard background colors (0-7)
            uint8_t fg = VTERM_ATTR_FG(vt->current_attr);
            uint8_t bg = num - 40;
            vt->current_attr = VTERM_ATTR(fg, bg);
        } else if (num == 48) {
            // Extended background: 48;5;N (256-color) or 48;2;R;G;B (truecolor)
            // Gracefully skip - we only support 16-color mode
            int mode = sgr_parse_num(&p);
            if (mode == 5) {
                sgr_parse_num(&p);  // Skip color index
            } else if (mode == 2) {
                sgr_parse_num(&p); sgr_parse_num(&p); sgr_parse_num(&p);  // Skip R, G, B
            }
        } else if (num == 49) {
            // Default background
            uint8_t fg = VTERM_ATTR_FG(vt->current_attr);
            vt->current_attr = VTERM_ATTR(fg, VTERM_BLACK);
        } else if (num >= 100 && num <= 107) {
            // Bright background colors (8-15)
            uint8_t fg = VTERM_ATTR_FG(vt->current_attr);
            uint8_t bg = (num - 100) | VTERM_BRIGHT;
            vt->current_attr = VTERM_ATTR(fg, bg);
        }
    }
}

static int vterm_handle_escape(vterm_t *vt, char c)
{
    if (vt->escape_state == 0) {
        if (c == '\033') {
            vt->escape_state = 1;
            vt->escape_len = 0;
            return 1;
        }
        return 0;
    }

    if (vt->escape_state == 1) {
        if (c == '[') {
            vt->escape_state = 2;
            return 1;
        }
        // Non-CSI escape sequences: ESC <letter>
        if (c == 'D') {
            // IND - Index: move cursor down, scroll if at bottom
            if (vt->cursor_y >= VTERM_ROWS - 1) {
                vterm_scroll(vt);
            } else {
                vt->cursor_y++;
            }

            vt->escape_state = 0;
            return 1;
        }
        if (c == 'M') {
            // RI - Reverse Index: move cursor up, scroll down if at top
            if (vt->cursor_y <= 0) {
                // Scroll down: move lines 0..N-2 to 1..N-1
                memmove(&vt->cells[VTERM_COLS], &vt->cells[0],
                        (VTERM_ROWS - 1) * VTERM_COLS * sizeof(vterm_cell_t));
                
                // Clear top line
                vterm_cell_t *top_row = &vt->cells[0];
                for (int x = 0; x < VTERM_COLS; x++) {
                    top_row[x].ch = ' ';
                    top_row[x].attr = VTERM_DEFAULT_ATTR;
                }
            } else {
                vt->cursor_y--;
            }

            vt->escape_state = 0;
            return 1;
        }
        if (c == 'E') {
            // NEL - Next Line: move to column 1 of next line, scroll if needed
            vt->cursor_x = 0;
            if (vt->cursor_y >= VTERM_ROWS - 1) {
                vterm_scroll(vt);
            } else {
                vt->cursor_y++;
            }

            vt->escape_state = 0;
            return 1;
        }
        vt->escape_state = 0;
        return 0;
    }

    if (vt->escape_len < (int)sizeof(vt->escape_buf) - 1) {
        vt->escape_buf[vt->escape_len++] = c;
        vt->escape_buf[vt->escape_len] = '\0';
    }

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        vt->escape_buf[vt->escape_len - 1] = '\0';

        // DEC private mode sequences (ESC [ ? ...)
        if (vt->escape_buf[0] == '?') {
            // Parse ?25h (show cursor) and ?25l (hide cursor)
            if (c == 'h' && strcmp(vt->escape_buf, "?25") == 0) {
                vt->cursor_visible = 1;
            } else if (c == 'l' && strcmp(vt->escape_buf, "?25") == 0) {
                vt->cursor_visible = 0;
            }
            // Other DEC private modes gracefully ignored
            vt->escape_state = 0;
            vt->escape_len = 0;
            return 1;
        }

        switch (c) {
        case 'm':
            if (vt->sgr_enabled) {
                vterm_apply_sgr(vt, vt->escape_buf);
            }
            break;
        case 'J':
            if (strcmp(vt->escape_buf, "2") == 0 || strcmp(vt->escape_buf, "") == 0) {
                vterm_clear_internal(vt);
            }
            break;
        case 'H':
        case 'f':
            if (vt->escape_buf[0] == '\0' || strcmp(vt->escape_buf, "1;1") == 0) {
                vt->cursor_x = 0;
                vt->cursor_y = 0;
            } else {
                int row = 1, col = 1;
                sscanf(vt->escape_buf, "%d;%d", &row, &col);
                vt->cursor_y = (row > 0 ? row - 1 : 0);
                vt->cursor_x = (col > 0 ? col - 1 : 0);
                if (vt->cursor_y >= VTERM_ROWS) vt->cursor_y = VTERM_ROWS - 1;
                if (vt->cursor_x >= VTERM_COLS) vt->cursor_x = VTERM_COLS - 1;
            }
            break;
        case 'A': { // Cursor Up
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            vt->cursor_y -= n;
            if (vt->cursor_y < 0) vt->cursor_y = 0;
            break;
        }
        case 'B': { // Cursor Down
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            vt->cursor_y += n;
            if (vt->cursor_y >= VTERM_ROWS) vt->cursor_y = VTERM_ROWS - 1;
            break;
        }
        case 'C': { // Cursor Right
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            vt->cursor_x += n;
            if (vt->cursor_x >= VTERM_COLS) vt->cursor_x = VTERM_COLS - 1;
            break;
        }
        case 'D': { // Cursor Left
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            vt->cursor_x -= n;
            if (vt->cursor_x < 0) vt->cursor_x = 0;
            break;
        }
        case 'K': { // Erase in Line
            int mode = 0;
            if (vt->escape_buf[0]) mode = atoi(vt->escape_buf);
            int start = 0, end = VTERM_COLS;
            if (mode == 0) start = vt->cursor_x; // Cursor to end
            else if (mode == 1) end = vt->cursor_x + 1; // Start to cursor
            
            // Get pointer to current row
            vterm_cell_t *row = &vt->cells[vt->cursor_y * VTERM_COLS];
            for (int x = start; x < end; x++) {
                row[x].ch = ' ';
                row[x].attr = vt->current_attr;
            }
            break;
        }
        case 'X': { // Erase N Chars
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            int end = vt->cursor_x + n;
            if (end > VTERM_COLS) end = VTERM_COLS;

            vterm_cell_t *row = &vt->cells[vt->cursor_y * VTERM_COLS];
            for (int x = vt->cursor_x; x < end; x++) {
                row[x].ch = ' ';
                row[x].attr = vt->current_attr;
            }
            break;
        }
        case 'L': {
            // IL - Insert Lines: insert N blank lines at cursor row, scroll down
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            if (n > VTERM_ROWS - vt->cursor_y) n = VTERM_ROWS - vt->cursor_y;

            // Move lines down
            int lines_to_move = VTERM_ROWS - vt->cursor_y - n;
            if (lines_to_move > 0) {
                memmove(&vt->cells[(vt->cursor_y + n) * VTERM_COLS],
                        &vt->cells[vt->cursor_y * VTERM_COLS],
                        lines_to_move * VTERM_COLS * sizeof(vterm_cell_t));
            }

            // Clear inserted lines
            for (int y = vt->cursor_y; y < vt->cursor_y + n; y++) {
                vterm_cell_t *row = &vt->cells[y * VTERM_COLS];
                for (int x = 0; x < VTERM_COLS; x++) {
                    row[x].ch = ' ';
                    row[x].attr = VTERM_DEFAULT_ATTR;
                }
            }
            break;
        }
        case 'M': {
            // DL - Delete Lines: delete N lines at cursor row, scroll up
            int n = 1;
            if (vt->escape_buf[0]) n = atoi(vt->escape_buf);
            if (n < 1) n = 1;
            if (n > VTERM_ROWS - vt->cursor_y) n = VTERM_ROWS - vt->cursor_y;

            // Move lines up
            int lines_to_move = VTERM_ROWS - vt->cursor_y - n;
            if (lines_to_move > 0) {
                memmove(&vt->cells[vt->cursor_y * VTERM_COLS],
                        &vt->cells[(vt->cursor_y + n) * VTERM_COLS],
                        lines_to_move * VTERM_COLS * sizeof(vterm_cell_t));
            }

            // Clear vacated lines at bottom
            for (int y = VTERM_ROWS - n; y < VTERM_ROWS; y++) {
                vterm_cell_t *row = &vt->cells[y * VTERM_COLS];
                for (int x = 0; x < VTERM_COLS; x++) {
                    row[x].ch = ' ';
                    row[x].attr = VTERM_DEFAULT_ATTR;
                }
            }
            break;
        }
        case 'n':
            if (vt->escape_buf[0] == '6' && vt->escape_buf[1] == '\0') {
                char resp[32];
                snprintf(resp, sizeof(resp), "\x1b[%d;%dR", vt->cursor_y + 1, vt->cursor_x + 1);
                for (int i = 0; resp[i] != '\0'; i++) vterm_send_input(s_active_vt, resp[i]);
            }
            break;
        }

        vt->escape_state = 0;
        vt->escape_len = 0;
        return 1;
    }

    return 1;
}

// ============ Public API ============

esp_err_t vterm_init(void)
{
    // 1. Allocate the shared HOT buffer in IRAM (Internal RAM)
    s_iram_buffer = (vterm_cell_t *)heap_caps_malloc(BUFFER_SIZE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_iram_buffer) {
        printf("Failed to allocate IRAM vterm buffer\n");
        return ESP_ERR_NO_MEM;
    }
    // Clear it initially
    memset(s_iram_buffer, 0, BUFFER_SIZE_BYTES);

    // 2. Allocate VTs struct
    //s_vterms = (vterm_t *)heap_caps_calloc(VTERM_COUNT, sizeof(vterm_t), MALLOC_CAP_SPIRAM);
    s_vterms = (vterm_t *)heap_caps_calloc(VTERM_COUNT, sizeof(vterm_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_vterms) return ESP_ERR_NO_MEM;

    for (int i = 0; i < VTERM_COUNT; i++) {
        vterm_t *vt = &s_vterms[i];
        vt->history_enabled = true;
        vt->sgr_enabled = true;
        vt->input_queue = xQueueCreate(INPUT_QUEUE_SIZE, sizeof(char));
        vt->mutex = xSemaphoreCreateMutex();

#if VTERM_COUNT > 1
        // Keep inactive VT backing stores in internal RAM on Cardputer, since
        // this port does not have PSRAM enabled.
        vt->storage_cells = (vterm_cell_t *)heap_caps_malloc(BUFFER_SIZE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        vt->history_cells = (vterm_cell_t *)heap_caps_malloc(HISTORY_SIZE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!vt->storage_cells || !vt->history_cells) {
            printf("Failed to allocate backing store for VT %d\n", i);
            return ESP_ERR_NO_MEM;
        }

        // Default: point to storage. Switch() will fix the active one.
        vt->cells = vt->storage_cells;
#else
        // Single VT mode: no storage needed, always uses IRAM buffer
        vt->storage_cells = NULL;
        vt->cells = (vterm_cell_t *)heap_caps_malloc(BUFFER_SIZE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        vt->history_cells = (vterm_cell_t *)heap_caps_malloc(HISTORY_SIZE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!vt->cells || !vt->history_cells) {
            printf("Failed to allocate vterm backing store\n");
            return ESP_ERR_NO_MEM;
        }
#endif

        vterm_clear_internal(vt);
    }

    // 3. Set up initial active VT (0)
    s_active_vt = 0;
    vterm_refresh_view(&s_vterms[0]);

    return ESP_OK;
}

vterm_cell_t *vterm_get_direct_buffer(void)
{
    return s_iram_buffer;
}

void vterm_switch(int vt_id)
{
#if VTERM_COUNT > 1
    if (vt_id < 0 || vt_id >= VTERM_COUNT) return;
    if (vt_id == s_active_vt) return;

    vterm_t *old_vt = &s_vterms[s_active_vt];
    vterm_t *new_vt = &s_vterms[vt_id];

    // Lock both to ensure no writing happens while switching the active view.
    xSemaphoreTake(old_vt->mutex, portMAX_DELAY);
    xSemaphoreTake(new_vt->mutex, portMAX_DELAY);

    s_active_vt = vt_id;
    s_scrollback_offset = 0;
    vterm_refresh_view(new_vt);

    xSemaphoreGive(new_vt->mutex);
    xSemaphoreGive(old_vt->mutex);

    if (s_on_switch_cb) s_on_switch_cb(vt_id);
#else
    (void)vt_id; // Single VT mode: switching disabled
#endif
}

void vterm_write(int vt_id, const char *data, size_t len)
{
    if (vt_id < 0 || vt_id >= VTERM_COUNT) return;
    vterm_t *vt = &s_vterms[vt_id];
    size_t processed_since_yield = 0;

    xSemaphoreTake(vt->mutex, portMAX_DELAY);
    const char *p = data;
    const char *end = data + len;

    // Cache state
    int cx = vt->cursor_x;
    int cy = vt->cursor_y;
    uint8_t current_attr = vt->current_attr;
    int escape_mode = vt->escape_state;

    vterm_cell_t *cells_base = vt->cells;
    vterm_cell_t *cursor_ptr = &cells_base[cy * VTERM_COLS + cx];
    vterm_cell_t *row_end = &cells_base[cy * VTERM_COLS + VTERM_COLS];

    while (p < end) {
        char c = *p++;
        processed_since_yield++;

        // Fast path: printable ASCII characters (most common case)
        if (escape_mode == 0 && c >= 32 && c < 127) {
            cursor_ptr->ch = c;
            cursor_ptr->attr = current_attr;
            cursor_ptr++;
            cx++;
            if (cursor_ptr >= row_end) {
                cx = 0; cy++;
                if (cy >= VTERM_ROWS) {
                    vt->cursor_x = cx; vt->cursor_y = cy;
                    vterm_scroll(vt);
                    cy = vt->cursor_y;
                    cursor_ptr = &cells_base[cy * VTERM_COLS + cx];
                    row_end = &cells_base[cy * VTERM_COLS + VTERM_COLS];
                } else {
                    row_end += VTERM_COLS;
                }
            }
            continue;
        }

        // Slow path: escape sequences and control characters
        vt->cursor_x = cx;
        vt->cursor_y = cy;
        vt->current_attr = current_attr;
        vt->escape_state = escape_mode;

        if (!vterm_handle_escape(vt, c)) {
            vterm_putchar_internal(vt, c);
        }

        escape_mode = vt->escape_state;
        current_attr = vt->current_attr;

        if (vt->cursor_x != cx || vt->cursor_y != cy) {
            cx = vt->cursor_x;
            cy = vt->cursor_y;
            cursor_ptr = &cells_base[cy * VTERM_COLS + cx];
            row_end = &cells_base[cy * VTERM_COLS + VTERM_COLS];
        }

        if (processed_since_yield >= WRITE_YIELD_CHUNK) {
            vt->cursor_x = cx;
            vt->cursor_y = cy;
            vt->current_attr = current_attr;
            vt->escape_state = escape_mode;
            if (vt_id == s_active_vt) {
                vterm_refresh_view(vt);
            }
            processed_since_yield = 0;
            vTaskDelay(1);
            cells_base = vt->cells;
            cursor_ptr = &cells_base[cy * VTERM_COLS + cx];
            row_end = &cells_base[cy * VTERM_COLS + VTERM_COLS];
        }
    }

    vt->cursor_x = cx;
    vt->cursor_y = cy;
    vt->current_attr = current_attr;
    vt->escape_state = escape_mode;


    if (vt_id == s_active_vt) {
        vterm_refresh_view(vt);
    }
    xSemaphoreGive(vt->mutex);
}

// Helpers
void vterm_set_switch_callback(void (*cb)(int)) { s_on_switch_cb = cb; }
int vterm_get_active(void) { return s_active_vt; }
void vterm_get_size(int *r, int *c) { if(r) *r=VTERM_ROWS; if(c) *c=VTERM_COLS; }

void vterm_get_cursor(int vt_id, int *col, int *row, int *visible) {
    if (vt_id >= 0 && vt_id < VTERM_COUNT) {
        vterm_t *vt = &s_vterms[vt_id];
        if (col) *col = vt->cursor_x;
        if (row) *row = vt->cursor_y;
        if (visible) *visible = (s_scrollback_offset == 0) ? vt->cursor_visible : 0;
    }
}

int vterm_getchar(int vt_id, int timeout_ms) {
    if (vt_id < 0 || vt_id >= VTERM_COUNT) return -1;
    char c;
    TickType_t wait = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_vterms[vt_id].input_queue, &c, wait) == pdTRUE) return (unsigned char)c;
    return -1;
}

void vterm_send_input(int vt_id, char c) {
    if (vt_id >= 0 && vt_id < VTERM_COUNT) xQueueSend(s_vterms[vt_id].input_queue, &c, 0);
}

void vterm_input_flush(int vt_id) {
    if (vt_id < 0 || vt_id >= VTERM_COUNT) return;
    xQueueReset(s_vterms[vt_id].input_queue);
}

void vterm_scrollback(int delta_lines)
{
    vterm_t *vt = &s_vterms[s_active_vt];
    if (!vt->history_enabled) {
        s_scrollback_offset = 0;
        xSemaphoreTake(vt->mutex, portMAX_DELAY);
        vterm_refresh_view(vt);
        xSemaphoreGive(vt->mutex);
        return;
    }
    int total_lines = vt->history_count + VTERM_ROWS;
    int max_offset = total_lines - VTERM_ROWS;

    if (max_offset < 0) {
        max_offset = 0;
    }

    s_scrollback_offset += delta_lines;
    if (s_scrollback_offset < 0) {
        s_scrollback_offset = 0;
    }
    if (s_scrollback_offset > max_offset) {
        s_scrollback_offset = max_offset;
    }

    xSemaphoreTake(vt->mutex, portMAX_DELAY);
    vterm_refresh_view(vt);
    xSemaphoreGive(vt->mutex);
}

void vterm_scrollback_bottom(void)
{
    if (s_scrollback_offset == 0) {
        return;
    }
    vterm_scrollback(-s_scrollback_offset);
}

bool vterm_scrollback_active(void)
{
    return s_scrollback_offset > 0;
}

int vterm_set_history_enabled(int vt_id, bool enabled)
{
    if (vt_id < 0 || vt_id >= VTERM_COUNT) {
        return -1;
    }

    vterm_t *vt = &s_vterms[vt_id];
    xSemaphoreTake(vt->mutex, portMAX_DELAY);
    vt->history_enabled = enabled;
    if (!enabled) {
        vt->history_head = 0;
        vt->history_count = 0;
        if (vt_id == s_active_vt) {
            s_scrollback_offset = 0;
        }
    }
    if (vt_id == s_active_vt) {
        vterm_refresh_view(vt);
    }
    xSemaphoreGive(vt->mutex);
    return 0;
}

bool vterm_get_history_enabled(int vt_id)
{
    if (vt_id < 0 || vt_id >= VTERM_COUNT) {
        return false;
    }
    return s_vterms[vt_id].history_enabled;
}

int vterm_set_sgr_enabled(int vt_id, bool enabled)
{
    if (vt_id < 0 || vt_id >= VTERM_COUNT) {
        return -1;
    }

    xSemaphoreTake(s_vterms[vt_id].mutex, portMAX_DELAY);
    s_vterms[vt_id].sgr_enabled = enabled;
    if (!enabled) {
        s_vterms[vt_id].current_attr = VTERM_DEFAULT_ATTR;
    }
    xSemaphoreGive(s_vterms[vt_id].mutex);
    return 0;
}

bool vterm_get_sgr_enabled(int vt_id)
{
    if (vt_id < 0 || vt_id >= VTERM_COUNT) {
        return false;
    }

    return s_vterms[vt_id].sgr_enabled;
}

// Hotkey / Input logic (Compact copy for completeness)
static char s_esc_buf[16];
static int s_esc_len = 0;
static TickType_t s_esc_start = 0;
static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;

static void flush_input_buffer(void) {
    // Use critical section to prevent shell task from preempting mid-flush
    // This ensures all chars of an escape sequence are added atomically
    portENTER_CRITICAL(&s_input_mux);
    for (int i = 0; i < s_esc_len; i++) vterm_send_input(s_active_vt, s_esc_buf[i]);
    s_esc_len = 0;
    portEXIT_CRITICAL(&s_input_mux);
}

// Check if buffer matches a VT switch sequence, return VT number or -1
static int match_vt_hotkey(void)
{
    if (s_esc_len < 2) return -1;
    
    // Must start with ESC
    if (s_esc_buf[0] != '\033') return -1;
    
    // ESC O <x> - F1-F4 or Ctrl+F1-F4
    if (s_esc_len >= 3 && s_esc_buf[1] == 'O') {
        char c = s_esc_buf[2];
        // ESC O P/Q/R/S = F1-F4
        if (c == 'P') return 0;
        if (c == 'Q') return 1;
        if (c == 'R') return 2;
        if (c == 'S') return 3;
        
        // ESC O 5 P/Q/R/S = Ctrl+F1-F4 (some terminals)
        if (s_esc_len >= 4 && c == '5') {
            char d = s_esc_buf[3];
            if (d == 'P') return 0;
            if (d == 'Q') return 1;
            if (d == 'R') return 2;
            if (d == 'S') return 3;
        }
    }
    
    // ESC [ ... sequences
    if (s_esc_len >= 3 && s_esc_buf[1] == '[') {
        // Null-terminate for easier parsing
        s_esc_buf[s_esc_len] = '\0';
        
        // ESC [ 1 ; 5 P/Q/R/S = Ctrl+F1-F4 (xterm)
        if (s_esc_len == 6 && s_esc_buf[2] == '1' && s_esc_buf[3] == ';' && s_esc_buf[4] == '5') {
            char c = s_esc_buf[5];
            if (c == 'P') return 0;
            if (c == 'Q') return 1;
            if (c == 'R') return 2;
            if (c == 'S') return 3;
        }
        
        // ESC [ 11~ to [14~ = F1-F4 (vt style)
        // ESC [ 11;5~ to [14;5~ = Ctrl+F1-F4 (vt style with modifier)
        if (s_esc_buf[s_esc_len - 1] == '~') {
            int num = 0;
            
            // Parse: ESC [ <num> ~ or ESC [ <num> ; <mod> ~
            char *p = &s_esc_buf[2];
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            // Skip optional modifier (;5 etc) - we accept with or without
            if (*p == ';') {
                p++;
                while (*p >= '0' && *p <= '9') p++;
            }
            
            // F1=11, F2=12, F3=13, F4=14
            if (num >= 11 && num <= 14) {
                return num - 11;
            }
        }
        
        // ESC [ 49;5u to [52;5u = Ctrl+1-4 (CSI u / fixterms)
        // 49='1', 50='2', 51='3', 52='4'
        if (s_esc_buf[s_esc_len - 1] == 'u') {
            int codepoint = 0;
            int modifier = 0;
            
            char *p = &s_esc_buf[2];
            while (*p >= '0' && *p <= '9') {
                codepoint = codepoint * 10 + (*p - '0');
                p++;
            }
            if (*p == ';') {
                p++;
                while (*p >= '0' && *p <= '9') {
                    modifier = modifier * 10 + (*p - '0');
                    p++;
                }
            }
            
            // Modifier 5 = Ctrl
            if (modifier == 5 && codepoint >= 49 && codepoint <= 52) {
                return codepoint - 49;  // '1'->0, '2'->1, etc.
            }
        }
    }
    
    return -1;
}

// Check if we're in the middle of a potential hotkey sequence
static int could_be_hotkey(void)
{
    if (s_esc_len == 0) return 0;
    if (s_esc_buf[0] != '\033') return 0;
    if (s_esc_len == 1) return 1;  // Just ESC, wait for more
    
    char c1 = s_esc_buf[1];
    
    // ESC O ... or ESC [ ... could be hotkeys
    if (c1 == 'O' || c1 == '[') {
        // Limit max length to avoid hanging on garbage
        if (s_esc_len > 10) return 0;
        
        // Check for terminal characters
        if (s_esc_len >= 3) {
            char last = s_esc_buf[s_esc_len - 1];
            // These end a sequence
            if (last == '~' || last == 'u' || 
                (last >= 'A' && last <= 'Z') ||
                (c1 == 'O' && s_esc_len >= 3 && last >= 'P' && last <= 'S')) {
                return 0;  // Sequence complete, should be matched by now
            }
        }
        return 1;  // Still building sequence
    }
    
    return 0;
}

int vterm_input_feed(char c)
{
    if (s_scrollback_offset > 0) {
        vterm_scrollback_bottom();
    }

    // Timeout: if we're in escape state too long, flush and reset
    if (s_esc_len > 0) {
        if ((xTaskGetTickCount() - s_esc_start) > pdMS_TO_TICKS(20)) {
            flush_input_buffer();
        }
    }
    
    // Start of new escape sequence?
    if (s_esc_len == 0 && c == '\033') {
        s_esc_buf[s_esc_len++] = c;
        s_esc_start = xTaskGetTickCount();
        return 0;  // Buffering
    }
    
    // If we're in an escape sequence, add to buffer
    if (s_esc_len > 0) {
        if (s_esc_len < (int)sizeof(s_esc_buf) - 1) {
            s_esc_buf[s_esc_len++] = c;
        }
        
        // Check for complete hotkey match
        int vt = match_vt_hotkey();
        if (vt >= 0) {
            s_esc_len = 0;
            vterm_switch(vt);
            return 1;  // Hotkey handled
        }
        
        // Could this still become a hotkey?
        if (could_be_hotkey()) {
            return 0;  // Keep buffering
        }
        
        // Not a hotkey, flush as regular input
        flush_input_buffer();
        return 0;
    }
    
    // Normal character, pass through
    vterm_send_input(s_active_vt, c);
    return 0;
}

// ============ Palette API ============

// Default xterm-compatible 16-color palette (RGB565)
static uint16_t s_palette[16] = {
    0x0000,  // 0: Black
    0x8000,  // 1: Red (dark)
    0x0400,  // 2: Green (dark)
    0x8400,  // 3: Yellow (dark/brown)
    0x0010,  // 4: Blue (dark)
    0x8010,  // 5: Magenta (dark)
    0x0410,  // 6: Cyan (dark)
    0xC618,  // 7: White (light gray)
    0x8410,  // 8: Bright Black (dark gray)
    0xF800,  // 9: Bright Red
    0x07E0,  // 10: Bright Green
    0xFFE0,  // 11: Bright Yellow
    0x001F,  // 12: Bright Blue
    0xF81F,  // 13: Bright Magenta
    0x07FF,  // 14: Bright Cyan
    0xFFFF   // 15: Bright White
};

void vterm_set_palette(const uint16_t palette[16])
{
    for (int i = 0; i < 16; i++) {
        s_palette[i] = palette[i];
    }
}

const uint16_t *vterm_get_palette(void)
{
    return s_palette;
}

// ============ Graphics Mode Integration ============

static int s_graphics_mode_active = 0;
static int s_saved_active_vt = -1;

int vterm_enter_graphics_mode(void)
{
    if (s_graphics_mode_active) {
        return 0;  // Already in graphics mode
    }

    // Save current active VT index
    s_saved_active_vt = s_active_vt;

#if VTERM_COUNT > 1
    // Screen state already lives in each VT backing store.
#endif

    s_graphics_mode_active = 1;
    return 0;
}

int vterm_exit_graphics_mode(void)
{
    if (!s_graphics_mode_active) {
        return 0;  // Not in graphics mode
    }

#if VTERM_COUNT > 1
    if (s_saved_active_vt >= 0 && s_saved_active_vt < VTERM_COUNT) {
        vterm_t *vt = &s_vterms[s_saved_active_vt];
        xSemaphoreTake(vt->mutex, portMAX_DELAY);
        s_active_vt = s_saved_active_vt;
        vterm_refresh_view(vt);
        xSemaphoreGive(vt->mutex);
    }
#endif

    s_graphics_mode_active = 0;
    s_saved_active_vt = -1;
    return 0;
}

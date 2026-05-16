#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(BREEZY_BOARD_CARDPUTER)
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "host/ble_store.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "breezybox.h"
#include "breezy_pager.h"
#include "board_runtime.h"
#include "rgb_display.h"
#include "my_console_io.h"
#include "bt_keyboard.h"
#include "vterm.h"

static const char *TAG = "main";
static bool s_bt_initialized = false;
enum { HELP_WIDTH = 40, HELP_DESC_INDENT = 2 };

static int cmd_help(int argc, char **argv);
int cmd_btscan(int argc, char **argv);
int cmd_btconnect(int argc, char **argv);
static int cmd_btclear(int argc, char **argv);
static int cmd_btstatus(int argc, char **argv);
static int cmd_vt(int argc, char **argv);
static int cmd_keytest(int argc, char **argv);
static int cmd_colortest(int argc, char **argv);
static int cmd_setcon(int argc, char **argv);
extern int cmd_testgfx(int argc, char **argv);
extern int cmd_plasma_builtin_main(int argc, char **argv);
extern int cmd_termbench_builtin_main(int argc, char **argv);
extern int cmd_vi_builtin_main(int argc, char **argv);
extern int cmd_wget_builtin_main(int argc, char **argv);
extern int cmd_gzip_builtin_main(int argc, char **argv);
extern int cmd_gunzip_builtin_main(int argc, char **argv);
#if defined(BREEZY_BOARD_CARDPUTER)
extern int ssh_app_mode_run(ssh_session session);
#endif

static const esp_console_cmd_t s_app_cmds[] = {
    { .command = "help", .help = "List all commands", .hint = NULL, .func = &cmd_help },
    { .command = "btscan", .help = "Scan for BT keyboards", .hint = "[-v]", .func = &cmd_btscan },
    { .command = "btconnect", .help = "Reconnect saved keyboard", .func = &cmd_btconnect },
    { .command = "btclear", .help = "Clear saved BT devices", .func = &cmd_btclear },
    { .command = "btstatus", .help = "Show BT keyboard status", .func = &cmd_btstatus },
    { .command = "vt", .help = "Switch VT", .func = &cmd_vt },
    { .command = "keytest", .help = "Keys test", .func = &cmd_keytest },
    { .command = "colortest", .help = "ANSI colors test", .func = &cmd_colortest },
    { .command = "setcon", .help = "Set console output", .hint = "<lcd|usb|both>", .func = &cmd_setcon },
    { .command = "plasma", .help = "ANSI plasma demo", .func = &cmd_plasma_builtin_main },
    { .command = "termbench", .help = "Terminal benchmark", .hint = "[-q] [-d seconds] [-s cols rows]", .func = &cmd_termbench_builtin_main },
    { .command = "vi", .help = "Text editor", .hint = "[file]", .func = &cmd_vi_builtin_main },
    { .command = "wget", .help = "Download file over HTTP(S)", .hint = "<url> [filename]", .func = &cmd_wget_builtin_main },
    { .command = "gzip", .help = "Compress file", .hint = "<file> [outfile]", .func = &cmd_gzip_builtin_main },
    { .command = "gunzip", .help = "Decompress gzip file", .hint = "<file.gz> [outfile]", .func = &cmd_gunzip_builtin_main },
    { .command = "testgfx", .help = "Graphics demo", .hint = "[-t seconds] [-v]", .func = &cmd_testgfx },
};

static const breezybox_help_entry_t s_app_help[] = {
    { "help", "help [command]", "Show command list or detailed help for one command.", NULL, "help\nhelp wifi\nhelp vi" },
    { "btscan", "btscan [-v]", "Scan for Bluetooth keyboards and auto-connect when one is found.", "-v  verbose scan output", "btscan\nbtscan -v" },
    { "btconnect", "btconnect", "Reconnect to the previously saved keyboard.", NULL, "btconnect" },
    { "btclear", "btclear", "Clear saved Bluetooth device bonds.", NULL, "btclear" },
    { "btstatus", "btstatus", "Show Bluetooth keyboard status.", NULL, "btstatus" },
    { "vt", "vt [n]", "Show or switch virtual terminal.", "n  terminal number", "vt\nvt 1" },
    { "keytest", "keytest", "Print raw keypresses until Ctrl+C.", NULL, "keytest" },
    { "colortest", "colortest", "Show ANSI color output samples.", NULL, "colortest" },
    { "setcon", "setcon <lcd|usb|both|usbreset>", "Set console output routing.", "lcd      LCD only\nusb      USB only\nboth     LCD and USB\nusbreset reset USB detection", "setcon both\nsetcon lcd\nsetcon usbreset" },
    { "plasma", "plasma", "Run the ANSI plasma demo.", NULL, "plasma" },
    { "termbench", "termbench [-q] [-d seconds] [-s cols rows]", "Run the terminal benchmark.", "-q           quiet mode\n-d seconds   duration\n-s cols rows screen size", "termbench\ntermbench -d 10\ntermbench -q -s 40 16" },
    { "vi", "vi [file]", "Open the text editor.", NULL, "vi\nvi notes.txt" },
    { "wget", "wget <url> [filename]", "Download a file over HTTP or HTTPS.", NULL, "wget https://example.com\nwget https://example.com index.html" },
    { "gzip", "gzip <file> [outfile]", "Compress a file.", NULL, "gzip log.txt\ngzip log.txt log.txt.gz" },
    { "gunzip", "gunzip <file.gz> [outfile]", "Decompress a gzip file.", NULL, "gunzip log.txt.gz\ngunzip log.txt.gz log.txt" },
    { "testgfx", "testgfx [-t seconds] [-v]", "Run the graphics demo.", "-t seconds  run duration\n-v          verbose output", "testgfx\ntestgfx -t 5\ntestgfx -v" },
};

static bool pager_print_wrapped(breezy_pager_t *pager, const char *text, int indent)
{
    const int width = HELP_WIDTH - indent;
    const char *p = text ? text : "";

    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }

        const char *line_end = p;
        const char *last_space = NULL;
        int len = 0;

        while (*line_end && *line_end != '\n' && len < width) {
            if (*line_end == ' ') {
                last_space = line_end;
            }
            ++line_end;
            ++len;
        }

        if (*line_end == '\n') {
            printf("%*s%.*s\n", indent, "", (int)(line_end - p), p);
            if (!breezy_pager_step(pager)) {
                return false;
            }
            p = line_end + 1;
            continue;
        }

        if (*line_end && len >= width && last_space && last_space > p) {
            line_end = last_space;
        }

        while (line_end > p && *(line_end - 1) == ' ') {
            --line_end;
        }

        printf("%*s%.*s\n", indent, "", (int)(line_end - p), p);
        if (!breezy_pager_step(pager)) {
            return false;
        }

        p = line_end;
        while (*p == ' ') {
            ++p;
        }
    }

    return true;
}

static bool print_help_line(breezy_pager_t *pager, const esp_console_cmd_t *cmd)
{
    char usage[96];
    const char *help = cmd->help ? cmd->help : "";

    if (cmd->hint && cmd->hint[0]) {
        snprintf(usage, sizeof(usage), "%s %s", cmd->command, cmd->hint);
    } else {
        snprintf(usage, sizeof(usage), "%s", cmd->command);
    }

    if (!pager_print_wrapped(pager, usage, 0)) {
        return false;
    }
    if (!pager_print_wrapped(pager, help, HELP_DESC_INDENT)) {
        return false;
    }

    printf("\n");
    return breezy_pager_step(pager);
}

static int print_help_entry(breezy_pager_t *pager, const breezybox_help_entry_t *entry)
{
    if (!entry) {
        return 1;
    }

    if (!pager_print_wrapped(pager, "Usage:", 0)) return 1;
    if (!pager_print_wrapped(pager, entry->usage, HELP_DESC_INDENT)) return 1;

    if (entry->summary && entry->summary[0]) {
        printf("\n");
        if (!breezy_pager_step(pager)) return 1;
        if (!pager_print_wrapped(pager, "Description:", 0)) return 1;
        if (!pager_print_wrapped(pager, entry->summary, HELP_DESC_INDENT)) return 1;
    }

    if (entry->options && entry->options[0]) {
        printf("\n");
        if (!breezy_pager_step(pager)) return 1;
        if (!pager_print_wrapped(pager, "Options:", 0)) return 1;
        if (!pager_print_wrapped(pager, entry->options, HELP_DESC_INDENT)) return 1;
    }

    if (entry->examples && entry->examples[0]) {
        printf("\n");
        if (!breezy_pager_step(pager)) return 1;
        if (!pager_print_wrapped(pager, "Examples:", 0)) return 1;
        if (!pager_print_wrapped(pager, entry->examples, HELP_DESC_INDENT)) return 1;
    }

    return 0;
}

static int cmd_help(int argc, char **argv)
{
    const esp_console_cmd_t *core_cmds = NULL;
    size_t core_count = 0;
    breezy_pager_t pager;

    breezy_pager_init(&pager, 15);
    core_cmds = breezybox_get_core_commands(&core_count);

    if (argc > 1) {
        const breezybox_help_entry_t *entry = breezybox_find_help_entry(argv[1]);
        if (!entry) {
            printf("No help for: %s\n", argv[1]);
            return 1;
        }
        return print_help_entry(&pager, entry);
    }

    printf("Commands\n");
    printf("--------\n");
    if (!breezy_pager_step(&pager) || !breezy_pager_step(&pager)) {
        return 0;
    }

    for (size_t i = 0; i < core_count; i++) {
        if (!print_help_line(&pager, &core_cmds[i])) {
            return 0;
        }
    }

    for (size_t i = 0; i < sizeof(s_app_cmds) / sizeof(s_app_cmds[0]); i++) {
        if (!print_help_line(&pager, &s_app_cmds[i])) {
            return 0;
        }
    }

    return 0;
}

static esp_err_t ensure_bt_initialized(void)
{
    if (s_bt_initialized) {
        return ESP_OK;
    }

    esp_err_t err = bt_keyboard_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BT init failed, err=%s", esp_err_to_name(err));
        return err;
    }

    s_bt_initialized = true;
    return ESP_OK;
}

// BreezyBox command to scan for BT
int cmd_btscan(int argc, char **argv) {
    int verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    if (ensure_bt_initialized() != ESP_OK) {
        printf("BT init failed\n");
        return 1;
    }
    printf("Scanning for Bluetooth keyboards...\n");
    if (bt_keyboard_scan_ex(verbose) != ESP_OK) {
        printf("BT scan failed\n");
        return 1;
    }
    return 0;
}

// Command Wrapper
int cmd_btconnect(int argc, char **argv) {
    if (ensure_bt_initialized() != ESP_OK) {
        printf("BT init failed\n");
        return 1;
    }
    if (bt_keyboard_connected()) {
        printf("Bluetooth keyboard already connected\n");
        return 0;
    }
    if (!bt_keyboard_has_saved_target()) {
        printf("No saved keyboard. Run 'btscan' first.\n");
        return 1;
    }

    printf("Reconnecting to saved keyboard...\n");
    if (bt_keyboard_connect_native() != ESP_OK) {
        printf("BT reconnect failed\n");
        return 1;
    }
    return 0;
}

static int cmd_btclear(int argc, char **argv) {
    bt_keyboard_clear_bonds();
    printf("Bonds cleared. Restart device.\n");
    return 0;
}

// BreezyBox command to check BT status
static int cmd_btstatus(int argc, char **argv)
{
    if (!s_bt_initialized) {
        printf("BT keyboard: not initialized\n");
        printf("Run 'btscan' or 'btconnect' to initialize BLE keyboard support\n");
        return 0;
    }

    if (bt_keyboard_connected()) {
        printf("BT keyboard: connected\n");
    } else {
        printf("BT keyboard: not connected\n");
        printf("Use 'btscan' to search for keyboards\n");
    }
    return 0;
}

// DEBUG
static int cmd_vt(int argc, char **argv)
{
    if (argc < 2) {
        printf("Active: VT%d\n", vterm_get_active());
        return 0;
    }
    int n = atoi(argv[1]);
    if (n >= 0 && n < VTERM_COUNT) {
        vterm_switch(n);
        printf("Switched to VT%d\n", n);
    }
    return 0;
}

// DEBUG
static int cmd_keytest(int argc, char **argv)
{
    printf("Press keys (Ctrl+C to exit):\n");
    while (1) {
        int c = getchar();
        if (c == 3) break;  // Ctrl+C
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c >= 32 && c < 127) {
            printf("0x%02X '%c'\n", c, c);
        } else {
            printf("0x%02X\n", c);
        }
    }
    return 0;
}

// DEBUG
static int cmd_colortest(int argc, char **argv)
{
    printf("\033[31mRed\033[0m ");
    printf("\033[32mGreen\033[0m ");
    printf("\033[33mYellow\033[0m ");
    printf("\033[34mBlue\033[0m ");
    printf("\033[1;35mBright Magenta\033[0m\n");
    printf("\033[41;37mWhite on Red\033[0m\n");
    return 0;
}

// Set console output routing (lcd, usb, or both)
static int cmd_setcon(int argc, char **argv)
{
    if (argc < 2) {
        console_output_mode_t mode = my_console_get_output_mode();
        const char *mode_str = (mode == CONSOLE_OUT_LCD) ? "lcd" :
                                (mode == CONSOLE_OUT_USB) ? "usb" : "both";
        int usb_connected = my_console_usb_connected();
        printf("Console output: %s\n", mode_str);
        printf("USB status: %s\n", usb_connected ? "connected" : "disconnected (auto-skipped)");
        printf("Usage: setcon <lcd|usb|both|usbreset>\n");
        return 0;
    }

    const char *arg = argv[1];

    // Handle USB reset command
    if (strcmp(arg, "usbreset") == 0) {
        my_console_usb_reconnect();
        printf("USB detection reset - will re-probe on next write\n");
        return 0;
    }

    console_output_mode_t mode;

    if (strcmp(arg, "lcd") == 0) {
        mode = CONSOLE_OUT_LCD;
    } else if (strcmp(arg, "usb") == 0) {
        mode = CONSOLE_OUT_USB;
    } else if (strcmp(arg, "both") == 0) {
        mode = CONSOLE_OUT_BOTH;
    } else {
        printf("Invalid mode: %s\n", arg);
        printf("Usage: setcon <lcd|usb|both|usbreset>\n");
        return 1;
    }

    my_console_set_output_mode(mode);
    printf("Console output: %s\n", arg);
    return 0;
}

// Main loop - keeps task alive while DMA renders display
static void main_loop(void)
{
    while (1) {
        board_input_poll();
        vTaskDelay(pdMS_TO_TICKS(board_input_get_poll_interval_ms()));
    }
}

void app_main(void)
{
#if defined(BREEZY_BOARD_CARDPUTER)
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
#endif

    printf("\n--- Boot sequence complete. Starting ESP32-DOS ---\n");

    printf("Initializing display...\n");
    rgb_display_init();
    printf("Display initialized\n");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize VTerm & Console (also links vterm buffer to display)
    if (my_console_init() != ESP_OK) {
        ESP_LOGE(TAG, "Console init failed!");
        return;
    }

    bt_keyboard_set_char_callback(my_console_bt_receive);

    if (board_input_init(my_console_bt_receive) != ESP_OK) {
        ESP_LOGW(TAG, "%s input init failed", board_runtime_name());
    }

    breezybox_set_extra_commands(s_app_cmds, sizeof(s_app_cmds) / sizeof(s_app_cmds[0]));
    breezybox_set_extra_help_entries(s_app_help, sizeof(s_app_help) / sizeof(s_app_help[0]));
#if defined(BREEZY_BOARD_CARDPUTER)
    breezybox_set_ssh_app_mode_runner(ssh_app_mode_run);
#endif
    breezybox_start_stdio(16384, 5);

    // Register Cardputer-specific commands after BreezyBox core commands.
    for (size_t i = 0; i < sizeof(s_app_cmds) / sizeof(s_app_cmds[0]); i++) {
        esp_console_cmd_register(&s_app_cmds[i]);
    }

    // Keep main task alive (display renders via DMA callbacks)
    main_loop();
}

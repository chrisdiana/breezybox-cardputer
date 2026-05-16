#include "breezy_exec.h"
#include "breezybox.h"
#include "breezy_vfs.h"
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#if CONFIG_ELF_LOADER
#include "esp_elf.h"
#endif

#define TEMP_PIPE_FILE BREEZYBOX_MOUNT_POINT "/.pipe_tmp"
#define TEMP_OUT_FILE  BREEZYBOX_MOUNT_POINT "/.out_tmp"

// PATH for executable search (colon-separated like Unix)
#define EXEC_PATH "/root/bin"

#if CONFIG_ELF_LOADER
// ELF magic bytes
static const uint8_t ELF_MAGIC[4] = {0x7f, 'E', 'L', 'F'};
#endif

static const char *TAG = "exec";
static vprintf_like_t s_orig_vprintf = NULL;

#if !CONFIG_ELF_LOADER
static int s_warned_no_elf_loader = 0;
#endif

// Custom log handler that suppresses logs during redirects
static int null_vprintf(const char *fmt, va_list args)
{
    return 0;
}

static char *trim_in_place(char *s)
{
    if (!s) {
        return s;
    }

    while (*s == ' ') {
        s++;
    }

    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[--len] = '\0';
    }

    return s;
}

static char *find_top_level_operator(char *s, const char *op)
{
    if (!s || !op || !*op) {
        return NULL;
    }

    size_t op_len = strlen(op);
    char quote = '\0';
    for (char *p = s; *p; ++p) {
        if (quote) {
            if (*p == quote) {
                quote = '\0';
            }
            continue;
        }

        if (*p == '"' || *p == '\'') {
            quote = *p;
            continue;
        }

        if (strncmp(p, op, op_len) == 0) {
            return p;
        }
    }

    return NULL;
}

void breezybox_exec_init(void)
{
    s_orig_vprintf = esp_log_set_vprintf(vprintf);
    esp_log_set_vprintf(s_orig_vprintf);
}

// Check if file exists
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Check if file has ELF magic
static int is_elf_file(const char *path)
{
#if !CONFIG_ELF_LOADER
    (void)path;
    return 0;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    
    uint8_t magic[4];
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    
    return (n == 4 && memcmp(magic, ELF_MAGIC, 4) == 0);
#endif
}

// Search for executable in PATH and CWD
// Returns allocated string with full path, or NULL if not found
static char *find_executable(const char *name)
{
    char path[BREEZYBOX_MAX_PATH * 2];
    
    // If name contains '/', treat as path (absolute or relative)
    if (strchr(name, '/')) {
        if (name[0] == '/') {
            // Absolute path
            if (file_exists(name)) {
                return strdup(name);
            }
        } else {
            // Relative path - resolve from CWD
            breezybox_resolve_path(name, path, sizeof(path));
            if (file_exists(path)) {
                return strdup(path);
            }
        }
        return NULL;
    }
    
    // Search in CWD first
    breezybox_resolve_path(name, path, sizeof(path));
    if (file_exists(path)) {
        return strdup(path);
    }
    
    // Search in PATH
    snprintf(path, sizeof(path), "%s/%s", EXEC_PATH, name);
    if (file_exists(path)) {
        return strdup(path);
    }
    
    return NULL;
}

// Argument parsing context
typedef struct {
    char *buffer;   // Original strdup'd buffer
    char **argv;    // Argument pointers
    int argc;       // Argument count
} parsed_args_t;

// Parse command line into argc/argv with basic quote support
// Returns 0 on success, -1 on error
static int parse_args(const char *cmdline, parsed_args_t *args)
{
    args->buffer = NULL;
    args->argv = NULL;
    args->argc = 0;
    
    if (!cmdline || !*cmdline) return 0;
    
    // Make a working copy
    char *buf = strdup(cmdline);
    if (!buf) return -1;
    
    // First pass: count arguments
    int argc = 0;
    char *p = buf;
    
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        
        argc++;
        
        // Handle quoted strings
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) p++;
            if (*p) p++;
        } else {
            while (*p && *p != ' ') p++;
        }
    }
    
    if (argc == 0) {
        free(buf);
        return 0;
    }
    
    // Allocate argv array
    char **argv = malloc((argc + 1) * sizeof(char *));
    if (!argv) {
        free(buf);
        return -1;
    }
    
    // Second pass: extract arguments
    p = buf;
    int i = 0;
    
    while (*p && i < argc) {
        while (*p == ' ') p++;
        if (!*p) break;
        
        // Handle quoted strings
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[i++] = p;
            while (*p && *p != quote) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[i++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    
    args->buffer = buf;
    args->argv = argv;
    args->argc = argc;
    
    return 0;
}

// Free parsed args
static void free_args(parsed_args_t *args)
{
    if (args) {
        free(args->argv);
        free(args->buffer);
        args->argv = NULL;
        args->buffer = NULL;
        args->argc = 0;
    }
}

// Run an ELF file
static int run_elf(const char *path, int argc, char **argv)
{
#if !CONFIG_ELF_LOADER
    (void)path;
    (void)argc;
    (void)argv;
    if (!s_warned_no_elf_loader) {
        printf("external ELF apps are not supported on this board\n");
        s_warned_no_elf_loader = 1;
    }
    return 1;
#else
    ESP_LOGI(TAG, "Loading ELF: %s", path);
    
    // Read entire file into memory
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Cannot open: %s\n", path);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0) {
        printf("Invalid file: %s\n", path);
        fclose(f);
        return -1;
    }
    
    uint8_t *elf_data = heap_caps_malloc((size_t)file_size, MALLOC_CAP_8BIT);
    if (!elf_data) {
        elf_data = malloc((size_t)file_size);
    }
    if (!elf_data) {
        printf("Out of memory (%ld bytes needed)\n", file_size);
        fclose(f);
        return -1;
    }
    
    size_t bytes_read = fread(elf_data, 1, file_size, f);
    fclose(f);
    
    if (bytes_read != (size_t)file_size) {
        printf("Read error\n");
        free(elf_data);
        return -1;
    }
    
    ESP_LOGI(TAG, "Loaded %ld bytes, initializing ELF loader", file_size);
    
    // Initialize and relocate ELF
    esp_elf_t elf;
    int ret;
    
    ret = esp_elf_init(&elf);
    if (ret < 0) {
        printf("ELF init failed: %d\n", ret);
        free(elf_data);
        return ret;
    }
    
    ret = esp_elf_relocate(&elf, elf_data);
    if (ret < 0) {
        printf("ELF relocate failed: %d\n", ret);
        esp_elf_deinit(&elf);
        free(elf_data);
        return ret;
    }
    
    ESP_LOGI(TAG, "Executing with %d args", argc);
    
    // Execute - pass argc/argv like a normal main()
    ret = esp_elf_request(&elf, 0, argc, argv);
    
    ESP_LOGI(TAG, "ELF returned: %d", ret);
    
    esp_elf_deinit(&elf);
    free(elf_data);
    
    return ret;
#endif
}

// Sentinel value meaning "command not found as external"
#define EXEC_NOT_FOUND INT_MIN

static void print_command_help(const esp_console_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    const breezybox_help_entry_t *entry = breezybox_find_help_entry(cmd->command);
    if (entry) {
        printf("Usage: %s\n", entry->usage ? entry->usage : cmd->command);
        if (entry->summary && entry->summary[0]) {
            printf("%s\n", entry->summary);
        }
        if (entry->options && entry->options[0]) {
            printf("\nOptions:\n%s\n", entry->options);
        }
        if (entry->examples && entry->examples[0]) {
            printf("\nExamples:\n%s\n", entry->examples);
        }
        return;
    }

    printf("Usage: %s", cmd->command);
    if (cmd->hint && cmd->hint[0]) {
        printf(" %s", cmd->hint);
    }
    printf("\n");
    if (cmd->help && cmd->help[0]) {
        printf("%s\n", cmd->help);
    }
}

static int run_builtin_command(const char *cmdline)
{
    parsed_args_t args;
    memset(&args, 0, sizeof(args));

    if (parse_args(cmdline, &args) == 0 && args.argc > 1) {
        const char *help_arg = args.argv[1];
        if ((strcmp(help_arg, "-h") == 0 || strcmp(help_arg, "--help") == 0)) {
            const esp_console_cmd_t *cmd = breezybox_find_command(args.argv[0]);
            if (cmd) {
                print_command_help(cmd);
                free_args(&args);
                return 0;
            }
        }
    }
    free_args(&args);

    int ret = 0;
    esp_err_t err = esp_console_run(cmdline, &ret);

    if (err == ESP_ERR_NOT_FOUND) {
        if (parse_args(cmdline, &args) == 0 && args.argc > 0) {
            printf("command not found: %s\n", args.argv[0]);
            free_args(&args);
        } else {
            printf("command not found\n");
        }
        return 127;
    }

    if (err != ESP_OK) {
        printf("command failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    return ret;
}

// Try to run as external command (ELF binary)
// Returns EXEC_NOT_FOUND if not found, otherwise returns ELF's return code
static int try_run_external(const char *cmdline)
{
    parsed_args_t args;
    
    if (parse_args(cmdline, &args) != 0 || args.argc == 0) {
        return EXEC_NOT_FOUND;
    }
    
    // Find executable
    char *exe_path = find_executable(args.argv[0]);
    if (!exe_path) {
        free_args(&args);
        return EXEC_NOT_FOUND;  // Not found
    }
    
    // Check if it's an ELF
    if (!is_elf_file(exe_path)) {
        free(exe_path);
        free_args(&args);
        return EXEC_NOT_FOUND;  // Not an ELF
    }
    
    int ret = run_elf(exe_path, args.argc, args.argv);
    
    free(exe_path);
    free_args(&args);
    
    return ret;
}

// Execute with output redirect using temp file
static int exec_with_output_redirect(const char *cmd, const char *outfile, int append)
{
    int ret = 0;
    
    esp_log_set_vprintf(null_vprintf);
    
    FILE *old_stdout = stdout;
    FILE *tmp = fopen(TEMP_OUT_FILE, "w");
    if (!tmp) {
        esp_log_set_vprintf(s_orig_vprintf);
        printf("Cannot create temp file\n");
        return -1;
    }
    
    // Swap stdout
    stdout = tmp;
    
    // Try external first, then builtin
    ret = try_run_external(cmd);
    if (ret == EXEC_NOT_FOUND) {
        ret = run_builtin_command(cmd);
    }
    fflush(stdout);
    
    // Restore stdout
    fclose(tmp);
    stdout = old_stdout;
    esp_log_set_vprintf(s_orig_vprintf);
    
    // Copy temp to destination
    FILE *src = fopen(TEMP_OUT_FILE, "r");
    if (!src) return ret;
    
    FILE *dst = fopen(outfile, append ? "a" : "w");
    if (!dst) {
        fclose(src);
        unlink(TEMP_OUT_FILE);
        return -1;
    }
    
    char buf[128];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    
    fclose(src);
    fclose(dst);
    unlink(TEMP_OUT_FILE);
    
    return ret;
}

static int exec_with_input_redirect(const char *cmd, const char *infile)
{
    int ret = 0;
    
    FILE *old_stdin = stdin;
    FILE *in = fopen(infile, "r");
    if (!in) {
        printf("Cannot open: %s\n", infile);
        return -1;
    }
    
    stdin = in;
    
    // Try external first, then builtin
    ret = try_run_external(cmd);
    if (ret == EXEC_NOT_FOUND) {
        ret = run_builtin_command(cmd);
    }
    
    fclose(in);
    stdin = old_stdin;
    
    return ret;
}

static int breezybox_exec_single(const char *cmdline)
{
    if (!cmdline || !*cmdline) return 0;
    
    // Make a working copy
    char *line = strdup(cmdline);
    if (!line) return -1;
    
    char *cmd1 = NULL, *cmd2 = NULL;
    char *infile = NULL, *outfile = NULL;
    int append = 0;
    int ret = 0;
    
    // Check for pipe first
    char *pipe_pos = strchr(line, '|');
    if (pipe_pos) {
        *pipe_pos = '\0';
        cmd1 = line;
        cmd2 = pipe_pos + 1;
        
        // Trim whitespace
        cmd1 = trim_in_place(cmd1);
        cmd2 = trim_in_place(cmd2);
        
        // Execute: cmd1 > tmpfile; cmd2 < tmpfile
        exec_with_output_redirect(cmd1, TEMP_PIPE_FILE, 0);
        ret = exec_with_input_redirect(cmd2, TEMP_PIPE_FILE);
        unlink(TEMP_PIPE_FILE);
        
        free(line);
        return ret;
    }
    
    // Check for output redirect (>> or >)
    char *redir_out = strstr(line, ">>");
    if (redir_out) {
        append = 1;
        *redir_out = '\0';
        outfile = redir_out + 2;
    } else {
        redir_out = strchr(line, '>');
        if (redir_out) {
            *redir_out = '\0';
            outfile = redir_out + 1;
        }
    }
    
    // Check for input redirect
    char *redir_in = strchr(line, '<');
    if (redir_in) {
        *redir_in = '\0';
        infile = redir_in + 1;
    }
    
    // Trim whitespace from all parts
    cmd1 = trim_in_place(line);

    if (outfile) {
        outfile = trim_in_place(outfile);
    }

    if (infile) {
        infile = trim_in_place(infile);
    }
    
    // Resolve relative paths for redirects
    char resolved_in[BREEZYBOX_MAX_PATH * 2];
    char resolved_out[BREEZYBOX_MAX_PATH * 2];
    
    if (infile && infile[0] != '/') {
        breezybox_resolve_path(infile, resolved_in, sizeof(resolved_in));
        infile = resolved_in;
    }
    if (outfile && outfile[0] != '/') {
        breezybox_resolve_path(outfile, resolved_out, sizeof(resolved_out));
        outfile = resolved_out;
    }
    
    // Execute with appropriate redirects
    if (outfile && infile) {
        // Both redirects - output takes precedence for now
        ret = exec_with_output_redirect(cmd1, outfile, append);
    } else if (outfile) {
        ret = exec_with_output_redirect(cmd1, outfile, append);
    } else if (infile) {
        ret = exec_with_input_redirect(cmd1, infile);
    } else {
        // No redirects - try external first, then builtin
        ret = try_run_external(cmd1);
        if (ret == EXEC_NOT_FOUND) {
            ret = run_builtin_command(cmd1);
        }
    }
    
    free(line);
    return ret;
}

// Parse and execute a command with simple chaining, pipe, and redirect support
int breezybox_exec(const char *cmdline)
{
    if (!cmdline || !*cmdline) {
        return 0;
    }

    char *line = strdup(cmdline);
    if (!line) {
        return -1;
    }

    int last_ret = 0;
    char *cursor = line;

    while (1) {
        char *and_pos = find_top_level_operator(cursor, "&&");
        char *semi_pos = find_top_level_operator(cursor, ";");
        char *sep = NULL;
        int is_and = 0;

        if (and_pos && (!semi_pos || and_pos < semi_pos)) {
            sep = and_pos;
            is_and = 1;
        } else if (semi_pos) {
            sep = semi_pos;
        }

        if (!sep) {
            char *segment = trim_in_place(cursor);
            if (*segment) {
                last_ret = breezybox_exec_single(segment);
            }
            break;
        }

        *sep = '\0';
        char *segment = trim_in_place(cursor);
        if (*segment) {
            last_ret = breezybox_exec_single(segment);
        } else {
            last_ret = 0;
        }

        cursor = sep + (is_and ? 2 : 1);
        if (is_and && last_ret != 0) {
            while (1) {
                char *next_sep = find_top_level_operator(cursor, ";");
                if (!next_sep) {
                    free(line);
                    return last_ret;
                }
                cursor = next_sep + 1;
                char *remaining = trim_in_place(cursor);
                if (*remaining) {
                    break;
                }
            }
        }
    }

    free(line);
    return last_ret;
}

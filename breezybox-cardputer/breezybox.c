#include "breezybox.h"
#include "breezy_vfs.h"
#include "breezy_cmd.h"
#include "breezy_exec.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define INIT_SCRIPT BREEZYBOX_MOUNT_POINT "/init.sh"
#define DEFAULT_INIT "echo Welcome to BreezyBox!\n"
#define HISTORY_FILE BREEZYBOX_MOUNT_POINT "/.history"

static esp_console_repl_t *s_repl = NULL;
static const esp_console_cmd_t *s_extra_cmds = NULL;
static size_t s_extra_cmd_count = 0;
static const breezybox_help_entry_t *s_extra_help_entries = NULL;
static size_t s_extra_help_count = 0;

static const breezybox_help_entry_t s_core_help[] = {
    { "echo", "echo [text...]", "Print arguments to stdout.", NULL, "echo Hello world" },
    { "pwd", "pwd", "Print the current working directory.", NULL, "pwd" },
    { "cd", "cd [path]", "Change directory. With no path, go to /root.", NULL, "cd /sd\ncd ..\ncd" },
    { "ls", "ls [-a] [path]", "List directory contents.", "-a  show hidden files", "ls\nls -a /sd" },
    { "cat", "cat [file|-]", "Print file contents or stdin.", NULL, "cat /root/init.sh\ncat < /root/init.sh" },
    { "head", "head [-n N] <file>", "Show the first lines of a file.", "-n N  number of lines", "head -n 20 notes.txt" },
    { "tail", "tail [-n N] <file>", "Show the last lines of a file.", "-n N  number of lines", "tail -n 20 log.txt" },
    { "more", "more <file>", "Paginate file contents.", "Space next page\nEnter next line\nq quit", "more README.txt" },
    { "wc", "wc [-lwc] <file>", "Count lines, words, and characters.", "-l  lines only\n-w  words only\n-c  chars only", "wc notes.txt\nwc -l notes.txt" },
    { "touch", "touch <file...>", "Create empty files or update timestamps.", NULL, "touch notes.txt" },
    { "chmod", "chmod <mode> <file...>", "Change file permissions using octal mode.", NULL, "chmod 644 notes.txt" },
    { "ln", "ln <target> <link>", "Create a hard link.", NULL, "ln file copy" },
    { "find", "find [path] [-name pattern] [-type f|d]", "Recursively search for files and directories.", NULL, "find /sd -name '*.txt'\nfind . -type d" },
    { "grep", "grep [-i] [-n] <pattern> [file]", "Search lines for a pattern.", "-i  ignore case\n-n  show line numbers", "grep wifi boot.log\ngrep -n main /root/init.sh" },
    { "sed", "sed 's/old/new/[g]' [file]", "Stream-edit text using simple substitution.", NULL, "sed 's/foo/bar/g' file.txt" },
    { "sort", "sort [-r] [-u] [file]", "Sort lines of text.", "-r  reverse\n-u  unique", "sort names.txt\nsort -u names.txt" },
    { "uniq", "uniq [-c] [file]", "Filter adjacent duplicate lines.", "-c  show counts", "uniq sorted.txt\nuniq -c sorted.txt" },
    { "cut", "cut [-d delim] [-f field] [file]", "Extract a delimited field.", "-d delim  field delimiter\n-f field  1-based field number", "cut -d , -f 2 data.csv" },
    { "tr", "tr <set1> <set2> [file]", "Translate characters.", NULL, "tr abc ABC < file.txt" },
    { "mkdir", "mkdir <dir>", "Create a directory.", NULL, "mkdir /sd/projects" },
    { "cp", "cp <src> <dst>", "Copy a file.", NULL, "cp a.txt /sd/a.txt" },
    { "mv", "mv <src> <dst>", "Move or rename a file.", NULL, "mv old.txt new.txt" },
    { "rm", "rm [-r] <file...>", "Remove files or directories.", "-r  remove recursively", "rm file.txt\nrm -r /sd/olddir" },
    { "df", "df", "Show filesystem usage.", NULL, "df" },
    { "du", "du [-s] [path]", "Show disk usage.", "-s  summary only", "du /sd\n du -s /root" },
    { "free", "free", "Show memory usage.", NULL, "free" },
    { "date", "date [\"YYYY-MM-DD HH:MM:SS\"]", "Show or set date and time.", NULL, "date\ndate \"2026-05-05 12:34:56\"" },
    { "sleep", "sleep <seconds>", "Pause for a number of seconds.", NULL, "sleep 1\nsleep 0.5" },
    { "which", "which <command...>", "Show where a command resolves.", NULL, "which ls\nwhich vi" },
    { "type", "type <command...>", "Describe a command as builtin or external.", NULL, "type ls\ntype vi" },
    { "ps", "ps", "List FreeRTOS tasks.", NULL, "ps" },
    { "kill", "kill <task-name>", "Delete a FreeRTOS task by name.", NULL, "kill httpd" },
    { "uname", "uname [-a]", "Show system information.", "-a  show full information", "uname\nuname -a" },
    { "env", "env", "Print environment variables.", NULL, "env" },
    { "export", "export [NAME=VALUE...]", "Set or print environment variables.", NULL, "export FOO=bar\nexport FOO" },
    { "unset", "unset <name...>", "Remove environment variables.", NULL, "unset FOO" },
    { "history", "history", "Show command history.", NULL, "history" },
    { "source", "source <script>", "Run a shell script.", NULL, "source /root/init.sh" },
    { ".", ". <script>", "Run a shell script.", NULL, ". /root/init.sh" },
    { "true", "true", "Return success.", NULL, "true" },
    { "false", "false", "Return failure.", NULL, "false" },
    { "test", "test EXPR", "Evaluate simple file and string conditions.", NULL, "test -f file.txt\n[ -d /sd ]" },
    { "[", "[ EXPR ]", "Evaluate simple file and string conditions.", NULL, "[ -d /sd ]" },
    { "sync", "sync", "Flush filesystem writes.", NULL, "sync" },
    { "clear", "clear", "Clear the screen.", NULL, "clear" },
    { "sh", "sh <script>", "Run a shell script file.", NULL, "sh /root/init.sh" },
    { "eget", "eget <user/repo>", "Download ELF files from a GitHub release.", NULL, "eget user/repo" },
    { "wifi", "wifi <scan|connect|disconnect|status|forget>", "WiFi management commands.", "connect [ssid] [password]\nIf ssid is omitted, use saved credentials.", "wifi scan\nwifi connect MySSID secret\nwifi connect\nwifi status" },
    { "ping", "ping [-c count] [-W timeout_ms] <host>", "Send ICMP echo requests to a host.", "-c count       number of packets\n-W timeout_ms  timeout per packet in milliseconds", "ping example.com\nping -c 2 1.1.1.1\nping -W 2000 google.com" },
    { "lua", "lua [guide|shell|-e <chunk>|<script.lua> [args...]]", "Run embedded Lua scripts, open a Lua REPL, or show the on-device guide.", "guide         open the on-device Lua guide\nshell         open the interactive Lua REPL\n-e <chunk>    run a one-line Lua chunk\n<script.lua>  run a Lua script from shared storage", "lua guide\nlua\nlua shell\nlua -e 'print(2+2)'\nlua /root/apps/demo.lua" },
    { "ssh", "ssh [-p port] [-l user] [-pw password] <host|alias> [command...]", "Open an SSH session or run a remote command.", "-p port       remote SSH port\n-l user       remote username\n-pw password  password auth for this run\nCtrl+Q        disconnect interactive session\nSaved aliases resolve via sshcfg", "ssh user@example.com\nssh pi2w\nssh -p 2222 host uname -a" },
    { "sshcfg", "sshcfg <add|list|show|rm> ...", "Manage saved SSH host aliases.", "add <name> <host|user@host> [-l user] [-p port] [-pw password]\nlist\nshow <name>\nrm <name>", "sshcfg add pi2w pi@pi2w\nsshcfg add lab labhost -l pi -pw secret\nsshcfg list\nssh pi2w" },
    { "scp", "scp [-P port] [-l user] [-pw password] <src> <dst>", "Copy a single file to or from an SSH host.", "-P port       remote SSH port\n-l user       remote username\n-pw password  password auth for this run\nRemote paths use [user@]host:/path", "scp /root/file.txt user@example.com:/tmp/file.txt\nscp user@example.com:/tmp/file.txt /sd/file.txt" },
    { "httpd", "httpd [dir] [-p port]", "Start the HTTP file server.", "-p port  listen port", "httpd /sd -p 8080" },
};

static void add_command_matches(const esp_console_cmd_t *cmds, size_t count, const char *buf,
                                linenoiseCompletions *lc)
{
    size_t prefix_len;

    if (!cmds || !buf) {
        return;
    }

    prefix_len = strlen(buf);
    for (size_t i = 0; i < count; ++i) {
        const char *name = cmds[i].command;
        if (!name) {
            continue;
        }
        if (prefix_len > strlen(name) || strncmp(name, buf, prefix_len) != 0) {
            continue;
        }
        linenoiseAddCompletion(lc, name);
    }
}

const esp_console_cmd_t *breezybox_find_command(const char *cmd)
{
    size_t core_count = 0;
    const esp_console_cmd_t *core_cmds = breezybox_get_core_commands(&core_count);

    for (size_t i = 0; i < core_count; ++i) {
        if (strcmp(core_cmds[i].command, cmd) == 0) {
            return &core_cmds[i];
        }
    }

    for (size_t i = 0; i < s_extra_cmd_count; ++i) {
        if (strcmp(s_extra_cmds[i].command, cmd) == 0) {
            return &s_extra_cmds[i];
        }
    }

    return NULL;
}

const breezybox_help_entry_t *breezybox_find_help_entry(const char *cmd)
{
    for (size_t i = 0; i < sizeof(s_core_help) / sizeof(s_core_help[0]); ++i) {
        if (strcmp(s_core_help[i].command, cmd) == 0) {
            return &s_core_help[i];
        }
    }

    for (size_t i = 0; i < s_extra_help_count; ++i) {
        if (strcmp(s_extra_help_entries[i].command, cmd) == 0) {
            return &s_extra_help_entries[i];
        }
    }

    return NULL;
}

static bool command_supports_path_completion(const char *cmd)
{
    static const char *const s_path_cmds[] = {
        "cat", "cd", "cp", "du", "gunzip", "gzip", "head", "httpd", "ls",
        "mkdir", "more", "mv", "rm", "sh", "source", ".", "tail", "touch",
        "chmod", "ln", "find", "grep", "sed", "sort", "uniq", "cut", "tr",
        "vi", "wc", "wget", "scp", "lua"
    };

    for (size_t i = 0; i < sizeof(s_path_cmds) / sizeof(s_path_cmds[0]); ++i) {
        if (strcmp(cmd, s_path_cmds[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void add_path_completion(const char *buf, size_t token_start, const char *candidate, bool is_dir,
                                linenoiseCompletions *lc)
{
    char completion[2 * BREEZYBOX_MAX_PATH + 32];

    if (token_start >= sizeof(completion)) {
        return;
    }

    memcpy(completion, buf, token_start);
    completion[token_start] = '\0';
    strlcat(completion, candidate, sizeof(completion));
    if (is_dir) {
        strlcat(completion, "/", sizeof(completion));
    }
    linenoiseAddCompletion(lc, completion);
}

static bool should_complete_hint_token(const char *token, bool force)
{
    if (!token || !token[0]) {
        return false;
    }
    if (force || token[0] == '-') {
        return true;
    }
    return true;
}

static void add_hint_completion(const char *buf, size_t token_start, const char *candidate,
                                linenoiseCompletions *lc)
{
    char completion[2 * BREEZYBOX_MAX_PATH + 32];

    if (token_start >= sizeof(completion)) {
        return;
    }

    memcpy(completion, buf, token_start);
    completion[token_start] = '\0';
    strlcat(completion, candidate, sizeof(completion));
    linenoiseAddCompletion(lc, completion);
}

static void maybe_add_hint_candidate(const char *buf, size_t token_start, const char *token,
                                     const char *prefix, bool force, linenoiseCompletions *lc)
{
    size_t token_len = strlen(token);
    size_t prefix_len = strlen(prefix);

    if (!should_complete_hint_token(token, force)) {
        return;
    }
    if (prefix_len > token_len || strncmp(token, prefix, prefix_len) != 0) {
        return;
    }
    add_hint_completion(buf, token_start, token, lc);
}

static void complete_hint_argument(const char *buf, linenoiseCompletions *lc)
{
    const char *space = strchr(buf, ' ');
    const char *token = NULL;
    const esp_console_cmd_t *cmd = NULL;
    char cmd_name[32];
    char current[64];
    char token_buf[64];
    size_t cmd_len;
    size_t token_start;
    const char *hint;

    if (!space) {
        return;
    }

    cmd_len = (size_t)(space - buf);
    if (cmd_len == 0 || cmd_len >= sizeof(cmd_name)) {
        return;
    }

    memcpy(cmd_name, buf, cmd_len);
    cmd_name[cmd_len] = '\0';

    cmd = breezybox_find_command(cmd_name);
    if (!cmd || !cmd->hint || !cmd->hint[0]) {
        return;
    }

    token = buf + strlen(buf);
    while (token > space + 1 && !isspace((unsigned char)token[-1])) {
        token--;
    }
    token_start = (size_t)(token - buf);
    strlcpy(current, token, sizeof(current));

    hint = cmd->hint;
    while (*hint) {
        size_t len = 0;
        bool force = false;
        const char *token_start_ptr;
        const char *token_end_ptr;
        const char *prev = hint;

        while (*hint && !isalnum((unsigned char)*hint) && *hint != '-' && *hint != '_') {
            hint++;
        }
        if (!*hint) {
            break;
        }

        token_start_ptr = hint;

        while (hint[len] &&
               (isalnum((unsigned char)hint[len]) || hint[len] == '-' || hint[len] == '_')) {
            if (len + 1 < sizeof(token_buf)) {
                token_buf[len] = hint[len];
            }
            len++;
        }

        token_end_ptr = hint + len;

        while (prev > cmd->hint && isspace((unsigned char)prev[-1])) {
            --prev;
        }
        if (token_start_ptr > cmd->hint && token_start_ptr[-1] == '|') {
            force = true;
        }
        if (*token_end_ptr == '|') {
            force = true;
        }

        if (len > 0 && len < sizeof(token_buf)) {
            token_buf[len] = '\0';
            maybe_add_hint_candidate(buf, token_start, token_buf, current, force, lc);
        }
        hint += len;
    }
}

static void complete_path_argument(const char *buf, linenoiseCompletions *lc)
{
    const char *space = strchr(buf, ' ');
    const char *token = NULL;
    const char *slash = NULL;
    char cmd[32];
    char prefix[NAME_MAX + 1];
    char dir_part[BREEZYBOX_MAX_PATH * 2 + 2];
    char resolved_dir[BREEZYBOX_MAX_PATH * 2 + 2];
    size_t cmd_len;
    size_t token_start;

    if (!space) {
        return;
    }

    cmd_len = (size_t)(space - buf);
    if (cmd_len == 0 || cmd_len >= sizeof(cmd)) {
        return;
    }

    memcpy(cmd, buf, cmd_len);
    cmd[cmd_len] = '\0';
    if (!command_supports_path_completion(cmd)) {
        return;
    }

    token = buf + strlen(buf);
    while (token > space + 1 && !isspace((unsigned char)token[-1])) {
        token--;
    }
    token_start = (size_t)(token - buf);

    slash = strrchr(token, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - token) + 1;
        if (dir_len >= sizeof(dir_part)) {
            return;
        }
        memcpy(dir_part, token, dir_len);
        dir_part[dir_len] = '\0';
        strlcpy(prefix, slash + 1, sizeof(prefix));
        if (dir_part[0] == '/') {
            strlcpy(resolved_dir, dir_part, sizeof(resolved_dir));
        } else if (!breezybox_resolve_path(dir_part, resolved_dir, sizeof(resolved_dir))) {
            return;
        }
    } else {
        dir_part[0] = '\0';
        strlcpy(prefix, token, sizeof(prefix));
        if (strlcpy(resolved_dir, breezybox_cwd(), sizeof(resolved_dir)) >= sizeof(resolved_dir)) {
            return;
        }
    }

    DIR *dir = opendir(resolved_dir);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[BREEZYBOX_MAX_PATH * 2 + 2];
        struct stat st;
        bool is_dir = false;

        if (entry->d_name[0] == '.' && prefix[0] != '.') {
            continue;
        }
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }

        if (strcmp(resolved_dir, "/") == 0) {
            if (strlcpy(full_path, "/", sizeof(full_path)) >= sizeof(full_path) ||
                strlcat(full_path, entry->d_name, sizeof(full_path)) >= sizeof(full_path)) {
                continue;
            }
        } else {
            if (strlcpy(full_path, resolved_dir, sizeof(full_path)) >= sizeof(full_path) ||
                strlcat(full_path, "/", sizeof(full_path)) >= sizeof(full_path) ||
                strlcat(full_path, entry->d_name, sizeof(full_path)) >= sizeof(full_path)) {
                continue;
            }
        }

        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            is_dir = true;
        }

        if (slash) {
            char candidate[BREEZYBOX_MAX_PATH * 2 + 2];
            if (strlcpy(candidate, dir_part, sizeof(candidate)) >= sizeof(candidate) ||
                strlcat(candidate, entry->d_name, sizeof(candidate)) >= sizeof(candidate)) {
                continue;
            }
            add_path_completion(buf, token_start, candidate, is_dir, lc);
        } else {
            add_path_completion(buf, token_start, entry->d_name, is_dir, lc);
        }
    }

    closedir(dir);
}

static void breezybox_get_completion(const char *buf, linenoiseCompletions *lc)
{
    if (strchr(buf, ' ') == NULL) {
        size_t core_count = 0;
        const esp_console_cmd_t *core_cmds = breezybox_get_core_commands(&core_count);

        add_command_matches(core_cmds, core_count, buf, lc);
        add_command_matches(s_extra_cmds, s_extra_cmd_count, buf, lc);
        return;
    }

    complete_hint_argument(buf, lc);
    if (lc->len == 0) {
        complete_path_argument(buf, lc);
    }
}

// ============ Short Commands (inline) ============

int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], (i < argc - 1) ? " " : "");
    }
    printf("\n");
    return 0;
}

int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%s\n", breezybox_cwd());
    return 0;
}

int cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        if (breezybox_set_cwd(BREEZYBOX_MOUNT_POINT) != 0) {
            printf("cd: %s: No such directory\n", BREEZYBOX_MOUNT_POINT);
            return 1;
        }
        return 0;
    }
    if (breezybox_set_cwd(argv[1]) != 0) {
        printf("cd: %s: No such directory\n", argv[1]);
        return 1;
    }
    return 0;
}

int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\033[2J\033[H");  // Clear screen + cursor home
    return 0;
}

int cmd_free(int argc, char **argv)
{
    (void)argc; (void)argv;
    
    // Internal SRAM (DMA-capable, needed for WiFi/BT)
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_exec = heap_caps_get_free_size(MALLOC_CAP_EXEC);
    size_t largest_exec = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
    
    printf("SRAM:  %6uK free, %6uK min, %6uK total, %6uK largest\n", 
           (unsigned)(free_internal / 1024),
           (unsigned)(min_internal / 1024),
           (unsigned)(total_internal / 1024),
           (unsigned)(largest_internal / 1024));
    printf("EXEC:  %6uK free, %6uK largest\n",
           (unsigned)(free_exec / 1024),
           (unsigned)(largest_exec / 1024));
    
#ifdef CONFIG_SPIRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    
    printf("PSRAM: %6uK free, %6uK min, %6uK total\n",
           (unsigned)(free_psram / 1024),
           (unsigned)(min_psram / 1024),
           (unsigned)(total_psram / 1024));
#endif
    
    return 0;
}

// Run a script file with redirect support
int cmd_sh(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sh <script>\n");
        return 1;
    }
    
    char resolved[BREEZYBOX_MAX_PATH * 2 + 2];
    const char *path = argv[1];
    if (path[0] != '/') {
        if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
            printf("sh: path too long\n");
            return 1;
        }
        path = resolved;
    }
    
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("sh: %s: No such file\n", argv[1]);
        return 1;
    }
    
    char line[256];
    int ret = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) {
            line[--len] = '\0';
        }
        
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        
        ret = breezybox_exec(p);
    }
    fclose(f);
    return ret;
}

// ============ Init Script ============

static void create_default_init(void)
{
    FILE *f = fopen(INIT_SCRIPT, "w");
    if (f) {
        fputs(DEFAULT_INIT, f);
        fclose(f);
    }
}

static void run_init_script(void)
{
    FILE *f = fopen(INIT_SCRIPT, "r");
    if (!f) {
        create_default_init();
        f = fopen(INIT_SCRIPT, "r");
        if (!f) return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) {
            line[--len] = '\0';
        }
        
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        // Execute with redirect support
        breezybox_exec(p);
    }
    fclose(f);
}

// ============ Command Registration ============

static const esp_console_cmd_t s_breezybox_cmds[] = {
    { .command = "echo",  .help = "Print arguments",         .hint = "[text...]", .func = &cmd_echo  },
    { .command = "pwd",   .help = "Print working directory", .hint = NULL,        .func = &cmd_pwd   },
    { .command = "cd",    .help = "Change directory",        .hint = "[path]",    .func = &cmd_cd    },
    { .command = "ls",    .help = "List directory",          .hint = "[path]",    .func = &cmd_ls    },
    { .command = "cat",   .help = "Print file contents",     .hint = "[file|-]",  .func = &cmd_cat   },
    { .command = "head",  .help = "Show first lines",        .hint = "[-n N] <file>", .func = &cmd_head },
    { .command = "tail",  .help = "Show last lines",         .hint = "[-n N] <file>", .func = &cmd_tail },
    { .command = "more",  .help = "Paginate file",           .hint = "<file>",    .func = &cmd_more  },
    { .command = "wc",    .help = "Count lines/words/chars", .hint = "[-lwc] <file>", .func = &cmd_wc },
    { .command = "touch", .help = "Create empty file",       .hint = "<file...>", .func = &cmd_touch },
    { .command = "chmod", .help = "Change file mode",        .hint = "<mode> <file...>", .func = &cmd_chmod },
    { .command = "ln",    .help = "Create hard link",        .hint = "<target> <link>", .func = &cmd_ln },
    { .command = "find",  .help = "Find files",              .hint = "[path] [-name pattern] [-type f|d]", .func = &cmd_find },
    { .command = "grep",  .help = "Search text",             .hint = "[-i] [-n] <pattern> [file]", .func = &cmd_grep },
    { .command = "sed",   .help = "Stream edit text",        .hint = "'s/old/new/[g]' [file]", .func = &cmd_sed },
    { .command = "sort",  .help = "Sort lines",              .hint = "[-r] [-u] [file]", .func = &cmd_sort },
    { .command = "uniq",  .help = "Filter duplicate lines",  .hint = "[-c] [file]", .func = &cmd_uniq },
    { .command = "cut",   .help = "Extract fields",          .hint = "[-d delim] [-f field] [file]", .func = &cmd_cut },
    { .command = "tr",    .help = "Translate chars",         .hint = "<set1> <set2> [file]", .func = &cmd_tr },
    { .command = "mkdir", .help = "Create directory",        .hint = "<dir>",     .func = &cmd_mkdir },
    { .command = "cp",    .help = "Copy file",               .hint = "<src> <dst>", .func = &cmd_cp  },
    { .command = "mv",    .help = "Move/rename file",        .hint = "<src> <dst>", .func = &cmd_mv  },
    { .command = "rm",    .help = "Remove file/directory",   .hint = "[-r] <file...>", .func = &cmd_rm },
    { .command = "df",    .help = "Show disk free space",    .hint = NULL,        .func = &cmd_df    },
    { .command = "du",    .help = "Show disk usage",         .hint = "[-s] [path]", .func = &cmd_du  },
    { .command = "free",  .help = "Show memory usage",       .hint = NULL,        .func = &cmd_free  },
    { .command = "date",  .help = "Show/set date and time",  .hint = "[\"YYYY-MM-DD HH:MM:SS\"]", .func = &cmd_date },
    { .command = "sleep", .help = "Pause for seconds",       .hint = "<seconds>", .func = &cmd_sleep },
    { .command = "which", .help = "Locate command",          .hint = "<command...>", .func = &cmd_which },
    { .command = "type",  .help = "Describe command",        .hint = "<command...>", .func = &cmd_type },
    { .command = "ps",    .help = "List tasks",              .hint = NULL,        .func = &cmd_ps },
    { .command = "kill",  .help = "Delete task by name",     .hint = "<task-name>", .func = &cmd_kill },
    { .command = "uname", .help = "System information",      .hint = "[-a]",      .func = &cmd_uname },
    { .command = "env",   .help = "Print environment",       .hint = NULL,        .func = &cmd_env },
    { .command = "export", .help = "Set environment",        .hint = "[NAME=VALUE...]", .func = &cmd_export },
    { .command = "unset", .help = "Unset environment",       .hint = "<name...>", .func = &cmd_unset },
    { .command = "history", .help = "Show command history",  .hint = NULL,        .func = &cmd_history },
    { .command = "source", .help = "Run script file",        .hint = "<script>",  .func = &cmd_source },
    { .command = ".",     .help = "Run script file",         .hint = "<script>",  .func = &cmd_source },
    { .command = "true",  .help = "Return success",          .hint = NULL,        .func = &cmd_true },
    { .command = "false", .help = "Return failure",          .hint = NULL,        .func = &cmd_false },
    { .command = "test",  .help = "Evaluate conditions",     .hint = "EXPR",      .func = &cmd_test },
    { .command = "[",     .help = "Evaluate conditions",     .hint = "EXPR ]",    .func = &cmd_test },
    { .command = "sync",  .help = "Flush filesystem writes", .hint = NULL,        .func = &cmd_sync },
    { .command = "clear", .help = "Clear screen",            .hint = NULL,        .func = &cmd_clear },
    { .command = "sh",    .help = "Run script file",         .hint = "<script>",  .func = &cmd_sh    },
    { .command = "eget",  .help = "Download ELF from GitHub", .hint = "<user/repo>", .func = &cmd_eget },
    { .command = "wifi",  .help = "WiFi commands",           .hint = "<scan|connect|disconnect|status|forget>", .func = &cmd_wifi },
    { .command = "ping",  .help = "Ping a host",             .hint = "[-c count] [-W timeout_ms] <host>", .func = &cmd_ping },
    { .command = "lua",   .help = "Run embedded Lua",        .hint = "[guide|shell|-e <chunk>|<script.lua> [args...]]", .func = &cmd_lua },
    { .command = "ssh",   .help = "SSH client",              .hint = "[-p port] [-l user] [-pw password] <host|alias> [command...]", .func = &cmd_ssh },
    { .command = "sshcfg", .help = "Manage saved SSH hosts", .hint = "<add|list|show|rm> ...", .func = &cmd_sshcfg },
    { .command = "scp",   .help = "SSH copy client",         .hint = "[-P port] [-l user] [-pw password] <src> <dst>", .func = &cmd_scp },
    { .command = "httpd", .help = "HTTP file server",        .hint = "[dir] [-p port]", .func = &cmd_httpd },
};

const esp_console_cmd_t *breezybox_get_core_commands(size_t *count)
{
    if (count) {
        *count = sizeof(s_breezybox_cmds) / sizeof(s_breezybox_cmds[0]);
    }
    return s_breezybox_cmds;
}

void breezybox_set_extra_commands(const esp_console_cmd_t *cmds, size_t count)
{
    s_extra_cmds = cmds;
    s_extra_cmd_count = count;
}

void breezybox_set_extra_help_entries(const breezybox_help_entry_t *entries, size_t count)
{
    s_extra_help_entries = entries;
    s_extra_help_count = count;
}

esp_err_t breezybox_register_commands(void)
{
    const esp_console_cmd_t *cmds = s_breezybox_cmds;
    const size_t cmd_count = sizeof(s_breezybox_cmds) / sizeof(s_breezybox_cmds[0]);

    for (size_t i = 0; i < cmd_count; i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ============ Common Init ============

static esp_err_t breezybox_init_common(void)
{
    // Force-export symbols for ELF runtime linking
    breezybox_export_symbols();
    
    // Initialize filesystem
    esp_err_t ret = breezybox_vfs_init();
    if (ret != ESP_OK) {
        printf("BreezyBox: filesystem init failed\n");
        return ret;
    }

    // Initialize exec subsystem (for redirects)
    breezybox_exec_init();

    // Initialize console (for command parsing)
    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    // Register commands
    breezybox_register_commands();

    // Run init script
    run_init_script();

    return ESP_OK;
}

// ============ REPL Implementations ============

// Linenoise-based REPL task for stdio mode
static void stdio_repl_task(void *arg)
{
    // Skip probe for now - our VFS console handles terminal queries internally
    // The probe can cause issues when responses get mixed up
    // linenoiseSetDumbMode(1);  // Uncomment to force dumb mode for debugging
    
    // Setup linenoise with esp_console's completion/hints
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&breezybox_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(100);
    linenoiseHistoryLoad(HISTORY_FILE);
    
    printf("\nType 'help' to get the list of commands.\n");
    
    while (true) {
        char *line = linenoise("$ ");
        
        if (line == NULL) {
            // Ctrl+D or read error, just continue
            continue;
        }
        
        // Skip empty lines
        if (strlen(line) > 0) {
            linenoiseHistoryAdd(line);
            linenoiseHistorySave(HISTORY_FILE);
            breezybox_exec(line);
        }
        
        linenoiseFree(line);
    }
}

esp_err_t breezybox_start_stdio(size_t stack_size, uint32_t priority)
{
    esp_err_t ret = breezybox_init_common();
    if (ret != ESP_OK) return ret;

    xTaskCreatePinnedToCore(stdio_repl_task, "breezy_repl", stack_size, NULL, priority, NULL, 1);
    return ESP_OK;
}

#ifdef ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT
esp_err_t breezybox_start_usb(size_t stack_size, uint32_t priority)
{
    // Initialize filesystem and exec
    esp_err_t ret = breezybox_vfs_init();
    if (ret != ESP_OK) {
        printf("BreezyBox: filesystem init failed\n");
        return ret;
    }
    breezybox_exec_init();

    // Setup USB Serial JTAG REPL (this also initializes console)
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "$ ";
    repl_config.task_stack_size = stack_size;
    repl_config.task_priority = priority;

    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &s_repl));
    
    breezybox_register_commands();
    run_init_script();

    return esp_console_start_repl(s_repl);
}
#endif // ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT

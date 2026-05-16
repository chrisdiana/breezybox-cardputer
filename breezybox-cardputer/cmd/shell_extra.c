#include "breezy_cmd.h"
#include "breezy_vfs.h"
#include "breezybox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_FILE BREEZYBOX_MOUNT_POINT "/.history"
#define EXEC_PATH "/root/bin"
#define EXTRA_PATH_BUF (BREEZYBOX_MAX_PATH * 2 + 2)

extern char **environ;

static const char *skip_path(const char *path)
{
    return (path && path[0]) ? path : ".";
}

static const char *resolve_path_arg(const char *arg, char *buf, size_t size)
{
    if (!arg) {
        return NULL;
    }
    if (arg[0] == '/') {
        return arg;
    }
    return breezybox_resolve_path(arg, buf, size);
}

static int copy_stream(FILE *in, FILE *out)
{
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            return 1;
        }
    }
    fflush(out);
    return ferror(in) ? 1 : 0;
}

static char *find_external_command(const char *name, char *buf, size_t size)
{
    if (!name || !*name) {
        return NULL;
    }

    if (strchr(name, '/')) {
        const char *path = resolve_path_arg(name, buf, size);
        struct stat st;
        if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            return buf;
        }
        return NULL;
    }

    struct stat st;
    if (breezybox_resolve_path(name, buf, size) && stat(buf, &st) == 0 && S_ISREG(st.st_mode)) {
        return buf;
    }

    if (snprintf(buf, size, "%s/%s", EXEC_PATH, name) >= (int)size) {
        return NULL;
    }
    if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) {
        return buf;
    }
    return NULL;
}

static int cat_stream_or_file(const char *filename, const char *label)
{
    char resolved[EXTRA_PATH_BUF];
    FILE *f = stdin;

    if (filename && strcmp(filename, "-") != 0) {
        const char *path = resolve_path_arg(filename, resolved, sizeof(resolved));
        if (!path) {
            printf("cat: path too long\n");
            return 1;
        }
        f = fopen(path, "r");
        if (!f) {
            printf("cat: %s: No such file\n", label ? label : filename);
            return 1;
        }
    }

    int ret = copy_stream(f, stdout);
    if (f != stdin) {
        fclose(f);
    }
    return ret;
}

int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: touch <file...>\n");
        return 1;
    }

    int errors = 0;
    for (int i = 1; i < argc; ++i) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(argv[i], resolved, sizeof(resolved));
        if (!path) {
            printf("touch: %s: path too long\n", argv[i]);
            errors++;
            continue;
        }
        FILE *f = fopen(path, "a");
        if (!f) {
            printf("touch: %s: cannot create\n", argv[i]);
            errors++;
            continue;
        }
        fclose(f);
    }
    return errors ? 1 : 0;
}

int cmd_chmod(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: chmod <mode> <file...>\n");
        return 1;
    }

    char *end = NULL;
    long mode = strtol(argv[1], &end, 8);
    if (end == argv[1] || *end != '\0' || mode < 0 || mode > 0777) {
        printf("chmod: invalid mode: %s\n", argv[1]);
        return 1;
    }

    int errors = 0;
    for (int i = 2; i < argc; ++i) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(argv[i], resolved, sizeof(resolved));
        if (!path || chmod(path, (mode_t)mode) != 0) {
            printf("chmod: cannot change mode of %s\n", argv[i]);
            errors++;
        }
    }
    return errors ? 1 : 0;
}

int cmd_ln(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: ln <target> <link>\n");
        return 1;
    }

    char target_resolved[EXTRA_PATH_BUF];
    char link_resolved[EXTRA_PATH_BUF];
    const char *target = resolve_path_arg(argv[1], target_resolved, sizeof(target_resolved));
    const char *linkpath = resolve_path_arg(argv[2], link_resolved, sizeof(link_resolved));
    if (!target || !linkpath) {
        printf("ln: path too long\n");
        return 1;
    }

    int rc = link(target, linkpath);
    if (rc != 0) {
        printf("ln: failed to create link\n");
        return 1;
    }
    return 0;
}

static int find_walk(const char *path, const char *pattern, int type_filter)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 1;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!pattern || fnmatch(pattern, base, 0) == 0) {
        if (type_filter == 0 ||
            (type_filter == 'f' && S_ISREG(st.st_mode)) ||
            (type_filter == 'd' && S_ISDIR(st.st_mode))) {
            printf("%s\n", path);
        }
    }

    if (!S_ISDIR(st.st_mode)) {
        return 0;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return 1;
    }

    struct dirent *entry;
    char child[EXTRA_PATH_BUF * 2];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") == 0 ? "" : path, entry->d_name) >= (int)sizeof(child)) {
            continue;
        }
        find_walk(child[0] ? child : "/", pattern, type_filter);
    }
    closedir(dir);
    return 0;
}

int cmd_find(int argc, char **argv)
{
    const char *start = ".";
    const char *pattern = NULL;
    int type_filter = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            type_filter = argv[++i][0];
        } else if (argv[i][0] != '-') {
            start = argv[i];
        }
    }

    char resolved[EXTRA_PATH_BUF];
    const char *path = resolve_path_arg(start, resolved, sizeof(resolved));
    if (!path) {
        printf("find: path too long\n");
        return 1;
    }
    return find_walk(path, pattern, type_filter);
}

static int grep_stream(FILE *in, const char *pattern, bool ignore_case, bool show_line_numbers)
{
    char line[512];
    int lineno = 0;
    int matched = 1;
    while (fgets(line, sizeof(line), in)) {
        lineno++;
        char haystack[512];
        char needle[128];
        const char *search_line = line;
        const char *search_pat = pattern;
        if (ignore_case) {
            size_t i;
            for (i = 0; i < sizeof(haystack) - 1 && line[i]; ++i) haystack[i] = (char)tolower((unsigned char)line[i]);
            haystack[i] = '\0';
            for (i = 0; i < sizeof(needle) - 1 && pattern[i]; ++i) needle[i] = (char)tolower((unsigned char)pattern[i]);
            needle[i] = '\0';
            search_line = haystack;
            search_pat = needle;
        }
        if (strstr(search_line, search_pat)) {
            if (show_line_numbers) {
                printf("%d:%s", lineno, line);
            } else {
                fputs(line, stdout);
            }
            matched = 0;
        }
    }
    return matched;
}

int cmd_grep(int argc, char **argv)
{
    bool ignore_case = false;
    bool show_line_numbers = false;
    const char *pattern = NULL;
    const char *file = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0) {
            ignore_case = true;
        } else if (strcmp(argv[i], "-n") == 0) {
            show_line_numbers = true;
        } else if (!pattern) {
            pattern = argv[i];
        } else if (!file) {
            file = argv[i];
        }
    }

    if (!pattern) {
        printf("Usage: grep [-i] [-n] <pattern> [file]\n");
        return 1;
    }

    if (!file || strcmp(file, "-") == 0) {
        return grep_stream(stdin, pattern, ignore_case, show_line_numbers);
    }

    char resolved[EXTRA_PATH_BUF];
    const char *path = resolve_path_arg(file, resolved, sizeof(resolved));
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        printf("grep: %s: No such file\n", file);
        return 1;
    }
    int ret = grep_stream(f, pattern, ignore_case, show_line_numbers);
    fclose(f);
    return ret;
}

static int parse_sed_subst(const char *expr, char *old, size_t old_sz, char *newv, size_t new_sz, bool *global)
{
    if (!expr || expr[0] != 's' || !expr[1]) {
        return -1;
    }
    char delim = expr[1];
    const char *p = expr + 2;
    const char *a = strchr(p, delim);
    if (!a) return -1;
    const char *b = strchr(a + 1, delim);
    if (!b) return -1;
    size_t old_len = (size_t)(a - p);
    size_t new_len = (size_t)(b - (a + 1));
    if (old_len >= old_sz || new_len >= new_sz) return -1;
    memcpy(old, p, old_len);
    old[old_len] = '\0';
    memcpy(newv, a + 1, new_len);
    newv[new_len] = '\0';
    *global = strchr(b + 1, 'g') != NULL;
    return 0;
}

static void sed_apply_line(const char *line, const char *old, const char *newv, bool global)
{
    const char *p = line;
    size_t old_len = strlen(old);
    if (old_len == 0) {
        fputs(line, stdout);
        return;
    }
    while (*p) {
        const char *m = strstr(p, old);
        if (!m) {
            fputs(p, stdout);
            break;
        }
        fwrite(p, 1, (size_t)(m - p), stdout);
        fputs(newv, stdout);
        p = m + old_len;
        if (!global) {
            fputs(p, stdout);
            break;
        }
    }
}

int cmd_sed(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sed 's/old/new/[g]' [file]\n");
        return 1;
    }

    char old[128], newv[128];
    bool global = false;
    if (parse_sed_subst(argv[1], old, sizeof(old), newv, sizeof(newv), &global) != 0) {
        printf("sed: only substitution form s/old/new/[g] is supported\n");
        return 1;
    }

    FILE *f = stdin;
    if (argc > 2 && strcmp(argv[2], "-") != 0) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(argv[2], resolved, sizeof(resolved));
        f = path ? fopen(path, "r") : NULL;
        if (!f) {
            printf("sed: %s: No such file\n", argv[2]);
            return 1;
        }
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        sed_apply_line(line, old, newv, global);
    }

    if (f != stdin) fclose(f);
    return 0;
}

static int cmp_lines_asc(const void *a, const void *b)
{
    const char *const *la = a;
    const char *const *lb = b;
    return strcmp(*la, *lb);
}

static int cmp_lines_desc(const void *a, const void *b)
{
    return cmp_lines_asc(b, a);
}

static int read_lines(FILE *f, char ***out_lines, size_t *out_count)
{
    char **lines = NULL;
    size_t count = 0;
    size_t cap = 0;
    char buf[512];

    while (fgets(buf, sizeof(buf), f)) {
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 16;
            char **tmp = realloc(lines, new_cap * sizeof(char *));
            if (!tmp) {
                goto fail;
            }
            lines = tmp;
            cap = new_cap;
        }
        lines[count] = strdup(buf);
        if (!lines[count]) {
            goto fail;
        }
        count++;
    }

    *out_lines = lines;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; ++i) free(lines[i]);
    free(lines);
    return 1;
}

static void free_lines(char **lines, size_t count)
{
    for (size_t i = 0; i < count; ++i) free(lines[i]);
    free(lines);
}

int cmd_sort(int argc, char **argv)
{
    bool reverse = false;
    bool unique = false;
    const char *file = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-r") == 0) reverse = true;
        else if (strcmp(argv[i], "-u") == 0) unique = true;
        else if (!file) file = argv[i];
    }

    FILE *f = stdin;
    if (file && strcmp(file, "-") != 0) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(file, resolved, sizeof(resolved));
        f = path ? fopen(path, "r") : NULL;
        if (!f) {
            printf("sort: %s: No such file\n", file);
            return 1;
        }
    }

    char **lines = NULL;
    size_t count = 0;
    if (read_lines(f, &lines, &count) != 0) {
        if (f != stdin) fclose(f);
        printf("sort: out of memory\n");
        return 1;
    }
    if (f != stdin) fclose(f);

    qsort(lines, count, sizeof(char *), reverse ? cmp_lines_desc : cmp_lines_asc);
    for (size_t i = 0; i < count; ++i) {
        if (unique && i > 0 && strcmp(lines[i - 1], lines[i]) == 0) {
            continue;
        }
        fputs(lines[i], stdout);
    }
    free_lines(lines, count);
    return 0;
}

int cmd_uniq(int argc, char **argv)
{
    bool show_count = false;
    const char *file = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0) show_count = true;
        else if (!file) file = argv[i];
    }

    FILE *f = stdin;
    if (file && strcmp(file, "-") != 0) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(file, resolved, sizeof(resolved));
        f = path ? fopen(path, "r") : NULL;
        if (!f) {
            printf("uniq: %s: No such file\n", file);
            return 1;
        }
    }

    char prev[512] = {0};
    char line[512];
    int run = 0;
    while (fgets(line, sizeof(line), f)) {
        if (run == 0) {
            strlcpy(prev, line, sizeof(prev));
            run = 1;
            continue;
        }
        if (strcmp(prev, line) == 0) {
            run++;
            continue;
        }
        if (show_count) printf("%4d %s", run, prev);
        else fputs(prev, stdout);
        strlcpy(prev, line, sizeof(prev));
        run = 1;
    }
    if (run > 0) {
        if (show_count) printf("%4d %s", run, prev);
        else fputs(prev, stdout);
    }
    if (f != stdin) fclose(f);
    return 0;
}

int cmd_cut(int argc, char **argv)
{
    const char *file = NULL;
    char delim = '\t';
    int field = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) delim = argv[++i][0];
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) field = atoi(argv[++i]);
        else if (!file) file = argv[i];
    }
    if (field <= 0) {
        printf("cut: invalid field\n");
        return 1;
    }

    FILE *f = stdin;
    if (file && strcmp(file, "-") != 0) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(file, resolved, sizeof(resolved));
        f = path ? fopen(path, "r") : NULL;
        if (!f) {
            printf("cut: %s: No such file\n", file);
            return 1;
        }
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int current = 1;
        char *start = line;
        for (char *p = line;; ++p) {
            if (*p == delim || *p == '\n' || *p == '\0') {
                if (current == field) {
                    char saved = *p;
                    *p = '\0';
                    printf("%s\n", start);
                    *p = saved;
                    break;
                }
                if (*p == '\0' || *p == '\n') {
                    break;
                }
                current++;
                start = p + 1;
            }
        }
    }
    if (f != stdin) fclose(f);
    return 0;
}

int cmd_tr(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: tr <set1> <set2> [file]\n");
        return 1;
    }

    const char *set1 = argv[1];
    const char *set2 = argv[2];
    const char *file = argc > 3 ? argv[3] : NULL;
    FILE *f = stdin;
    if (file && strcmp(file, "-") != 0) {
        char resolved[EXTRA_PATH_BUF];
        const char *path = resolve_path_arg(file, resolved, sizeof(resolved));
        f = path ? fopen(path, "r") : NULL;
        if (!f) {
            printf("tr: %s: No such file\n", file);
            return 1;
        }
    }

    int ch;
    size_t len1 = strlen(set1), len2 = strlen(set2);
    while ((ch = fgetc(f)) != EOF) {
        const char *m = strchr(set1, ch);
        if (m) {
            size_t idx = (size_t)(m - set1);
            if (idx < len2) ch = (unsigned char)set2[idx];
            else if (len2 > 0) ch = (unsigned char)set2[len2 - 1];
        }
        fputc(ch, stdout);
    }
    if (f != stdin) fclose(f);
    return 0;
}

static int describe_command(const char *name, bool verbose)
{
    const esp_console_cmd_t *cmd = breezybox_find_command(name);
    char path[EXTRA_PATH_BUF];
    char *ext = find_external_command(name, path, sizeof(path));

    if (cmd) {
        if (verbose) printf("%s is a shell builtin\n", name);
        else printf("%s\n", name);
        return 0;
    }
    if (ext) {
        if (verbose) printf("%s is %s\n", name, ext);
        else printf("%s\n", ext);
        return 0;
    }
    printf("%s: not found\n", name);
    return 1;
}

int cmd_which(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: which <command...>\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; ++i) ret |= describe_command(argv[i], false);
    return ret;
}

int cmd_type(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: type <command...>\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; ++i) ret |= describe_command(argv[i], true);
    return ret;
}

int cmd_uname(int argc, char **argv)
{
    bool all = argc > 1 && strcmp(argv[1], "-a") == 0;
    esp_chip_info_t info;
    esp_chip_info(&info);
    if (all) {
        printf("BreezyBox ESP32-S3 IDF-%s cores=%d rev=%d wifi%s%s\n",
               IDF_VER,
               info.cores,
               info.revision,
               (info.features & CHIP_FEATURE_BT) ? " bt" : "",
               (info.features & CHIP_FEATURE_BLE) ? " ble" : "");
    } else {
        printf("ESP32-S3\n");
    }
    return 0;
}

int cmd_env(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!environ) return 0;
    for (char **p = environ; *p; ++p) {
        printf("%s\n", *p);
    }
    return 0;
}

int cmd_export(int argc, char **argv)
{
    if (argc < 2) {
        return cmd_env(argc, argv);
    }
    int errors = 0;
    for (int i = 1; i < argc; ++i) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *val = getenv(argv[i]);
            if (val) printf("%s=%s\n", argv[i], val);
            else printf("%s is not set\n", argv[i]);
            continue;
        }
        *eq = '\0';
        if (setenv(argv[i], eq + 1, 1) != 0) {
            printf("export: failed to set %s\n", argv[i]);
            errors++;
        }
        *eq = '=';
    }
    return errors ? 1 : 0;
}

int cmd_unset(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: unset <name...>\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        unsetenv(argv[i]);
    }
    return 0;
}

int cmd_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) {
        return 0;
    }
    char line[256];
    int n = 1;
    while (fgets(line, sizeof(line), f)) {
        printf("%4d  %s", n++, line);
    }
    fclose(f);
    return 0;
}

int cmd_source(int argc, char **argv)
{
    return cmd_sh(argc, argv);
}

int cmd_true(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

int cmd_false(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

static int test_eval(int argc, char **argv)
{
    if (argc == 2) {
        return argv[1][0] ? 0 : 1;
    }
    if (argc == 3) {
        if (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "-f") == 0 || strcmp(argv[1], "-d") == 0) {
            char resolved[EXTRA_PATH_BUF];
            const char *path = resolve_path_arg(argv[2], resolved, sizeof(resolved));
            struct stat st;
            if (!path || stat(path, &st) != 0) return 1;
            if (strcmp(argv[1], "-e") == 0) return 0;
            if (strcmp(argv[1], "-f") == 0) return S_ISREG(st.st_mode) ? 0 : 1;
            return S_ISDIR(st.st_mode) ? 0 : 1;
        }
        if (strcmp(argv[1], "-n") == 0) return argv[2][0] ? 0 : 1;
        if (strcmp(argv[1], "-z") == 0) return argv[2][0] ? 1 : 0;
    }
    if (argc == 4) {
        if (strcmp(argv[2], "=") == 0) return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        if (strcmp(argv[2], "!=") == 0) return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
    }
    printf("Usage: test [-e|-f|-d path] | [-n|-z string] | <a> = <b>\n");
    return 2;
}

int cmd_test(int argc, char **argv)
{
    if (argc > 0 && strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            printf("[: missing ]\n");
            return 2;
        }
        argv[argc - 1] = NULL;
        argc--;
    }
    return test_eval(argc, argv);
}

int cmd_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fflush(NULL);
    return 0;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = calloc(count ? count : 1, sizeof(TaskStatus_t));
    if (!tasks) {
        printf("ps: out of memory\n");
        return 1;
    }
    count = uxTaskGetSystemState(tasks, count, NULL);
    printf("Name            State Prio Stack Handle\n");
    for (UBaseType_t i = 0; i < count; ++i) {
        char state = '?';
        switch (tasks[i].eCurrentState) {
            case eRunning: state = 'R'; break;
            case eReady: state = 'r'; break;
            case eBlocked: state = 'S'; break;
            case eSuspended: state = 'T'; break;
            case eDeleted: state = 'X'; break;
            default: break;
        }
        printf("%-15s %c %4u %5u %p\n",
               tasks[i].pcTaskName,
               state,
               (unsigned)tasks[i].uxCurrentPriority,
               (unsigned)tasks[i].usStackHighWaterMark,
               tasks[i].xHandle);
    }
    free(tasks);
    return 0;
}

int cmd_kill(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: kill <task-name>\n");
        return 1;
    }
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = calloc(count ? count : 1, sizeof(TaskStatus_t));
    if (!tasks) {
        printf("kill: out of memory\n");
        return 1;
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    count = uxTaskGetSystemState(tasks, count, NULL);
    for (UBaseType_t i = 0; i < count; ++i) {
        if (strcmp(tasks[i].pcTaskName, argv[1]) == 0) {
            if (tasks[i].xHandle == self) {
                free(tasks);
                printf("kill: refusing to delete current task\n");
                return 1;
            }
            vTaskDelete(tasks[i].xHandle);
            free(tasks);
            return 0;
        }
    }
    free(tasks);
    printf("kill: task not found: %s\n", argv[1]);
    return 1;
}
#else
int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("ps: task listing not enabled in this build\n");
    return 1;
}

int cmd_kill(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("kill: task control not enabled in this build\n");
    return 1;
}
#endif

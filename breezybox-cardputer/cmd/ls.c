#include "breezy_cmd.h"
#include "breezy_vfs.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int cmd_ls(int argc, char **argv)
{
    int show_all = 0;
    const char *path = breezybox_cwd();
    char resolved[BREEZYBOX_MAX_PATH * 2 + 2];
    int argi = 1;

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        show_all = 1;
        argi = 2;
    }
    if (argi < argc) {
        path = argv[argi];
    }

    if (path[0] != '/') {
        if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
            printf("ls: path too long\n");
            return 1;
        }
        path = resolved;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        printf("ls: cannot access '%s'\n", path);
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!show_all && entry->d_name[0] == '.') {
            continue;
        }

        char entry_path[BREEZYBOX_MAX_PATH * 2 + 258];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(entry_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                printf("%-20s  <DIR>\n", entry->d_name);
            } else {
                printf("%-20s  %7ld\n", entry->d_name, st.st_size);
            }
        } else {
            printf("%-20s\n", entry->d_name);
        }
    }
    closedir(dir);
    return 0;
}

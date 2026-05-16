#include "breezy_cmd.h"
#include "breezy_pager.h"
#include "breezy_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PAGE_LINES 15

int cmd_more(int argc, char **argv)
{
    int page_lines = DEFAULT_PAGE_LINES;
    const char *filename = NULL;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            page_lines = atoi(argv[++i]);
            if (page_lines <= 0) page_lines = DEFAULT_PAGE_LINES;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }
    
    if (!filename) {
        printf("Usage: more [-n lines] <file>\n");
        return 1;
    }
    
    char resolved[BREEZYBOX_MAX_PATH * 2 + 2];
    const char *path = filename;
    if (path[0] != '/') {
        if (!breezybox_resolve_path(path, resolved, sizeof(resolved))) {
            printf("more: path too long\n");
            return 1;
        }
        path = resolved;
    }
    
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("more: %s: No such file\n", filename);
        return 1;
    }
    
    char line[256];
    breezy_pager_t pager;
    breezy_pager_init(&pager, page_lines);
    
    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);

        if (strchr(line, '\n') && !breezy_pager_step(&pager)) {
            break;
        }
    }
    
    fclose(f);
    return 0;
}

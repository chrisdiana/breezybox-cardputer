#pragma once

#include <stdbool.h>

typedef struct {
    int page_lines;
    int line_count;
} breezy_pager_t;

void breezy_pager_init(breezy_pager_t *pager, int page_lines);
bool breezy_pager_step(breezy_pager_t *pager);

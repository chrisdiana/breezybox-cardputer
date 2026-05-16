#include "breezy_pager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#define DEFAULT_PAGE_LINES 15
#define MORE_PROMPT "--More-- (Enter=next line, Space=next page, q=quit)"
#define CLEAR_PROMPT "\r                                                        \r"

void breezy_pager_init(breezy_pager_t *pager, int page_lines)
{
    if (!pager) {
        return;
    }

    pager->page_lines = (page_lines > 0) ? page_lines : DEFAULT_PAGE_LINES;
    pager->line_count = 0;
}

bool breezy_pager_step(breezy_pager_t *pager)
{
    if (!pager) {
        return true;
    }

    pager->line_count++;
    if (pager->line_count < pager->page_lines) {
        return true;
    }

    printf("%s", MORE_PROMPT);
    fflush(stdout);

    int c;
    while (1) {
        c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        break;
    }

    printf("%s", CLEAR_PROMPT);

    if (c == 'q' || c == 'Q') {
        return false;
    }

    if (c == '\n' || c == '\r') {
        pager->line_count = pager->page_lines - 1;
    } else {
        pager->line_count = 0;
    }

    return true;
}

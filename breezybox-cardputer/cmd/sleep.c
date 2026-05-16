#include "breezy_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_sleep(int argc, char **argv)
{
    char *end = NULL;
    double seconds = 0.0;

    if (argc != 2) {
        printf("Usage: sleep <seconds>\n");
        return 1;
    }

    seconds = strtod(argv[1], &end);
    if (end == argv[1] || (end && *end != '\0') || seconds < 0.0) {
        printf("sleep: invalid duration: %s\n", argv[1]);
        return 1;
    }

    TickType_t ticks = (TickType_t)(seconds * 1000.0 / portTICK_PERIOD_MS);
    if (seconds > 0.0 && ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
    return 0;
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

typedef struct {
    TaskHandle_t waiter;
    int rc;
} ccleste_ctx_t;

#define main cmd_ccleste_game_main
#include "../../breezybox-cardputer/apps/ccleste/main.c"
#undef main

static void ccleste_task(void *arg)
{
    ccleste_ctx_t *ctx = (ccleste_ctx_t *)arg;
    int rc = cmd_ccleste_game_main(0, NULL);

    if (ctx) {
        ctx->rc = rc;
        if (ctx->waiter) {
            xTaskNotifyGive(ctx->waiter);
        }
    }

    vTaskDelete(NULL);
}

int cmd_ccleste_builtin_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ccleste_ctx_t *ctx = calloc(1, sizeof(*ctx));
    TaskHandle_t task = NULL;
    int rc = 1;
    static const uint32_t stack_sizes[] = { 24576, 20480, 16384 };

    if (!ctx) {
        printf("ccleste: out of memory\n");
        return 1;
    }

    ctx->waiter = xTaskGetCurrentTaskHandle();
    ctx->rc = 1;

    BaseType_t ok = pdFAIL;
    for (size_t i = 0; i < sizeof(stack_sizes) / sizeof(stack_sizes[0]); ++i) {
        ok = xTaskCreatePinnedToCore(
            ccleste_task,
            "ccleste",
            stack_sizes[i],
            ctx,
            5,
            &task,
            0);
        if (ok == pdPASS && task) {
            break;
        }
        task = NULL;
    }

    if (ok != pdPASS || !task) {
        printf("ccleste: failed to start task (heap=%u largest=%u)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(ctx);
        return 1;
    }

    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    rc = ctx->rc;
    free(ctx);
    return rc;
}

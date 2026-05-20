/*
 * breezy_http.c - HTTP/HTTPS download helper
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "breezybox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int check_network(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) return 0;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return 0;
    return ip_info.ip.addr != 0;
}

static int do_download(const char *url, const char *dest_path)
{
    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = 30000,
        .buffer_size       = 512,   /* minimize SRAM pre-allocation */
        .buffer_size_tx    = 512,
        .method            = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;

    FILE *f   = NULL;
    int   ret = -1;

    if (esp_http_client_open(client, 0) != ESP_OK) goto cleanup;

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        printf("wget: HTTP %d\n", status);
        goto cleanup;
    }

    f = fopen(dest_path, "wb");
    if (!f) goto cleanup;

    char buf[512];
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) goto cleanup;
    }
    if (n < 0) goto cleanup;

    ret = 0;

cleanup:
    if (f && ret != 0) { fclose(f); unlink(dest_path); }
    else if (f) fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

int breezy_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return -1;
    if (!check_network()) return -2;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(2000));
        if (do_download(url, dest_path) == 0) return 0;
    }
    return -1;
}

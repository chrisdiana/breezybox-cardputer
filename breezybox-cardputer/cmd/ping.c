#include "breezy_cmd.h"
#include "esp_err.h"
#include "esp_ping.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SemaphoreHandle_t done;
} ping_ctx_t;

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr;
    char addr_str[64];

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    if (!ipaddr_ntoa_r(&target_addr, addr_str, sizeof(addr_str))) {
        strlcpy(addr_str, "?", sizeof(addr_str));
    }

    printf("%lu bytes from %s: icmp_seq=%u ttl=%u time=%lu ms\n",
           (unsigned long)recv_len,
           addr_str,
           (unsigned)seqno,
           (unsigned)ttl,
           (unsigned long)elapsed);
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    printf("Request timeout for icmp_seq %u\n", (unsigned)seqno);
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration = 0;
    ip_addr_t target_addr;
    char addr_str[64];

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &duration, sizeof(duration));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    if (!ipaddr_ntoa_r(&target_addr, addr_str, sizeof(addr_str))) {
        strlcpy(addr_str, "?", sizeof(addr_str));
    }

    printf("\n--- %s ping statistics ---\n", addr_str);
    printf("%lu packets transmitted, %lu received, %lu%% packet loss, time %lu ms\n",
           (unsigned long)transmitted,
           (unsigned long)received,
           transmitted ? (unsigned long)(((transmitted - received) * 100UL) / transmitted) : 0UL,
           (unsigned long)duration);

    if (ctx && ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}

static int resolve_ping_target(const char *host, ip_addr_t *target_addr)
{
    struct sockaddr_in6 sock_addr6;
    struct addrinfo hint = {0};
    struct addrinfo *res = NULL;

    memset(target_addr, 0, sizeof(*target_addr));
    if (inet_pton(AF_INET6, host, &sock_addr6.sin6_addr) == 1) {
        return ipaddr_aton(host, target_addr) ? 0 : -1;
    }

    if (ipaddr_aton(host, target_addr)) {
        return 0;
    }

    if (getaddrinfo(host, NULL, &hint, &res) != 0 || !res) {
        return -1;
    }

#if CONFIG_LWIP_IPV4
    if (res->ai_family == AF_INET) {
        struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(target_addr), &addr4);
        freeaddrinfo(res);
        return 0;
    }
#endif
#if CONFIG_LWIP_IPV6
    if (res->ai_family == AF_INET6) {
        struct in6_addr addr6 = ((struct sockaddr_in6 *)(res->ai_addr))->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(target_addr), &addr6);
        freeaddrinfo(res);
        return 0;
    }
#endif

    freeaddrinfo(res);
    return -1;
}

int cmd_ping(int argc, char **argv)
{
    const char *host = NULL;
    uint32_t count = 4;
    uint32_t timeout_ms = 1000;
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    esp_ping_callbacks_t cbs = {0};
    esp_ping_handle_t ping = NULL;
    ping_ctx_t ctx = {0};
    ip_addr_t target_addr;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) && i + 1 < argc) {
            count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if ((strcmp(argv[i], "-W") == 0 || strcmp(argv[i], "--timeout") == 0) && i + 1 < argc) {
            timeout_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-') {
            printf("ping: unknown option: %s\n", argv[i]);
            return 1;
        } else if (!host) {
            host = argv[i];
        } else {
            printf("ping: unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!host) {
        printf("Usage: ping [-c count] [-W timeout_ms] <host>\n");
        return 1;
    }

    if (resolve_ping_target(host, &target_addr) != 0) {
        printf("ping: unknown host %s\n", host);
        return 1;
    }

    config.target_addr = target_addr;
    config.count = count;
    config.timeout_ms = timeout_ms;
    config.interval_ms = 1000;

    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        printf("ping: out of memory\n");
        return 1;
    }

    cbs.cb_args = &ctx;
    cbs.on_ping_success = ping_on_success;
    cbs.on_ping_timeout = ping_on_timeout;
    cbs.on_ping_end = ping_on_end;

    if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        printf("ping: failed to create session\n");
        return 1;
    }

    printf("PING %s:\n", host);
    if (esp_ping_start(ping) != ESP_OK) {
        esp_ping_delete_session(ping);
        vSemaphoreDelete(ctx.done);
        printf("ping: failed to start\n");
        return 1;
    }

    xSemaphoreTake(ctx.done, portMAX_DELAY);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);
    return 0;
}

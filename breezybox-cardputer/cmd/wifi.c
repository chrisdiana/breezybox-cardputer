#include "breezy_cmd.h"
#include "breezy_vfs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#define NVS_NAMESPACE "breezy_wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define CONNECT_TIMEOUT_MS 15000
#define WIFI_CREDS_FILENAME ".wifi_credentials"

static esp_netif_t *s_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_event_loop_ready = false;
static bool s_netif_ready = false;
static bool s_handlers_registered = false;
static volatile bool s_connected = false;
static volatile bool s_got_ip = false;

static void get_preferred_credentials_path(char *path, size_t path_len)
{
    const char *base = breezybox_sd_mounted() ? BREEZYBOX_SD_MOUNT_POINT : BREEZYBOX_MOUNT_POINT;
    snprintf(path, path_len, "%s/%s", base, WIFI_CREDS_FILENAME);
}

static esp_err_t load_credentials_from_file(const char *path, char *ssid, size_t ssid_len,
                                            char *password, size_t pass_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    char ssid_buf[128] = {0};
    char pass_buf[128] = {0};
    char *newline;

    if (!fgets(ssid_buf, sizeof(ssid_buf), f)) {
        fclose(f);
        return ESP_FAIL;
    }
    if (!fgets(pass_buf, sizeof(pass_buf), f)) {
        pass_buf[0] = '\0';
    }
    fclose(f);

    newline = strpbrk(ssid_buf, "\r\n");
    if (newline) {
        *newline = '\0';
    }
    newline = strpbrk(pass_buf, "\r\n");
    if (newline) {
        *newline = '\0';
    }

    if (ssid_buf[0] == '\0') {
        return ESP_FAIL;
    }

    strlcpy(ssid, ssid_buf, ssid_len);
    strlcpy(password, pass_buf, pass_len);
    return ESP_OK;
}

static esp_err_t save_credentials_to_file(const char *path, const char *ssid, const char *password)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }

    if (fprintf(f, "%s\n%s\n", ssid, password ? password : "") < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    return ESP_OK;
}

static const char *auth_mode_str(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        default:                        return "?";
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_CONNECTED:
                s_connected = true;
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                s_got_ip = false;
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_got_ip = true;
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_initialized) return ESP_OK;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    if (!s_event_loop_ready) {
        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
        s_event_loop_ready = true;
    }

    if (!s_netif_ready) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) return ESP_FAIL;
        s_netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 3;
    cfg.dynamic_rx_buf_num = 4;
    cfg.static_tx_buf_num = 3;
    cfg.rx_mgmt_buf_num = 3;
    cfg.mgmt_sbuf_num = 6;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.amsdu_tx_enable = 0;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_handlers_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
        s_handlers_registered = true;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        return ret;
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        return ret;
    }

    s_wifi_initialized = true;
    return ESP_OK;
}

// ============ NVS Storage ============

static esp_err_t save_credentials_nvs(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, password ? password : "");
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t load_credentials_nvs(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);  // OK if missing
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_credentials(const char *ssid, const char *password, char *saved_path, size_t saved_path_len)
{
    char path[BREEZYBOX_MAX_PATH + 32];
    get_preferred_credentials_path(path, sizeof(path));

    esp_err_t file_ret = save_credentials_to_file(path, ssid, password);
    esp_err_t nvs_ret = save_credentials_nvs(ssid, password);

    if (saved_path && saved_path_len > 0) {
        saved_path[0] = '\0';
        if (file_ret == ESP_OK) {
            strlcpy(saved_path, path, saved_path_len);
        }
    }

    if (file_ret == ESP_OK) {
        return ESP_OK;
    }
    return nvs_ret;
}

static esp_err_t load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len,
                                  char *loaded_path, size_t loaded_path_len)
{
    char path[BREEZYBOX_MAX_PATH + 32];
    char fallback_path[BREEZYBOX_MAX_PATH + 32];
    esp_err_t ret;

    if (loaded_path && loaded_path_len > 0) {
        loaded_path[0] = '\0';
    }

    ret = load_credentials_nvs(ssid, ssid_len, password, pass_len);
    if (ret == ESP_OK && loaded_path) {
        strlcpy(loaded_path, "nvs", loaded_path_len);
        return ret;
    }

    snprintf(fallback_path, sizeof(fallback_path), "%s/%s", BREEZYBOX_MOUNT_POINT, WIFI_CREDS_FILENAME);
    ret = load_credentials_from_file(fallback_path, ssid, ssid_len, password, pass_len);
    if (ret == ESP_OK) {
        (void)save_credentials_nvs(ssid, password);
        if (loaded_path) {
            strlcpy(loaded_path, fallback_path, loaded_path_len);
        }
        return ret;
    }

    if (breezybox_sd_mounted()) {
        snprintf(path, sizeof(path), "%s/%s", BREEZYBOX_SD_MOUNT_POINT, WIFI_CREDS_FILENAME);
        ret = load_credentials_from_file(path, ssid, ssid_len, password, pass_len);
        if (ret == ESP_OK) {
            (void)save_credentials_nvs(ssid, password);
            if (loaded_path) {
                strlcpy(loaded_path, path, loaded_path_len);
            }
            return ret;
        }
    }

    return ret;
}

// ============ Commands ============

static int wifi_scan(void)
{
    if (wifi_init_once() != ESP_OK) {
        printf("WiFi init failed\n");
        return 1;
    }

    wifi_scan_config_t scan_cfg = { .show_hidden = true };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        printf("Scan failed\n");
        return 1;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        printf("No networks found\n");
        return 0;
    }

    wifi_ap_record_t *records = malloc(count * sizeof(wifi_ap_record_t));
    if (!records) {
        printf("Out of memory\n");
        return 1;
    }

    esp_wifi_scan_get_ap_records(&count, records);

    printf("%-32s  %4s  %s\n", "SSID", "RSSI", "AUTH");
    printf("--------------------------------  ----  ------\n");
    for (int i = 0; i < count; i++) {
        printf("%-32s  %4d  %s\n", 
               records[i].ssid, 
               records[i].rssi, 
               auth_mode_str(records[i].authmode));
    }

    free(records);
    return 0;
}

static int wifi_connect(const char *ssid, const char *password)
{
    if (wifi_init_once() != ESP_OK) {
        printf("WiFi init failed\n");
        return 1;
    }

    char stored_ssid[33] = {0};
    char stored_pass[65] = {0};
    char loaded_path[BREEZYBOX_MAX_PATH + 32] = {0};

    // If no SSID provided, try SD first, then LittleFS, then NVS.
    if (!ssid) {
        if (load_credentials(stored_ssid, sizeof(stored_ssid),
                            stored_pass, sizeof(stored_pass),
                            loaded_path, sizeof(loaded_path)) != ESP_OK ||
            stored_ssid[0] == '\0') {
            printf("No saved network. Usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        ssid = stored_ssid;
        password = stored_pass[0] ? stored_pass : NULL;
        if (loaded_path[0]) {
            printf("Using saved network: %s (%s)\n", ssid, loaded_path);
        } else {
            printf("Using saved network: %s\n", ssid);
        }
    }

    wifi_config_t cfg = {
        .sta = {
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        printf("Invalid config\n");
        return 1;
    }

    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    s_connected = false;
    s_got_ip = false;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));

    printf("Connecting to %s...\n", ssid);

    if (esp_wifi_connect() != ESP_OK) {
        printf("Connect failed\n");
        return 1;
    }

    int elapsed = 0;
    while (elapsed < CONNECT_TIMEOUT_MS) {
        if (s_got_ip) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;

        if (elapsed % 3000 == 0 && elapsed > 0) {
            if (s_connected && !s_got_ip) {
                printf("  Associated, waiting for IP...\n");
            }
        }
    }

    if (s_got_ip) {
        printf("Connected!\n");
        // Save credentials on success (only if user provided them)
        if (ssid != stored_ssid) {
            char saved_path[BREEZYBOX_MAX_PATH + 32] = {0};
            esp_err_t save_ret = save_credentials(ssid, password, saved_path, sizeof(saved_path));
            if (save_ret == ESP_OK) {
                if (saved_path[0]) {
                    printf("Saved network: %s (%s)\n", ssid, saved_path);
                } else {
                    printf("Saved network: %s (nvs)\n", ssid);
                }
            } else {
                printf("Warning: failed to save network (%s)\n", esp_err_to_name(save_ret));
            }
        }
        return 0;
    } else if (s_connected) {
        printf("Associated but no IP (DHCP timeout)\n");
        return 1;
    } else {
        printf("Connection failed\n");
        return 1;
    }
}

static int wifi_disconnect_cmd(void)
{
    if (!s_wifi_initialized) {
        printf("WiFi not initialized\n");
        return 1;
    }

    esp_wifi_disconnect();
    s_connected = false;
    s_got_ip = false;
    printf("Disconnected\n");
    return 0;
}

static int wifi_status(void)
{
    if (!s_wifi_initialized) {
        printf("WiFi not initialized\n");
        return 0;
    }

    wifi_ap_record_t ap;
    if (!s_got_ip || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        printf("Not connected\n");
        
        // Show saved network if any
        char saved_ssid[33] = {0};
        char saved_pass[65] = {0};
        char loaded_path[BREEZYBOX_MAX_PATH + 32] = {0};
        if (load_credentials(saved_ssid, sizeof(saved_ssid),
                            saved_pass, sizeof(saved_pass),
                            loaded_path, sizeof(loaded_path)) == ESP_OK &&
            saved_ssid[0] != '\0') {
            if (loaded_path[0]) {
                printf("Saved: %s (%s)\n", saved_ssid, loaded_path);
            } else {
                printf("Saved: %s\n", saved_ssid);
            }
        }
        return 0;
    }

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_netif, &ip);

    printf("SSID:    %s\n", ap.ssid);
    printf("RSSI:    %d dBm\n", ap.rssi);
    printf("IP:      " IPSTR "\n", IP2STR(&ip.ip));
    printf("Gateway: " IPSTR "\n", IP2STR(&ip.gw));
    printf("Netmask: " IPSTR "\n", IP2STR(&ip.netmask));

    return 0;
}

static int wifi_forget(void)
{
    int forgot_any = 0;
    char path[BREEZYBOX_MAX_PATH + 32];

    snprintf(path, sizeof(path), "%s/%s", BREEZYBOX_SD_MOUNT_POINT, WIFI_CREDS_FILENAME);
    if (remove(path) == 0) {
        forgot_any = 1;
    }

    snprintf(path, sizeof(path), "%s/%s", BREEZYBOX_MOUNT_POINT, WIFI_CREDS_FILENAME);
    if (remove(path) == 0) {
        forgot_any = 1;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_SSID);
        nvs_erase_key(handle, NVS_KEY_PASS);
        nvs_commit(handle);
        nvs_close(handle);
        forgot_any = 1;
    }

    if (forgot_any) {
        printf("Saved network forgotten\n");
    } else {
        printf("No saved network\n");
    }
    return 0;
}

int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: wifi <scan|connect|disconnect|status|forget>\n");
        printf("       wifi connect [ssid] [password]\n");
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "scan") == 0) {
        return wifi_scan();
    } 
    else if (strcmp(subcmd, "connect") == 0) {
        const char *ssid = (argc > 2) ? argv[2] : NULL;
        const char *pass = (argc > 3) ? argv[3] : NULL;
        return wifi_connect(ssid, pass);
    } 
    else if (strcmp(subcmd, "disconnect") == 0) {
        return wifi_disconnect_cmd();
    } 
    else if (strcmp(subcmd, "status") == 0) {
        return wifi_status();
    }
    else if (strcmp(subcmd, "forget") == 0) {
        return wifi_forget();
    }
    else {
        printf("Unknown: %s\n", subcmd);
        return 1;
    }
}
